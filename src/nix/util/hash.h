#pragma once
///@file

#include "nix/util/configuration.h"
#include "nix/util/file-system.h"
#include "nix/util/json-impls.h"
#include "nix/util/serialise.h"
#include "nix/util/types.h"

namespace nix {

make_error(BadHash, Error);

enum struct hash_algorithm_t : char {
  md5 = 42,
  sha1 = 43,
  sha256 = 44,
  sha512 = 45,
  blake3 = 46,
  // Uppercase aliases for backward compatibility
  MD5 = md5,
  SHA1 = sha1,
  SHA256 = sha256,
  SHA512 = sha512,
  BLAKE3 = blake3,
};

/// Hash size constants for each algorithm
namespace hash_sizes {
constexpr size_t md5 = 16;
constexpr size_t sha1 = 20;
constexpr size_t sha256 = 32;
constexpr size_t sha512 = 64;
constexpr size_t blake3 = 32;
} // namespace hash_sizes

/**
 * @return the size of a hash for the given algorithm
 */
[[nodiscard]] constexpr auto regular_hash_size(hash_algorithm_t type) -> size_t {
  switch (type) {
    case hash_algorithm_t::blake3:
      return hash_sizes::blake3;
    case hash_algorithm_t::md5:
      return hash_sizes::md5;
    case hash_algorithm_t::sha1:
      return hash_sizes::sha1;
    case hash_algorithm_t::sha256:
      return hash_sizes::sha256;
    case hash_algorithm_t::sha512:
      return hash_sizes::sha512;
    default:
      assert(false);
  }
}

extern const string_set_t hash_algorithms;

/**
 * @brief Enumeration representing the hash formats.
 */
enum struct hash_format_t : std::uint8_t {
  /// @brief Base 64 encoding.
  /// @see [IETF RFC 4648, section 4](https://datatracker.ietf.org/doc/html/rfc4648#section-4).
  base64,
  /// @brief Nix-specific base-32 encoding. @see BaseNix32
  nix32,
  /// @brief Lowercase hexadecimal encoding. @see base16Chars
  base16,
  /// @brief "<hash algo>:<Base 64 hash>", format of the SRI integrity attribute.
  /// @see W3C recommendation [Subresource Integrity](https://www.w3.org/TR/SRI/).
  sri
};

extern const string_set_t hash_formats;

struct hash_t {
  /** opaque_t handle type for the hash calculation state. */
  union ctx_t;

  constexpr static size_t max_hash_size = 64;

private:
  size_t hash_size_ = 0;
  std::array<uint8_t, max_hash_size> hash_ = {};
  hash_algorithm_t algo_;

public:
  /**
   * Create a zero-filled hash object.
   */
  explicit hash_t(hash_algorithm_t algo, const experimental_feature_settings_t& xp_settings =
                                             experimental_feature_settings);

  /**
   * Parse the hash from a string representation in the format
   * "[<type>:]<base16|base32|base64>" or "<type>-<base64>" (a
   * Subresource Integrity hash expression). If the 'type' argument
   * is not present, then the hash algorithm must be specified in the
   * string.
   */
  [[nodiscard]] static auto parse_any(std::string_view s, std::optional<hash_algorithm_t> opt_algo)
      -> hash_t;

  /**
   * Like `parse_any`, but also returns the format the hash was parsed from.
   */
  [[nodiscard]] static auto parse_any_returning_format(std::string_view s,
                                                       std::optional<hash_algorithm_t> opt_algo)
      -> std::pair<hash_t, hash_format_t>;

  /**
   * Parse a hash from a string representation like the above, except the
   * type prefix is mandatory is there is no separate argument.
   */
  [[nodiscard]] static auto parse_any_prefixed(std::string_view s) -> hash_t;

  /**
   * Parse a plain hash that musst not have any prefix indicating the type.
   * The type is passed in to disambiguate.
   */
  [[nodiscard]] static auto parse_non_sri_unprefixed(std::string_view s, hash_algorithm_t algo)
      -> hash_t;

  /**
   * Like `parse_non_sri_unprefixed`, but the hash format has been
   * explicitly given.
   *
   * @param explicit_format cannot be SRI, but must be one of the
   * "bases".
   */
  [[nodiscard]] static auto parse_explicit_format_unprefixed(
      std::string_view s, hash_algorithm_t algo, hash_format_t explicit_format,
      const experimental_feature_settings_t& xp_settings = experimental_feature_settings) -> hash_t;

  [[nodiscard]] static auto
  parse_sri(std::string_view original,
            const experimental_feature_settings_t& xp_settings = experimental_feature_settings)
      -> hash_t;

  /**
   * Check whether two hashes are equal.
   */
  [[nodiscard]] auto operator==(const hash_t& h2) const noexcept -> bool;

  /**
   * Compare how two hashes are ordered.
   */
  [[nodiscard]] auto operator<=>(const hash_t& h2) const noexcept -> std::strong_ordering;

  /**
   * Return a string representation of the hash, in base-16, base-32
   * or base-64. By default, this is prefixed by the hash algo
   * (e.g. "sha256:").
   */
  [[nodiscard]] auto to_string(hash_format_t hash_format, bool include_algo) const -> std::string;

