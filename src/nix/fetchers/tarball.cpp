#include "nix/fetchers/tarball.h"

#include "nix/fetchers/cache.h"
#include "nix/fetchers/fetch-settings.h"
#include "nix/fetchers/fetchers.h"
#include "nix/fetchers/git-utils.h"
#include "nix/store/filetransfer.h"
#include "nix/store/store-api.h"
#include "nix/util/archive.h"
#include "nix/util/tarfile.h"
#include "nix/util/types.h"

namespace nix::fetchers {

DownloadFileResult download_file(Store& store, const settings_t& settings, const std::string& url,
                                const std::string& name, const headers_t& headers) {
  // FIXME: check store

  cache_t::Key key{"file",
                 {{
                     {"url", url},
                     {"name", name},
                 }}};

  auto cached = settings.get_cache()->lookupStorePath(key, store);

  auto use_cached = [&]() -> DownloadFileResult {
    return {
        .store_path = std::move(cached->store_path),
        .etag = get_str_attr(cached->value, "etag"),
        .effectiveUrl = get_str_attr(cached->value, "url"),
        .immutableUrl = maybe_get_str_attr(cached->value, "immutableUrl"),
    };
  };

  if (cached && !cached->expired)
    return use_cached();

  FileTransferRequest request(verbatim_url_t{url});
  request.headers = headers;
  if (cached)
    request.expectedETag = get_str_attr(cached->value, "etag");
  FileTransferResult res;
  try {
    res = get_file_transfer()->download(request);
  } catch (FileTransferError& e) {
    if (cached) {
      warn("%s; using cached version", e.msg());
      return use_cached();
    } else
      throw;
  }

  Attrs info_attrs({
      {"etag", res.etag},
  });

  if (res.immutableUrl)
    info_attrs.emplace("immutableUrl", *res.immutableUrl);

  std::optional<StorePath> store_path;

  if (res.cached) {
    assert(cached);
    store_path = std::move(cached->store_path);
  } else {
    string_sink_t sink;
    dump_string(res.data, sink);
    auto hash = hash_string(hash_algorithm_t::SHA256, res.data);
    auto info = ValidPathInfo::makeFromCA(store, name,
                                          FixedOutputInfo{
                                              .method = file_ingestion_method_t::flat,
                                              .hash = hash,
                                              .references = {},
                                          },
                                          hash_string(hash_algorithm_t::SHA256, sink.s));
    info.nar_size = sink.s.size();
    auto source = string_source_t{sink.s};
    store.add_to_store(info, source, NoRepair, NoCheckSigs);
    store_path = std::move(info.path);
  }

  /* cache_t metadata for all URLs in the redirect chain. */
  for (auto& url : res.urls) {
    key.second.insert_or_assign("url", url);
    assert(!res.urls.empty());
    info_attrs.insert_or_assign("url", *res.urls.rbegin());
    settings.get_cache()->upsert(key, store, info_attrs, *store_path);
  }

  return {
      .store_path = std::move(*store_path),
      .etag = res.etag,
      .effectiveUrl = *res.urls.rbegin(),
      .immutableUrl = res.immutableUrl,
  };
}

static DownloadTarballResult download_tarball_(const settings_t& settings, const std::string& url_s,
                                              const headers_t& headers,
                                              const std::string& display_prefix) {
  parsed_url_t url = parse_url(url_s);

  // Some friendly error messages for common mistakes.
  // Namely lets catch when the url is a local file path, but
  // it is not in fact a tarball.
  if (url.scheme == "file") {
    std::filesystem::path local_path = render_url_path_ensure_legal(url.path);
    if (!exists(local_path)) {
      throw Error("tarball '%s' does not exist.", local_path);
    }
    if (is_directory(local_path)) {
      if (exists(local_path / ".git")) {
        throw Error(
            "tarball '%s' is a git repository, not a tarball. Please use `git+file` as the scheme.",
            local_path);
      }
      throw Error("tarball '%s' is a directory, not a file.", local_path);
    }
  }

  cache_t::Key cache_key{"tarball", {{"url", url_s}}};

  auto cached = settings.get_cache()->lookupExpired(cache_key);

  auto attrs_to_result = [&](const Attrs& info_attrs) {
    auto tree_hash = get_rev_attr(info_attrs, "treeHash");
    return DownloadTarballResult{
        .tree_hash = tree_hash,
        .last_modified = (time_t)get_int_attr(info_attrs, "lastModified"),
        .immutableUrl = maybe_get_str_attr(info_attrs, "immutableUrl"),
        .accessor = settings.getTarballCache()->get_accessor(tree_hash, {}, display_prefix),
    };
  };

  if (cached && !settings.getTarballCache()->hasObject(get_rev_attr(cached->value, "treeHash")))
    cached.reset();

  if (cached && !cached->expired)
    /* We previously downloaded this tarball and it's younger than
       `tarballTtl`, so no need to check the server. */
    return attrs_to_result(cached->value);

  auto _res = std::make_shared<sync_t<FileTransferResult>>();

  auto source = sink_to_source([&](Sink& sink) {
    FileTransferRequest req(url);
    req.expectedETag = cached ? get_str_attr(cached->value, "etag") : "";
    get_file_transfer()->download(std::move(req), sink,
                                [_res](FileTransferResult r) { *_res->lock() = r; });
  });

  // TODO: fall back to cached value if download fails.

  auto act = std::make_unique<activity_t>(*logger, lvl_info, act_unknown,
                                        fmt("unpacking '%s' into the Git cache", url));

  auto_delete_t cleanup_temp;

  /* Note: if the download is cached, `importTarball()` will receive
     no data, which causes it to import an empty tarball. */
  auto archive = !url.path.empty() && has_suffix(to_lower(url.path.back()), ".zip")
                     ? ({
                         /* In streaming mode, libarchive doesn't handle
                            symlinks in zip files correctly (#10649). So write
                            the entire file to disk so libarchive can access it
                            in random-access mode. */
                         auto [fdTemp, path] = create_temp_file("nix-zipfile");
                         cleanup_temp.reset(path);
                         debug("downloading '%s' into '%s'...", url, path);
                         {
                           fd_sink_t sink(fdTemp.get());
                           source->drain_into(sink);
                         }
                         tar_archive_t{path};
                       })
                     : tar_archive_t{*source};
  auto tarball_cache = settings.getTarballCache();
  auto parse_sink = tarball_cache->get_file_system_object_sink();
  auto last_modified = unpack_tarfile_to_sink(archive, *parse_sink);
  auto tree = parse_sink->flush();

  act.reset();

  auto res(_res->lock());

  Attrs info_attrs;

  if (res->cached) {
    /* The server says that the previously downloaded version is
       still current. */
    info_attrs = cached->value;
  } else {
    info_attrs.insert_or_assign("etag", res->etag);
    info_attrs.insert_or_assign("treeHash",
                               tarball_cache->dereferenceSingletonDirectory(tree).git_rev());
    info_attrs.insert_or_assign("lastModified", uint64_t(last_modified));
    if (res->immutableUrl)
      info_attrs.insert_or_assign("immutableUrl", *res->immutableUrl);
  }

  /* Insert a cache entry for every URL in the redirect chain. */
  for (auto& url : res->urls) {
    cache_key.second.insert_or_assign("url", url);
    settings.get_cache()->upsert(cache_key, info_attrs);
  }

  // FIXME: add a cache entry for immutableUrl? That could allow
  // cache poisoning.

  return attrs_to_result(info_attrs);
}

ref<SourceAccessor> download_tarball(Store& store, const settings_t& settings,
                                    const std::string& url) {
  /* Go through Input::get_accessor() to ensure that the resulting
     accessor has a fingerprint. */
  fetchers::Attrs attrs;
  attrs.insert_or_assign("type", "tarball");
  attrs.insert_or_assign("url", url);

  auto input = Input::fromAttrs(settings, std::move(attrs));

  return input.get_accessor(settings, store).first;
}

// An input scheme corresponding to a curl-downloadable resource.
struct curl_input_scheme_t : InputScheme {
  const string_set_t transport_url_schemes = {"file", "http", "https"};

