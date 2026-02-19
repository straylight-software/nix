#pragma once
///@file

#include "nix/store/common-ssh-store-config.h"
#include "nix/store/serve-protocol.h"
#include "nix/store/ssh.h"
#include "nix/store/store-api.h"
#include "nix/util/callback.h"
#include "nix/util/pool.h"

namespace nix {

struct LegacySSHStoreConfig : std::enable_shared_from_this<LegacySSHStoreConfig>,
                              virtual CommonSSHStoreConfig {
  using CommonSSHStoreConfig::CommonSSHStoreConfig;

  LegacySSHStoreConfig(std::string_view scheme, std::string_view authority, const Params& params);

#ifndef _WIN32
  // Hack for getting remote build log output.
  // Intentionally not in `LegacySSHStoreConfig` so that it doesn't appear in
  // the documentation
  const setting_t<int> logFD{this, INVALID_DESCRIPTOR, "log-fd",
                           "file descriptor to which SSH's stderr is connected"};
#else
  descriptor_t logFD = INVALID_DESCRIPTOR;
#endif

  const setting_t<strings_t> remoteProgram{this,
                                       {"nix-store"},
                                       "remote-program",
                                       "Path to the `nix-store` executable on the remote machine."};

  const setting_t<int> maxConnections{this, 1, "max-connections",
                                    "Maximum number of concurrent SSH connections."};

  /**
   * Hack for hydra
   */
  strings_t extraSshArgs = {};

  /**
   * Exposed for hydra
   */
  std::optional<size_t> connPipeSize;

  static const std::string name() { return "SSH Store"; }

  static string_set_t uriSchemes() { return {"ssh"}; }

  static std::string doc();

  ref<Store> open_store() const override;

  StoreReference getReference() const override;
};

struct LegacySSHStore : public virtual Store {
  using config_t = LegacySSHStoreConfig;

  ref<const config_t> config;

  struct Connection;

  ref<Pool<Connection>> connections;

  SSHMaster master;

  LegacySSHStore(ref<const config_t>);

  ref<Connection> open_connection();

  void
  query_path_info_uncached(const StorePath& path,
                        Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept override;

  std::map<StorePath, UnkeyedValidPathInfo> queryPathInfosUncached(const StorePathSet& paths);

  void add_to_store(const ValidPathInfo& info, Source& source, RepairFlag repair,
                  CheckSigsFlag check_sigs) override;

  void nar_from_path(const StorePath& path, Sink& sink) override;

  /**
   * Hands over the connection temporarily as source to the given
   * function. The function must not consume beyond the NAR; it can
   * not just blindly try to always read more bytes until it is
   * cut-off.
   *
   * This is exposed for sake of Hydra.
   */
  void nar_from_path(const StorePath& path, std::function<void(Source&)> fun);

  std::optional<StorePath> queryPathFromHashPart(const std::string& hash_part) override {
    unsupported("queryPathFromHashPart");
  }

  StorePath add_to_store(std::string_view name, const source_path_t& path, ContentAddressMethod method,
                       hash_algorithm_t hash_algo, const StorePathSet& references, path_filter_t& filter,
                       RepairFlag repair) override {
    unsupported("addToStore");
  }

  StorePath
  add_to_store_from_dump(Source& dump, std::string_view name,
                     file_serialisation_method_t dump_method = file_serialisation_method_t::nix_archive,
                     ContentAddressMethod hash_method = file_ingestion_method_t::nix_archive,
                     hash_algorithm_t hash_algo = hash_algorithm_t::SHA256,
                     const StorePathSet& references = StorePathSet(),
                     RepairFlag repair = NoRepair) override {
    unsupported("addToStore");
  }

  void register_drv_output(const Realisation& output) override { unsupported("registerDrvOutput"); }

public:
  BuildResult buildDerivation(const StorePath& drv_path, const BasicDerivation& drv,
                              BuildMode build_mode) override;

  /**
   * Note, the returned function must only be called once, or we'll
   * try to read from the connection twice.
   *
   * @todo use C++23 `std::move_only_function`.
   */
  std::function<BuildResult()> buildDerivationAsync(const StorePath& drv_path,
                                                    const BasicDerivation& drv,
                                                    const ServeProto::BuildOptions& options);

  void build_paths(const std::vector<DerivedPath>& drv_paths, BuildMode build_mode,
                  std::shared_ptr<Store> eval_store) override;

  void ensure_path(const StorePath& path) override { unsupported("ensurePath"); }

  ref<SourceAccessor> getFSAccessor(bool require_valid_path) override {
    unsupported("getFSAccessor");
  }

  std::shared_ptr<SourceAccessor> getFSAccessor(const StorePath& path,
                                                bool require_valid_path) override {
    unsupported("getFSAccessor");
  }

  /**
   * The default instance would schedule the work on the client side, but
   * for consistency with `build_paths` and `buildDerivation` it should happen
   * on the remote side.
   *
   * We make this fail for now so we can add implement this properly later
   * without it being a breaking change.
   */
  void repairPath(const StorePath& path) override { unsupported("repairPath"); }

  void computeFSClosure(const StorePathSet& paths, StorePathSet& out, bool flipDirection = false,
                        bool includeOutputs = false, bool includeDerivers = false) override;

  StorePathSet queryValidPaths(const StorePathSet& paths,
                               SubstituteFlag maybeSubstitute = NoSubstitute) override;

  /**
   * Custom variation that atomically creates temp locks on the remote
   * side.
   *
   * This exists to prevent a race where the remote host
   * garbage-collects paths that are already there. Optionally, ask
   * the remote host to substitute missing paths.
   */
  StorePathSet queryValidPaths(const StorePathSet& paths, bool lock,
                               SubstituteFlag maybeSubstitute = NoSubstitute);

  void connect() override;

  unsigned int getProtocol() override;

  struct ConnectionStats {
    size_t bytesReceived, bytesSent;
  };

  ConnectionStats getConnectionStats();

  pid_t getConnectionPid();

  /**
   * The legacy ssh protocol doesn't support checking for trusted-user.
   * Try using ssh-ng:// instead if you want to know.
   */
  std::optional<TrustedFlag> isTrustedClient() override;

  void query_realisation_uncached(
      const DrvOutput&,
      Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept override
  // TODO: Implement
  {
    unsupported("queryRealisation");
  }
};

} // namespace nix
