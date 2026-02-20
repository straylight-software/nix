#include "nix/store/s3-binary-cache-store.h"

#include <cassert>
#include <cstring>
#include <ranges>
#include <regex>
#include <span>

#include "nix/store/http-binary-cache-store.h"
#include "nix/store/store-registration.h"
#include "nix/util/error.h"
#include "nix/util/logging.h"
#include "nix/util/serialise.h"
#include "nix/util/util.h"

namespace nix {

make_error(UploadToS3, Error);

static constexpr uint64_t AWS_MIN_PART_SIZE = 5 * 1024 * 1024;           // 5MiB
static constexpr uint64_t AWS_MAX_PART_SIZE = 5ULL * 1024 * 1024 * 1024; // 5GiB
static constexpr uint64_t AWS_MAX_PART_COUNT = 10000;

class s3_binary_cache_store_t : public virtual http_binary_cache_store {
public:
  s3_binary_cache_store_t(ref<S3BinaryCacheStoreConfig> config)
      : store_t{*config},
        binary_cache_store{*config},
        http_binary_cache_store{config},
        s3_config{config} {}

  void upsert_file(const std::string& path, restartable_source_t& source,
                   const std::string& mime_type, uint64_t size_hint) override;

private:
  ref<S3BinaryCacheStoreConfig> s3_config;

  /**
   * Uploads a file to S3 using a regular (non-multipart) upload.
   *
   * This method is suitable for files up to 5GiB in size. For larger files,
   * multipart upload should be used instead.
   *
   * @see https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutObject.html
   */
  void upload(std::string_view path, restartable_source_t& source, uint64_t size_hint,
              std::string_view mime_type, std::optional<headers_t> headers);

  /**
   * Uploads a file to S3 using multipart upload.
   *
   * This method is suitable for large files that exceed the multipart threshold.
   * It orchestrates the complete multipart upload process: creating the upload,
   * splitting the data into parts, uploading each part, and completing the upload.
   * If any error occurs, the multipart upload is automatically aborted.
   *
   * @see https://docs.aws.amazon.com/AmazonS3/latest/userguide/mpuoverview.html
   */
  void upload_multipart(std::string_view path, restartable_source_t& source, uint64_t size_hint,
                        std::string_view mime_type, std::optional<headers_t> headers);

  /**
   * A sink_t that manages a complete S3 multipart upload lifecycle.
   * Creates the upload on construction, buffers and uploads chunks as data arrives,
   * and completes or aborts the upload appropriately.
   */
  struct multipart_sink_t : sink_t {
    s3_binary_cache_store_t& store;
    std::string_view path;
    std::string upload_id;
    std::string::size_type chunk_size;

    std::vector<std::string> part_etags;
    std::string buffer;

    multipart_sink_t(s3_binary_cache_store_t& store, std::string_view path, uint64_t size_hint,
                     std::string_view mime_type, std::optional<headers_t> headers);

    void operator()(std::string_view data) override;
    void finish();
    void upload_chunk(std::string chunk);
  };

  /**
   * Creates a multipart upload for large objects to S3.
   *
   * @see
   * https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateMultipartUpload.html#API_CreateMultipartUpload_RequestSyntax
   */
  std::string create_multipart_upload(std::string_view key, std::string_view mime_type,
                                      std::optional<headers_t> headers);

  /**
   * Uploads a single part of a multipart upload
   *
   * @see
   * https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPart.html#API_UploadPart_RequestSyntax
   *
   * @returns the [ETag](https://en.wikipedia.org/wiki/HTTP_ETag)
   */
  std::string upload_part(std::string_view key, std::string_view upload_id, uint64_t part_number,
                          std::string data);

  /**
   * Completes a multipart upload by combining all uploaded parts.
   * @see
   * https://docs.aws.amazon.com/AmazonS3/latest/API/API_CompleteMultipartUpload.html#API_CompleteMultipartUpload_RequestSyntax
   */
  void complete_multipart_upload(std::string_view key, std::string_view upload_id,
                                 std::span<const std::string> part_etags);

