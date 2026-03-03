#include "nix/store/filetransfer.h"

#include <optional>

// mimalloc thread cleanup workaround
#if __has_include(<mimalloc.h>)
#  include <mimalloc.h>
#endif

#include "nix/store/globals.h"
#include "nix/store/s3-url.h"
#include "nix/store/store-api.h"
#include "nix/util/callback.h"
#include "nix/util/compression.h"
#include "nix/util/config-global.h"
#include "nix/util/finally.h"
#include "nix/util/signals.h"
#include "store-config-private.h"
#if NIX_WITH_AWS_AUTH
#  include "nix/store/aws-creds.h"
#endif

#ifdef __linux__
#  include "nix/util/linux-namespaces.h"
#endif

#include <cmath>
#include <cstring>
#include <queue>
#include <random>
#include <regex>
#include <thread>

#include <curl/curl.h>
#include <fcntl.h>
#include <unistd.h>

namespace nix {

const unsigned int RETRY_TIME_MS_DEFAULT = 250;
const unsigned int RETRY_TIME_MS_TOO_MANY_REQUESTS = 60000;

FileTransferSettings file_transfer_settings;

static global_config_t::Register r_file_transfer_settings(&file_transfer_settings);

struct curl_file_transfer_t : public FileTransfer {
  CURLM* curlm = 0;

  std::random_device rd;
  std::mt19937 mt19937;

