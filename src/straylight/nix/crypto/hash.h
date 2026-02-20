// straylight::nix::crypto
//
// High-performance cryptographic hash functions with SIMD acceleration.
//
// Supported algorithms:
//   - BLAKE3 (256-bit, AVX-512/AVX2 accelerated via official C library)
//   - SHA256 (256-bit, SHA-NI accelerated via OpenSSL)
//   - SHA512 (512-bit, via OpenSSL)
//   - SHA1   (160-bit, legacy, via OpenSSL)
//   - MD5    (128-bit, legacy/insecure, via OpenSSL)
//
// Usage:
//   auto h = hash::sha256("hello world");
//   auto h = hash::blake3(data);
//   auto hex = h.to_hex();
//   auto b64 = h.to_base64();
//
// Streaming:
//   hash::Hasher hasher(hash::Algorithm::SHA256);
//   hasher.update(chunk1);
//   hasher.update(chunk2);
//   auto h = hasher.finish();

#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "hash_config.h"

namespace straylight::nix::crypto {

// ─────────────────────────────────────────────────────────────────────────────
// Hash algorithm enumeration
// ─────────────────────────────────────────────────────────────────────────────

enum class Algorithm : uint8_t {
  MD5,    // 128 bits - insecure, legacy only
  SHA1,   // 160 bits - weak, legacy/git compatibility
  SHA256, // 256 bits - primary algorithm for nix
  SHA512, // 512 bits - rarely used
  BLAKE3, // 256 bits - modern, fastest
};

/// Get hash output size in bytes for algorithm
[[nodiscard]] constexpr std::size_t hash_size(Algorithm algo) noexcept {
  switch (algo) {
    case Algorithm::MD5:
      return md5_size;
    case Algorithm::SHA1:
      return sha1_size;
    case Algorithm::SHA256:
      return sha256_size;
    case Algorithm::SHA512:
      return sha512_size;
    case Algorithm::BLAKE3:
      return blake3_size;
  }
  return 0; // unreachable
}

/// Get algorithm name as string
[[nodiscard]] constexpr std::string_view algorithm_name(Algorithm algo) noexcept {
  switch (algo) {
    case Algorithm::MD5:
      return "md5";
    case Algorithm::SHA1:
      return "sha1";
    case Algorithm::SHA256:
      return "sha256";
    case Algorithm::SHA512:
      return "sha512";
    case Algorithm::BLAKE3:
      return "blake3";
  }
  return "unknown";
}

// ─────────────────────────────────────────────────────────────────────────────
// Hash result type
// ─────────────────────────────────────────────────────────────────────────────

/// Fixed-size hash result
/// Stores the hash bytes and provides encoding methods.
class Hash {
public:
  static constexpr std::size_t max_size = max_hash_size;

private:
  std::array<uint8_t, max_size> bytes_{};
  std::size_t size_ = 0;
  Algorithm algo_ = Algorithm::SHA256;

public:
  /// Default constructor (empty hash)
  constexpr Hash() noexcept = default;

  /// Construct with algorithm (zeroed bytes)
  explicit constexpr Hash(Algorithm algo) noexcept : size_(hash_size(algo)), algo_(algo) {}

  /// Construct from raw bytes
  Hash(Algorithm algo, std::span<const uint8_t> data) noexcept;

  /// Algorithm used
  [[nodiscard]] constexpr Algorithm algorithm() const noexcept { return algo_; }

  /// Hash size in bytes
  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }

  /// Raw hash bytes
  [[nodiscard]] std::span<const uint8_t> bytes() const noexcept {
    return std::span<const uint8_t>(bytes_.data(), size_);
  }

  /// Access raw data pointer
  [[nodiscard]] const uint8_t* data() const noexcept { return bytes_.data(); }

  /// Mutable access to bytes (for internal use)
  [[nodiscard]] uint8_t* data() noexcept { return bytes_.data(); }

  // ─────────────────────────────────────────────────────────────────────────
  // Encoding methods
  // ─────────────────────────────────────────────────────────────────────────

  /// Encode as lowercase hexadecimal (base16)
  [[nodiscard]] std::string to_hex() const;

  /// Encode as base64 (RFC 4648)
  [[nodiscard]] std::string to_base64() const;

  /// Encode as Nix base32 (omits e, o, u, t)
  [[nodiscard]] std::string to_nix32() const;

  /// Encode as SRI format: "<algo>-<base64>"
  [[nodiscard]] std::string to_sri() const;

  // ─────────────────────────────────────────────────────────────────────────
  // Parsing (static constructors)
  // ─────────────────────────────────────────────────────────────────────────

  /// Parse from hexadecimal string
  [[nodiscard]] static Hash from_hex(Algorithm algo, std::string_view hex);