  bool has_tarball_extension(const parsed_url_t& url) const {
    if (url.path.empty())
      return false;
    const auto& path = url.path.back();
    return has_suffix(path, ".zip") || has_suffix(path, ".tar") || has_suffix(path, ".tgz") ||
           has_suffix(path, ".tar.gz") || has_suffix(path, ".tar.xz") ||
           has_suffix(path, ".tar.bz2") || has_suffix(path, ".tar.zst");
  }

  virtual bool is_valid_url(const parsed_url_t& url, bool require_tree) const = 0;

  static const string_set_t special_params;

  std::optional<Input> inputFromURL(const settings_t& settings, const parsed_url_t& _url,
                                    bool require_tree) const override {
    if (!is_valid_url(_url, require_tree))
      return std::nullopt;

    Input input{};

    auto url = _url;

    url.scheme = parse_url_scheme(url.scheme).transport;

    auto nar_hash = url.query.find("narHash");
    if (nar_hash != url.query.end())
      input.attrs.insert_or_assign("narHash", nar_hash->second);

    if (auto i = get(url.query, "rev"))
      input.attrs.insert_or_assign("rev", *i);

    if (auto i = get(url.query, "revCount"))
      if (auto n = string2_int<uint64_t>(*i))
        input.attrs.insert_or_assign("revCount", *n);

    if (auto i = get(url.query, "lastModified"))
      if (auto n = string2_int<uint64_t>(*i))
        input.attrs.insert_or_assign("lastModified", *n);

    /* The URL query parameters serve two roles: specifying fetch
       settings for Nix itself, and arbitrary data as part of the
       HTTP request. Now that we've processed the Nix-specific
       attributes above, remove them so we don't also send them as
       part of the HTTP request. */
    for (auto& [param, _] : allowed_attrs())
      url.query.erase(param);

    input.attrs.insert_or_assign("type", std::string{schemeName()});
    input.attrs.insert_or_assign("url", url.to_string());
    return input;
  }