  struct transfer_item_t : public std::enable_shared_from_this<transfer_item_t>,
                           public FileTransfer::Item {
    curl_file_transfer_t& file_transfer;
    FileTransferRequest request;
    FileTransferResult result;
    std::unique_ptr<activity_t> _act;
    bool done = false; // whether either the success or failure function has been called
    Callback<FileTransferResult> callback;
    CURL* req = 0;
    // buffer to accompany the `req` above
    char errbuf[CURL_ERROR_SIZE];
    bool active = false; // whether the handle has been added to the multi object
    bool paused = false; // whether the request has been paused previously
    std::string status_msg;

    unsigned int attempt = 0;

    /* Don't start this download until the specified time point
       has been reached. */
    std::chrono::steady_clock::time_point embargo;

    struct curl_slist* request_headers = 0;

    std::string encoding;

    bool accept_ranges = false;

    curl_off_t written_to_sink = 0;

    std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();

    inline static const std::set<long> successful_statuses{200, 201, 204,
                                                           206, 304, 0 /* other protocol */};

    /* Get the HTTP status code, or 0 for other protocols. */
    long get_http_status() {
      long http_status = 0;
      long protocol = 0;
      curl_easy_getinfo(req, CURLINFO_PROTOCOL, &protocol);
      if (protocol == CURLPROTO_HTTP || protocol == CURLPROTO_HTTPS) {
        curl_easy_getinfo(req, CURLINFO_RESPONSE_CODE, &http_status);
      }
      return http_status;
    }

    transfer_item_t(curl_file_transfer_t& file_transfer, const FileTransferRequest& request,
                    Callback<FileTransferResult>&& callback)
        : file_transfer(file_transfer),
          request(request),
          callback(std::move(callback)),
          final_sink([this](std::string_view data) {
            if (error_sink) {
              (*error_sink)(data);
            }

            if (this->request.dataCallback) {
              auto http_status = get_http_status();

              /* Only write data to the sink if this is a
                 successful response. */
              if (successful_statuses.count(http_status)) {
                written_to_sink += data.size();
                PauseTransfer needsPause = this->request.dataCallback(data);
                if (needsPause == PauseTransfer::yes) {
                  /* Smuggle the boolean flag into write_callback. Note that
                     the final_sink might get called multiple times if there's
                     decompression going on. */
                  paused = true;
                }
              }
            } else {
              this->result.data.append(data);
            }
          }) {
      result.urls.push_back(request.uri.to_string());

      request_headers =
          curl_slist_append(request_headers, "Accept-Encoding: zstd, br, gzip, deflate, bzip2, xz");
      if (!request.expectedETag.empty()) {
        request_headers =
            curl_slist_append(request_headers, ("If-None-Match: " + request.expectedETag).c_str());
      }
      if (!request.mime_type.empty()) {
        request_headers =
            curl_slist_append(request_headers, ("Content-Type: " + request.mime_type).c_str());
      }
      for (auto it = request.headers.begin(); it != request.headers.end(); ++it) {
        request_headers =
            curl_slist_append(request_headers, fmt("%s: %s", it->first, it->second).c_str());
      }
    }

    ~transfer_item_t() {
      if (req) {
        if (active) {
          curl_multi_remove_handle(file_transfer.curlm, req);
        }
        curl_easy_cleanup(req);
      }
      if (request_headers) {
        curl_slist_free_all(request_headers);
      }
      try {
        // Only invoke the callback if we're not shutting down. During shutdown,
        // invoking the callback can cause deadlocks if the callback tries to
        // acquire locks held by threads waiting for the worker thread to finish.
        // See: https://github.com/NixOS/nix/issues/3017
        if (!done && !file_transfer.state_.lock()->is_quitting()) {
          fail(FileTransferError(Interrupted, {}, "%s of '%s' was interrupted",
                                 uncolored_t(request.noun()), request.uri));
        }
      } catch (...) {
        ignore_exception_in_destructor();
      }
    }

    void fail_ex(std::exception_ptr ex) noexcept {
      assert(!done);
      done = true;
      try {
        std::rethrow_exception(ex);
      } catch (nix::Error& e) {
        /* Add more context to the error message. */
        e.add_trace({}, "during %s of '%s'", uncolored_t(request.noun()), request.uri.to_string());
      } catch (...) {
        /* Can't add more context to the error. */
      }
      callback.rethrow(ex);
    }

    template <class T>
    void fail(T&& e) noexcept {
      fail_ex(std::make_exception_ptr(std::forward<T>(e)));
    }

    lambda_sink_t final_sink;
    std::shared_ptr<finish_sink_t> decompression_sink;
    std::optional<string_sink_t> error_sink;

    std::exception_ptr callback_exception;

    size_t write_callback(void* contents, size_t size, size_t nmemb) noexcept try {
      // Check for multiplication overflow before computing real_size
      if (size != 0 && nmemb > SIZE_MAX / size) {
        throw nix::Error("file transfer size overflow");
      }
      size_t real_size = size * nmemb;
      result.bodySize += real_size;

      if (!decompression_sink) {
        decompression_sink = make_decompression_sink(encoding, final_sink);
        if (!successful_statuses.count(get_http_status())) {
          // In this case we want to construct a TeeSink, to keep
          // the response around (which we figure won't be big
          // like an actual download should be) to improve error
          // messages.
          error_sink = string_sink_t{};
        }
      }

      (*decompression_sink)({(char*)contents, real_size});
      if (paused) {
        /* The callback has signaled that the transfer needs to be
           paused. Already consumed data won't be returned twice unlike
           when returning CURL_WRITEFUNC_PAUSE.
           https://curl-library.cool.haxx.narkive.com/larE1cRA/curl-easy-pause-documentation-question
           */
        curl_easy_pause(req, CURLPAUSE_RECV);
      }

      return real_size;
    } catch (...) {
      callback_exception = std::current_exception();
      return 0;
    }

    static size_t write_callback_wrapper(void* contents, size_t size, size_t nmemb, void* userp) {
      return ((transfer_item_t*)userp)->write_callback(contents, size, nmemb);
    }

    void append_current_url() {
      char* effective_uri_c_str = nullptr;
      curl_easy_getinfo(req, CURLINFO_EFFECTIVE_URL, &effective_uri_c_str);
      if (effective_uri_c_str && *result.urls.rbegin() != effective_uri_c_str) {
        result.urls.push_back(effective_uri_c_str);
      }
    }

    size_t header_callback(void* contents, size_t size, size_t nmemb) noexcept try {
      size_t real_size = size * nmemb;
      std::string line((char*)contents, real_size);
      printMsg(lvl_vomit, "got header for '%s': %s", request.uri, trim(line));

      static std::regex status_line("HTTP/[^ ]+ +[0-9]+(.*)",
                                    std::regex::extended | std::regex::icase);
      if (std::smatch match; std::regex_match(line, match, status_line)) {
        result.etag = "";
        result.data.clear();
        result.bodySize = 0;
        status_msg = trim(match.str(1));
        accept_ranges = false;
        encoding = "";
        append_current_url();
      } else {
        auto i = line.find(':');
        if (i != std::string::npos) {
          std::string name = to_lower(trim(line.substr(0, i)));

          if (name == "etag") {
            result.etag = trim(line.substr(i + 1));
            /* Hack to work around a GitHub bug: it sends
               ETags, but ignores If-None-Match. So if we get
               the expected ETag on a 200 response, then shut
               down the connection because we already have the
               data. */
            long http_status = 0;
            curl_easy_getinfo(req, CURLINFO_RESPONSE_CODE, &http_status);
            if (result.etag == request.expectedETag && http_status == 200) {
              debug("shutting down on 200 HTTP response with expected ETag");
              return 0;
            }
          }

          else if (name == "content-encoding") {
            encoding = trim(line.substr(i + 1));
          }

          else if (name == "accept-ranges" && to_lower(trim(line.substr(i + 1))) == "bytes") {
            accept_ranges = true;
          }

          else if (name == "link" || name == "x-amz-meta-link") {
            auto value = trim(line.substr(i + 1));
            static std::regex linkRegex("<([^>]*)>; rel=\"immutable\"",
                                        std::regex::extended | std::regex::icase);
            if (std::smatch match; std::regex_match(value, match, linkRegex)) {
              result.immutableUrl = match.str(1);
            } else {
              debug("got invalid link header '%s'", value);
            }
          }
        }
      }
      return real_size;
    } catch (...) {
#if LIBCURL_VERSION_NUM >= 0x075700
      /* https://curl.se/libcurl/c/CURLOPT_HEADERFUNCTION.html:
         You can also abort the transfer by returning CURL_WRITEFUNC_ERROR. */
      callback_exception = std::current_exception();
      return CURL_WRITEFUNC_ERROR;
#else
      return real_size;
#endif
    }

    static size_t header_callback_wrapper(void* contents, size_t size, size_t nmemb, void* userp) {
      return ((transfer_item_t*)userp)->header_callback(contents, size, nmemb);
    }

    /**
     * Lazily start an `activity_t`. We don't do this in the `transfer_item_t` constructor to avoid
     * showing downloads that are only enqueued but not actually started.
     */
    activity_t& act() {
      if (!_act) {
        _act = std::make_unique<activity_t>(
            *logger, lvl_talkative, act_file_transfer,
            fmt("%s '%s'", request.verb(/*continuous=*/true), request.uri),
            logger_t::fields_t{logger_t::field_t{request.uri.to_string()}}, request.parentAct);
        // Reset the start time to when we actually started the download.
        start_time = std::chrono::steady_clock::now();
      }
      return *_act;
    }

    int progress_callback(curl_off_t dltotal, curl_off_t dlnow) noexcept try {
      act().progress(dlnow, dltotal);
      return get_interrupted();
    } catch (nix::Interrupted&) {
      assert(get_interrupted());
      return 1;
    } catch (...) {
      /* Something unexpected has happened like logger throwing an exception. */
      callback_exception = std::current_exception();
      return 1;
    }

    static int progress_callback_wrapper(void* userp, curl_off_t dltotal, curl_off_t dlnow,
                                         curl_off_t ultotal, curl_off_t ulnow) {
      auto& item = *static_cast<transfer_item_t*>(userp);
      auto is_upload = bool(item.request.data);
      return item.progress_callback(is_upload ? ultotal : dltotal, is_upload ? ulnow : dlnow);
    }

    static int debug_callback(CURL* handle, curl_infotype type, char* data, size_t size,
                              void* userptr) noexcept try {
      if (type == CURLINFO_TEXT) {
        vomit("curl: %s", chomp(std::string(data, size)));
      }
      return 0;
    } catch (...) {
      /* Swallow the exception. Nothing left to do. */
      return 0;
    }

    size_t read_callback(char* buffer, size_t size, size_t nitems) noexcept try {
      auto data = request.data;
      return data->source->read(buffer, nitems * size);
    } catch (EndOfFile&) {
      return 0;
    } catch (...) {
      callback_exception = std::current_exception();
      return CURL_READFUNC_ABORT;
    }

    static size_t read_callback_wrapper(char* buffer, size_t size, size_t nitems,
                                        void* userp) noexcept {
      return ((transfer_item_t*)userp)->read_callback(buffer, size, nitems);
    }

#if !defined(_WIN32) && LIBCURL_VERSION_NUM >= 0x071000
    static int cloexec_callback(void*, curl_socket_t curlfd, curlsocktype purpose) {
      unix::close_on_exec(curlfd);
      vomit("cloexec set for fd %i", curlfd);
      return CURL_SOCKOPT_OK;
    }
#endif

    size_t seek_callback(curl_off_t offset, int origin) noexcept try {
      auto source = request.data->source;
      if (origin == SEEK_SET) {
        source->restart();
        source->skip(offset);
      } else if (origin == SEEK_CUR) {
        source->skip(offset);
      } else if (origin == SEEK_END) {
        null_sink_t sink{};
        source->drain_into(sink);
      }
      return CURL_SEEKFUNC_OK;
    } catch (...) {
      callback_exception = std::current_exception();
      return CURL_SEEKFUNC_FAIL;
    }

    static size_t seek_callback_wrapper(void* clientp, curl_off_t offset, int origin) noexcept {
      return ((transfer_item_t*)clientp)->seek_callback(offset, origin);
    }

    static int resolver_callback_wrapper(void*, void*, void* clientp) noexcept try {
      // Create the `Activity` associated with this download.
      ((transfer_item_t*)clientp)->act();
      return 0;
    } catch (...) {
      return 1;
    }

    void unpause() {
      /* Unpausing an already unpaused transfer is a no-op. */
      if (paused) {
        curl_easy_pause(req, CURLPAUSE_CONT);
        paused = false;
      }
    }

    void init() {
      if (!req) {
        req = curl_easy_init();
      }

      curl_easy_reset(req);

      if (verbosity >= lvl_vomit) {
        curl_easy_setopt(req, CURLOPT_VERBOSE, 1);
        curl_easy_setopt(req, CURLOPT_DEBUGFUNCTION, transfer_item_t::debug_callback);
      }

      curl_easy_setopt(req, CURLOPT_URL, request.uri.to_string().c_str());
      curl_easy_setopt(req, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(req, CURLOPT_MAXREDIRS, 10);
      curl_easy_setopt(req, CURLOPT_NOSIGNAL, 1);
      curl_easy_setopt(req, CURLOPT_USERAGENT,
                       ("curl/" LIBCURL_VERSION " Nix/" + nix_version + " DeterminateNix/" +
                        determinate_nix_version +
                        (file_transfer_settings.userAgentSuffix != ""
                             ? " " + file_transfer_settings.userAgentSuffix.get()
                             : ""))
                           .c_str());
#if LIBCURL_VERSION_NUM >= 0x072b00
      curl_easy_setopt(req, CURLOPT_PIPEWAIT, 1);
#endif
#if LIBCURL_VERSION_NUM >= 0x072f00
      if (file_transfer_settings.enableHttp2) {
        curl_easy_setopt(req, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
      } else {
        curl_easy_setopt(req, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
      }
#endif
      curl_easy_setopt(req, CURLOPT_WRITEFUNCTION, transfer_item_t::write_callback_wrapper);
      curl_easy_setopt(req, CURLOPT_WRITEDATA, this);
      curl_easy_setopt(req, CURLOPT_HEADERFUNCTION, transfer_item_t::header_callback_wrapper);
      curl_easy_setopt(req, CURLOPT_HEADERDATA, this);

      curl_easy_setopt(req, CURLOPT_XFERINFOFUNCTION, progress_callback_wrapper);
      curl_easy_setopt(req, CURLOPT_XFERINFODATA, this);
      curl_easy_setopt(req, CURLOPT_NOPROGRESS, 0);

      curl_easy_setopt(req, CURLOPT_HTTPHEADER, request_headers);

      if (settings.downloadSpeed.get() > 0) {
        curl_easy_setopt(req, CURLOPT_MAX_RECV_SPEED_LARGE,
                         (curl_off_t)(settings.downloadSpeed.get() * 1024));
      }

      if (request.method == HttpMethod::Head) {
        curl_easy_setopt(req, CURLOPT_NOBODY, 1);
      }

      if (request.method == HttpMethod::Delete) {
        curl_easy_setopt(req, CURLOPT_CUSTOMREQUEST, "DELETE");
      }

      if (request.data) {
        if (request.method == HttpMethod::Post) {
          curl_easy_setopt(req, CURLOPT_POST, 1L);
          curl_easy_setopt(req, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)request.data->size_hint);
        } else if (request.method == HttpMethod::Put) {
          curl_easy_setopt(req, CURLOPT_UPLOAD, 1L);
          curl_easy_setopt(req, CURLOPT_INFILESIZE_LARGE, (curl_off_t)request.data->size_hint);
        } else {
          unreachable();
        }
        curl_easy_setopt(req, CURLOPT_READFUNCTION, read_callback_wrapper);
        curl_easy_setopt(req, CURLOPT_READDATA, this);
        curl_easy_setopt(req, CURLOPT_SEEKFUNCTION, seek_callback_wrapper);
        curl_easy_setopt(req, CURLOPT_SEEKDATA, this);
      }

      if (settings.ca_file != "") {
        curl_easy_setopt(req, CURLOPT_CAINFO, settings.ca_file.get().c_str());
      }

#if !defined(_WIN32) && LIBCURL_VERSION_NUM >= 0x071000
      curl_easy_setopt(req, CURLOPT_SOCKOPTFUNCTION, cloexec_callback);
#endif

      curl_easy_setopt(req, CURLOPT_CONNECTTIMEOUT, file_transfer_settings.connectTimeout.get());

      curl_easy_setopt(req, CURLOPT_LOW_SPEED_LIMIT, 1L);
      curl_easy_setopt(req, CURLOPT_LOW_SPEED_TIME,
                       file_transfer_settings.stalledDownloadTimeout.get());

      /* If no file exist in the specified path, curl continues to work
         anyway as if netrc support was disabled. */
      curl_easy_setopt(req, CURLOPT_NETRC_FILE, settings.netrcFile.get().c_str());
      curl_easy_setopt(req, CURLOPT_NETRC, CURL_NETRC_OPTIONAL);

      if (written_to_sink) {
        curl_easy_setopt(req, CURLOPT_RESUME_FROM_LARGE, written_to_sink);
      }

      curl_easy_setopt(req, CURLOPT_ERRORBUFFER, errbuf);
      errbuf[0] = 0;

      // Set up username/password authentication if provided
      if (request.usernameAuth) {
        curl_easy_setopt(req, CURLOPT_USERNAME, request.usernameAuth->username.c_str());
        if (request.usernameAuth->password) {
          curl_easy_setopt(req, CURLOPT_PASSWORD, request.usernameAuth->password->c_str());
        }
      }

#if NIX_WITH_AWS_AUTH
      // Set up AWS SigV4 signing if this is an S3 request
      // Note: AWS SigV4 support guaranteed available (curl >= 7.75.0 checked at build time)
      // The username/password (access key ID and secret key) are set via the general
      // usernameAuth mechanism above.
      if (request.awsSigV4Provider) {
        curl_easy_setopt(req, CURLOPT_AWS_SIGV4, request.awsSigV4Provider->c_str());
      }
#endif

      // This seems to be the earliest libcurl callback that signals that the download is happening,
      // so we can call act().
      curl_easy_setopt(req, CURLOPT_RESOLVER_START_FUNCTION, resolver_callback_wrapper);
      curl_easy_setopt(req, CURLOPT_RESOLVER_START_DATA, this);

      result.data.clear();
      result.bodySize = 0;
    }

    void finish(CURLcode code) {
      auto finish_time = std::chrono::steady_clock::now();

      auto retry_time_ms = request.baseRetryTimeMs;

      auto http_status = get_http_status();

      debug(
          "finished %s of '%s'; curl status = %d, HTTP status = %d, body = %d bytes, duration = "
          "%.2f s",
          request.noun(), request.uri, code, http_status, result.bodySize,
          std::chrono::duration_cast<std::chrono::milliseconds>(finish_time - start_time).count() /
              1000.0f);

      append_current_url();

      if (decompression_sink) {
        try {
          decompression_sink->finish();
        } catch (...) {
          callback_exception = std::current_exception();
        }
      }

      if (code == CURLE_WRITE_ERROR && result.etag == request.expectedETag) {
        code = CURLE_OK;
        http_status = 304;
      }

      // Skip callback invocation during shutdown to prevent deadlocks.
      // The callback might try to acquire locks held by threads waiting for
      // the worker thread to finish. See: https://github.com/NixOS/nix/issues/3017
      if (file_transfer.state_.lock()->is_quitting()) {
        done = true; // Mark as done to prevent destructor from also invoking callback
        return;
      }

      if (callback_exception) {
        fail_ex(callback_exception);
      }

      else if (code == CURLE_OK && successful_statuses.count(http_status)) {
        result.cached = http_status == 304;

        // In 2021, GitHub responds to If-None-Match with 304,
        // but omits ETag. We just use the If-None-Match etag
        // since 304 implies they are the same.
        if (http_status == 304 && result.etag == "") {
          result.etag = request.expectedETag;
        }

        act().progress(result.bodySize, result.bodySize);
        done = true;
        callback(std::move(result));
      }

      else {
        // We treat most errors as transient, but won't retry when hopeless
        Error err = Transient;

        if (http_status == 404 || http_status == 410 || code == CURLE_FILE_COULDNT_READ_FILE) {
          // The file is definitely not there
          err = NotFound;
        } else if (http_status == 401 || http_status == 403 || http_status == 407) {
          // Don't retry on authentication/authorization failures
          err = Forbidden;
        } else if (http_status == 429) {
          // 429 means too many requests, so we retry (with a substantially longer delay)
          retry_time_ms = RETRY_TIME_MS_TOO_MANY_REQUESTS;
        } else if (http_status >= 400 && http_status < 500 && http_status != 408) {
          // Most 4xx errors are client errors and are probably not worth retrying:
          //   * 408 means the server timed out waiting for us, so we try again
          err = Misc;
        } else if (http_status == 501 || http_status == 505 || http_status == 511) {
          // Let's treat most 5xx (server) errors as transient, except for a handful:
          //   * 501 not implemented
          //   * 505 http version not supported
          //   * 511 we're behind a captive portal
          err = Misc;
        } else {
// Don't bother retrying on certain cURL errors either

// Allow selecting a subset of enum values
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
          switch (code) {
            case CURLE_FAILED_INIT:
            case CURLE_URL_MALFORMAT:
            case CURLE_NOT_BUILT_IN:
            case CURLE_REMOTE_ACCESS_DENIED:
            case CURLE_FILE_COULDNT_READ_FILE:
            case CURLE_FUNCTION_NOT_FOUND:
            case CURLE_ABORTED_BY_CALLBACK:
            case CURLE_BAD_FUNCTION_ARGUMENT:
            case CURLE_INTERFACE_FAILED:
            case CURLE_UNKNOWN_OPTION:
            case CURLE_SSL_CACERT_BADFILE:
            case CURLE_TOO_MANY_REDIRECTS:
            case CURLE_WRITE_ERROR:
            case CURLE_UNSUPPORTED_PROTOCOL:
              err = Misc;
              break;
            default: // Shut up warnings
              break;
          }
#pragma GCC diagnostic pop
        }

        attempt++;

        std::optional<std::string> response;
        if (error_sink) {
          response = std::move(error_sink->str());
        }
        auto exc =
            code == CURLE_ABORTED_BY_CALLBACK && get_interrupted()
                ? FileTransferError(Interrupted, std::move(response), "%s of '%s' was interrupted",
                                    request.noun(), request.uri)
            : http_status != 0
                ? FileTransferError(
                      err, std::move(response), "unable to %s '%s': HTTP error %d%s",
                      request.verb(), request.uri, http_status,
                      code == CURLE_OK ? "" : fmt(" (curl error: %s)", curl_easy_strerror(code)))
                : FileTransferError(err, std::move(response), "unable to %s '%s': %s (%d) %s",
                                    request.verb(), request.uri, curl_easy_strerror(code), code,
                                    errbuf);

        /* If this is a transient error, then maybe retry the
           download after a while. If we're writing to a
           sink, we can only retry if the server supports
           ranged requests. */
        if (err == Transient && attempt < request.tries &&
            (!this->request.dataCallback || written_to_sink == 0 ||
             (accept_ranges && encoding.empty()))) {
          int ms =
              retry_time_ms *
              std::pow(2.0f, attempt - 1 +
                                 std::uniform_real_distribution<>(0.0, 0.5)(file_transfer.mt19937));
          if (written_to_sink) {
            warn("%s; retrying from offset %d in %d ms", exc.what(), written_to_sink, ms);
          } else {
            warn("%s; retrying in %d ms", exc.what(), ms);
          }
          decompression_sink.reset();
          error_sink.reset();
          embargo = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
          try {
            file_transfer.enqueue_item(ref{shared_from_this()});
          } catch (const nix::Error& e) {
            // If enqueue fails (e.g., during shutdown), fail the transfer properly
            // instead of letting the exception propagate, which would leave done=false
            // and cause the destructor to attempt a second callback invocation
            fail(std::move(exc));
          }
        } else {
          fail(std::move(exc));
        }
      }
    }
  };

  struct State {
    struct embargo_comparator_t {
      bool operator()(const ref<transfer_item_t>& i1, const ref<transfer_item_t>& i2) {
        return i1->embargo > i2->embargo;
      }
    };

    std::priority_queue<ref<transfer_item_t>, std::vector<ref<transfer_item_t>>,
                        embargo_comparator_t>
        incoming;
    std::vector<ref<transfer_item_t>> unpause;

  private:
    bool quitting = false;

  public:
    void quit() {
      quitting = true;
      /* We will not be processing any more incoming requests */
      while (!incoming.empty()) {
        incoming.pop();
      }
      unpause.clear();
    }

    bool is_quitting() { return quitting; }
  };

  sync_t<State> state_;

#ifndef _WIN32 // TODO need graceful async exit support on Windows?
  /* We can't use a std::condition_variable to wake up the curl
     thread, because it only monitors file descriptors. So use a
     pipe instead. */
  pipe_t wakeup_pipe;
#endif

  // Use pthread directly for larger stack size (see constructor)
  pthread_t worker_thread_handle{};

  static void* worker_thread_entry_static(void* self) {
    static_cast<curl_file_transfer_t*>(self)->worker_thread_entry();
    return nullptr;
  }

  const size_t max_queue_size = file_transfer_settings.httpConnections.get() * 5;

  curl_file_transfer_t() : mt19937(rd()) {
    static std::once_flag global_init;
    std::call_once(global_init, curl_global_init, CURL_GLOBAL_ALL);

    curlm = curl_multi_init();

#if LIBCURL_VERSION_NUM >= 0x072b00 // Multiplex requires >= 7.43.0
    curl_multi_setopt(curlm, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
#endif
#if LIBCURL_VERSION_NUM >= 0x071e00 // Max connections requires >= 7.30.0
    curl_multi_setopt(curlm, CURLMOPT_MAX_TOTAL_CONNECTIONS,
                      file_transfer_settings.httpConnections.get());
#endif

#ifndef _WIN32 // TODO need graceful async exit support on Windows?
    wakeup_pipe.create();
    fcntl(wakeup_pipe.read_side.get(), F_SETFL, O_NONBLOCK);
#endif

    // Create worker thread with larger stack size (8MB) to handle deep recursion
    // in libcurl/LibreSSL during TLS handshakes. Musl's default stack (128KB on some
    // configs) is too small and causes stack overflow during flake registry fetch.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    constexpr size_t stack_size = 8 * 1024 * 1024; // 8MB
    pthread_attr_setstacksize(&attr, stack_size);

    if (pthread_create(&worker_thread_handle, &attr, worker_thread_entry_static, this) != 0) {
      pthread_attr_destroy(&attr);
      throw sys_error_t("creating file transfer worker thread");
    }
    pthread_attr_destroy(&attr);
  }

  ~curl_file_transfer_t() {
    stop_worker_thread();

    pthread_join(worker_thread_handle, nullptr);

    if (curlm) {
      curl_multi_cleanup(curlm);
    }
  }

  void stop_worker_thread() {
    /* Signal the worker thread to exit. */
    {
      auto state(state_.lock());
      state->quit();
    }
#ifndef _WIN32 // TODO need graceful async exit support on Windows?
    write_full(wakeup_pipe.write_side.get(), " ", false);
#endif
  }

  void worker_thread_main() {
/* Cause this thread to be notified on SIGINT. */
#ifndef _WIN32 // TODO need graceful async exit support on Windows?
    auto callback = create_interrupt_callback([&]() { stop_worker_thread(); });
#endif

#ifdef __linux__
    try {
      try_unshare_filesystem();
    } catch (nix::Error& e) {
      e.add_trace({}, "in download thread");
      throw;
    }
#endif

    std::map<CURL*, std::shared_ptr<transfer_item_t>> items;

    bool quit = false;

    std::chrono::steady_clock::time_point next_wakeup;

    while (!quit) {
      check_interrupt();

      /* Let curl do its thing. */
      int running;
      CURLMcode mc = curl_multi_perform(curlm, &running);
      if (mc != CURLM_OK) {
        throw nix::Error("unexpected error from curl_multi_perform(): %s", curl_multi_strerror(mc));
      }

      /* Set the promises of any finished requests. */
      CURLMsg* msg;
      int left;
      while ((msg = curl_multi_info_read(curlm, &left))) {
        if (msg->msg == CURLMSG_DONE) {
          auto i = items.find(msg->easy_handle);
          assert(i != items.end());
          i->second->finish(msg->data.result);
          curl_multi_remove_handle(curlm, i->second->req);
          i->second->active = false;
          items.erase(i);
        }
      }

      /* Wait for activity, including wakeup events. */
      int numfds = 0;
      struct curl_waitfd extra_f_ds[1];
#ifndef _WIN32 // TODO need graceful async exit support on Windows?
      extra_f_ds[0].fd = wakeup_pipe.read_side.get();
      extra_f_ds[0].events = CURL_WAIT_POLLIN;
      extra_f_ds[0].revents = 0;
#endif
      long max_sleep_time_ms = items.empty() ? 10000 : 100;
      auto sleep_time_ms =
          next_wakeup != std::chrono::steady_clock::time_point()
              ? std::max(0, (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                                next_wakeup - std::chrono::steady_clock::now())
                                .count())
              : max_sleep_time_ms;
      vomit("download thread waiting for %d ms", sleep_time_ms);
      mc = curl_multi_wait(curlm, extra_f_ds, 1, sleep_time_ms, &numfds);
      if (mc != CURLM_OK) {
        throw nix::Error("unexpected error from curl_multi_wait(): %s", curl_multi_strerror(mc));
      }

      next_wakeup = std::chrono::steady_clock::time_point();

      /* Add new curl requests from the incoming requests queue,
         except for requests that are embargoed (waiting for a
         retry timeout to expire). */
      if (extra_f_ds[0].revents & CURL_WAIT_POLLIN) {
        char buf[1024];
        auto res = read(extra_f_ds[0].fd, buf, sizeof(buf));
        if (res == -1 && errno != EINTR) {
          throw sys_error_t("reading curl wakeup socket");
        }
      }

      std::vector<std::shared_ptr<transfer_item_t>> incoming;
      auto now = std::chrono::steady_clock::now();

      {
        auto state(state_.lock());
        while (!state->incoming.empty()) {
          /* Limit the number of active curl handles, since curl doesn't scale well. */
          if (items.size() + incoming.size() >= max_queue_size) {
            auto t = now + std::chrono::milliseconds(100);
            if (next_wakeup == std::chrono::steady_clock::time_point() || t < next_wakeup) {
              next_wakeup = t;
            }
            break;
          }
          auto item = state->incoming.top();
          if (item->embargo <= now) {
            incoming.push_back(item);
            state->incoming.pop();
          } else {
            if (next_wakeup == std::chrono::steady_clock::time_point() ||
                item->embargo < next_wakeup) {
              next_wakeup = item->embargo;
            }
            break;
          }
        }
        quit = state->is_quitting();
      }

      for (auto& item : incoming) {
        debug("starting %s of %s", item->request.noun(), item->request.uri);
        item->init();
        curl_multi_add_handle(curlm, item->req);
        item->active = true;
        items[item->req] = item;
      }

      /* NOTE: Unpausing may invoke callbacks to flush all buffers. */
      auto unpause = [&]() {
        auto state(state_.lock());
        auto res = state->unpause;
        state->unpause.clear();
        return res;
      }();

      for (auto& item : unpause) {
        item->unpause();
      }
    }

    debug("download thread shutting down");
  }

  void worker_thread_entry() {
    // Unwinding or because someone called `quit`.
    bool normal_exit = true;
    try {
      worker_thread_main();
    } catch (nix::Interrupted& e) {
      normal_exit = false;
    } catch (std::exception& e) {
      printError("unexpected error in download thread: %s", e.what());
      normal_exit = false;
    }

    if (!normal_exit) {
      auto state(state_.lock());
      state->quit();
    }

    // Explicitly release mimalloc's thread-local state before pthread exit.
    // This works around a hang in mimalloc's TSD destructor with musl libc
    // where _mi_page_free_collect gets stuck in an infinite loop during
    // thread cleanup.
#if __has_include(<mimalloc.h>)
    mi_thread_done();
#endif
  }

  ItemHandle enqueue_item(ref<transfer_item_t> item) {
    if (item->request.data && item->request.uri.scheme() != "http" &&
        item->request.uri.scheme() != "https" && item->request.uri.scheme() != "s3") {
      throw nix::Error("uploading to '%s' is not supported", item->request.uri.to_string());
    }

    {
      auto state(state_.lock());
      if (state->is_quitting()) {
        throw nix::Error(
            "cannot enqueue download request because the download thread is shutting down");
      }
      state->incoming.push(item);
    }
#ifndef _WIN32 // TODO need graceful async exit support on Windows?
    write_full(wakeup_pipe.write_side.get(), " ");
#endif

    return ItemHandle(static_cast<Item&>(*item));
  }

  ItemHandle enqueueFileTransfer(const FileTransferRequest& request,
                                 Callback<FileTransferResult> callback) override {
    /* Handle s3:// URIs by converting to HTTPS and optionally adding auth */
    if (request.uri.scheme() == "s3") {
      auto modified_request = request;
      modified_request.setupForS3();
      return enqueue_item(
          make_ref<transfer_item_t>(*this, std::move(modified_request), std::move(callback)));
    }

    return enqueue_item(make_ref<transfer_item_t>(*this, request, std::move(callback)));
  }

  void unpause_transfer(ref<transfer_item_t> item) {
    auto state(state_.lock());
    state->unpause.push_back(std::move(item));
#ifndef _WIN32 // TODO need graceful async exit support on Windows?
    write_full(wakeup_pipe.write_side.get(), " ");
#endif
  }

  void unpause_transfer(ItemHandle handle) override {
    unpause_transfer(ref{static_cast<transfer_item_t&>(handle.item.get()).shared_from_this()});
  }
};

static sync_t<std::shared_ptr<curl_file_transfer_t>> _fileTransfer;

ref<FileTransfer> get_file_transfer() {
  auto file_transfer(_fileTransfer.lock());

  if (!*file_transfer || (*file_transfer)->state_.lock()->is_quitting()) {
    *file_transfer = std::make_shared<curl_file_transfer_t>();
  }

  return ref<FileTransfer>(*file_transfer);
}

ref<FileTransfer> make_file_transfer() {
  return make_ref<curl_file_transfer_t>();
}

std::shared_ptr<FileTransfer> reset_file_transfer() {
  auto file_transfer(_fileTransfer.lock());
  std::shared_ptr<curl_file_transfer_t> prev;
  file_transfer->swap(prev);
  return prev;
}

void FileTransferRequest::setupForS3() {
  auto parsedS3 = ParsedS3URL::parse(uri.parsed());
  // Update the request URI to use HTTPS (works without AWS SDK)
  uri = parsedS3.toHttpsUrl();

#if NIX_WITH_AWS_AUTH
  // Auth-specific code only compiled when AWS support is available
  awsSigV4Provider = "aws:amz:" + parsedS3.region.value_or("us-east-1") + ":s3";

  // check if the request already has pre-resolved credentials
  std::optional<std::string> sessionToken;
  if (usernameAuth) {
    debug("Using pre-resolved AWS credentials from parent process");
    sessionToken = preResolvedAwsSessionToken;
  } else if (auto creds = getAwsCredentialsProvider()->maybeGetCredentials(parsedS3)) {
    usernameAuth = UsernameAuth{
        .username = creds->accessKeyId,
        .password = creds->secretAccessKey,
    };
    sessionToken = creds->sessionToken;
  }
  if (sessionToken) {
    headers.emplace_back("x-amz-security-token", *sessionToken);
  }
#else
  // When built without AWS support, just try as public bucket
  debug("S3 request without authentication (built without AWS support)");
#endif
}

std::future<FileTransferResult>
FileTransfer::enqueueFileTransfer(const FileTransferRequest& request) {
  auto promise = std::make_shared<std::promise<FileTransferResult>>();
  enqueueFileTransfer(request, {[promise](std::future<FileTransferResult> fut) {
                        try {
                          promise->set_value(fut.get());
                        } catch (...) {
                          promise->set_exception(std::current_exception());
                        }
                      }});
  return promise->get_future();
}

FileTransferResult FileTransfer::download(const FileTransferRequest& request) {
  return enqueueFileTransfer(request).get();
}

FileTransferResult FileTransfer::upload(const FileTransferRequest& request) {
  /* Note: this method is the same as download, but helps in readability */
  return enqueueFileTransfer(request).get();
}

FileTransferResult FileTransfer::deleteResource(const FileTransferRequest& request) {
  return enqueueFileTransfer(request).get();
}

void FileTransfer::download(FileTransferRequest&& request, sink_t& sink,
                            std::function<void(FileTransferResult)> resultCallback) {
  /* Note: we can't call 'sink' via request.dataCallback, because
     that would cause the sink to execute on the file_transfer
     thread. If 'sink' is a coroutine, this will fail. Also, if the
     sink is expensive (e.g. one that does decompression and writing
     to the Nix store), it would stall the download thread too much.
     Therefore we use a buffer to communicate data between the
     download thread and the calling thread. */

  struct State {
    bool quit = false;
    bool paused = false;
    std::exception_ptr exc;
    std::string data;
    std::condition_variable avail, request;
  };

  auto _state = std::make_shared<sync_t<State>>();

  /* In case of an exception, wake up the download thread. FIXME:
     abort the download request. */
  finally_t finally([&]() {
    auto state(_state->lock());
    state->quit = true;
    state->request.notify_one();
  });

  request.dataCallback = [_state,
                          uri = request.uri.to_string()](std::string_view data) -> PauseTransfer {
    auto state(_state->lock());

    if (state->quit) {
      return PauseTransfer::No;
    }

    /* Append data to the buffer and wake up the calling
       thread. */
    state->data.append(data);
    state->avail.notify_one();

    if (state->data.size() <= file_transfer_settings.downloadBufferSize) {
      return PauseTransfer::No;
    }

    /* dataCallback gets called multiple times by an intermediate sink. Only
       issue the debug message the first time around. */
    if (!state->paused) {
      debug("pausing transfer for '%s': download buffer is full (%d > %d)", uri, state->data.size(),
            file_transfer_settings.downloadBufferSize);
    }

    state->paused = true;

    /* Technically the buffer might become larger than
       downloadBufferSize, but with sinks there's no way to avoid
       consuming data. */
    return PauseTransfer::yes;
  };

  auto handle = enqueueFileTransfer(request, {[_state, resultCallback{std::move(resultCallback)}](
                                                  std::future<FileTransferResult> fut) {
                                      auto state(_state->lock());
                                      state->quit = true;
                                      try {
                                        auto res = fut.get();
                                        if (resultCallback) {
                                          resultCallback(std::move(res));
                                        }
                                      } catch (...) {
                                        state->exc = std::current_exception();
                                      }
                                      state->avail.notify_one();
                                      state->request.notify_one();
                                    }});

  while (true) {
    check_interrupt();

    std::string chunk;

    /* Grab data if available, otherwise wait for the download
       thread to wake us up. */
    {
      auto state(_state->lock());

      if (state->data.empty()) {
        if (state->quit) {
          if (state->exc) {
            std::rethrow_exception(state->exc);
          }
          return;
        }

        if (state->paused) {
          unpause_transfer(handle);
          state->paused = false;
        }
        state.wait(state->avail);

        if (state->data.empty()) {
          continue;
        }
      }

      chunk = std::move(state->data);
      /* Reset state->data after the move, since we check data.empty() */
      state->data = "";

      state->request.notify_one();
    }

    /* Flush the data to the sink and wake up the download thread
       if it's blocked on a full buffer. We don't hold the state
       lock while doing this to prevent blocking the download
       thread if sink() takes a long time. */
    sink(chunk);
  }
}

template <typename... args_t>
FileTransferError::FileTransferError(FileTransfer::Error error, std::optional<std::string> response,
                                     const args_t&... args)
    : Error(args...), error(error), response(response) {
  const auto hf = hint_fmt_t(args...);
  // FIXME: Due to https://github.com/NixOS/nix/issues/3841 we don't know how
  // to print different messages for different verbosity levels. For now
  // we add some heuristics for detecting when we want to show the response.
  if (response && (response->size() < 1024 || response->find("<html>") != std::string::npos)) {
    err_.msg_ = hint_fmt_t("%1%\n\nresponse body:\n\n%2%", uncolored_t(hf.str()), chomp(*response));
  } else {
    err_.msg_ = hf;
  }
}

} // namespace nix
