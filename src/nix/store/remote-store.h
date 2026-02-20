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
class process_handle_t;
struct fd_sink_t;
struct fd_source_t;
template <typename T>
class pool_t;
class RemoteFSAccessor;

struct remote_store_config_t : virtual store_config_t {
  using store_config_t::store_config_t;

  const setting_t<int> maxConnections{
      this, 64, "max-connections", "Maximum number of concurrent connections to the Nix daemon."};

  const setting_t<unsigned int> maxConnectionAge{
      this, std::numeric_limits<unsigned int>::max(), "max-connection-age",
      "Maximum age of a connection before it is closed."};
};

/**
 * \todo remote_store is a misnomer - should be something like
 * DaemonStore.
 */
struct remote_store : public virtual store_t,
                      public virtual GcStore,
                      public virtual LogStore,
                      public virtual QueryActiveBuildsStore {
  using config_t = remote_store_config_t;

  const config_t& config;

  remote_store(const config_t& config);

  /* Implementations of abstract store API methods. */

  bool isValidPathUncached(const store_path_t& path) override;

  store_path_set_t queryValidPaths(const store_path_set_t& paths,
                                   SubstituteFlag maybeSubstitute = NoSubstitute) override;

  store_path_set_t query_all_valid_paths() override;

  void query_path_info_uncached(
      const store_path_t& path,
      Callback<std::shared_ptr<const valid_path_info_t>> callback) noexcept override;

  void query_referrers(const store_path_t& path, store_path_set_t& referrers) override;

  store_path_set_t queryValidDerivers(const store_path_t& path) override;

  store_path_set_t queryDerivationOutputs(const store_path_t& path) override;

  std::map<std::string, std::optional<store_path_t>>
  queryPartialDerivationOutputMap(const store_path_t& path, store_t* eval_store = nullptr) override;
  std::optional<store_path_t> queryPathFromHashPart(const std::string& hash_part) override;

  store_path_set_t querySubstitutablePaths(const store_path_set_t& paths) override;

  void querySubstitutablePathInfos(const StorePathCAMap& paths,
                                   SubstitutablePathInfos& infos) override;

  /**
   * Add a content-addressable store path. `dump` will be drained.
   */
  ref<const valid_path_info_t> addCAToStore(source_t& dump, std::string_view name,
                                            content_address_method_t ca_method,
                                            hash_algorithm_t hash_algo,
                                            const store_path_set_t& references, RepairFlag repair);

  /**
   * Add a content-addressable store path. `dump` will be drained.
   */
  store_path_t add_to_store_from_dump(
      source_t& dump, std::string_view name,
      file_serialisation_method_t dump_method = file_serialisation_method_t::nix_archive,
      content_address_method_t hash_method = file_ingestion_method_t::nix_archive,
      hash_algorithm_t hash_algo = hash_algorithm_t::SHA256,
      const store_path_set_t& references = store_path_set_t(),
      RepairFlag repair = NoRepair) override;

  void add_to_store(const valid_path_info_t& info, source_t& nar, RepairFlag repair,
                    CheckSigsFlag check_sigs) override;

  void addMultipleToStore(source_t& source, RepairFlag repair, CheckSigsFlag check_sigs) override;

  void addMultipleToStore(PathsSource&& paths_to_copy, activity_t& act, RepairFlag repair,
                          CheckSigsFlag check_sigs) override;

  void register_drv_output(const realisation_t& info) override;

  void query_realisation_uncached(
      const DrvOutput&,
      Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept override;

  void build_paths(const std::vector<derived_path_t>& paths, BuildMode build_mode,
                   std::shared_ptr<store_t> eval_store) override;

  std::vector<keyed_build_result_t>
  build_paths_with_results(const std::vector<derived_path_t>& paths, BuildMode build_mode,
                           std::shared_ptr<store_t> eval_store) override;

  build_result_t buildDerivation(const store_path_t& drv_path, const basic_derivation_t& drv,
                                 BuildMode build_mode) override;

  void ensure_path(const store_path_t& path) override;

  void addTempRoot(const store_path_t& path) override;

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
  void repairPath(const store_path_t& path) override { unsupported("repairPath"); }

  void addSignatures(const store_path_t& store_path, const string_set_t& sigs) override;

  MissingPaths query_missing(const std::vector<derived_path_t>& targets) override;

  void addBuildLog(const store_path_t& drv_path, std::string_view log) override;

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

  ref<pool_t<Connection>> connections;

  virtual void setOptions(Connection& conn);

  void setOptions() override;

  struct ConnectionHandle;

  ConnectionHandle getConnection();

  friend struct ConnectionHandle;

  virtual ref<source_accessor_t> getFSAccessor(bool require_valid_path = true) override;

  virtual std::shared_ptr<source_accessor_t> getFSAccessor(const store_path_t& path,
                                                           bool require_valid_path = true) override;

  virtual void nar_from_path(const store_path_t& path, sink_t& sink) override;

private:
  /**
   * Same as the default implemenation of `remote_store::getFSAccessor`, but with a more preceise
   * return type.
   */
  ref<RemoteFSAccessor> getRemoteFSAccessor(bool require_valid_path = true);

  std::atomic_bool failed{false};

  void copyDrvsFromEvalStore(const std::vector<derived_path_t>& paths,
                             std::shared_ptr<store_t> eval_store);
};

} // namespace nix
