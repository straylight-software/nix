// straylight::nix::primitives::strings_config
//
// Compile-time configuration for string operation backend selection.
// Allows tuning based on CPU architecture and workload characteristics.
//
// Usage:
//   // Before including strings.h, define overrides:
//   #define STRAYLIGHT_STRINGS_FIND_THRESHOLD 256
//   #include <straylight/nix/text/strings.h>
//
// Or pass via compiler flags:
//   -DSTRAYLIGHT_STRINGS_FIND_THRESHOLD=256

#pragma once

#include <cstddef>

namespace straylight::nix::text {

// ─────────────────────────────────────────────────────────────────────────────
// Architecture Detection
// ─────────────────────────────────────────────────────────────────────────────

// Detect AVX-512 support (set by -march=znver4, -march=znver5, -march=skylake-avx512, etc.)
#if defined(__AVX512F__) && defined(__AVX512BW__)
inline constexpr bool has_avx512 = true;
#else
inline constexpr bool has_avx512 = false;
#endif

// Detect AVX2 support
#if defined(__AVX2__)
inline constexpr bool has_avx2 = true;
#else
inline constexpr bool has_avx2 = false;
#endif

// Detect NEON (ARM)
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
inline constexpr bool has_neon = true;
#else
inline constexpr bool has_neon = false;
#endif

// Best available SIMD level
enum class simd_level : int {
  none = 0,
  neon = 1,   // ARM NEON
  avx2 = 2,   // x86 AVX2
  avx512 = 3, // x86 AVX-512
};

inline constexpr simd_level active_simd_level = has_avx512 ? simd_level::avx512
                                                : has_avx2 ? simd_level::avx2
                                                : has_neon ? simd_level::neon
                                                           : simd_level::none;

// ─────────────────────────────────────────────────────────────────────────────
// Threshold Configuration
//
// These thresholds determine when to use SIMD-accelerated stringzilla vs
// simple std::string_view operations. Based on benchmarks on Zen 5 (9950X3D):
//
// - SIMD wins for find/contains on strings > ~64 bytes
// - SIMD loses for prefix/suffix checks on strings < ~256 bytes
// - SIMD wins for replace when multiple replacements expected
//
// Tune these for your specific CPU and workload.
// ─────────────────────────────────────────────────────────────────────────────

// Minimum haystack size to use SIMD find/contains (bytes)
// Below this, std::string_view::find is faster due to SIMD setup overhead
#if !defined(STRAYLIGHT_STRINGS_FIND_THRESHOLD)
#  if defined(__AVX512F__)
#    define STRAYLIGHT_STRINGS_FIND_THRESHOLD 64
#  elif defined(__AVX2__)
#    define STRAYLIGHT_STRINGS_FIND_THRESHOLD 128
#  else
#    define STRAYLIGHT_STRINGS_FIND_THRESHOLD 256
#  endif
#endif
inline constexpr std::size_t find_threshold = STRAYLIGHT_STRINGS_FIND_THRESHOLD;

// Minimum string size to use SIMD prefix/suffix checks (bytes)
// For short prefixes like "/nix/store/" (11 bytes), std is always faster
#if !defined(STRAYLIGHT_STRINGS_PREFIX_THRESHOLD)
#  define STRAYLIGHT_STRINGS_PREFIX_THRESHOLD 256
#endif
inline constexpr std::size_t prefix_threshold = STRAYLIGHT_STRINGS_PREFIX_THRESHOLD;

// Minimum string size to use SIMD replace (bytes)
// SIMD helps when there are multiple replacements to find
#if !defined(STRAYLIGHT_STRINGS_REPLACE_THRESHOLD)
#  if defined(__AVX512F__)
#    define STRAYLIGHT_STRINGS_REPLACE_THRESHOLD 128
#  else
#    define STRAYLIGHT_STRINGS_REPLACE_THRESHOLD 256
#  endif
#endif
inline constexpr std::size_t replace_threshold = STRAYLIGHT_STRINGS_REPLACE_THRESHOLD;

// Minimum string size to use SIMD split/tokenize (bytes)
// For small strings, allocation overhead dominates anyway
#if !defined(STRAYLIGHT_STRINGS_SPLIT_THRESHOLD)
#  define STRAYLIGHT_STRINGS_SPLIT_THRESHOLD 128
#endif
inline constexpr std::size_t split_threshold = STRAYLIGHT_STRINGS_SPLIT_THRESHOLD;

// ─────────────────────────────────────────────────────────────────────────────
// Backend Selection
//
// Force a specific backend regardless of thresholds:
//   STRAYLIGHT_STRINGS_FORCE_STD=1    - Always use std::string_view
//   STRAYLIGHT_STRINGS_FORCE_SZ=1     - Always use stringzilla
//
// By default, use adaptive selection based on thresholds.
// ─────────────────────────────────────────────────────────────────────────────

#if defined(STRAYLIGHT_STRINGS_FORCE_STD) && STRAYLIGHT_STRINGS_FORCE_STD
inline constexpr bool force_std_backend = true;
inline constexpr bool force_sz_backend = false;
#elif defined(STRAYLIGHT_STRINGS_FORCE_SZ) && STRAYLIGHT_STRINGS_FORCE_SZ
inline constexpr bool force_std_backend = false;
inline constexpr bool force_sz_backend = true;
#else
inline constexpr bool force_std_backend = false;
inline constexpr bool force_sz_backend = false;
#endif

inline constexpr bool adaptive_backend = !force_std_backend && !force_sz_backend;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: Should use SIMD for this operation?
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] constexpr bool use_simd_find(std::size_t haystack_size) noexcept {
  if (force_std_backend) {
    return false;
  }
  if (force_sz_backend) {
    return true;
  }
  return haystack_size >= find_threshold;
}

[[nodiscard]] constexpr bool use_simd_prefix(std::size_t string_size) noexcept {
  if (force_std_backend) {
    return false;
  }
  if (force_sz_backend) {
    return true;
  }
  return string_size >= prefix_threshold;
}

[[nodiscard]] constexpr bool use_simd_replace(std::size_t string_size) noexcept {
  if (force_std_backend) {
    return false;
  }
  if (force_sz_backend) {
    return true;
  }
  return string_size >= replace_threshold;
}

[[nodiscard]] constexpr bool use_simd_split(std::size_t string_size) noexcept {
  if (force_std_backend) {
    return false;
  }
  if (force_sz_backend) {
    return true;
  }
  return string_size >= split_threshold;
}

// ─────────────────────────────────────────────────────────────────────────────
// Runtime info (for debugging/logging)
// ─────────────────────────────────────────────────────────────────────────────

inline constexpr const char* simd_level_name() noexcept {
  switch (active_simd_level) {
    case simd_level::avx512:
      return "AVX-512";
    case simd_level::avx2:
      return "AVX2";
    case simd_level::neon:
      return "NEON";
    default:
      return "none";
  }
}

inline constexpr const char* backend_mode_name() noexcept {
  if (force_std_backend) {
    return "std (forced)";
  }
  if (force_sz_backend) {
    return "stringzilla (forced)";
  }
  return "adaptive";
}

} // namespace straylight::nix::text
