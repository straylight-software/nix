#pragma once

#include <map>
#include <optional>

#include "nix/util/signature/local-keys.h"
#include "nix/util/types.h"

namespace nix {

/**
 * An abstract signer
 *
 * Derive from this class to implement a custom signature scheme.
 *
 * It is only necessary to implement signature of bytes and provide a
 * public key.
 */
struct signer_t {
  virtual ~signer_t() = default;

  /**
   * Sign the given data, creating a (detached) signature.
   *
   * @param data data to be signed.
   *
   * @return the [detached
   * signature](https://en.wikipedia.org/wiki/Detached_signature),
   * i.e. just the signature itself without a copy of the signed data.
   */
  virtual std::string sign_detached(std::string_view data) const = 0;

  /**
   * View the public key associated with this `signer_t`.
   */
  virtual const public_key_t& get_public_key() = 0;
};

using signers_t = std::map<std::string, signer_t*>;

/**
 * Local signer
 *
 * The private key is held in this machine's RAM
 */
struct local_signer_t : signer_t {
  local_signer_t(secret_key_t&& private_key);

  std::string sign_detached(std::string_view s) const override;

  const public_key_t& get_public_key() override;

private:
  secret_key_t private_key;
  public_key_t publicKey;
};

} // namespace nix
