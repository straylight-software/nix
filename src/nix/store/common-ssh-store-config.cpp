#include "nix/store/common-ssh-store-config.h"

#include <regex>

#include "nix/store/ssh.h"

namespace nix {

CommonSSHStoreConfig::CommonSSHStoreConfig(std::string_view scheme, std::string_view authority,
                                           const Params& params)
    : CommonSSHStoreConfig(scheme, parsed_url_t::authority_t::parse(authority), params) {}

CommonSSHStoreConfig::CommonSSHStoreConfig(std::string_view scheme,
                                           const parsed_url_t::authority_t& authority,
                                           const Params& params)
    : StoreConfig(params), authority(authority) {}

SSHMaster CommonSSHStoreConfig::createSSHMaster(bool useMaster, descriptor_t logFD) const {
  return {
      authority, sshKey.get(), ssh_public_host_key.get(), useMaster, compress, logFD,
  };
}

} // namespace nix
