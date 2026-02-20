// straylight // nix-language // runtime
//
// I/O Backend Interface for WASM Evaluator
//
// This provides the host callback implementations for I/O operations.
// The actual implementation is selected at compile time via io_config.h.
//
// Operations provided:
//   - readFile(path) → string
//   - readDir(path) → attrset {name → type}
//   - pathExists(path) → bool
//   - import(path) → value (requires parser/compiler integration)
//   - hashFile(algo, path) → string
//   - derivation(attrs) → drv attrset (requires store integration)

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "straylight/language/runtime/io_config.h"

namespace straylight::language::runtime {

// Forward declarations
struct runtime_context;

// ─────────────────────────────────────────────────────────────────────────────
// Import Callback Type
// ─────────────────────────────────────────────────────────────────────────────

/// Callback signature for evaluating a Nix file.
/// Takes: (source_code, file_path) → nix_value
///
/// This is injected by the executor to break the circular dependency:
///   io_backend → (callback) → parser → compiler → executor
///
/// Returns: packed nix_value (i64), or throws on error
using import_eval_fn = std::function<std::int64_t(std::string_view source, std::string_view path)>;

// ─────────────────────────────────────────────────────────────────────────────
// I/O Error Types
// ─────────────────────────────────────────────────────────────────────────────

enum class io_error : std::uint8_t {
  not_found,         // File/directory not found
  permission_denied, // Access denied
  is_directory,      // Expected file, got directory
  not_directory,     // Expected directory, got file
  io_failed,         // General I/O failure
  not_supported,     // Operation not supported by backend
  invalid_path,      // Malformed path
  import_cycle,      // Circular import detected
  parse_error,       // Failed to parse imported file
  eval_error,        // Failed to evaluate imported file
};

template <typename T>
using io_result = std::expected<T, io_error>;

// ─────────────────────────────────────────────────────────────────────────────
// File Types (for readDir)
// ─────────────────────────────────────────────────────────────────────────────

enum class file_type : std::uint8_t {
  regular,
  directory,
  symlink,
  unknown,
};

/// Entry from readDir
struct dir_entry {
  std::string name;
  file_type type;
};

// ─────────────────────────────────────────────────────────────────────────────
// I/O Backend Interface
// ─────────────────────────────────────────────────────────────────────────────

/// Abstract interface for I/O operations.
/// Different backends implement this for sync/async/none modes.
class io_backend_interface {
public:
  virtual ~io_backend_interface() = default;

  // -------------------------------------------------------------------------
  // Import Callback Configuration
  // -------------------------------------------------------------------------

  /// Set the import evaluation callback.
  /// This must be called before any import operations.
  /// The callback is injected by the executor to break circular deps.
  virtual void set_import_callback(import_eval_fn fn) { import_eval_ = std::move(fn); }

  /// Set the base path for relative imports (typically the file being evaluated).
  virtual void set_import_base_path(std::filesystem::path path) {
    import_base_path_ = std::move(path);
  }

  // -------------------------------------------------------------------------
  // File Operations
  // -------------------------------------------------------------------------

protected:
  import_eval_fn import_eval_;
  std::filesystem::path import_base_path_;

public:
  /// Read file contents as string.
  [[nodiscard]] virtual auto read_file(std::string_view path) -> io_result<std::string> = 0;

  /// List directory contents.
  [[nodiscard]] virtual auto read_dir(std::string_view path)
      -> io_result<std::vector<dir_entry>> = 0;

  /// Check if path exists.
  [[nodiscard]] virtual auto path_exists(std::string_view path) -> bool = 0;

  /// Hash file contents.
  [[nodiscard]] virtual auto hash_file(std::string_view algo, std::string_view path)
      -> io_result<std::string> = 0;

  // -------------------------------------------------------------------------
  // Import (requires parser/compiler/runtime integration)
  // -------------------------------------------------------------------------

  /// Import a Nix file. Returns the evaluated value as nix_value.
  /// This is the most complex operation - it needs to:
  ///   1. Resolve the path (relative, absolute, search paths)
  ///   2. Check import cache (memoization)
  ///   3. Read the file
  ///   4. Parse → AST
  ///   5. Compile → WASM
  ///   6. Execute → value
  ///   7. Cache the result
  ///
  /// The nix_value is returned as int64_t (packed value representation).
  [[nodiscard]] virtual auto import_file(runtime_context& ctx, std::string_view path)
      -> io_result<std::int64_t> = 0;

  // -------------------------------------------------------------------------
  // Derivation (requires store integration)
  // -------------------------------------------------------------------------

  /// Create a derivation. Takes the derivation attributes, returns the drv attrset.
  /// The input is a nix_value (attrset), output is nix_value (drv attrset).
  [[nodiscard]] virtual auto derivation(runtime_context& ctx, std::int64_t attrs)
      -> io_result<std::int64_t> = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Backend Implementations
// ─────────────────────────────────────────────────────────────────────────────

#if defined(STRAYLIGHT_EVAL_IO_NONE)

/// No-op backend that throws on any I/O operation.
/// Used for pure evaluation and testing.
class io_backend_none final : public io_backend_interface {
public:
  [[nodiscard]] auto read_file(std::string_view /*path*/) -> io_result<std::string> override {
    return std::unexpected(io_error::not_supported);
  }

