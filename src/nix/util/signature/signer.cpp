#include "nix/util/signature/signer.h"

#include <sodium.h>

#include "nix/util/error.h"

namespace nix {

LocalSigner::LocalSigner(SecretKey&& privateKey)
    : privateKey(privateKey), publicKey(privateKey.toPublicKey()) {}

std::string LocalSigner::signDetached(std::string_view s) const {
  return privateKey.signDetached(s);
}

const PublicKey& LocalSigner::getPublicKey() {
  return publicKey;
}

} // namespace nix