  [[nodiscard]] auto git_rev() const -> std::string {
    return to_string(hash_format_t::base16, false);
  }

  [[nodiscard]] auto git_short_rev() const -> std::string {
    constexpr size_t git_short_rev_length = 7;
    return {to_string(hash_format_t::base16, false), 0, git_short_rev_length};
  }

  static hash_t dummy;

  /**
   * @return a random hash with hash algorithm `algo`
   */
  [[nodiscard]] static auto random(hash_algorithm_t algo) -> hash_t;

  // Accessors
  [[nodiscard]] auto hash_size() const noexcept -> size_t { return hash_size_; }
  auto set_hash_size(size_t size) noexcept -> void { hash_size_ = size; }

  [[nodiscard]] auto hash() const noexcept -> const uint8_t* { return hash_.data(); }
  [[nodiscard]] auto hash() noexcept -> uint8_t* { return hash_.data(); }

  [[nodiscard]] auto algo() const noexcept -> hash_algorithm_t { return algo_; }
  auto set_algo(hash_algorithm_t algo) noexcept -> void { algo_ = algo; }
};

/**
 * Helper that defaults empty hashes to the 0 hash.
 */
[[nodiscard]] auto new_hash_allow_empty(std::string_view hash_str,
                                        std::optional<hash_algorithm_t> ha) -> hash_t;

/**
 * Compute the hash of the given string.
 */
[[nodiscard]] auto
hash_string(hash_algorithm_t ha, std::string_view s,
            const experimental_feature_settings_t& xp_settings = experimental_feature_settings)
    -> hash_t;

/**
 * Compute the hash of the given file, hashing its contents directly.
 *
 * (Metadata, such as the executable permission bit, is ignored.)
 */
[[nodiscard]] auto hash_file(hash_algorithm_t ha, const Path& path) -> hash_t;

/**
 * The final hash and the number of bytes digested.
 */
struct hash_result_t {
  hash_t hash;
  uint64_t num_bytes_digested = 0;
};

/**
 * Compress a hash to the specified number of bytes by cyclically
 * XORing bytes together.
 */
[[nodiscard]] auto compress_hash(const hash_t& hash, unsigned int new_size) -> hash_t;

/**
 * Parse a string representing a hash format.
 */
[[nodiscard]] auto parse_hash_format(std::string_view hash_format_name) -> hash_format_t;

/**
 * std::optional version of parse_hash_format that doesn't throw error.
 */
[[nodiscard]] auto parse_hash_format_opt(std::string_view hash_format_name)
    -> std::optional<hash_format_t>;

/**
 * The reverse of parse_hash_format.
 */
[[nodiscard]] auto print_hash_format(hash_format_t hash_format) -> std::string_view;

/**
 * Parse a string representing a hash algorithm.
 */
[[nodiscard]] auto
parse_hash_algo(std::string_view s,
                const experimental_feature_settings_t& xp_settings = experimental_feature_settings)
    -> hash_algorithm_t;

/**
 * Will return nothing on parse error
 */
[[nodiscard]] auto parse_hash_algo_opt(
    std::string_view s,
    const experimental_feature_settings_t& xp_settings = experimental_feature_settings)
    -> std::optional<hash_algorithm_t>;

/**
 * And the reverse.
 */
[[nodiscard]] auto print_hash_algo(hash_algorithm_t ha) -> std::string_view;

struct abstract_hash_sink_t : virtual sink_t {
  [[nodiscard]] virtual auto finish() -> hash_result_t = 0;
};

class hash_sink_t : public buffered_sink_t, public abstract_hash_sink_t {
private:
  hash_algorithm_t ha_;
  hash_t::ctx_t* ctx_;
  uint64_t bytes_;

public:
  explicit hash_sink_t(hash_algorithm_t ha);
  hash_sink_t(const hash_sink_t& h);
  hash_sink_t(hash_sink_t&& h) noexcept;
  auto operator=(const hash_sink_t& h) -> hash_sink_t&;
  auto operator=(hash_sink_t&& h) noexcept -> hash_sink_t&;
  ~hash_sink_t() override;
  auto write_unbuffered(std::string_view data) -> void override;
  [[nodiscard]] auto finish() -> hash_result_t override;
  [[nodiscard]] auto current_hash() -> hash_result_t;
};

template <>
struct json_avoids_null<hash_t> : std::true_type {};

// PascalCase alias for backward compatibility
using Hash = hash_t;

} // namespace nix

template <>
struct std::hash<nix::hash_t> {
  [[nodiscard]] auto operator()(const nix::hash_t& hash) const noexcept -> std::size_t {
    assert(hash.hash_size() > sizeof(size_t));
    std::size_t result = 0;
    std::memcpy(&result, hash.hash(), sizeof(result));
    return result;
  }
};

namespace nix {

[[nodiscard]] inline auto hash_value(const hash_t& h) -> std::size_t {
  return std::hash<hash_t>{}(h);
}

} // namespace nix

JSON_IMPL_WITH_XP_FEATURES(hash_t)