  [[nodiscard]] auto read_dir(std::string_view /*path*/)
      -> io_result<std::vector<dir_entry>> override {
    return std::unexpected(io_error::not_supported);
  }

  [[nodiscard]] auto path_exists(std::string_view /*path*/) -> bool override { return false; }

  [[nodiscard]] auto hash_file(std::string_view /*algo*/, std::string_view /*path*/)
      -> io_result<std::string> override {
    return std::unexpected(io_error::not_supported);
  }

  [[nodiscard]] auto import_file(runtime_context& /*ctx*/, std::string_view /*path*/)
      -> io_result<std::int64_t> override {
    return std::unexpected(io_error::not_supported);
  }

  [[nodiscard]] auto derivation(runtime_context& /*ctx*/, std::int64_t /*attrs*/)
      -> io_result<std::int64_t> override {
    return std::unexpected(io_error::not_supported);
  }
};

using io_backend_default = io_backend_none;

#elif defined(STRAYLIGHT_EVAL_IO_SYNC)

/// Synchronous blocking I/O backend.
/// Uses standard filesystem operations.
class io_backend_sync final : public io_backend_interface {
public:
  /// Construct with optional base directory for relative paths.
  explicit io_backend_sync(std::filesystem::path base_dir = std::filesystem::current_path())
      : base_dir_(std::move(base_dir)) {}

  [[nodiscard]] auto read_file(std::string_view path) -> io_result<std::string> override;

  [[nodiscard]] auto read_dir(std::string_view path) -> io_result<std::vector<dir_entry>> override;

  [[nodiscard]] auto path_exists(std::string_view path) -> bool override;

  [[nodiscard]] auto hash_file(std::string_view algo, std::string_view path)
      -> io_result<std::string> override;

  [[nodiscard]] auto import_file(runtime_context& ctx, std::string_view path)
      -> io_result<std::int64_t> override;

  [[nodiscard]] auto derivation(runtime_context& ctx, std::int64_t attrs)
      -> io_result<std::int64_t> override;

  /// Clear the import cache (useful for tests or reloading).
  void clear_import_cache() {
    import_cache_.clear();
    import_in_progress_.clear();
  }

private:
  std::filesystem::path base_dir_;

  // Import cache: canonical_path → evaluated value
  // This prevents redundant evaluation and provides memoization.
  std::unordered_map<std::string, std::int64_t> import_cache_;

  // Paths currently being imported (for cycle detection).
  // If we encounter a path that's in this set, we have a cycle.
  std::unordered_set<std::string> import_in_progress_;

  /// Resolve an import path to a canonical path.
  /// Handles: relative paths, absolute paths, directory → default.nix
  [[nodiscard]] auto resolve_import_path(std::string_view path) -> io_result<std::filesystem::path>;
};

using io_backend_default = io_backend_sync;

#elif defined(STRAYLIGHT_EVAL_IO_ASYNC)

// Forward declare evring types
namespace evring {
class ring;
}

/// Async I/O backend using evring/io_uring.
/// High throughput for bulk operations.
class io_backend_async final : public io_backend_interface {
public:
  /// Construct with an evring ring for async I/O.
  explicit io_backend_async(evring::ring& ring,
                            std::filesystem::path base_dir = std::filesystem::current_path())
      : ring_(ring), base_dir_(std::move(base_dir)) {}

  [[nodiscard]] auto read_file(std::string_view path) -> io_result<std::string> override;

  [[nodiscard]] auto read_dir(std::string_view path) -> io_result<std::vector<dir_entry>> override;

  [[nodiscard]] auto path_exists(std::string_view path) -> bool override;

  [[nodiscard]] auto hash_file(std::string_view algo, std::string_view path)
      -> io_result<std::string> override;

  [[nodiscard]] auto import_file(runtime_context& ctx, std::string_view path)
      -> io_result<std::int64_t> override;

  [[nodiscard]] auto derivation(runtime_context& ctx, std::int64_t attrs)
      -> io_result<std::int64_t> override;

private:
  evring::ring& ring_;
  std::filesystem::path base_dir_;
};

using io_backend_default = io_backend_async;

#endif

// ─────────────────────────────────────────────────────────────────────────────
// Factory
// ─────────────────────────────────────────────────────────────────────────────

/// Create the default I/O backend for the current build configuration.
[[nodiscard]] inline auto make_default_io_backend() -> std::unique_ptr<io_backend_interface> {
#if defined(STRAYLIGHT_EVAL_IO_NONE)
  return std::make_unique<io_backend_none>();
#elif defined(STRAYLIGHT_EVAL_IO_SYNC)
  return std::make_unique<io_backend_sync>();
#elif defined(STRAYLIGHT_EVAL_IO_ASYNC)
  // async requires a ring, can't create default
  static_assert(false, "Async backend requires explicit ring, use make_async_io_backend()");
  return nullptr;
#endif
}

} // namespace straylight::language::runtime
