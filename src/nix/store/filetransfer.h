#pragma once
///@file

#include <future>
#include <string>

#include "nix/store/config.h"
#include "nix/util/callback.h"
#include "nix/util/configuration.h"
#include "nix/util/logging.h"
#include "nix/util/ref.h"
#include "nix/util/serialise.h"
#include "nix/util/types.h"
#include "nix/util/url.h"
#if NIX_WITH_AWS_AUTH
#  include "nix/store/aws-creds.h"
#endif
#include "nix/store/s3-url.h"

namespace nix {

struct FileTransferSettings : config_t {
  setting_t<bool> enableHttp2{this, true, "http2", "Whether to enable HTTP/2 support."};

  setting_t<std::string> userAgentSuffix{this, "", "user-agent-suffix",
                                         "String appended to the user agent in HTTP requests."};

  setting_t<size_t> httpConnections{this,
                                    25,
                                    "http-connections",
                                    R"(
          The maximum number of parallel TCP connections used to fetch
          files from binary caches and by other downloads. It defaults
          to 25. 0 means no limit.
        )",
                                    {"binary-caches-parallel-connections"}};

  /* Do not set this too low. On glibc, getaddrinfo() contains fallback code
     paths that deal with ill-behaved DNS servers. setting_t this too low
     prevents some fallbacks from occurring.

     See description of options timeout, single-request, single-request-reopen
     in resolv.conf(5). Also see https://github.com/NixOS/nix/pull/13985 for
     details on the interaction between getaddrinfo(3) behavior and libcurl
     CURLOPT_CONNECTTIMEOUT. */
  setting_t<unsigned long> connectTimeout{this, 15, "connect-timeout",
                                          R"(
          The timeout (in seconds) for establishing connections in the
          binary cache substituter. It corresponds to `curl`’s
          `--connect-timeout` option. A value of 0 means no limit.
        )"};

  setting_t<unsigned long> stalledDownloadTimeout{this, 300, "stalled-download-timeout",
                                                  R"(
          The timeout (in seconds) for receiving data from servers
          during download. Nix cancels idle downloads after this
          timeout's duration.
        )"};

  setting_t<unsigned int> tries{
      this, 5, "download-attempts",
      "The number of times Nix attempts to download a file before giving up."};

  setting_t<size_t> downloadBufferSize{this, 1 * 1024 * 1024, "download-buffer-size",
                                       R"(
          The size of Nix's internal download buffer in bytes during `curl` transfers. If data is
          not processed quickly enough to exceed the size of this buffer, downloads may stall.
          The default is 1048576 (1 MiB).
        )"};
};

extern FileTransferSettings file_transfer_settings;

extern const unsigned int RETRY_TIME_MS_DEFAULT;

/**
 * HTTP methods supported by FileTransfer.
 */
enum struct HttpMethod {
  Get,
  Put,
  Head,
  Post,
  Delete,
};

/**
 * Username and optional password for HTTP basic authentication.
 * These are used with curl's CURLOPT_USERNAME and CURLOPT_PASSWORD options
 * for various protocols including HTTP, FTP, and others.
 */
struct UsernameAuth {
  std::string username;
  std::optional<std::string> password;
};

enum class PauseTransfer : bool {
  No = false,
  yes = true,
};

struct FileTransferRequest {
  verbatim_url_t uri;
  headers_t headers;
  std::string expectedETag;
  HttpMethod method = HttpMethod::Get;
  size_t tries = file_transfer_settings.tries;
  unsigned int baseRetryTimeMs = RETRY_TIME_MS_DEFAULT;
  activity_id_t parentAct;
  bool decompress = true;

  struct UploadData {
    UploadData(string_source_t& s) : size_hint(s.view().length()), source(&s) {}

    UploadData(std::size_t size_hint, restartable_source_t& source)
        : size_hint(size_hint), source(&source) {}

    std::size_t size_hint = 0;
    restartable_source_t* source = nullptr;
  };

  std::optional<UploadData> data;
  std::string mime_type;

  /**
   * Callbacked invoked with a chunk of received data.
   * Can pause the transfer by returning PauseTransfer::yes. No data must be consumed
   * if transfer is paused.
   */
  std::function<PauseTransfer(std::string_view data)> dataCallback;

