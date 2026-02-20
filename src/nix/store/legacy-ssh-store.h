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

  static const std::string name() { return "SSH store_t"; }

  static string_set_t uriSchemes() { return {"ssh"}; }

  static std::string doc();

  ref<store_t> open_store() const override;

  StoreReference getReference() const override;
};

struct LegacySSHStore : public virtual store_t {
  using config_t = LegacySSHStoreConfig;

  ref<const config_t> config;

  struct Connection;

  ref<pool_t<Connection>> connections;

  SSHMaster master;

  LegacySSHStore(ref<const config_t>);

  ref<Connection> open_connection();

  void
  query_path_info_uncached(const store_path_t& path,
                        Callback<std::shared_ptr<const valid_path_info_t>> callback) noexcept override;

  std::map<store_path_t, UnkeyedValidPathInfo> queryPathInfosUncached(const store_path_set_t& paths);

  void add_to_store(const valid_path_info_t& info, source_t& source, RepairFlag repair,
                  CheckSigsFlag check_sigs) override;

  void nar_from_path(const store_path_t& path, sink_t& sink) override;

  /**
   * Hands over the connection temporarily as source to the given
   * function. The function must not consume beyond the NAR; it can
   * not just blindly try to always read more bytes until it is
   * cut-off.
   *
   * This is exposed for sake of Hydra.
   */
  void nar_from_path(const store_path_t& path, std::function<void(source_t&)> fun);

  std::optional<store_path_t> queryPathFromHashPart(const std::string& hash_part) override {
    unsupported("queryPathFromHashPart");
  }

  store_path_t add_to_store(std::string_view name, const source_path_t& path, content_address_method_t method,
                       hash_algorithm_t hash_algo, const store_path_set_t& references, path_filter_t& filter,
                       RepairFlag repair) override {
    unsupported("addToStore");
  }

  store_path_t
  add_to_store_from_dump(source_t& dump, std::string_view name,
                     file_serialisation_method_t dump_method = file_serialisation_method_t::nix_archive,
                     content_address_method_t hash_method = file_ingestion_method_t::nix_archive,
                     hash_algorithm_t hash_algo = hash_algorithm_t::SHA256,
                     const store_path_set_t& references = store_path_set_t(),
                     RepairFlag repair = NoRepair) override {
    unsupported("addToStore");
  }

  void register_drv_output(const realisation_t& output) override { unsupported("registerDrvOutput"); }

public:
  build_result_t buildDerivation(const store_path_t& drv_path, const basic_derivation_t& drv,
                              BuildMode build_mode) override;

  /**
   * Note, the returned function must only be called once, or we'll
   * try to read from the connection twice.
   *
   * @todo use C++23 `std::move_only_function`.
   */
  std::function<build_result_t()> buildDerivationAsync(const store_path_t& drv_path,
                                                    const basic_derivation_t& drv,
                                                    const ServeProto::BuildOptions& options);

  void build_paths(const std::vector<derived_path_t>& drv_paths, BuildMode build_mode,
                  std::shared_ptr<store_t> eval_store) override;

  void ensure_path(const store_path_t& path) override { unsupported("ensurePath"); }

  ref<source_accessor_t> getFSAccessor(bool require_valid_path) override {
    unsupported("getFSAccessor");
  }

  std::shared_ptr<source_accessor_t> getFSAccessor(const store_path_t& path,
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
  void repairPath(const store_path_t& path) override { unsupported("repairPath"); }

  void computeFSClosure(const store_path_set_t& paths, store_path_set_t& out, bool flipDirection = false,
                        bool includeOutputs = false, bool includeDerivers = false) override;

  store_path_set_t queryValidPaths(const store_path_set_t& paths,
                               SubstituteFlag maybeSubstitute = NoSubstitute) override;

  /**
   * Custom variation that atomically creates temp locks on the remote
   * side.
   *
   * This exists to prevent a race where the remote host
   * garbage-collects paths that are already there. Optionally, ask
   * the remote host to substitute missing paths.
   */
  store_path_set_t queryValidPaths(const store_path_set_t& paths, bool lock,
                               SubstituteFlag maybeSubstitute = NoSubstitute);

  void connect() override;

  unsigned int getProtocol() override;

  struct ConnectionStats {
    size_t bytesReceived, bytesSent;
  };

  ConnectionStats getConnectionStats();

  ::pid_t get_connection_pid();

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
