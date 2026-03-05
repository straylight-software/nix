// straylight::nix::primitives::two_tier_store
//
// Unified two-tier store architecture.
//
// This is the "bulletproof" daemonless store combining:
//   - CA tier: Content-addressed blob storage (coordination-free)
//   - Legacy tier: SQLite for input-addressed paths (flock coordination)
//
// Dispatch rules:
//   - Paths with CA assertions (ca: field) → ca_store
//   - Input-addressed paths → legacy_store
//
// Benefits:
//   - No daemon required (kernel flock + atomic rename)
//   - CA paths are fully parallel (no coordination)
//   - Legacy paths use SQLite ACID guarantees
//   - Crash-safe (WAL + atomic rename)
//   - Process-death safe (kernel releases flock)

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "straylight/nix/store/ca_store.h"
#include "straylight/nix/store/legacy_store.h"

namespace straylight::nix::store {

// ============================================================================
// Two-tier store errors
// ============================================================================

enum class store_tier_error : std::uint8_t {
  not_found,
  io_error,
  database_error,
  corrupt_data,
  already_exists,
  invalid_path,
  lock_failed,
  hash_mismatch,
};

template <typename T>
using tier_result = std::expected<T, store_tier_error>;

// ============================================================================
// two_tier_store - Unified CA + Legacy store
// ============================================================================

/// Unified two-tier store.
///
/// Automatically dispatches operations to the appropriate tier:
/// - Content-addressed paths → ca_store (lockless)
/// - Input-addressed paths → legacy_store (flock + SQLite)
///
/// Usage:
///   two_tier_store store("/nix/var/nix");
///   store.init();
///
///   // Register a CA path (goes to ca_store)
///   legacy_path_info info;
///   info.path = "/nix/store/abc123-foo";
///   info.ca = "fixed:sha256:...";
///   store.register_path(info, {});
///
///   // Register an input-addressed path (goes to legacy_store)
///   legacy_path_info info2;
///   info2.path = "/nix/store/def456-bar";
///   store.register_path(info2, {"/nix/store/abc123-foo"});
///
class two_tier_store {
public:
  /// Create a two-tier store at the given root directory.
  ///
  /// Layout:
  ///   {root}/
  ///     ca/         # Content-addressed blobs
  ///     db/         # SQLite database for input-addressed
  ///       db.sqlite
  ///       db.lock
  ///
  explicit two_tier_store(std::filesystem::path root);

  ~two_tier_store();

  // Non-copyable, movable
  two_tier_store(const two_tier_store&) = delete;
  auto operator=(const two_tier_store&) -> two_tier_store& = delete;
  two_tier_store(two_tier_store&&) noexcept;
  auto operator=(two_tier_store&&) noexcept -> two_tier_store&;

  // --------------------------------------------------------------------------
  // Initialization
  // --------------------------------------------------------------------------

  /// Initialize both tiers.
  [[nodiscard]] auto init() -> tier_result<void>;

  // --------------------------------------------------------------------------
  // Read operations
  // --------------------------------------------------------------------------

  /// Query path info by store path.
  /// Checks legacy store (CA metadata is stored there).
  [[nodiscard]] auto query_path_info(std::string_view store_path) -> tier_result<legacy_path_info>;

  /// Check if path is valid.
  [[nodiscard]] auto is_valid_path(std::string_view store_path) -> bool;

  /// Get paths that this path references.
  [[nodiscard]] auto query_references(std::string_view store_path)
      -> tier_result<std::vector<std::string>>;

  /// Get paths that reference this path.
  [[nodiscard]] auto query_referrers(std::string_view store_path)
      -> tier_result<std::vector<std::string>>;

  /// Find derivation output path.
  [[nodiscard]] auto query_derivation_output(std::string_view drv_path,
                                             std::string_view output_name)
      -> tier_result<std::string>;

  /// Get all valid paths.
  [[nodiscard]] auto query_all_valid_paths() -> tier_result<std::vector<std::string>>;

  // --------------------------------------------------------------------------
  // Write operations
  // --------------------------------------------------------------------------

  /// Register a valid path with its references.
  ///
  /// If info.ca is set, the path content is stored in ca_store.
  /// Metadata is always stored in legacy_store.
  [[nodiscard]] auto register_path(const legacy_path_info& info,
                                   std::span<const std::string> references) -> tier_result<void>;

