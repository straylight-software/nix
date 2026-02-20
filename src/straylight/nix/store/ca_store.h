// straylight::nix::primitives::ca_store
//
// Content-addressed blob store.
//
// This is the "trivial tier" of the two-tier store architecture.
// Content-addressing eliminates coordination problems entirely:
// - hash(content) → content means races are benign
// - Two writers produce identical results
// - No locks, no daemon, no coordination
//
// Design:
//   Store layout: {root}/{shard}/{hash}
//   Shard = first 2 chars of hash (256 directories)
//   Files are write-once, read-many
//
// Operations:
//   put(data) → hash         # Idempotent write
//   get(hash) → data         # Direct read
//   has(hash) → bool         # Existence check
//   remove(hash) → bool      # GC cleanup
//
// Crash safety:
//   - Writes use atomic rename (write .tmp, fsync, rename)
//   - Incomplete writes leave .tmp files (cleaned on startup)
//   - No WAL or journal needed - content is self-validating
//
// Performance:
//   - Lockless reads (just open/read/close)
//   - Lockless writes (atomic rename is idempotent)
//   - io_uring compatible (statx + read)
//   - O(1) lookup (hash → file path)

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "straylight/nix/crypto/hash.h"

namespace hash = straylight::nix::crypto;

namespace straylight::nix::store {

// ============================================================================
// CA Store errors
// ============================================================================

enum class ca_error : std::uint8_t {
  not_found,
  io_error,
  hash_mismatch, // Content doesn't match hash (corruption)
  invalid_hash,  // Malformed hash string
};

template <typename T>
using ca_result = std::expected<T, ca_error>;

// ============================================================================
// ca_store - Content-addressed blob storage
// ============================================================================

/// Content-addressed blob store.
///
/// Thread-safety: All operations are thread-safe and process-safe.
/// No locking required - content-addressing makes writes idempotent.
///
/// Usage:
///   ca_store store("/nix/var/nix/ca");
///   store.init();
///
///   // Store a blob
///   auto hash = store.put(my_data);
///
///   // Retrieve it
///   auto data = store.get(*hash);
///
///   // Verify integrity
///   store.verify_all();
///
class ca_store {
public:
  /// Hash algorithm used for content addressing (BLAKE3 - 256-bit, fastest)
  static constexpr hash::Algorithm hash_algorithm = hash::Algorithm::BLAKE3;

  /// Size of hash output in bytes
  static constexpr std::size_t hash_size = 32;

  /// Size of hex-encoded hash string
  static constexpr std::size_t hash_hex_size = hash_size * 2;

  /// Create a store at the given root directory
  explicit ca_store(std::filesystem::path root);

  ~ca_store() = default;

  // Non-copyable, movable
  ca_store(const ca_store&) = delete;
  auto operator=(const ca_store&) -> ca_store& = delete;
  ca_store(ca_store&&) noexcept = default;
  auto operator=(ca_store&&) noexcept -> ca_store& = default;

  // --------------------------------------------------------------------------
  // Initialization
  // --------------------------------------------------------------------------

  /// Initialize the store directory structure.
  /// Creates shard directories and cleans up any incomplete writes.
  [[nodiscard]] auto init() -> ca_result<void>;

  // --------------------------------------------------------------------------
  // Core operations
  // --------------------------------------------------------------------------

  /// Store content and return its hash.
  ///
  /// This is idempotent: storing the same content twice has no effect.
  /// Concurrent puts of the same content are safe (last rename wins,
  /// but all produce identical files).
  ///
  /// @param data  Content to store
  /// @return Hash of the content (hex-encoded BLAKE3)
  [[nodiscard]] auto put(std::span<const std::byte> data) -> ca_result<std::string>;

  /// Store content with a pre-computed hash.
  ///
  /// @param hash  Expected hash (hex-encoded)
  /// @param data  Content to store
  /// @return Success if content matches hash, hash_mismatch otherwise
  [[nodiscard]] auto put(std::string_view hash, std::span<const std::byte> data) -> ca_result<void>;

