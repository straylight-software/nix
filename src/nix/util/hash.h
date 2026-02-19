#pragma once
///@file

#include "nix/util/configuration.h"
#include "nix/util/file-system.h"
#include "nix/util/json-impls.h"
#include "nix/util/serialise.h"
#include "nix/util/types.h"

namespace nix {

MakeError(BadHash, Error);

enum struct hash_algorithm_t : char { MD5 = 42, SHA1, SHA256, SHA512, BLAKE3 };

/**
 * @return the size of a hash for the given algorithm
 */
constexpr inline size_t regularHashSize(hash_algorithm_t type) {
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

extern const string_set_t hashAlgorithms;

/**
 * @brief Enumeration representing the hash formats.
 */
enum struct hash_format_t : int {
  /// @brief Base 64 encoding.
  /// @see [IETF RFC 4648, section 4](https://datatracker.ietf.org/doc/html/rfc4648#section-4).
  Base64,
  /// @brief Nix-specific base-32 encoding. @see BaseNix32
  Nix32,
  /// @brief Lowercase hexadecimal encoding. @see base16Chars
  Base16,
  /// @brief "<hash algo>:<Base 64 hash>", format of the SRI integrity attribute.
  /// @see W3C recommendation [Subresource Integrity](https://www.w3.org/TR/SRI/).
  SRI
};

extern const string_set_t hashFormats;

struct Hash {
  /** opaque_t handle type for the hash calculation state. */
  union Ctx;

  constexpr static size_t maxHashSize = 64;
  size_t hashSize = 0;
  uint8_t hash[maxHashSize] = {};

  hash_algorithm_t algo;

  /**
   * Create a zero-filled hash object.
   */
  explicit Hash(hash_algorithm_t algo,
                const experimental_feature_settings_t& xpSettings = experimentalFeatureSettings);

  /**
   * Parse the hash from a string representation in the format
   * "[<type>:]<base16|base32|base64>" or "<type>-<base64>" (a
   * Subresource Integrity hash expression). If the 'type' argument
   * is not present, then the hash algorithm must be specified in the
   * string.
   */
  static Hash parseAny(std::string_view s, std::optional<hash_algorithm_t> optAlgo);

  /**
   * Like `parseAny`, but also returns the format the hash was parsed from.
   */
  static std::pair<Hash, hash_format_t> parseAnyReturningFormat(std::string_view s,
                                                             std::optional<hash_algorithm_t> optAlgo);

  /**
   * Parse a hash from a string representation like the above, except the
   * type prefix is mandatory is there is no separate argument.
   */
  static Hash parseAnyPrefixed(std::string_view s);

  /**
   * Parse a plain hash that musst not have any prefix indicating the type.
   * The type is passed in to disambiguate.
   */
  static Hash parseNonSRIUnprefixed(std::string_view s, hash_algorithm_t algo);

  /**
   * Like `parseNonSRIUnprefixed`, but the hash format has been
   * explicitly given.
   *
   * @param explicitFormat cannot be SRI, but must be one of the
   * "bases".
   */
  static Hash parseExplicitFormatUnprefixed(
      std::string_view s, hash_algorithm_t algo, hash_format_t explicitFormat,
      const experimental_feature_settings_t& xpSettings = experimentalFeatureSettings);

  static Hash parseSRI(std::string_view original,
                       const experimental_feature_settings_t& xpSettings = experimentalFeatureSettings);

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
  [[nodiscard]] std::string to_string(hash_format_t hashFormat, bool includeAlgo) const;

  [[nodiscard]] std::string gitRev() const { return to_string(hash_format_t::Base16, false); }

  [[nodiscard]] std::string gitShortRev() const {
    return std::string(to_string(hash_format_t::Base16, false), 0, 7);
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
Hash newHashAllowEmpty(std::string_view hashStr, std::optional<hash_algorithm_t> ha);

/**
 * Compute the hash of the given string.
 */
Hash hashString(hash_algorithm_t ha, std::string_view s,
                const experimental_feature_settings_t& xpSettings = experimentalFeatureSettings);

/**
 * Compute the hash of the given file, hashing its contents directly.
 *
 * (Metadata, such as the executable permission bit, is ignored.)
 */
Hash hashFile(hash_algorithm_t ha, const Path& path);

/**
 * The final hash and the number of bytes digested.
 */
struct hash_result_t {
  Hash hash;
  uint64_t numBytesDigested;
};

/**
 * Compress a hash to the specified number of bytes by cyclically
 * XORing bytes together.
 */
Hash compressHash(const Hash& hash, unsigned int newSize);

/**
 * Parse a string representing a hash format.
 */
hash_format_t parseHashFormat(std::string_view hashFormatName);

/**
 * std::optional version of parseHashFormat that doesn't throw error.
 */
std::optional<hash_format_t> parseHashFormatOpt(std::string_view hashFormatName);

/**
 * The reverse of parseHashFormat.
 */
std::string_view printHashFormat(hash_format_t hashFormat);

/**
 * Parse a string representing a hash algorithm.
 */
hash_algorithm_t
parseHashAlgo(std::string_view s,
              const experimental_feature_settings_t& xpSettings = experimentalFeatureSettings);

/**
 * Will return nothing on parse error
 */
std::optional<hash_algorithm_t>
parseHashAlgoOpt(std::string_view s,
                 const experimental_feature_settings_t& xpSettings = experimentalFeatureSettings);

/**
 * And the reverse.
 */
std::string_view printHashAlgo(hash_algorithm_t ha);

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
  void writeUnbuffered(std::string_view data) override;
  hash_result_t finish() override;
  hash_result_t currentHash();
};

template <>
struct json_avoids_null<Hash> : std::true_type {};

} // namespace nix

template <>
struct std::hash<nix::Hash> {
  std::size_t operator()(const nix::Hash& hash) const noexcept {
    assert(hash.hashSize > sizeof(size_t));
    return *reinterpret_cast<const std::size_t*>(&hash.hash);
  }
};

namespace nix {

inline std::size_t hash_value(const Hash& hash) {
  return std::hash<Hash>{}(hash);
}

} // namespace nix

JSON_IMPL_WITH_XP_FEATURES(Hash)
