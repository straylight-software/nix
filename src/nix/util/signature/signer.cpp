#include "nix/util/signature/signer.h"

#include <sodium.h>

#include "nix/util/error.h"

namespace nix {

local_signer_t::local_signer_t(secret_key_t&& private_key)
    : private_key(private_key), publicKey(private_key.to_public_key()) {}

std::string local_signer_t::sign_detached(std::string_view s) const {
  return private_key.sign_detached(s);
}

const public_key_t& local_signer_t::get_public_key() {
  return publicKey;
}

} // namespace nix