  /// Retrieve content by hash.
  ///
  /// @param hash  Content hash (hex-encoded)
  /// @return Content bytes, or not_found if missing
  [[nodiscard]] auto get(std::string_view hash) -> ca_result<std::vector<std::byte>>;

  /// Check if content exists.
  ///
  /// @param hash  Content hash (hex-encoded)
  /// @return true if content exists
  [[nodiscard]] auto has(std::string_view hash) const -> bool;

  /// Remove content by hash.
  ///
  /// @param hash  Content hash (hex-encoded)
  /// @return true if removed, false if didn't exist
  [[nodiscard]] auto remove(std::string_view hash) -> ca_result<bool>;

  // --------------------------------------------------------------------------
  // Bulk operations (io_uring accelerated)
  // --------------------------------------------------------------------------

  /// Check existence of multiple hashes in parallel.
  [[nodiscard]] auto bulk_has(std::span<const std::string> hashes) -> std::vector<bool>;

  /// Retrieve multiple blobs in parallel.
  [[nodiscard]] auto bulk_get(std::span<const std::string> hashes)
      -> std::vector<ca_result<std::vector<std::byte>>>;

  // --------------------------------------------------------------------------
  // Maintenance
  // --------------------------------------------------------------------------

  /// Verify all stored content matches its hash.
  ///
  /// @return Number of corrupt entries found (0 = all good)
  [[nodiscard]] auto verify_all() -> ca_result<std::size_t>;

  /// Verify a single blob.
  [[nodiscard]] auto verify(std::string_view hash) -> ca_result<bool>;

  /// Clean up incomplete writes (.tmp files).
  [[nodiscard]] auto cleanup_temps() -> ca_result<std::size_t>;

  /// Get total size of all stored content.
  [[nodiscard]] auto total_size() -> ca_result<std::uint64_t>;

  /// Get count of stored blobs.
  [[nodiscard]] auto count() -> ca_result<std::size_t>;

  /// List all stored hashes.
  [[nodiscard]] auto list_all() -> ca_result<std::vector<std::string>>;

  // --------------------------------------------------------------------------
  // Direct access
  // --------------------------------------------------------------------------

  /// Get the root directory.
  [[nodiscard]] auto root() const -> const std::filesystem::path& { return root_; }

  /// Get path to a blob by hash (even if it doesn't exist).
  [[nodiscard]] auto blob_path(std::string_view hash) const -> std::filesystem::path;

private:
  // Path helpers
  [[nodiscard]] auto shard_path(std::string_view hash) const -> std::filesystem::path;
  [[nodiscard]] auto temp_path(std::string_view hash) const -> std::filesystem::path;

  // Internal operations
  [[nodiscard]] auto write_blob(std::string_view hash, std::span<const std::byte> data)
      -> ca_result<void>;
  [[nodiscard]] auto read_blob(std::string_view hash) -> ca_result<std::vector<std::byte>>;

  // Validate hash format
  [[nodiscard]] static auto is_valid_hash(std::string_view hash) -> bool;

  std::filesystem::path root_;
};

// ============================================================================
// Implementation
// ============================================================================

inline ca_store::ca_store(std::filesystem::path root) : root_(std::move(root)) {}

inline auto ca_store::is_valid_hash(std::string_view hash) -> bool {
  if (hash.size() != hash_hex_size) {
    return false;
  }
  for (char c : hash) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

inline auto ca_store::shard_path(std::string_view hash) const -> std::filesystem::path {
  // First 2 characters of hash determine shard
  return root_ / std::string(hash.substr(0, 2));
}

inline auto ca_store::blob_path(std::string_view hash) const -> std::filesystem::path {
  return shard_path(hash) / std::string(hash);
}

inline auto ca_store::temp_path(std::string_view hash) const -> std::filesystem::path {
  auto path = blob_path(hash);
  path += ".tmp";
  return path;
}

inline auto ca_store::has(std::string_view hash) const -> bool {
  if (!is_valid_hash(hash)) {
    return false;
  }
  std::error_code ec;
  return std::filesystem::exists(blob_path(hash), ec) && !ec;
}

} // namespace straylight::nix::store
