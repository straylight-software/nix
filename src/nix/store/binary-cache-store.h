#pragma once
///@file

#include <atomic>

#include "nix/store/log-store.h"
#include "nix/store/store-api.h"
#include "nix/util/pool.h"
#include "nix/util/signature/local-keys.h"

namespace nix {

struct NarInfo;
class RemoteFSAccessor;

struct BinaryCacheStoreConfig : virtual StoreConfig {
  using StoreConfig::StoreConfig;

  const setting_t<std::string> compression{
      this, "xz", "compression",
      "NAR compression method (`xz`, `bzip2`, `gzip`, `zstd`, or `none`)."};

  const setting_t<bool> writeNARListing{
      this, false, "write-nar-listing",
      "Whether to write a JSON file that lists the files in each NAR."};

  const setting_t<bool> writeDebugInfo{this, false, "index-debug-info",
                                     R"(
          Whether to index DWARF debug info files by build ID. This allows [`dwarffs`](https://github.com/edolstra/dwarffs) to
          fetch debug info on demand
        )"};

  const setting_t<Path> secret_key_file{this, "", "secret-key",
                                    "Path to the secret key used to sign the binary cache."};

  const setting_t<std::string> secretKeyFiles{
      this, "", "secret-keys",
      "List of comma-separated paths to the secret keys used to sign the binary cache."};

  const setting_t<Path> localNarCache{this, "", "local-nar-cache",
                                    "Path to a local cache of NARs fetched from this binary cache, "
                                    "used by commands such as `nix store cat`."};

  const setting_t<bool> parallelCompression{this, false, "parallel-compression",
                                          "Enable multi-threaded compression of NARs. This is "
                                          "currently only available for `xz` and `zstd`."};

  const setting_t<int> compressionLevel{this, -1, "compression-level",
                                      R"(
          The *preset level* to be used when compressing NARs.
          The meaning and accepted values depend on the compression method selected.
          `-1` specifies that the default compression level should be used.
        )"};
};

/**
 * @note subclasses must implement at least one of the two
 * virtual getFile() methods.
 */
struct alignas(8) /* Work around ASAN failures on i686-linux. */
    binary_cache_store : virtual Store,
                       virtual LogStore {
  using config_t = BinaryCacheStoreConfig;

  /**
   * Intentionally mutable because some things we update due to the
   * cache's own (remote side) settings.
   */
  config_t& config;

private:
  std::vector<std::unique_ptr<signer_t>> signers;

protected:
  /**
   * The prefix under which realisation infos will be stored
   */
  constexpr const static std::string realisationsPrefix = "realisations";

  constexpr const static std::string cacheInfoFile = "nix-cache-info";

  binary_cache_store(config_t&);

  /**
   * Compute the path to the given realisation
   *
   * It's `${realisationsPrefix}/${drvOutput}.doi`.
   */
  std::string makeRealisationPath(const DrvOutput& id);

public:
  virtual bool file_exists(const std::string& path) = 0;

  virtual void upsert_file(const std::string& path, restartable_source_t& source,
                          const std::string& mime_type, uint64_t size_hint) = 0;

  void upsert_file(const std::string& path,
                  // FIXME: use std::string_view
                  std::string&& data, const std::string& mime_type, uint64_t size_hint);

  void upsert_file(const std::string& path,
                  // FIXME: use std::string_view
                  std::string&& data, const std::string& mime_type) {
    auto size = data.size();
    upsert_file(path, std::move(data), mime_type, size);
  }

  /**
   * Dump the contents of the specified file to a sink.
   */
  virtual void getFile(const std::string& path, Sink& sink);

  /**
   * Get the contents of /nix-cache-info. Return std::nullopt if it
   * doesn't exist.
   */
  virtual std::optional<std::string> getNixCacheInfo();

  /**
   * Fetch the specified file and call the specified callback with
   * the result. A subclass may implement this asynchronously.
   */
  virtual void getFile(const std::string& path,
                       Callback<std::optional<std::string>> callback) noexcept;

  std::optional<std::string> getFile(const std::string& path);

public:
  virtual void init() override;

private:
  std::string narMagic;

  std::string narInfoFileFor(const StorePath& store_path);

  void writeNarInfo(ref<NarInfo> narInfo);

  ref<const ValidPathInfo> addToStoreCommon(Source& nar_source, RepairFlag repair,
                                            CheckSigsFlag check_sigs,
                                            std::function<ValidPathInfo(hash_result_t)> mkInfo);

  /**
   * Same as `getFSAccessor`, but with a more preceise return type.
   */
  ref<RemoteFSAccessor> getRemoteFSAccessor(bool require_valid_path = true);

public:
  bool isValidPathUncached(const StorePath& path) override;

  void
  query_path_info_uncached(const StorePath& path,
                        Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept override;

  std::optional<StorePath> queryPathFromHashPart(const std::string& hash_part) override;

  void add_to_store(const ValidPathInfo& info, Source& nar_source, RepairFlag repair,
                  CheckSigsFlag check_sigs) override;

  StorePath add_to_store_from_dump(Source& dump, std::string_view name,
                               file_serialisation_method_t dump_method, ContentAddressMethod hash_method,
                               hash_algorithm_t hash_algo, const StorePathSet& references,
                               RepairFlag repair) override;

  StorePath add_to_store(std::string_view name, const source_path_t& path, ContentAddressMethod method,
                       hash_algorithm_t hash_algo, const StorePathSet& references, path_filter_t& filter,
                       RepairFlag repair) override;

  void register_drv_output(const Realisation& info) override;

  void query_realisation_uncached(
      const DrvOutput&,
      Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept override;

  void nar_from_path(const StorePath& path, Sink& sink) override;

  ref<SourceAccessor> getFSAccessor(bool require_valid_path = true) override;

  std::shared_ptr<SourceAccessor> getFSAccessor(const StorePath&,
                                                bool require_valid_path = true) override;

  void addSignatures(const StorePath& store_path, const string_set_t& sigs) override;

  std::optional<std::string> getBuildLogExact(const StorePath& path) override;

  void addBuildLog(const StorePath& drv_path, std::string_view log) override;
};

make_error(NoSuchBinaryCacheFile, Error);

} // namespace nix
