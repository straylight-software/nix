#pragma once
///@file

#include <limits>
#include <string>

#include "nix/store/active-builds.h"
#include "nix/store/gc-store.h"
#include "nix/store/log-store.h"
#include "nix/store/store-api.h"

namespace nix {

class pipe_t;
class Pid;
struct fd_sink_t;
struct fd_source_t;
template <typename T>
class Pool;
class RemoteFSAccessor;

struct RemoteStoreConfig : virtual StoreConfig {
  using StoreConfig::StoreConfig;

  const setting_t<int> maxConnections{this, 64, "max-connections",
                                    "Maximum number of concurrent connections to the Nix daemon."};

  const setting_t<unsigned int> maxConnectionAge{this, std::numeric_limits<unsigned int>::max(),
                                               "max-connection-age",
                                               "Maximum age of a connection before it is closed."};
};

/**
 * \todo remote_store is a misnomer - should be something like
 * DaemonStore.
 */
struct remote_store : public virtual Store,
                     public virtual GcStore,
                     public virtual LogStore,
                     public virtual QueryActiveBuildsStore {
  using config_t = RemoteStoreConfig;

  const config_t& config;

  remote_store(const config_t& config);

  /* Implementations of abstract store API methods. */

  bool isValidPathUncached(const StorePath& path) override;

  StorePathSet queryValidPaths(const StorePathSet& paths,
                               SubstituteFlag maybeSubstitute = NoSubstitute) override;

  StorePathSet query_all_valid_paths() override;

  void
  query_path_info_uncached(const StorePath& path,
                        Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept override;

  void query_referrers(const StorePath& path, StorePathSet& referrers) override;

  StorePathSet queryValidDerivers(const StorePath& path) override;

  StorePathSet queryDerivationOutputs(const StorePath& path) override;

  std::map<std::string, std::optional<StorePath>>
  queryPartialDerivationOutputMap(const StorePath& path, Store* eval_store = nullptr) override;
  std::optional<StorePath> queryPathFromHashPart(const std::string& hash_part) override;

  StorePathSet querySubstitutablePaths(const StorePathSet& paths) override;

  void querySubstitutablePathInfos(const StorePathCAMap& paths,
                                   SubstitutablePathInfos& infos) override;

  /**
   * Add a content-addressable store path. `dump` will be drained.
   */
  ref<const ValidPathInfo> addCAToStore(Source& dump, std::string_view name,
                                        ContentAddressMethod ca_method, hash_algorithm_t hash_algo,
                                        const StorePathSet& references, RepairFlag repair);

  /**
   * Add a content-addressable store path. `dump` will be drained.
   */
  StorePath
  add_to_store_from_dump(Source& dump, std::string_view name,
                     file_serialisation_method_t dump_method = file_serialisation_method_t::nix_archive,
                     ContentAddressMethod hash_method = file_ingestion_method_t::nix_archive,
                     hash_algorithm_t hash_algo = hash_algorithm_t::SHA256,
                     const StorePathSet& references = StorePathSet(),
                     RepairFlag repair = NoRepair) override;

  void add_to_store(const ValidPathInfo& info, Source& nar, RepairFlag repair,
                  CheckSigsFlag check_sigs) override;

  void addMultipleToStore(Source& source, RepairFlag repair, CheckSigsFlag check_sigs) override;

  void addMultipleToStore(PathsSource&& paths_to_copy, activity_t& act, RepairFlag repair,
                          CheckSigsFlag check_sigs) override;

  void register_drv_output(const Realisation& info) override;

  void query_realisation_uncached(
      const DrvOutput&,
      Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept override;

  void build_paths(const std::vector<DerivedPath>& paths, BuildMode build_mode,
                  std::shared_ptr<Store> eval_store) override;

  std::vector<KeyedBuildResult> build_paths_with_results(const std::vector<DerivedPath>& paths,
                                                      BuildMode build_mode,
                                                      std::shared_ptr<Store> eval_store) override;

  BuildResult buildDerivation(const StorePath& drv_path, const BasicDerivation& drv,
                              BuildMode build_mode) override;

  void ensure_path(const StorePath& path) override;

  void addTempRoot(const StorePath& path) override;

  Roots findRoots(bool censor) override;

  void collectGarbage(const GCOptions& options, GCResults& results) override;

  void optimiseStore() override;

  bool verifyStore(bool check_contents, RepairFlag repair) override;

  /**
   * The default instance would schedule the work on the client side, but
   * for consistency with `build_paths` and `buildDerivation` it should happen
   * on the remote side.
   *
   * We make this fail for now so we can add implement this properly later
   * without it being a breaking change.
   */
  void repairPath(const StorePath& path) override { unsupported("repairPath"); }

  void addSignatures(const StorePath& store_path, const string_set_t& sigs) override;

  MissingPaths query_missing(const std::vector<DerivedPath>& targets) override;

  void addBuildLog(const StorePath& drv_path, std::string_view log) override;

  std::vector<ActiveBuildInfo> queryActiveBuilds() override;

  std::optional<std::string> getVersion() override;

  void connect() override;

  unsigned int getProtocol() override;

  std::optional<TrustedFlag> isTrustedClient() override;

  void flushBadConnections();

  struct Connection;

  ref<Connection> openConnectionWrapper();

protected:
  virtual ref<Connection> open_connection() = 0;

  void initConnection(Connection& conn);

  ref<Pool<Connection>> connections;

  virtual void setOptions(Connection& conn);

  void setOptions() override;

  struct ConnectionHandle;

  ConnectionHandle getConnection();

  friend struct ConnectionHandle;

  virtual ref<SourceAccessor> getFSAccessor(bool require_valid_path = true) override;

  virtual std::shared_ptr<SourceAccessor> getFSAccessor(const StorePath& path,
                                                        bool require_valid_path = true) override;

  virtual void nar_from_path(const StorePath& path, Sink& sink) override;

private:
  /**
   * Same as the default implemenation of `remote_store::getFSAccessor`, but with a more preceise
   * return type.
   */
  ref<RemoteFSAccessor> getRemoteFSAccessor(bool require_valid_path = true);

  std::atomic_bool failed{false};

  void copyDrvsFromEvalStore(const std::vector<DerivedPath>& paths,
                             std::shared_ptr<Store> eval_store);
};

} // namespace nix