  /// Invalidate a path (for GC).
  [[nodiscard]] auto invalidate_path(std::string_view store_path) -> tier_result<void>;

  /// Register derivation output mapping.
  [[nodiscard]] auto add_derivation_output(std::string_view drv_path, std::string_view output_name,
                                           std::string_view output_path) -> tier_result<void>;

  // --------------------------------------------------------------------------
  // CA-specific operations
  // --------------------------------------------------------------------------

  /// Store content and return its hash.
  [[nodiscard]] auto put_ca(std::span<const std::byte> data) -> tier_result<std::string>;

  /// Retrieve content by hash.
  [[nodiscard]] auto get_ca(std::string_view hash) -> tier_result<std::vector<std::byte>>;

  /// Check if CA content exists.
  [[nodiscard]] auto has_ca(std::string_view hash) const -> bool;

  // --------------------------------------------------------------------------
  // Maintenance
  // --------------------------------------------------------------------------

  /// Verify both tiers.
  [[nodiscard]] auto verify() -> tier_result<bool>;

  /// Vacuum the legacy store.
  [[nodiscard]] auto vacuum() -> tier_result<void>;

  // --------------------------------------------------------------------------
  // Direct access
  // --------------------------------------------------------------------------

  /// Get the root directory.
  [[nodiscard]] auto root() const -> const std::filesystem::path& { return root_; }

  /// Get the CA store.
  [[nodiscard]] auto ca() -> ca_store& { return *ca_; }
  [[nodiscard]] auto ca() const -> const ca_store& { return *ca_; }

  /// Get the legacy store.
  [[nodiscard]] auto legacy() -> legacy_store& { return *legacy_; }
  [[nodiscard]] auto legacy() const -> const legacy_store& { return *legacy_; }

private:
  /// Check if a path is content-addressed (has ca: field).
  [[nodiscard]] static auto is_ca_path(const legacy_path_info& info) -> bool;

  /// Convert legacy errors to tier errors.
  [[nodiscard]] static auto convert_legacy_error(legacy_error e) -> store_tier_error;

  /// Convert CA errors to tier errors.
  [[nodiscard]] static auto convert_ca_error(ca_error e) -> store_tier_error;

