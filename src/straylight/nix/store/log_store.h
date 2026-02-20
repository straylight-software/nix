#pragma once

/// @file store.h
/// @brief io_uring-native log-structured store for daemonless Nix
///
/// Design: Append-only log + materialized index
///
/// Layout:
///   /nix/var/nix/db/
///     log/                      # append-only operations log
///       0000000001.log          # sequence numbered log files
///       0000000002.log
///     index/                    # materialized view (lockless reads)
///       paths/{shard}/{hash}.meta
///       refs/{shard}/{hash}.refs
///       referrers/{shard}/{hash}.referrers
///     lock                      # flock for write serialization
///     head                      # current log sequence number
///
/// Write path (serialized via flock):
///   1. flock(lock, LOCK_EX)
///   2. Append log entry (zpp_bits serialized)
///   3. fsync log
///   4. Update index files (atomic rename per file)
///   5. funlock
///
/// Read path (lockless, io_uring):
///   - Direct reads from index/ directory
///   - No log replay needed (index always consistent)
///
/// Crash recovery:
///   - Replay log entries after last checkpoint
///   - Rebuild index from log if needed
///
/// Why this works for daemonless:
///   - flock is process-death safe (kernel releases lock)
///   - Writers serialize but reads are fully parallel
///   - No daemon needed for coordination

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "straylight/nix/data/serialise.h"

// Forward declare evring types (header included in .cpp)
namespace evring {
struct ring;
}

namespace straylight::nix::store {

// ============================================================================
// Data types
// ============================================================================

/// Path metadata (matches Nix's ValidPathInfo)
struct path_info {
  std::string path;               // /nix/store/hash-name
  std::string nar_hash;           // base16 hash of NAR
  std::int64_t registration_time; // unix timestamp
  std::string deriver;            // optional: derivation that built this
  std::int64_t nar_size;          // size of NAR in bytes
  bool ultimate;                  // built locally (not substituted)
  std::vector<std::string> sigs;  // signatures
  std::string ca;                 // content-addressability assertion
};

/// Log entry types
enum class log_op : std::uint8_t {
  register_path = 1,   // add/update path + refs
  invalidate_path = 2, // remove path (GC)
  add_derivation = 3,  // derivation output mapping
  checkpoint = 4,      // marks consistent index state
};

/// A single log entry
struct log_entry {
  log_op op;
  std::uint64_t sequence;      // monotonic sequence number
  std::int64_t timestamp;      // unix timestamp
  std::vector<std::byte> data; // op-specific payload (zpp_bits)
};

/// Payload for register_path
struct register_path_payload {
  path_info info;
  std::vector<std::string> references;
};

/// Payload for invalidate_path
struct invalidate_path_payload {
  std::string path;
};

/// Payload for add_derivation
struct add_derivation_payload {
  std::string drv_path;
  std::string output_name;
  std::string output_path;
};

// ============================================================================
// Store errors
// ============================================================================

enum class store_error : std::uint8_t {
  not_found,
  io_error,
  corrupt_data,
  already_exists,
  invalid_path,
  lock_failed,
};

template <typename T>
using store_result = std::expected<T, store_error>;

// ============================================================================
// Store
// ============================================================================

class store {
public:
  explicit store(std::filesystem::path root);
  ~store();

  // non-copyable, movable
  store(const store&) = delete;
  store& operator=(const store&) = delete;
  store(store&&) noexcept;
  store& operator=(store&&) noexcept;

  /// Initialize directory structure and recover from crash if needed
  auto init() -> store_result<void>;

  // --- Read operations (lockless, io_uring) ---

  /// Query path info by store path
  [[nodiscard]] auto query_path_info(std::string_view store_path) -> store_result<path_info>;

  /// Check if path is valid (just checks file existence)
  [[nodiscard]] auto is_valid_path(std::string_view store_path) -> bool;

  /// Get paths that this path references
  [[nodiscard]] auto query_references(std::string_view store_path)
      -> store_result<std::vector<std::string>>;

  /// Get paths that reference this path (reverse index)
  [[nodiscard]] auto query_referrers(std::string_view store_path)
      -> store_result<std::vector<std::string>>;