  /**
   * Abort a multipart upload
   *
   * @see
   * https://docs.aws.amazon.com/AmazonS3/latest/API/API_AbortMultipartUpload.html#API_AbortMultipartUpload_RequestSyntax
   */
  void abort_multipart_upload(std::string_view key, std::string_view upload_id) noexcept;
};

void s3_binary_cache_store_t::upsert_file(const std::string& path, restartable_source_t& source,
                                          const std::string& mime_type, uint64_t size_hint) {
  auto do_upload = [&](restartable_source_t& src, uint64_t size, std::optional<headers_t> headers) {
    headers_t upload_headers = headers.value_or(headers_t());
    if (auto storageClass = s3_config->storageClass.get()) {
      upload_headers.emplace_back("x-amz-storage-class", *storageClass);
    }
    if (s3_config->multipartUpload && size > s3_config->multipartThreshold) {
      upload_multipart(path, src, size, mime_type, std::move(upload_headers));
    } else {
      upload(path, src, size, mime_type, std::move(upload_headers));
    }
  };

  try {
    if (auto compression_method = get_compression_method(path)) {
      compressed_source_t compressed(source, *compression_method);
      headers_t headers = {{"Content-Encoding", *compression_method}};
      do_upload(compressed, compressed.size(), std::move(headers));
    } else {
      do_upload(source, size_hint, std::nullopt);
    }
  } catch (FileTransferError& e) {
    UploadToS3 err(e.message());
    err.add_trace({}, "while uploading to S3 binary cache at '%s'", config->cacheUri.to_string());
    throw err;
  }
}

void s3_binary_cache_store_t::upload(std::string_view path, restartable_source_t& source,
                                     uint64_t size_hint, std::string_view mime_type,
                                     std::optional<headers_t> headers) {
  debug("using S3 regular upload for '%s' (%d bytes)", path, size_hint);
  if (size_hint > AWS_MAX_PART_SIZE)
    throw Error("file too large for S3 upload without multipart: %s would exceed maximum size of "
                "%s. Consider enabling multipart-upload.",
                render_size(size_hint), render_size(AWS_MAX_PART_SIZE));

  http_binary_cache_store::upload(path, source, size_hint, mime_type, std::move(headers));
}

void s3_binary_cache_store_t::upload_multipart(std::string_view path, restartable_source_t& source,
                                               uint64_t size_hint, std::string_view mime_type,
                                               std::optional<headers_t> headers) {
  debug("using S3 multipart upload for '%s' (%d bytes)", path, size_hint);
  multipart_sink_t sink(*this, path, size_hint, mime_type, std::move(headers));
  source.drain_into(sink);
  sink.finish();
}

s3_binary_cache_store_t::multipart_sink_t::multipart_sink_t(s3_binary_cache_store_t& store,
                                                            std::string_view path,
                                                            uint64_t size_hint,
                                                            std::string_view mime_type,
                                                            std::optional<headers_t> headers)
    : store(store), path(path) {
  // Calculate chunk size and estimated parts
  chunk_size = store.s3_config->multipartChunkSize;
  uint64_t estimated_parts = (size_hint + chunk_size - 1) / chunk_size; // ceil division

  if (estimated_parts > AWS_MAX_PART_COUNT) {
    // Equivalent to ceil(sizeHint / AWS_MAX_PART_COUNT)
    uint64_t min_chunk_size = (size_hint + AWS_MAX_PART_COUNT - 1) / AWS_MAX_PART_COUNT;

    if (min_chunk_size > AWS_MAX_PART_SIZE) {
      throw Error("file too large for S3 multipart upload: %s would require chunk size of %s "
                  "(max %s) to stay within %d part limit",
                  render_size(size_hint), render_size(min_chunk_size),
                  render_size(AWS_MAX_PART_SIZE), AWS_MAX_PART_COUNT);
    }

    warn("adjusting S3 multipart chunk size from %s to %s "
         "to stay within %d part limit for %s file",
         render_size(store.s3_config->multipartChunkSize.get()), render_size(min_chunk_size),
         AWS_MAX_PART_COUNT, render_size(size_hint));

    chunk_size = min_chunk_size;
    estimated_parts = AWS_MAX_PART_COUNT;
  }

  buffer.reserve(chunk_size);
  part_etags.reserve(estimated_parts);
  upload_id = store.create_multipart_upload(path, mime_type, std::move(headers));
}

void s3_binary_cache_store_t::multipart_sink_t::operator()(std::string_view data) {
  buffer.append(data);

  while (buffer.size() >= chunk_size) {
    // Move entire buffer, extract excess, copy back remainder
    auto chunk = std::move(buffer);
    auto excess_size = chunk.size() > chunk_size ? chunk.size() - chunk_size : 0;
    if (excess_size > 0) {
      buffer.resize(excess_size);
      std::memcpy(buffer.data(), chunk.data() + chunk_size, excess_size);
    }
    chunk.resize(std::min(chunk_size, chunk.size()));
    upload_chunk(std::move(chunk));
  }
}

void s3_binary_cache_store_t::multipart_sink_t::finish() {
  if (!buffer.empty()) {
    upload_chunk(std::move(buffer));
  }

  try {
    if (part_etags.empty()) {
      throw Error("no data read from stream");
    }
    store.complete_multipart_upload(path, upload_id, part_etags);
  } catch (Error& e) {
    store.abort_multipart_upload(path, upload_id);
    e.add_trace({}, "while finishing an S3 multipart upload");
    throw;
  }
}

void s3_binary_cache_store_t::multipart_sink_t::upload_chunk(std::string chunk) {
  auto part_number = part_etags.size() + 1;
  try {
    std::string etag = store.upload_part(path, upload_id, part_number, std::move(chunk));
    part_etags.push_back(std::move(etag));
  } catch (Error& e) {
    store.abort_multipart_upload(path, upload_id);
    e.add_trace({}, "while uploading part %d of an S3 multipart upload", part_number);
    throw;
  }
}

std::string s3_binary_cache_store_t::create_multipart_upload(std::string_view key,
                                                             std::string_view mime_type,
                                                             std::optional<headers_t> headers) {
  auto req = makeRequest(key);

  // setupForS3() converts s3:// to https:// but strips query parameters
  // So we call it first, then add our multipart parameters
  req.setupForS3();

  auto url = req.uri.parsed();
  url.query()["uploads"] = "";
  req.uri = verbatim_url_t(url);

  req.method = HttpMethod::Post;
  string_source_t payload{std::string_view("")};
  req.data = {payload};
  req.mime_type = mime_type;

  if (headers) {
    req.headers.reserve(req.headers.size() + headers->size());
    std::move(headers->begin(), headers->end(), std::back_inserter(req.headers));
  }

  auto result = get_file_transfer()->enqueueFileTransfer(req).get();

  std::regex uploadIdRegex("<UploadId>([^<]+)</UploadId>");
  std::smatch match;

  if (std::regex_search(result.data, match, uploadIdRegex)) {
    return match[1];
  }

  throw Error("S3 CreateMultipartUpload response missing <UploadId>");
}

std::string s3_binary_cache_store_t::upload_part(std::string_view key, std::string_view upload_id,
                                                 uint64_t part_number, std::string data) {
  if (part_number > AWS_MAX_PART_COUNT) {
    throw Error("S3 multipart upload exceeded %d part limit", AWS_MAX_PART_COUNT);
  }

  auto req = makeRequest(key);
  req.method = HttpMethod::Put;
  req.setupForS3();

  auto url = req.uri.parsed();
  url.query()["partNumber"] = std::to_string(part_number);
  url.query()["uploadId"] = upload_id;
  req.uri = verbatim_url_t(url);
  string_source_t payload{data};
  req.data = {payload};
  req.mime_type = "application/octet-stream";

  auto result = get_file_transfer()->enqueueFileTransfer(req).get();

  if (result.etag.empty()) {
    throw Error("S3 UploadPart response missing ETag for part %d", part_number);
  }

  debug("Part %d uploaded, ETag: %s", part_number, result.etag);
  return std::move(result.etag);
}

void s3_binary_cache_store_t::abort_multipart_upload(std::string_view key,
                                                     std::string_view upload_id) noexcept {
  try {
    auto req = makeRequest(key);
    req.setupForS3();

    auto url = req.uri.parsed();
    url.query()["uploadId"] = upload_id;
    req.uri = verbatim_url_t(url);
    req.method = HttpMethod::Delete;

    get_file_transfer()->enqueueFileTransfer(req).get();
  } catch (...) {
    ignore_exception_in_destructor();
  }
}

void s3_binary_cache_store_t::complete_multipart_upload(std::string_view key,
                                                        std::string_view upload_id,
                                                        std::span<const std::string> part_etags) {
  auto req = makeRequest(key);
  req.setupForS3();

  auto url = req.uri.parsed();
  url.query()["uploadId"] = upload_id;
  req.uri = verbatim_url_t(url);
  req.method = HttpMethod::Post;

  std::string xml = "<CompleteMultipartUpload>";
  for (const auto& [idx, etag] : enumerate(part_etags)) {
    xml += "<Part>";
    // S3 part numbers are 1-indexed, but vector indices are 0-indexed
    xml += "<PartNumber>" + std::to_string(idx + 1) + "</PartNumber>";
    xml += "<ETag>" + etag + "</ETag>";
    xml += "</Part>";
  }
  xml += "</CompleteMultipartUpload>";

  debug("S3 CompleteMultipartUpload XML (%d parts): %s", part_etags.size(), xml);

  string_source_t payload{xml};
  req.data = {payload};
  req.mime_type = "text/xml";

  get_file_transfer()->enqueueFileTransfer(req).get();

  debug("S3 multipart upload completed: %d parts uploaded for '%s'", part_etags.size(), key);
}

string_set_t S3BinaryCacheStoreConfig::uriSchemes() {
  return {"s3"};
}

S3BinaryCacheStoreConfig::S3BinaryCacheStoreConfig(std::string_view scheme,
                                                   std::string_view _cacheUri, const Params& params)
    : store_config_t(params), HttpBinaryCacheStoreConfig(scheme, _cacheUri, params) {
  assert(cacheUri.query().empty());
  assert(cacheUri.scheme() == "s3");

  for (const auto& [key, value] : params) {
    auto s3Params = std::views::transform(
        s3UriSettings, [](const abstract_setting_t* setting) { return setting->name; });
    if (std::ranges::contains(s3Params, key)) {
      cacheUri.query()[key] = value;
    }
  }

  if (multipartChunkSize < AWS_MIN_PART_SIZE) {
    throw UsageError("multipart-chunk-size must be at least %s, got %s",
                     render_size(AWS_MIN_PART_SIZE), render_size(multipartChunkSize.get()));
  }

  if (multipartChunkSize > AWS_MAX_PART_SIZE) {
    throw UsageError("multipart-chunk-size must be at most %s, got %s",
                     render_size(AWS_MAX_PART_SIZE), render_size(multipartChunkSize.get()));
  }

  if (multipartUpload && multipartThreshold < multipartChunkSize) {
    warn("multipart-threshold (%s) is less than multipart-chunk-size (%s), "
         "which may result in single-part multipart uploads",
         render_size(multipartThreshold.get()), render_size(multipartChunkSize.get()));
  }
}

std::string S3BinaryCacheStoreConfig::getHumanReadableURI() const {
  auto reference = getReference();
  reference.params = [&]() {
    Params relevantParams;
    for (auto& setting : s3UriSettings)
      if (setting->overridden)
        relevantParams.insert({setting->name, reference.params.at(setting->name)});
    return relevantParams;
  }();
  return reference.render();
}

std::string S3BinaryCacheStoreConfig::doc() {
  return
#include "s3-binary-cache-store.md"
      ;
}

ref<store_t> S3BinaryCacheStoreConfig::open_store() const {
  auto sharedThis = std::const_pointer_cast<S3BinaryCacheStoreConfig>(
      std::static_pointer_cast<const S3BinaryCacheStoreConfig>(shared_from_this()));
  return make_ref<s3_binary_cache_store_t>(ref{sharedThis});
}

static RegisterStoreImplementation<S3BinaryCacheStoreConfig> register_s3_binary_cache_store;

} // namespace nix
