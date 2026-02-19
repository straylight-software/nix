#pragma once
///@file

#include <map>

#include "nix/util/types.h"

namespace nix {

/**
 * Except where otherwise noted, Nix serializes keys and signatures in
 * the form:
 *
 * ```
 * <name>:<key/signature-in-Base64>
 * ```
 */
struct borrowed_crypto_value_t {
  std::string_view name;
  std::string_view payload;

  /**
   * This splits on the colon, the user can then separated decode the
   * Base64 payload separately.
   */
  static borrowed_crypto_value_t parse(std::string_view);
};

struct Key {
  std::string name;
  std::string key;

  std::string to_string() const;

protected:
  /**
   * Construct Key from a string in the format
   * ‘<name>:<key-in-base64>’.
   *
   * @param sensitiveValue Avoid displaying the raw Base64 in error
   * messages to avoid leaking private keys.
   */
  Key(std::string_view s, bool sensitiveValue);

  Key(std::string_view name, std::string&& key) : name(name), key(std::move(key)) {}
};

struct public_key_t;

struct secret_key_t : Key {
  secret_key_t(std::string_view s);

  /**
   * Return a detached signature of the given string.
   */
  std::string signDetached(std::string_view s) const;

  public_key_t toPublicKey() const;

  static secret_key_t generate(std::string_view name);

private:
  secret_key_t(std::string_view name, std::string&& key) : Key(name, std::move(key)) {}
};

struct public_key_t : Key {
  public_key_t(std::string_view data);

  /**
   * @return true iff `sig` and this key's names match, and `sig` is a
   * correct signature over `data` using the given public key.
   */
  bool verifyDetached(std::string_view data, std::string_view sigs) const;

  /**
   * @return true iff `sig` is a correct signature over `data` using the
   * given public key.
   *
   * @param just the Base64 signature itself, not a colon-separated pair of a
   * public key name and signature.
   */
  bool verifyDetachedAnon(std::string_view data, std::string_view sigs) const;

private:
  public_key_t(std::string_view name, std::string&& key) : Key(name, std::move(key)) {}
  friend struct secret_key_t;
};

/**
 * Map from key names to public keys
 */
typedef std::map<std::string, public_key_t> public_keys_t;

/**
 * @return true iff ‘sig’ is a correct signature over ‘data’ using one
 * of the given public keys.
 */
bool verifyDetached(std::string_view data, std::string_view sig, const public_keys_t& publicKeys);

} // namespace nix