  /// Find derivation output path
  [[nodiscard]] auto query_derivation_output(std::string_view drv_path,
                                             std::string_view output_name)
      -> store_result<std::string>;

  /// Get all valid paths (for GC)
  [[nodiscard]] auto query_all_valid_paths() -> store_result<std::vector<std::string>>;

  // --- Write operations (flock serialized) ---

  /// Register a valid path with its references
  auto register_path(const path_info& info, std::span<const std::string> references)
      -> store_result<void>;

  /// Invalidate a path (for GC)
  auto invalidate_path(std::string_view store_path) -> store_result<void>;

  /// Register derivation output mapping
  auto add_derivation_output(std::string_view drv_path, std::string_view output_name,
                             std::string_view output_path) -> store_result<void>;

  // --- Maintenance ---

  /// Write a checkpoint marker (allows log truncation)
  auto checkpoint() -> store_result<void>;

  /// Compact: truncate log entries before last checkpoint
  auto compact() -> store_result<void>;

  /// Verify index consistency against log
  auto verify() -> store_result<bool>;

  // --- Direct access ---

  [[nodiscard]] auto root() const -> const std::filesystem::path& { return root_; }

  // --- Bulk async operations (io_uring) ---
  // These use io_uring for high-throughput parallel I/O.
  // Requires ring to be initialized.

  /// Query multiple path_infos in parallel
  [[nodiscard]] auto bulk_query_path_info(std::span<const std::string> paths)
      -> std::vector<store_result<path_info>>;

  /// Query references for multiple paths in parallel
  [[nodiscard]] auto bulk_query_references(std::span<const std::string> paths)
      -> std::vector<store_result<std::vector<std::string>>>;

  /// Check validity of multiple paths in parallel (statx-based, very fast)
  [[nodiscard]] auto bulk_is_valid_path(std::span<const std::string> paths) -> std::vector<bool>;

  /// Compute transitive closure of references
  [[nodiscard]] auto compute_closure(std::span<const std::string> start_paths)
      -> std::vector<std::string>;

  /// Get the io_uring ring (for custom operations)
  [[nodiscard]] auto ring() -> evring::ring* { return ring_.get(); }

  /// Initialize io_uring ring (called by init() automatically)
  auto init_ring(unsigned entries = 256) -> store_result<void>;

private:
  // Path helpers
  [[nodiscard]] auto hash_from_path(std::string_view store_path) const -> std::string_view;
  [[nodiscard]] auto shard_prefix(std::string_view hash) const -> std::string_view;
  [[nodiscard]] auto index_path() const -> std::filesystem::path;
  [[nodiscard]] auto log_path() const -> std::filesystem::path;
  [[nodiscard]] auto lock_path() const -> std::filesystem::path;
  [[nodiscard]] auto head_path() const -> std::filesystem::path;
  [[nodiscard]] auto meta_path(std::string_view hash) const -> std::filesystem::path;
  [[nodiscard]] auto refs_path(std::string_view hash) const -> std::filesystem::path;
  [[nodiscard]] auto referrers_path(std::string_view hash) const -> std::filesystem::path;
  [[nodiscard]] auto derivation_path(std::string_view output_name, std::string_view hash) const
      -> std::filesystem::path;

  // Log operations
  auto append_log_entry(const log_entry& entry) -> store_result<void>;
  auto read_log_entries(std::uint64_t after_sequence) -> store_result<std::vector<log_entry>>;
  auto current_sequence() -> store_result<std::uint64_t>;

  // Index operations (called while holding lock)
  auto update_index_for_register(const path_info& info, std::span<const std::string> refs)
      -> store_result<void>;
  auto update_index_for_invalidate(std::string_view store_path) -> store_result<void>;
  auto update_index_for_derivation(std::string_view drv_path, std::string_view output_name,
                                   std::string_view output_path) -> store_result<void>;

  // Atomic file write (write to .tmp, fsync, rename)
  auto atomic_write(const std::filesystem::path& path, std::span<const std::byte> data)
      -> store_result<void>;

  // Lock management
  auto acquire_lock() -> store_result<int>; // returns fd
  void release_lock(int fd);