  static const std::map<std::string, AttributeInfo>& allowed_attrs_impl() {
    static const std::map<std::string, AttributeInfo> attrs = {
        {
            "url",
            {
                .type = "String",
                .required = true,
                .doc = R"(
                      Supported protocols:

                      - `https`

                        > **Example**
                        >
                        > ```nix
                        > fetch_tree {
                        >   type = "file";
                        >   url = "https://example.com/index.html";
                        > }
                        > ```

                      - `http`

                        Insecure HTTP transfer for legacy sources.

                        > **Warning**
                        >
                        > HTTP performs no encryption or authentication.
                        > use a `nar_hash` known in advance to ensure the output has expected contents.

                      - `file`

                        A file on the local file system.

                        > **Example**
                        >
                        > ```nix
                        > fetch_tree {
                        >   type = "file";
                        >   url = "file:///home/eelco/nix/README.md";
                        > }
                        > ```
                    )",
            },
        },
        {
            "narHash",
            {},
        },
        {
            "name",
            {},
        },
        {
            "unpack",
            {},
        },
        {
            "rev",
            {},
        },
        {
            "revCount",
            {},
        },
        {
            "lastModified",
            {},
        },
    };
    return attrs;
  }

  const std::map<std::string, AttributeInfo>& allowed_attrs() const override {
    return allowed_attrs_impl();
  }

  std::optional<Input> inputFromAttrs(const settings_t& settings, const Attrs& attrs) const override {
    Input input{};
    input.attrs = attrs;

    // input.locked = (bool) maybeGetStrAttr(input.attrs, "hash");
    return input;
  }

  parsed_url_t toURL(const Input& input, bool abbreviate) const override {
    auto url = parse_url(get_str_attr(input.attrs, "url"));
    // NAR hashes are preferred over file hashes since tar/zip
    // files don't have a canonical representation.
    if (auto nar_hash = input.getNarHash())
      url.query.insert_or_assign("narHash", nar_hash->to_string(hash_format_t::SRI, true));
    return url;
  }

  bool isLocked(const settings_t& settings, const Input& input) const override {
    return (bool)input.getNarHash();
  }
};

struct file_input_scheme_t : curl_input_scheme_t {
  std::string_view schemeName() const override { return "file"; }