  /**
   * Optional username and password for HTTP basic authentication.
   * When provided, these credentials will be used with curl's CURLOPT_USERNAME/PASSWORD option.
   */
  std::optional<UsernameAuth> usernameAuth;
#if NIX_WITH_AWS_AUTH
  /**
   * Pre-resolved AWS session token for S3 requests.
   * When provided along with usernameAuth, this will be used instead of fetching fresh credentials.
   */
  std::optional<std::string> preResolvedAwsSessionToken;
#endif

  FileTransferRequest(verbatim_url_t uri) : uri(std::move(uri)), parentAct(get_cur_activity()) {}

  /**
   * Returns the method description for logging purposes.
   */
  std::string verb(bool continuous = false) const {
    switch (method) {
      case HttpMethod::Head:
      case HttpMethod::Get:
        return continuous ? "downloading" : "download";
      case HttpMethod::Put:
      case HttpMethod::Post:
        assert(data);
        return continuous ? "uploading" : "upload";
      case HttpMethod::Delete:
        return continuous ? "deleting" : "delete";
    }
    unreachable();
  }

  std::string noun() const {
    switch (method) {
      case HttpMethod::Head:
      case HttpMethod::Get:
        return "download";
      case HttpMethod::Put:
      case HttpMethod::Post:
        assert(data);
        return "upload";
      case HttpMethod::Delete:
        return "deletion";
    }
    unreachable();
  }

  void setupForS3();

private:
  friend struct curl_file_transfer_t;
#if NIX_WITH_AWS_AUTH
  std::optional<std::string> awsSigV4Provider;
#endif
};

struct FileTransferResult {
  /**
   * Whether this is a cache hit (i.e. the ETag supplied in the
   * request is still valid). If so, `data` is empty.
   */
  bool cached = false;

  /**
   * The ETag of the object.
   */
  std::string etag;

  /**
   * All URLs visited in the redirect chain.
   *
   * @note Intentionally strings and not `parsed_url_t`s so we faithfully
   * return what cURL gave us.
   */
  std::vector<std::string> urls;

  /**
   * The response body.
   */
  std::string data;

  uint64_t bodySize = 0;

  /**
   * An "immutable" URL for this resource (i.e. one whose contents
   * will never change), as returned by the `Link: <url>;
   * rel="immutable"` header.
   */
  std::optional<std::string> immutableUrl;
};

class store_t;

struct FileTransfer {
protected:
  class Item {};

public:
  /**
   * An opaque handle to the file transfer. Can be used to reference an in-flight transfer
   * operations.
   */
  struct ItemHandle {
    std::reference_wrapper<Item> item;
    friend struct FileTransfer;

    ItemHandle(Item& item) : item(item) {}
  };

  virtual ~FileTransfer() {}

  /**
   * Enqueue a data transfer request, returning a future to the result of
   * the download. The future may throw a FileTransferError
   * exception.
   */
  virtual ItemHandle enqueueFileTransfer(const FileTransferRequest& request,
                                         Callback<FileTransferResult> callback) = 0;

  /**
   * Unpause a transfer that has been previously paused by a dataCallback.
   */
  virtual void unpause_transfer(ItemHandle handle) = 0;

  std::future<FileTransferResult> enqueueFileTransfer(const FileTransferRequest& request);

  /**
   * Synchronously download a file.
   */
  FileTransferResult download(const FileTransferRequest& request);

  /**
   * Synchronously upload a file.
   */
  FileTransferResult upload(const FileTransferRequest& request);

  /**
   * Synchronously delete a resource.
   */
  FileTransferResult deleteResource(const FileTransferRequest& request);

  /**
   * Download a file, writing its data to a sink. The sink will be
   * invoked on the thread of the caller.
   */
  void download(FileTransferRequest&& request, sink_t& sink,
                std::function<void(FileTransferResult)> resultCallback = {});

  enum Error { NotFound, Forbidden, Misc, Transient, Interrupted };
};

/**
 * @return a shared FileTransfer object.
 *
 * Using this object is preferred because it enables connection reuse
 * and HTTP/2 multiplexing.
 */
ref<FileTransfer> get_file_transfer();

/**
 * @return a new FileTransfer object
 *
 * Prefer get_file_transfer() to this; see its docs for why.
 */
ref<FileTransfer> make_file_transfer();

std::shared_ptr<FileTransfer> reset_file_transfer();

class FileTransferError : public Error {
public:
  FileTransfer::Error error;
  /// intentionally optional
  std::optional<std::string> response;

  template <typename... args_t>
  FileTransferError(FileTransfer::Error error, std::optional<std::string> response,
                    const args_t&... args);
};

} // namespace nix
