#pragma once

/// @file component.h
/// @brief Compile-time component selection for straylight vs legacy implementations
///
/// Use #define before including to enable straylight implementations:
///
///   #define STRAYLIGHT_EVAL 1
///   #include "straylight/nix/core/component.h"
///
/// Or set via compiler flags: -DSTRAYLIGHT_EVAL=1
///
/// Usage with if constexpr:
///
///   if constexpr (straylight::nix::core::use_straylight_eval) {
///     return eval_adapter{}.eval_string(expr);
///   } else {
///     return state.eval(expr);
///   }

#include <string_view>

namespace straylight::nix::core {

// ============================================================================
// Compile-time feature flags
// ============================================================================

#ifdef STRAYLIGHT_STORE
inline constexpr bool use_straylight_store = true;
#else
inline constexpr bool use_straylight_store = false;
#endif

#ifdef STRAYLIGHT_EVAL
inline constexpr bool use_straylight_eval = true;
#else
inline constexpr bool use_straylight_eval = false;
#endif

#ifdef STRAYLIGHT_FETCHER
inline constexpr bool use_straylight_fetcher = true;
#else
inline constexpr bool use_straylight_fetcher = false;
#endif

#ifdef STRAYLIGHT_BUILDER
inline constexpr bool use_straylight_builder = true;
#else
inline constexpr bool use_straylight_builder = false;
#endif

#ifdef STRAYLIGHT_GC
inline constexpr bool use_straylight_gc = true;
#else
inline constexpr bool use_straylight_gc = false;
#endif

// ============================================================================
// Component enumeration (for diagnostics/logging)
// ============================================================================

enum class Component {
  Store,
  Evaluator,
  Fetcher,
  Builder,
  GC,
};

/// Get human-readable component name
[[nodiscard]] constexpr auto component_name(Component c) noexcept -> std::string_view {
  switch (c) {
    case Component::Store:
      return "Store";
    case Component::Evaluator:
      return "Evaluator";
    case Component::Fetcher:
      return "Fetcher";
    case Component::Builder:
      return "Builder";
    case Component::GC:
      return "GC";
  }
  return "";
}

/// Check if straylight is enabled for a component (compile-time)
[[nodiscard]] constexpr auto use_straylight(Component c) noexcept -> bool {
  switch (c) {
    case Component::Store:
      return use_straylight_store;
    case Component::Evaluator:
      return use_straylight_eval;
    case Component::Fetcher:
      return use_straylight_fetcher;
    case Component::Builder:
      return use_straylight_builder;
    case Component::GC:
      return use_straylight_gc;
  }
  return false;
}

} // namespace straylight::nix::core
