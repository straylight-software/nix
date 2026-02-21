// straylight // nix // backend-config.h
//
// Compile-time configuration for A-team backend selection.
//
// This header defines the backend types available for:
//   - Store operations (legacy daemon, evring io_uring, etc.)
//   - Evaluation (AST interpreter, WASM compiler)
//   - I/O (blocking, io_uring)
//
// Usage:
//   if constexpr (store_backend == store_backend_t::evring) {
//     return evring_store_t{...};
//   } else {
//     return local_store_t{...};
//   }

#pragma once

#include <cstdint>

namespace nix {

// =============================================================================
// Store Backend Selection
// =============================================================================

enum class store_backend_t : std::uint8_t {
  legacy, // Original daemon-based store
  evring, // io_uring-based daemonless store
};

// Compile-time store backend selection
// Override with -DNIX_STORE_BACKEND=evring
#if defined(NIX_STORE_BACKEND_EVRING)
inline constexpr store_backend_t store_backend = store_backend_t::evring;
#else
inline constexpr store_backend_t store_backend = store_backend_t::legacy;
#endif

// =============================================================================
// Eval Backend Selection
// =============================================================================

enum class eval_backend_t : std::uint8_t {
  interpreter, // Original AST interpreter
  wasm,        // Ahead-of-time WASM compilation
};

// Compile-time eval backend selection
// Override with -DNIX_EVAL_BACKEND=wasm
#if defined(NIX_EVAL_BACKEND_WASM)
inline constexpr eval_backend_t eval_backend = eval_backend_t::wasm;
#else
inline constexpr eval_backend_t eval_backend = eval_backend_t::interpreter;
#endif

// =============================================================================
// I/O Backend Selection
// =============================================================================

enum class io_backend_t : std::uint8_t {
  blocking, // Traditional blocking I/O with threads
  io_uring, // Linux io_uring
};

// Compile-time I/O backend selection
// Override with -DNIX_IO_BACKEND=io_uring
#if defined(NIX_IO_BACKEND_IO_URING)
inline constexpr io_backend_t io_backend = io_backend_t::io_uring;
#else
inline constexpr io_backend_t io_backend = io_backend_t::blocking;
#endif

// =============================================================================
// Feature Detection Helpers
// =============================================================================

// Check if we're using any A-team components
inline constexpr bool uses_evring_store = (store_backend == store_backend_t::evring);
inline constexpr bool uses_wasm_eval = (eval_backend == eval_backend_t::wasm);
inline constexpr bool uses_io_uring = (io_backend == io_backend_t::io_uring);

// Check if we're fully A-team
inline constexpr bool full_a_team = uses_evring_store && uses_wasm_eval && uses_io_uring;

// =============================================================================
// Runtime Backend Names (for logging/debugging)
// =============================================================================

[[nodiscard]] constexpr auto store_backend_name() -> const char* {
  switch (store_backend) {
    case store_backend_t::legacy:
      return "legacy";
    case store_backend_t::evring:
      return "evring";
  }
  return "unknown";
}

[[nodiscard]] constexpr auto eval_backend_name() -> const char* {
  switch (eval_backend) {
    case eval_backend_t::interpreter:
      return "interpreter";
    case eval_backend_t::wasm:
      return "wasm";
  }
  return "unknown";
}

[[nodiscard]] constexpr auto io_backend_name() -> const char* {
  switch (io_backend) {
    case io_backend_t::blocking:
      return "blocking";
    case io_backend_t::io_uring:
      return "io_uring";
  }
  return "unknown";
}

} // namespace nix
