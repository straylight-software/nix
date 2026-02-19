#pragma once
///@file

#include "nix/store/indirect-root-store.h"
#include "nix/store/remote-store-connection.h"
#include "nix/store/remote-store.h"

namespace nix {

struct UDSRemoteStoreConfig : std::enable_shared_from_this<UDSRemoteStoreConfig>,
                              virtual LocalFSStoreConfig,
                              virtual RemoteStoreConfig {
  // TODO(fzakaria): Delete this constructor once moved over to the factory pattern
  // outlined in https://github.com/NixOS/nix/issues/10766
  using LocalFSStoreConfig::LocalFSStoreConfig;
  using RemoteStoreConfig::RemoteStoreConfig;

  /**
   * @param authority is the socket path.
   */
  UDSRemoteStoreConfig(std::string_view scheme, std::string_view authority, const Params& params);

  UDSRemoteStoreConfig(const Params& params);

  static const std::string name() { return "Local Daemon Store"; }

  static std::string doc();

  /**
   * The path to the unix domain socket.
   *
   * The default is `settings.nixDaemonSocketFile`, but we don't write
   * that below, instead putting in the constructor.
   */
  Path path;

  static string_set_t uriSchemes() { return {"unix"}; }

  ref<Store> open_store() const override;

  StoreReference getReference() const override;
};

struct UDSRemoteStore : virtual IndirectRootStore, virtual remote_store {
  using config_t = UDSRemoteStoreConfig;

  ref<const config_t> config;

  UDSRemoteStore(ref<const config_t>);

  ref<SourceAccessor> getFSAccessor(bool require_valid_path = true) override {
    return local_fs_store::getFSAccessor(require_valid_path);
  }

  std::shared_ptr<SourceAccessor> getFSAccessor(const StorePath& path,
                                                bool require_valid_path = true) override {
    return local_fs_store::getFSAccessor(path, require_valid_path);
  }

  void nar_from_path(const StorePath& path, Sink& sink) override { Store::nar_from_path(path, sink); }

  /**
   * Implementation of `IndirectRootStore::addIndirectRoot()` which
   * delegates to the remote store.
   *
   * The idea is that the client makes the direct symlink, so it is
   * owned managed by the client's user account, and the server makes
   * the indirect symlink.
   */
  void addIndirectRoot(const Path& path) override;

private:
  struct Connection : remote_store::Connection {
    auto_close_fd_t fd;
    void closeWrite() override;
  };

  ref<remote_store::Connection> open_connection() override;
};

} // namespace nix
