// straylight // nix-language // runtime
//
// I/O Backend Configuration for WASM Evaluator
//
// Configuration options (via #define before including, or compiler flags):
//
//   STRAYLIGHT_EVAL_IO_NONE   - Pure evaluator, no I/O (default for testing)
//                               All I/O operations throw runtime_error
//
//   STRAYLIGHT_EVAL_IO_SYNC   - Synchronous blocking I/O
//                               Simple, portable, single-threaded
//
//   STRAYLIGHT_EVAL_IO_ASYNC  - Async I/O via evring/io_uring
//                               High-throughput, Linux-only
//
// Default: STRAYLIGHT_EVAL_IO_NONE (safe default for sandboxed evaluation)

#pragma once

#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// Backend Selection
// ─────────────────────────────────────────────────────────────────────────────

// Default to no I/O if nothing specified
#if !defined(STRAYLIGHT_EVAL_IO_NONE) && !defined(STRAYLIGHT_EVAL_IO_SYNC) &&                      \
    !defined(STRAYLIGHT_EVAL_IO_ASYNC)
#  define STRAYLIGHT_EVAL_IO_NONE 1
#endif

// Validate exactly one backend is selected
#if (defined(STRAYLIGHT_EVAL_IO_NONE) + defined(STRAYLIGHT_EVAL_IO_SYNC) +                         \
     defined(STRAYLIGHT_EVAL_IO_ASYNC)) > 1
#  error                                                                                           \
      "Only one of STRAYLIGHT_EVAL_IO_NONE, STRAYLIGHT_EVAL_IO_SYNC, STRAYLIGHT_EVAL_IO_ASYNC can be defined"
#endif

namespace straylight::language::runtime {

// ─────────────────────────────────────────────────────────────────────────────
// Backend enumeration
// ─────────────────────────────────────────────────────────────────────────────

enum class io_backend : std::uint8_t {
  none,  // Pure evaluation, no I/O
  sync,  // Synchronous blocking I/O
  async, // Async I/O via evring
};

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time backend introspection
// ─────────────────────────────────────────────────────────────────────────────

#if defined(STRAYLIGHT_EVAL_IO_NONE)
inline constexpr io_backend active_io_backend = io_backend::none;
inline constexpr const char* io_backend_name = "none";
inline constexpr bool io_backend_has_import = false;
inline constexpr bool io_backend_has_read_file = false;
inline constexpr bool io_backend_has_derivation = false;
#elif defined(STRAYLIGHT_EVAL_IO_SYNC)
inline constexpr io_backend active_io_backend = io_backend::sync;
inline constexpr const char* io_backend_name = "sync";
inline constexpr bool io_backend_has_import = true;
inline constexpr bool io_backend_has_read_file = true;
inline constexpr bool io_backend_has_derivation = true;
#elif defined(STRAYLIGHT_EVAL_IO_ASYNC)
inline constexpr io_backend active_io_backend = io_backend::async;
inline constexpr const char* io_backend_name = "async";
inline constexpr bool io_backend_has_import = true;
inline constexpr bool io_backend_has_read_file = true;
inline constexpr bool io_backend_has_derivation = true;
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Feature capability queries
// ─────────────────────────────────────────────────────────────────────────────

/// Check if the current backend supports I/O operations
[[nodiscard]] consteval bool has_io() noexcept {
  return active_io_backend != io_backend::none;
}

/// Check if import is available
[[nodiscard]] consteval bool has_import() noexcept {
  return io_backend_has_import;
}

/// Check if file operations are available
[[nodiscard]] consteval bool has_file_ops() noexcept {
  return io_backend_has_read_file;
}

/// Check if derivation is available
[[nodiscard]] consteval bool has_derivation() noexcept {
  return io_backend_has_derivation;
}

} // namespace straylight::language::runtime
