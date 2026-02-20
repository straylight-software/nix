// straylight::nix::primitives::legacy_store
//
// SQLite-based store for input-addressed paths (Nix1 compatibility).
//
// This is the "legacy tier" of the two-tier store architecture.
// Uses SQLite in WAL mode with flock for daemonless coordination.
//
// Design:
//   - SQLite WAL mode for concurrent reads during writes
//   - flock() for write serialization (no daemon needed)
//   - Compatible schema with Nix1 (ValidPaths, Refs, DerivationOutputs)
//   - RAII lock acquisition (process death releases lock)
//
// Why SQLite for legacy:
//   - Input-addressed paths require coordination (unlike CA)
//   - SQLite provides ACID guarantees out of the box
//   - WAL mode enables concurrent readers with a single writer
//   - Existing Nix stores use this schema
//
// Why no daemon:
//   - flock() is kernel-managed and process-death safe
//   - WAL mode handles reader/writer concurrency
//   - Lock contention is rare in practice

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "straylight/nix/compat/sqlite.h"
#include "straylight/nix/sync/lock.h"

namespace straylight::nix::store {

// ============================================================================
// Legacy store errors
// ============================================================================

enum class legacy_error : std::uint8_t {
  not_found,
  io_error,
  database_error,
  corrupt_data,
  already_exists,
  invalid_path,
  lock_failed,
};

template <typename T>
using legacy_result = std::expected<T, legacy_error>;

// ============================================================================
// Path info (same as store.h for compatibility)
// ============================================================================

/// Path metadata (compatible with Nix's ValidPathInfo)
struct legacy_path_info {
  std::string path;               // /nix/store/hash-name
  std::string nar_hash;           // base16 hash of NAR
  std::int64_t registration_time; // unix timestamp
  std::string deriver;            // optional: derivation that built this
  std::int64_t nar_size;          // size of NAR in bytes
  bool ultimate;                  // built locally (not substituted)
  std::vector<std::string> sigs;  // signatures
  std::string ca;                 // content-addressability assertion
};

// ============================================================================
// legacy_store - SQLite-based store for input-addressed paths
// ============================================================================

/// SQLite-based store for input-addressed paths.
///
/// Thread-safety: Write operations are serialized via flock.
/// Read operations can proceed concurrently (SQLite WAL mode).
///
/// Usage:
///   legacy_store store("/nix/var/nix/db");
///   store.init();
///
///   // Register a path (acquires write lock)
///   store.register_path(info, refs);
///
///   // Query a path (no lock needed)
///   auto info = store.query_path_info("/nix/store/...");
///
class legacy_store {
public:
  /// Create a store at the given root directory.
  /// The database file will be at {root}/db.sqlite
  explicit legacy_store(std::filesystem::path root);

  ~legacy_store();

  // Non-copyable, movable
  legacy_store(const legacy_store&) = delete;
  auto operator=(const legacy_store&) -> legacy_store& = delete;
  legacy_store(legacy_store&&) noexcept;
  auto operator=(legacy_store&&) noexcept -> legacy_store&;

  // --------------------------------------------------------------------------
  // Initialization
  // --------------------------------------------------------------------------

  /// Initialize the database.
  /// Creates schema if needed, enables WAL mode.
  [[nodiscard]] auto init() -> legacy_result<void>;

  // --------------------------------------------------------------------------
  // Read operations (no lock required)
  // --------------------------------------------------------------------------

  /// Query path info by store path
  [[nodiscard]] auto query_path_info(std::string_view store_path)
      -> legacy_result<legacy_path_info>;

  /// Check if path is valid
  [[nodiscard]] auto is_valid_path(std::string_view store_path) -> bool;

  /// Get paths that this path references
  [[nodiscard]] auto query_references(std::string_view store_path)
      -> legacy_result<std::vector<std::string>>;

  /// Get paths that reference this path (reverse index)
  [[nodiscard]] auto query_referrers(std::string_view store_path)
      -> legacy_result<std::vector<std::string>>;

  /// Find derivation output path
  [[nodiscard]] auto query_derivation_output(std::string_view drv_path,
                                             std::string_view output_name)
      -> legacy_result<std::string>;

  /// Get all valid paths
  [[nodiscard]] auto query_all_valid_paths() -> legacy_result<std::vector<std::string>>;

  /// Get count of valid paths
  [[nodiscard]] auto count() -> legacy_result<std::size_t>;

  // --------------------------------------------------------------------------
  // Write operations (acquire write lock)
  // --------------------------------------------------------------------------

  /// Register a valid path with its references.
  /// Acquires exclusive lock during operation.
  [[nodiscard]] auto register_path(const legacy_path_info& info,
                                   std::span<const std::string> references) -> legacy_result<void>;

  /// Invalidate a path (for GC).
  /// Acquires exclusive lock during operation.
  [[nodiscard]] auto invalidate_path(std::string_view store_path) -> legacy_result<void>;

  /// Register derivation output mapping.
  /// Acquires exclusive lock during operation.
  [[nodiscard]] auto add_derivation_output(std::string_view drv_path, std::string_view output_name,
                                           std::string_view output_path) -> legacy_result<void>;

  // --------------------------------------------------------------------------
  // Maintenance
  // --------------------------------------------------------------------------

  /// Verify database integrity.
  [[nodiscard]] auto verify() -> legacy_result<bool>;

  /// Vacuum the database to reclaim space.
  [[nodiscard]] auto vacuum() -> legacy_result<void>;

  // --------------------------------------------------------------------------
  // Direct access
  // --------------------------------------------------------------------------

  /// Get the root directory.
  [[nodiscard]] auto root() const -> const std::filesystem::path& { return root_; }

  /// Get the database path.
  [[nodiscard]] auto db_path() const -> std::filesystem::path { return root_ / "db.sqlite"; }

  /// Get the lock file path.
  [[nodiscard]] auto lock_path() const -> std::filesystem::path { return root_ / "db.lock"; }

private:
  // Internal operations
  [[nodiscard]] auto ensure_db() -> legacy_result<void>;
  [[nodiscard]] auto create_schema() -> legacy_result<void>;
  [[nodiscard]] auto get_path_id(std::string_view store_path) -> legacy_result<std::int64_t>;

  // Acquire exclusive lock for write operations
  [[nodiscard]] auto acquire_write_lock() -> legacy_result<straylight::nix::sync::exclusive_lock>;

  // Serialize signatures for storage
  [[nodiscard]] static auto serialize_sigs(const std::vector<std::string>& sigs) -> std::string;
  [[nodiscard]] static auto deserialize_sigs(std::string_view data) -> std::vector<std::string>;

  std::filesystem::path root_;
  std::unique_ptr<straylight::nix::compat::Database> db_;
};

} // namespace straylight::nix::store
