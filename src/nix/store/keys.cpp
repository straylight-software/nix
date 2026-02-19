#include "nix/store/keys.h"

#include "nix/store/globals.h"
#include "nix/util/file-system.h"

namespace nix {

public_keys_t get_default_public_keys() {
  public_keys_t public_keys;

  // FIXME: filter duplicates

  for (const auto& s : settings.trustedPublicKeys.get()) {
    public_key_t key(s);
    public_keys.emplace(key.name, key);
  }

  for (const auto& secret_key_file : settings.secretKeyFiles.get()) {
    try {
      secret_key_t secret_key(read_file(secret_key_file));
      public_keys.emplace(secret_key.name, secret_key.to_public_key());
    } catch (SystemError& e) {
      /* Ignore unreadable key files. That's normal in a
         multi-user installation. */
    }
  }

  return public_keys;
}

} // namespace nix
