#pragma once
///@file

#include "nix/util/configuration.h"
#include "nix/util/file-system.h"
#include "nix/util/json-impls.h"
#include "nix/util/serialise.h"
#include "nix/util/types.h"

namespace nix {

make_error(BadHash, Error);

enum struct hash_algorithm_t : char { MD5 = 42, SHA1, SHA256, SHA512, BLAKE3 };

/**
 * @return the size of a hash for the given algorithm
 */
constexpr inline size_t regular_hash_size(hash_algorithm_t type) {
  switch (type) {
    case hash_algorithm_t::BLAKE3:
      return 32;
    case hash_algorithm_t::MD5:
      return 16;
    case hash_algorithm_t::SHA1:
      return 20;
    case hash_algorithm_t::SHA256:
      return 32;
    case hash_algorithm_t::SHA512:
      return 64;
    default:
      assert(false);
  }
}

extern const string_set_t hash_algorithms;

/**
 * @brief Enumeration representing the hash formats.
 */
enum struct hash_format_t : int {
  /// @brief Base 64 encoding.
  /// @see [IETF RFC 4648, section 4](https://datatracker.ietf.org/doc/html/rfc4648#section-4).
  base64,
  /// @brief Nix-specific base-32 encoding. @see BaseNix32
  nix32,
  /// @brief Lowercase hexadecimal encoding. @see base16Chars
  base16,
  /// @brief "<hash algo>:<Base 64 hash>", format of the SRI integrity attribute.
  /// @see W3C recommendation [Subresource Integrity](https://www.w3.org/TR/SRI/).
  SRI
};

extern const string_set_t hash_formats;

struct Hash {
  /** opaque_t handle type for the hash calculation state. */
  union Ctx;

  constexpr static size_t max_hash_size = 64;
  size_t hash_size = 0;
  uint8_t hash[max_hash_size] = {};

  hash_algorithm_t algo;

  /**
   * Create a zero-filled hash object.
   */
  explicit Hash(hash_algorithm_t algo,
                const experimental_feature_settings_t& xp_settings = experimental_feature_settings);

  /**
   * Parse the hash from a string representation in the format
   * "[<type>:]<base16|base32|base64>" or "<type>-<base64>" (a
   * Subresource Integrity hash expression). If the 'type' argument
   * is not present, then the hash algorithm must be specified in the
   * string.
   */
  static Hash parse_any(std::string_view s, std::optional<hash_algorithm_t> opt_algo);

  /**
   * Like `parse_any`, but also returns the format the hash was parsed from.
   */
  static std::pair<Hash, hash_format_t> parse_any_returning_format(std::string_view s,
                                                             std::optional<hash_algorithm_t> opt_algo);

  /**
   * Parse a hash from a string representation like the above, except the
   * type prefix is mandatory is there is no separate argument.
   */
  static Hash parse_any_prefixed(std::string_view s);

  /**
   * Parse a plain hash that musst not have any prefix indicating the type.
   * The type is passed in to disambiguate.
   */
  static Hash parse_non_sri_unprefixed(std::string_view s, hash_algorithm_t algo);

  /**
   * Like `parse_non_sri_unprefixed`, but the hash format has been
   * explicitly given.
   *
   * @param explicit_format cannot be SRI, but must be one of the
   * "bases".
   */
  static Hash parse_explicit_format_unprefixed(
      std::string_view s, hash_algorithm_t algo, hash_format_t explicit_format,
      const experimental_feature_settings_t& xp_settings = experimental_feature_settings);

  static Hash parse_sri(std::string_view original,
                       const experimental_feature_settings_t& xp_settings = experimental_feature_settings);

public:
  /**
   * Check whether two hashes are equal.
   */
  bool operator==(const Hash& h2) const noexcept;

  /**
   * Compare how two hashes are ordered.
   */
  std::strong_ordering operator<=>(const Hash& h2) const noexcept;

  /**
   * Return a string representation of the hash, in base-16, base-32
   * or base-64. By default, this is prefixed by the hash algo
   * (e.g. "sha256:").
   */
  [[nodiscard]] std::string to_string(hash_format_t hash_format, bool include_algo) const;

  [[nodiscard]] std::string git_rev() const { return to_string(hash_format_t::base16, false); }

  [[nodiscard]] std::string git_short_rev() const {
    return std::string(to_string(hash_format_t::base16, false), 0, 7);
  }

  static Hash dummy;

  /**
   * @return a random hash with hash algorithm `algo`
   */
  static Hash random(hash_algorithm_t algo);
};

/**
 * Helper that defaults empty hashes to the 0 hash.
 */
Hash new_hash_allow_empty(std::string_view hash_str, std::optional<hash_algorithm_t> ha);

/**
 * Compute the hash of the given string.
 */
Hash hash_string(hash_algorithm_t ha, std::string_view s,
                const experimental_feature_settings_t& xp_settings = experimental_feature_settings);

/**
 * Compute the hash of the given file, hashing its contents directly.
 *
 * (Metadata, such as the executable permission bit, is ignored.)
 */
Hash hash_file(hash_algorithm_t ha, const Path& path);

/**
 * The final hash and the number of bytes digested.
 */
struct hash_result_t {
  Hash hash;
  uint64_t num_bytes_digested;
};

/**
 * Compress a hash to the specified number of bytes by cyclically
 * XORing bytes together.
 */
Hash compress_hash(const Hash& hash, unsigned int new_size);

/**
 * Parse a string representing a hash format.
 */
hash_format_t parse_hash_format(std::string_view hash_format_name);

/**
 * std::optional version of parse_hash_format that doesn't throw error.
 */
std::optional<hash_format_t> parse_hash_format_opt(std::string_view hash_format_name);

/**
 * The reverse of parse_hash_format.
 */
std::string_view print_hash_format(hash_format_t hash_format);

/**
 * Parse a string representing a hash algorithm.
 */
hash_algorithm_t
parse_hash_algo(std::string_view s,
              const experimental_feature_settings_t& xp_settings = experimental_feature_settings);

/**
 * Will return nothing on parse error
 */
std::optional<hash_algorithm_t>
parse_hash_algo_opt(std::string_view s,
                 const experimental_feature_settings_t& xp_settings = experimental_feature_settings);

/**
 * And the reverse.
 */
std::string_view print_hash_algo(hash_algorithm_t ha);

struct abstract_hash_sink_t : virtual Sink {
  virtual hash_result_t finish() = 0;
};

class hash_sink_t : public buffered_sink_t, public abstract_hash_sink_t {
private:
  hash_algorithm_t ha;
  Hash::Ctx* ctx;
  uint64_t bytes;

public:
  hash_sink_t(hash_algorithm_t ha);
  hash_sink_t(const hash_sink_t& h);
  ~hash_sink_t();
  void write_unbuffered(std::string_view data) override;
  hash_result_t finish() override;
  hash_result_t current_hash();
};

template <>
struct json_avoids_null<Hash> : std::true_type {};

} // namespace nix

template <>
struct std::hash<nix::Hash> {
  std::size_t operator()(const nix::Hash& hash) const noexcept {
    assert(hash.hash_size > sizeof(size_t));
    return *reinterpret_cast<const std::size_t*>(&hash.hash);
  }
};

namespace nix {

inline std::size_t hash_value(const Hash& hash) {
  return std::hash<Hash>{}(hash);
}

} // namespace nix

JSON_IMPL_WITH_XP_FEATURES(Hash)
