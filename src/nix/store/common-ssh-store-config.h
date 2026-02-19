#pragma once
///@file

#include "nix/store/store-api.h"
#include "nix/util/url.h"

namespace nix {

class SSHMaster;

struct CommonSSHStoreConfig : virtual StoreConfig {
  using StoreConfig::StoreConfig;

  CommonSSHStoreConfig(std::string_view scheme, const parsed_url_t::authority_t& authority,
                       const Params& params);
  CommonSSHStoreConfig(std::string_view scheme, std::string_view authority, const Params& params);

  const setting_t<Path> sshKey{
      this, "", "ssh-key",
      "Path to the SSH private key used to authenticate to the remote machine."};

  const setting_t<std::string> sshPublicHostKey{this, "", "base64-ssh-public-host-key",
                                              "The public host key of the remote machine."};

  const setting_t<bool> compress{this, false, "compress", "Whether to enable SSH compression."};

  const setting_t<std::string> remoteStore{this, "", "remote-store",
                                         R"(
          [Store URL](@docroot@/store/types/index.md#store-url-format)
          to be used on the remote machine. The default is `auto`
          (i.e. use the Nix daemon or `/nix/store` directly).
        )"};

  /**
   * authority_t representing the SSH host to connect to.
   */
  parsed_url_t::authority_t authority;

  /**
   * Small wrapper around `SSHMaster::SSHMaster` that gets most
   * arguments from this configuration.
   *
   * See that constructor for details on the remaining two arguments.
   */
  SSHMaster createSSHMaster(bool useMaster, descriptor_t logFD = INVALID_DESCRIPTOR) const;
};

} // namespace nix
