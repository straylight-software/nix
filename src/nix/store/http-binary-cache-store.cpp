#include "nix/store/http-binary-cache-store.h"

#include "nix/store/filetransfer.h"
#include "nix/store/globals.h"
#include "nix/store/nar-info-disk-cache.h"
#include "nix/store/store-registration.h"
#include "nix/util/callback.h"

namespace nix {

make_error(UploadToHTTP, Error);

string_set_t HttpBinaryCacheStoreConfig::uriSchemes() {
  static bool force_http = get_env("_NIX_FORCE_HTTP") == "1";
  auto ret = string_set_t{"http", "https"};
  if (force_http)
    ret.insert("file");
  return ret;
}

HttpBinaryCacheStoreConfig::HttpBinaryCacheStoreConfig(std::string_view scheme,
                                                       std::string_view _cacheUri,
                                                       const Params& params)
    : StoreConfig(params),
      BinaryCacheStoreConfig(params),
      cacheUri(parse_url(
          std::string{scheme} + "://" +
          (!_cacheUri.empty()
               ? _cacheUri
               : throw UsageError("`%s` Store requires a non-empty authority in Store URL",
                                  scheme)))) {
  while (!cacheUri.path().empty() && cacheUri.path().back() == "")
    cacheUri.path().pop_back();
}

StoreReference HttpBinaryCacheStoreConfig::getReference() const {
  return {
      .variant =
          StoreReference::Specified{
              .scheme = cacheUri.scheme(),
              .authority = cacheUri.render_authority_and_path(),
          },
      .params = getQueryParams(),
  };
}

std::string HttpBinaryCacheStoreConfig::doc() {
  return
#include "http-binary-cache-store.md"
      ;
}

http_binary_cache_store::http_binary_cache_store(ref<config_t> config)
    : Store{*config} // TODO it will actually mutate the configuration
      ,
      binary_cache_store{*config},
      config{config} {
  diskCache = get_nar_info_disk_cache();
}

void http_binary_cache_store::init() {
  // FIXME: do this lazily?
  // For consistent cache key handling, use the reference without parameters
  // This matches what's used in Store::queryPathInfo() lookups
  auto cache_key = config->getReference().render(/*withParams=*/false);

  if (auto cacheInfo = diskCache->upToDateCacheExists(cache_key)) {
    config->want_mass_query.set_default(cacheInfo->want_mass_query);
    config->priority.set_default(cacheInfo->priority);
  } else {
    try {
      binary_cache_store::init();
    } catch (UploadToHTTP&) {
      throw Error("'%s' does not appear to be a binary cache", config->cacheUri.to_string());
    }
    diskCache->createCache(cache_key, config->store_dir, config->want_mass_query, config->priority);
  }
}

std::optional<std::string>
http_binary_cache_store::get_compression_method(const std::string& path) {
  if (has_suffix(path, ".narinfo") && !config->narinfoCompression.get().empty())
    return config->narinfoCompression;
  else if (has_suffix(path, ".ls") && !config->lsCompression.get().empty())
    return config->lsCompression;
  else if (has_prefix(path, "log/") && !config->logCompression.get().empty())
    return config->logCompression;
  else
    return std::nullopt;
}

void http_binary_cache_store::maybeDisable() {
  auto state(_state.lock());
  if (state->enabled && settings.try_fallback) {
    int t = 60;
    printError("disabling binary cache '%s' for %s seconds", config->getHumanReadableURI(), t);
    state->enabled = false;
    state->disabledUntil = std::chrono::steady_clock::now() + std::chrono::seconds(t);
  }
}

void http_binary_cache_store::checkEnabled() {
  auto state(_state.lock());
  if (state->enabled)
    return;
  if (std::chrono::steady_clock::now() > state->disabledUntil) {
    state->enabled = true;
    debug("re-enabling binary cache '%s'", config->getHumanReadableURI());
    return;
  }
  throw SubstituterDisabled("substituter '%s' is disabled", config->getHumanReadableURI());
}

bool http_binary_cache_store::file_exists(const std::string& path) {
  checkEnabled();

  try {
    FileTransferRequest request(makeRequest(path));
    request.method = HttpMethod::Head;
    get_file_transfer()->download(request);
    return true;
  } catch (FileTransferError& e) {
    /* S3 buckets return 403 if a file doesn't exist and the
       bucket is unlistable, so treat 403 as 404. */
    if (e.error == FileTransfer::NotFound || e.error == FileTransfer::Forbidden)
      return false;
    maybeDisable();
    throw;
  }
}

void http_binary_cache_store::upload(std::string_view path, restartable_source_t& source,
                                     uint64_t size_hint, std::string_view mime_type,
                                     std::optional<headers_t> headers) {
  auto req = makeRequest(path);
  req.method = HttpMethod::Put;

  if (headers) {
    req.headers.reserve(req.headers.size() + headers->size());
    std::ranges::move(std::move(*headers), std::back_inserter(req.headers));
  }

  req.data = {size_hint, source};
  req.mime_type = mime_type;

  get_file_transfer()->upload(req);
}

void http_binary_cache_store::upsert_file(const std::string& path, restartable_source_t& source,
                                          const std::string& mime_type, uint64_t size_hint) {
  try {
    if (auto compression_method = get_compression_method(path)) {
      compressed_source_t compressed(source, *compression_method);
      headers_t headers = {{"Content-Encoding", *compression_method}};
      upload(path, compressed, compressed.size(), mime_type, std::move(headers));
    } else {
      upload(path, source, size_hint, mime_type, std::nullopt);
    }
  } catch (FileTransferError& e) {
    UploadToHTTP err(e.message());
    err.add_trace({}, "while uploading to HTTP binary cache at '%s'", config->cacheUri.to_string());
    throw err;
  }
}

FileTransferRequest http_binary_cache_store::makeRequest(std::string_view path) {
  /* Otherwise the last path fragment will get discarded. */
  auto cacheUriWithTrailingSlash = config->cacheUri;
  if (!cacheUriWithTrailingSlash.path().empty())
    cacheUriWithTrailingSlash.path().push_back("");

  /* path is not a path, but a full relative or absolute
     URL, e.g. we've seen in the wild NARINFO files have a URL
     field which is
     `nar/15f99rdaf26k39knmzry4xd0d97wp6yfpnfk1z9avakis7ipb9yg.nar?hash=wvx0nans273vb7b0cjlplsmr2z905hwd`
     (note the query param) and that gets passed here. */
  auto result = parse_url_relative(path, cacheUriWithTrailingSlash);

  /* For S3 URLs, preserve query parameters from the base URL when the
     relative path doesn't have its own query parameters. This is needed
     to preserve S3-specific parameters like endpoint and region. */
  if (config->cacheUri.scheme() == "s3" && result.query().empty()) {
    result.set_query(config->cacheUri.query());
  }

  return FileTransferRequest(result);
}

void http_binary_cache_store::getFile(const std::string& path, Sink& sink) {
  checkEnabled();
  auto request(makeRequest(path));
  try {
    get_file_transfer()->download(std::move(request), sink);
  } catch (FileTransferError& e) {
    if (e.error == FileTransfer::NotFound || e.error == FileTransfer::Forbidden)
      throw NoSuchBinaryCacheFile("file '%s' does not exist in binary cache '%s'", path,
                                  config->getHumanReadableURI());
    maybeDisable();
    throw;
  }
}

void http_binary_cache_store::getFile(const std::string& path,
                                      Callback<std::optional<std::string>> callback) noexcept {
  auto callbackPtr = std::make_shared<decltype(callback)>(std::move(callback));

  try {
    checkEnabled();

    auto request(makeRequest(path));

    get_file_transfer()->enqueueFileTransfer(
        request, {[callbackPtr, this](std::future<FileTransferResult> result) {
          try {
            (*callbackPtr)(std::move(result.get().data));
          } catch (FileTransferError& e) {
            if (e.error == FileTransfer::NotFound || e.error == FileTransfer::Forbidden)
              return (*callbackPtr)({});
            maybeDisable();
            callbackPtr->rethrow();
          } catch (...) {
            callbackPtr->rethrow();
          }
        }});

  } catch (...) {
    callbackPtr->rethrow();
    return;
  }
}

std::optional<std::string> http_binary_cache_store::getNixCacheInfo() {
  try {
    auto result = get_file_transfer()->download(makeRequest(cacheInfoFile));
    return result.data;
  } catch (FileTransferError& e) {
    if (e.error == FileTransfer::NotFound)
      return std::nullopt;
    maybeDisable();
    throw;
  }
}

/**
 * This isn't actually necessary read only. We support "upsert" now, so we
 * have a notion of authentication via HTTP POST/PUT.
 *
 * For now, we conservatively say we don't know.
 *
 * \todo try to expose our HTTP authentication status.
 */
std::optional<TrustedFlag> http_binary_cache_store::isTrustedClient() {
  return std::nullopt;
}

ref<Store> http_binary_cache_store::config_t::open_store() const {
  return make_ref<http_binary_cache_store>(
      ref{// FIXME we shouldn't actually need a mutable config
          std::const_pointer_cast<http_binary_cache_store::config_t>(shared_from_this())});
}

static RegisterStoreImplementation<http_binary_cache_store::config_t> reg_http_binary_cache_store;

} // namespace nix