  std::filesystem::path root_;
  std::unique_ptr<ca_store> ca_;
  std::unique_ptr<legacy_store> legacy_;
};

// ============================================================================
// Inline implementation
// ============================================================================

inline two_tier_store::two_tier_store(std::filesystem::path root)
    : root_(std::move(root)),
      ca_(std::make_unique<ca_store>(root_ / "ca")),
      legacy_(std::make_unique<legacy_store>(root_ / "db")) {}

inline two_tier_store::~two_tier_store() = default;

inline two_tier_store::two_tier_store(two_tier_store&&) noexcept = default;
inline auto two_tier_store::operator=(two_tier_store&&) noexcept -> two_tier_store& = default;

inline auto two_tier_store::is_ca_path(const legacy_path_info& info) -> bool {
  return !info.ca.empty();
}

inline auto two_tier_store::convert_legacy_error(legacy_error error) -> store_tier_error {
  switch (error) {
  case legacy_error::not_found: {
      return store_tier_error::not_found;
  } break;

  case legacy_error::io_error: {
      return store_tier_error::io_error;
  } break;

  case legacy_error::database_error: {
      return store_tier_error::database_error;
  } break;

  case legacy_error::corrupt_data: {
      return store_tier_error::corrupt_data;
  } break;

  case legacy_error::already_exists: {
      return store_tier_error::already_exists;
  } break;

  case legacy_error::invalid_path: {
      return store_tier_error::invalid_path;
  } break;

  case legacy_error::lock_failed: {
      return store_tier_error::lock_failed;
  } break;
  }

  return store_tier_error::io_error;
}

inline auto two_tier_store::convert_ca_error(ca_error e) -> store_tier_error {
  switch (e) {
    case ca_error::not_found:
      return store_tier_error::not_found;
    case ca_error::io_error:
      return store_tier_error::io_error;
    case ca_error::hash_mismatch:
      return store_tier_error::hash_mismatch;
    case ca_error::invalid_hash:
      return store_tier_error::invalid_path;
  }
  return store_tier_error::io_error;
}

inline auto two_tier_store::init() -> tier_result<void> {
  auto ca_result = ca_->init();
  if (!ca_result) {
    return std::unexpected(convert_ca_error(ca_result.error()));
  }

  auto legacy_result = legacy_->init();
  if (!legacy_result) {
    return std::unexpected(convert_legacy_error(legacy_result.error()));
  }

  return {};
}

inline auto two_tier_store::query_path_info(std::string_view store_path)
    -> tier_result<legacy_path_info> {
  auto result = legacy_->query_path_info(store_path);
  if (!result) {
    return std::unexpected(convert_legacy_error(result.error()));
  }
  return *result;
}

inline auto two_tier_store::is_valid_path(std::string_view store_path) -> bool {
  return legacy_->is_valid_path(store_path);
}

inline auto two_tier_store::query_references(std::string_view store_path)
    -> tier_result<std::vector<std::string>> {
  auto result = legacy_->query_references(store_path);
  if (!result) {
    return std::unexpected(convert_legacy_error(result.error()));
  }
  return *result;
}

inline auto two_tier_store::query_referrers(std::string_view store_path)
    -> tier_result<std::vector<std::string>> {

  auto result = legacy_->query_referrers(store_path);

  if (!result) {
    return std::unexpected(convert_legacy_error(result.error()));

  }
  return *result;
}

inline auto two_tier_store::query_derivation_output(std::string_view drv_path,
                                                    std::string_view output_name)
    -> tier_result<std::string> {

  auto result = legacy_->query_derivation_output(drv_path, output_name);

  if (!result) {
    return std::unexpected(convert_legacy_error(result.error()));
  }

  return *result;
}

inline auto two_tier_store::query_all_valid_paths() -> tier_result<std::vector<std::string>> {
  auto result = legacy_->query_all_valid_paths();
  if (!result) {
    return std::unexpected(convert_legacy_error(result.error()));
  }
  return *result;
}

inline auto two_tier_store::register_path(const legacy_path_info& info,
                                          std::span<const std::string> references)
    -> tier_result<void> {

  // Register in legacy store (for metadata and references)
  auto result = legacy_->register_path(info, references);

  if (!result) {
    return std::unexpected(convert_legacy_error(result.error()));
  }

  return {};
}

inline auto two_tier_store::invalidate_path(std::string_view store_path) -> tier_result<void> {
  auto result = legacy_->invalidate_path(store_path);

  if (!result) {
    return std::unexpected(convert_legacy_error(result.error()));
  }

  return {};
}

inline auto two_tier_store::add_derivation_output(std::string_view drv_path,
                                                  std::string_view output_name,
                                                  std::string_view output_path)
    -> tier_result<void> {

  auto result = legacy_->add_derivation_output(drv_path, output_name, output_path);

  if (!result) {
    return std::unexpected(convert_legacy_error(result.error()));
  }

  return {};
}

inline auto two_tier_store::put_ca(std::span<const std::byte> data) -> tier_result<std::string> {
  auto result = ca_->put(data);

  if (!result) {
    return std::unexpected(convert_ca_error(result.error()));
  }

  return *result;
}

inline auto two_tier_store::get_ca(std::string_view hash) -> tier_result<std::vector<std::byte>> {
  auto result = ca_->get(hash);

  if (!result) {
    return std::unexpected(convert_ca_error(result.error()));
  }

  return *result;
}

inline auto two_tier_store::has_ca(std::string_view hash) const -> bool {
  return ca_->has(hash);
}

inline auto two_tier_store::verify() -> tier_result<bool> {
  // Verify CA store
  auto ca_result = ca_->verify_all();
  if (!ca_result) {
    return std::unexpected(convert_ca_error(ca_result.error()));
  }

  if (*ca_result > 0) {
    return false; // CA corruption found
  }

  // Verify legacy store
  auto legacy_result = legacy_->verify();
  if (!legacy_result) {
    return std::unexpected(convert_legacy_error(legacy_result.error()));
  }

  return *legacy_result;
}

inline auto two_tier_store::vacuum() -> tier_result<void> {
  auto result = legacy_->vacuum();

  if (!result) {
    return std::unexpected(convert_legacy_error(result.error()));
  }

  return {};
}

} // namespace straylight::nix::store
