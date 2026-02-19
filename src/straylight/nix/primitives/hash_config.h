// straylight::nix::primitives::hash_config
//
// Configuration for hash function backends and thresholds.
//
// Configuration options (via #define before including, or compiler flags):
//   STRAYLIGHT_HASH_FORCE_OPENSSL=1  - Always use OpenSSL (even for BLAKE3)
//   STRAYLIGHT_HASH_FORCE_BLAKE3=1   - Always use BLAKE3 (for testing)
//   STRAYLIGHT_HASH_BLAKE3_PARALLEL_THRESHOLD - Size threshold for parallel BLAKE3

#pragma once

#include <cstddef>

namespace straylight::nix::primitives::hash_config {

// ─────────────────────────────────────────────────────────────────────────────
// BLAKE3 parallel hashing threshold
//
// BLAKE3's official implementation supports multithreaded hashing for large
// inputs. This threshold determines when to use parallel mode.
//
// Default: 128KB (based on BLAKE3 official recommendations for x86_64)
// ─────────────────────────────────────────────────────────────────────────────

inline constexpr std::size_t blake3_parallel_threshold_default = 131072; // 128KB

#ifdef STRAYLIGHT_HASH_BLAKE3_PARALLEL_THRESHOLD
inline constexpr std::size_t blake3_parallel_threshold = STRAYLIGHT_HASH_BLAKE3_PARALLEL_THRESHOLD;
#else
inline constexpr std::size_t blake3_parallel_threshold = blake3_parallel_threshold_default;
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Backend selection helpers
// ─────────────────────────────────────────────────────────────────────────────

/// Check if parallel BLAKE3 should be used for given data size
[[nodiscard]] constexpr bool use_blake3_parallel(std::size_t size) noexcept {
  return size >= blake3_parallel_threshold;
}

// ─────────────────────────────────────────────────────────────────────────────
// Hash output sizes (in bytes)
// ─────────────────────────────────────────────────────────────────────────────

inline constexpr std::size_t md5_size = 16;    // 128 bits
inline constexpr std::size_t sha1_size = 20;   // 160 bits
inline constexpr std::size_t sha256_size = 32; // 256 bits
inline constexpr std::size_t sha512_size = 64; // 512 bits
inline constexpr std::size_t blake3_size = 32; // 256 bits (default, can be extended)

inline constexpr std::size_t max_hash_size = 64; // Maximum hash output size

// ─────────────────────────────────────────────────────────────────────────────
// Nix store path hash size
//
// Store paths use SHA256 compressed to 160 bits (20 bytes) for shorter paths.
// Encoded in Nix32 format, this produces 32-character hashes.
// ─────────────────────────────────────────────────────────────────────────────

inline constexpr std::size_t store_path_hash_size = 20; // 160 bits

} // namespace straylight::nix::primitives::hash_config