  std::string schemeDescription() const override {
    return strip_indentation(R"(
          Place a plain file into the Nix store.
          This is similar to [`builtins.fetchurl`](@docroot@/language/builtins.md#builtins-fetchurl)
        )");
  }

  bool is_valid_url(const parsed_url_t& url, bool require_tree) const override {
    auto parsed_url_scheme = parse_url_scheme(url.scheme);
    return transport_url_schemes.count(std::string(parsed_url_scheme.transport)) &&
           (parsed_url_scheme.application ? parsed_url_scheme.application.value() == schemeName()
                                        : (!require_tree && !has_tarball_extension(url)));
  }

  std::pair<ref<SourceAccessor>, Input> get_accessor(const settings_t& settings, Store& store,
                                                    const Input& _input) const override {
    auto input(_input);

    /* Unlike tarball_input_scheme_t, this stores downloaded files in
       the Nix store directly, since there is little deduplication
       benefit in using the git cache for single big files like
       tarballs. */
    auto file = download_file(store, settings, get_str_attr(input.attrs, "url"), input.get_name());

    auto nar_hash = store.queryPathInfo(file.store_path)->nar_hash;
    input.attrs.insert_or_assign("narHash", nar_hash.to_string(hash_format_t::SRI, true));

    auto accessor = ref{store.getFSAccessor(file.store_path)};

    accessor->set_path_display("«" + input.to_string(true) + "»");

    return {accessor, input};
  }
};

struct tarball_input_scheme_t : curl_input_scheme_t {
  std::string_view schemeName() const override { return "tarball"; }

  std::string schemeDescription() const override {
    return strip_indentation(R"(
          Download a tar archive and extract it into the Nix store.
          This has the same underlying implementation as [`builtins.fetchTarball`](@docroot@/language/builtins.md#builtins-fetchTarball)
        )");
  }

  const std::map<std::string, AttributeInfo>& allowed_attrs() const override {
    static const std::map<std::string, AttributeInfo> attrs = [] {
      auto attrs = curl_input_scheme_t::allowed_attrs_impl();
      // Override the "url" attribute to add tarball-specific example
      attrs["url"].doc = R"(
              > **Example**
              >
              > ```nix
              > fetch_tree {
              >   type = "tarball";
              >   url = "https://github.com/NixOS/nixpkgs/tarball/nixpkgs-23.11";
              > }
              > ```
            )";
      return attrs;
    }();
    return attrs;
  }

  bool is_valid_url(const parsed_url_t& url, bool require_tree) const override {
    auto parsed_url_scheme = parse_url_scheme(url.scheme);

    return transport_url_schemes.count(std::string(parsed_url_scheme.transport)) &&
           (parsed_url_scheme.application ? parsed_url_scheme.application.value() == schemeName()
                                        : (require_tree || has_tarball_extension(url)));
  }

  std::pair<ref<SourceAccessor>, Input> get_accessor(const settings_t& settings, Store& store,
                                                    const Input& _input) const override {
    auto input(_input);

    auto result = download_tarball_(settings, get_str_attr(input.attrs, "url"), {},
                                   "«" + input.to_string(true) + "»");

    if (result.immutableUrl) {
      auto immutable_input = Input::fromURL(settings, *result.immutableUrl);
      // FIXME: would be nice to support arbitrary flakerefs
      // here, e.g. git flakes.
      if (immutable_input.getType() != "tarball")
        throw Error("tarball 'Link' headers that redirect to non-tarball URLs are not supported");
      input = immutable_input;
    }

    if (result.last_modified && !input.attrs.contains("lastModified"))
      input.attrs.insert_or_assign("lastModified", uint64_t(result.last_modified));

    input.attrs.insert_or_assign("narHash", settings.getTarballCache()
                                                ->treeHashToNarHash(settings, result.tree_hash)
                                                .to_string(hash_format_t::SRI, true));

    return {result.accessor, input};
  }

  std::optional<std::string> get_fingerprint(Store& store, const Input& input) const override {
    if (auto nar_hash = input.getNarHash())
      return "tarball:" + nar_hash->to_string(hash_format_t::SRI, true);
    else if (auto rev = input.getRev())
      return "tarball:" + rev->git_rev();
    else
      return std::nullopt;
  }
};

static auto r_tarball_input_scheme =
    on_startup_t([] { register_input_scheme(std::make_unique<tarball_input_scheme_t>()); });
static auto r_file_input_scheme =
    on_startup_t([] { register_input_scheme(std::make_unique<file_input_scheme_t>()); });

} // namespace nix::fetchers
