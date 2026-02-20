#pragma once
///@file

#include <chrono>

#include "nix/store/binary-cache-store.h"
#include "nix/store/filetransfer.h"
#include "nix/util/sync.h"
#include "nix/util/url.h"

namespace nix {

struct HttpBinaryCacheStoreConfig : std::enable_shared_from_this<HttpBinaryCacheStoreConfig>,
                                    virtual store_t::config_t,
                                    binary_cache_store_config_t {
  using binary_cache_store_config_t::binary_cache_store_config_t;

  HttpBinaryCacheStoreConfig(std::string_view scheme, std::string_view cacheUri,
                             const store_t::config_t::Params& params);

  parsed_url_t cacheUri;

  const setting_t<std::string> narinfoCompression{this, "", "narinfo-compression",
                                                  "Compression method for `.narinfo` files."};

  const setting_t<std::string> lsCompression{this, "", "ls-compression",
                                             "Compression method for `.ls` files."};

  const setting_t<std::string> logCompression{this, "", "log-compression",
                                              R"(
          Compression method for `log/*` files. It is recommended to
          use a compression method supported by most web browsers
          (e.g. `brotli`).
        )"};

  static const std::string name() { return "HTTP Binary Cache store_t"; }

  static string_set_t uriSchemes();

  static std::string doc();

  ref<store_t> open_store() const override;

  StoreReference getReference() const override;
};

class http_binary_cache_store : public virtual binary_cache_store {
  struct State {
    bool enabled = true;
    std::chrono::steady_clock::time_point disabledUntil;
  };

  sync_t<State> _state;

public:
  using config_t = HttpBinaryCacheStoreConfig;

  ref<config_t> config;

  http_binary_cache_store(ref<config_t> config);

  void init() override;

protected:
  std::optional<std::string> get_compression_method(const std::string& path);

  void maybeDisable();

  void checkEnabled();

  bool file_exists(const std::string& path) override;

  void upsert_file(const std::string& path, restartable_source_t& source,
                   const std::string& mime_type, uint64_t size_hint) override;

  FileTransferRequest makeRequest(std::string_view path);

  /**
   * Uploads data to the binary cache.
   *
   * This is a lower-level method that handles the actual upload after
   * compression has been applied. It does not handle compression or
   * error wrapping - those are the caller's responsibility.
   *
   * @param path The path in the binary cache to upload to
   * @param source The data source (should already be compressed if needed)
   * @param size_hint Size hint for the data
   * @param mime_type The MIME type of the content
   * @param contentEncoding Optional Content-Encoding header value (e.g., "xz", "br")
   */
  void upload(std::string_view path, restartable_source_t& source, uint64_t size_hint,
              std::string_view mime_type, std::optional<headers_t> headers);

  void getFile(const std::string& path, sink_t& sink) override;

  void getFile(const std::string& path,
               Callback<std::optional<std::string>> callback) noexcept override;

  std::optional<std::string> getNixCacheInfo() override;

  std::optional<TrustedFlag> isTrustedClient() override;
};

} // namespace nix