  /// Parse from base64 string
  [[nodiscard]] static Hash from_base64(Algorithm algo, std::string_view b64);

  /// Parse from Nix base32 string
  [[nodiscard]] static Hash from_nix32(Algorithm algo, std::string_view nix32);

  /// Parse from SRI format (algorithm auto-detected)
  [[nodiscard]] static Hash from_sri(std::string_view sri);

  // ─────────────────────────────────────────────────────────────────────────
  // Comparison
  // ─────────────────────────────────────────────────────────────────────────

  [[nodiscard]] bool operator==(const Hash& other) const noexcept;
  [[nodiscard]] bool operator!=(const Hash& other) const noexcept { return !(*this == other); }
  [[nodiscard]] std::strong_ordering operator<=>(const Hash& other) const noexcept;

  // ─────────────────────────────────────────────────────────────────────────
  // Utilities
  // ─────────────────────────────────────────────────────────────────────────

  /// XOR-compress hash to smaller size (used for store paths)
  [[nodiscard]] Hash compress(std::size_t new_size) const;

  /// Check if hash is all zeros
  [[nodiscard]] bool is_zero() const noexcept;
};

// ─────────────────────────────────────────────────────────────────────────────
// Streaming hasher
// ─────────────────────────────────────────────────────────────────────────────

/// Streaming hash computation
/// Allows incremental hashing of large data.
class Hasher {
public:
  explicit Hasher(Algorithm algo);
  ~Hasher();

  // Non-copyable
  Hasher(const Hasher&) = delete;
  Hasher& operator=(const Hasher&) = delete;

  // Movable
  Hasher(Hasher&& other) noexcept;
  Hasher& operator=(Hasher&& other) noexcept;

  /// Add data to hash
  void update(std::string_view data);
  void update(std::span<const uint8_t> data);

  /// Finalize and return hash (consumes the hasher)
  [[nodiscard]] Hash finish();

  /// Get current hash without finalizing (for progress tracking)
  [[nodiscard]] Hash current() const;

  /// Total bytes hashed so far
  [[nodiscard]] uint64_t bytes_hashed() const noexcept { return bytes_hashed_; }

  /// Algorithm being used
  [[nodiscard]] Algorithm algorithm() const noexcept { return algo_; }

private:
  Algorithm algo_;
  void* ctx_ = nullptr; // Opaque context (BLAKE3 or OpenSSL)
  uint64_t bytes_hashed_ = 0;

  void init();
  void cleanup();
};

// ─────────────────────────────────────────────────────────────────────────────
// One-shot hashing functions
// ─────────────────────────────────────────────────────────────────────────────

/// Hash string/bytes with specified algorithm
[[nodiscard]] Hash compute(Algorithm algo, std::string_view data);
[[nodiscard]] Hash compute(Algorithm algo, std::span<const uint8_t> data);

/// Convenience functions for specific algorithms
[[nodiscard]] inline Hash blake3(std::string_view data) {
  return compute(Algorithm::BLAKE3, data);
}
[[nodiscard]] inline Hash sha256(std::string_view data) {
  return compute(Algorithm::SHA256, data);
}
[[nodiscard]] inline Hash sha512(std::string_view data) {
  return compute(Algorithm::SHA512, data);
}
[[nodiscard]] inline Hash sha1(std::string_view data) {
  return compute(Algorithm::SHA1, data);
}
[[nodiscard]] inline Hash md5(std::string_view data) {
  return compute(Algorithm::MD5, data);
}

/// Hash with span overloads
[[nodiscard]] inline Hash blake3(std::span<const uint8_t> data) {
  return compute(Algorithm::BLAKE3, data);
}
[[nodiscard]] inline Hash sha256(std::span<const uint8_t> data) {
  return compute(Algorithm::SHA256, data);
}
[[nodiscard]] inline Hash sha512(std::span<const uint8_t> data) {
  return compute(Algorithm::SHA512, data);
}
[[nodiscard]] inline Hash sha1(std::span<const uint8_t> data) {
  return compute(Algorithm::SHA1, data);
}
[[nodiscard]] inline Hash md5(std::span<const uint8_t> data) {
  return compute(Algorithm::MD5, data);
}

// ─────────────────────────────────────────────────────────────────────────────
// Store path hash helper
//
// Nix store paths use SHA256 compressed to 160 bits, encoded in Nix32.
// ─────────────────────────────────────────────────────────────────────────────

/// Compute store path hash (SHA256 compressed to 160 bits)
[[nodiscard]] inline Hash store_path_hash(std::string_view data) {
  return sha256(data).compress(store_path_hash_size);
}

} // namespace straylight::nix::crypto
