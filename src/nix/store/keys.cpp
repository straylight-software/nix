#include "nix/store/keys.h"

#include "nix/store/globals.h"
#include "nix/util/file-system.h"

namespace nix {

PublicKeys getDefaultPublicKeys() {
  PublicKeys publicKeys;

  // FIXME: filter duplicates

  for (const auto& s : settings.trustedPublicKeys.get()) {
    PublicKey key(s);
    publicKeys.emplace(key.name, key);
  }

  for (const auto& secretKeyFile : settings.secretKeyFiles.get()) {
    try {
      SecretKey secretKey(readFile(secretKeyFile));
      publicKeys.emplace(secretKey.name, secretKey.toPublicKey());
    } catch (SystemError& e) {
      /* Ignore unreadable key files. That's normal in a
         multi-user installation. */
    }
  }

  return publicKeys;
}

} // namespace nix