  // Recovery
  auto recover() -> store_result<void>;
  auto replay_entry(const log_entry& entry) -> store_result<void>;

  std::filesystem::path root_;
  std::unique_ptr<evring::ring> ring_;
};

// ============================================================================
// Serialization
// ============================================================================

// Serialize references as newline-separated hashes (simple, human-readable)
auto serialize_refs(std::span<const std::string> refs) -> std::vector<std::byte>;
auto deserialize_refs(std::span<const std::byte> data) -> std::vector<std::string>;

#if STRAYLIGHT_HAS_ZPP_BITS

// High-performance serialization using zpp_bits

inline auto serialize_path_info(const path_info& info) -> std::vector<std::byte> {
  return straylight::nix::data::serialize(info);
}

inline auto deserialize_path_info(std::span<const std::byte> data) -> store_result<path_info> {
  try {
    return straylight::nix::data::deserialize<path_info>(data);
  } catch (const straylight::nix::data::SerialisationError&) {
    return std::unexpected(store_error::corrupt_data);
  }
}

// Log entry serialization
auto serialize_log_entry(const log_entry& entry) -> std::vector<std::byte>;
auto deserialize_log_entry(std::span<const std::byte> data) -> store_result<log_entry>;

} // namespace straylight::nix::store

// ============================================================================
// zpp_bits reflection for our types
// ============================================================================

namespace zpp::bits {

// path_info serialization
template <>
constexpr auto serialize(auto& archive, const straylight::nix::store::path_info& info) {
  return archive(info.path, info.nar_hash, info.registration_time, info.deriver, info.nar_size,
                 info.ultimate, info.sigs, info.ca);
}

template <>
constexpr auto serialize(auto& archive, straylight::nix::store::path_info& info) {
  return archive(info.path, info.nar_hash, info.registration_time, info.deriver, info.nar_size,
                 info.ultimate, info.sigs, info.ca);
}

// register_path_payload
template <>
constexpr auto serialize(auto& archive,
                         const straylight::nix::store::register_path_payload& payload) {
  return archive(payload.info, payload.references);
}

template <>
constexpr auto serialize(auto& archive, straylight::nix::store::register_path_payload& payload) {
  return archive(payload.info, payload.references);
}

// invalidate_path_payload
template <>
constexpr auto serialize(auto& archive,
                         const straylight::nix::store::invalidate_path_payload& payload) {
  return archive(payload.path);
}

template <>
constexpr auto serialize(auto& archive, straylight::nix::store::invalidate_path_payload& payload) {
  return archive(payload.path);
}

// add_derivation_payload
template <>
constexpr auto serialize(auto& archive,
                         const straylight::nix::store::add_derivation_payload& payload) {
  return archive(payload.drv_path, payload.output_name, payload.output_path);
}

template <>
constexpr auto serialize(auto& archive, straylight::nix::store::add_derivation_payload& payload) {
  return archive(payload.drv_path, payload.output_name, payload.output_path);
}

// log_entry
template <>
constexpr auto serialize(auto& archive, const straylight::nix::store::log_entry& entry) {
  return archive(entry.op, entry.sequence, entry.timestamp, entry.data);
}

template <>
constexpr auto serialize(auto& archive, straylight::nix::store::log_entry& entry) {
  return archive(entry.op, entry.sequence, entry.timestamp, entry.data);
}

} // namespace zpp::bits

#else // !STRAYLIGHT_HAS_ZPP_BITS

// ============================================================================
// Fallback serialization (simple wire format, no zpp_bits)
// ============================================================================
// Format: length-prefixed strings using our existing Source/Sink helpers

/// Serialize path_info to bytes (fallback)
auto serialize_path_info(const path_info& info) -> std::vector<std::byte>;

/// Deserialize path_info from bytes (fallback)
auto deserialize_path_info(std::span<const std::byte> data) -> store_result<path_info>;

/// Serialize log entry (fallback)
auto serialize_log_entry(const log_entry& entry) -> std::vector<std::byte>;

/// Deserialize log entry (fallback)
auto deserialize_log_entry(std::span<const std::byte> data) -> store_result<log_entry>;

} // namespace straylight::nix::store

#endif // STRAYLIGHT_HAS_ZPP_BITS
