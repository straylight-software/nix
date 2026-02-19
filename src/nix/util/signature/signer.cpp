#include "nix/util/signature/signer.h"

#include <sodium.h>

#include "nix/util/error.h"

namespace nix {

local_signer_t::local_signer_t(secret_key_t&& privateKey)
    : privateKey(privateKey), publicKey(privateKey.toPublicKey()) {}

std::string local_signer_t::signDetached(std::string_view s) const {
  return privateKey.signDetached(s);
}

const public_key_t& local_signer_t::getPublicKey() {
  return publicKey;
}

} // namespace nix
