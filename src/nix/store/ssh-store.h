#pragma once
///@file

#include "nix/store/common-ssh-store-config.h"
#include "nix/store/local-fs-store.h"
#include "nix/store/remote-store.h"
#include "nix/store/store-api.h"

namespace nix {

struct SSHStoreConfig : std::enable_shared_from_this<SSHStoreConfig>,
                        virtual RemoteStoreConfig,
                        virtual CommonSSHStoreConfig {
  using CommonSSHStoreConfig::CommonSSHStoreConfig;
  using RemoteStoreConfig::RemoteStoreConfig;

  SSHStoreConfig(std::string_view scheme, std::string_view authority, const Params& params);

  const setting_t<strings_t> remoteProgram{
      this,
      {"nix-daemon"},
      "remote-program",
      "Path to the `nix-daemon` executable on the remote machine."};

  static const std::string name() { return "Experimental SSH Store"; }

  static string_set_t uriSchemes() { return {"ssh-ng"}; }

  static std::string doc();

  ref<Store> open_store() const override;

  StoreReference getReference() const override;
};

struct MountedSSHStoreConfig : virtual SSHStoreConfig, virtual LocalFSStoreConfig {
  MountedSSHStoreConfig(string_map_t params);
  MountedSSHStoreConfig(std::string_view scheme, std::string_view host, string_map_t params);

  static const std::string name() { return "Experimental SSH Store with filesystem mounted"; }

  static string_set_t uriSchemes() { return {"mounted-ssh-ng"}; }

  static std::string doc();

  static std::optional<experimental_feature_t> experimental_feature() {
    return experimental_feature_t::mounted_ssh_store_t;
  }

  ref<Store> open_store() const override;
};

} // namespace nix
