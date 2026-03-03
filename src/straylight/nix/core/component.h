#pragma once

/// @file component.h
/// @brief Runtime component registry for incremental straylight migration
///
/// This allows swapping legacy nix subsystems for straylight implementations
/// at runtime via environment variables, config settings, or CLI flags.
///
/// Usage:
///   // Check which implementation to use
///   if (straylight::nix::core::active(Component::Store) == Implementation::Straylight) {
///     return make_ref<straylight::nix::store::store_adapter>(uri);
///   }
///   return make_ref<LocalStore>(uri);
///
/// Configuration (in order of precedence):
///   1. CLI: --option store-backend straylight
///   2. Environment: STRAYLIGHT_STORE=1
///   3. Config file: experimental-features = straylight-store
///   4. Default: Legacy

#include <optional>
#include <string_view>

namespace straylight::nix::core {

// ============================================================================
// Component enumeration
// ============================================================================

/// Subsystems that can be swapped between legacy and straylight implementations
enum class Component {
  Store,     // log_store vs SQLite LocalStore
  Evaluator, // WASM compiler vs tree-walking interpreter
  Fetcher,   // straylight fetcher vs libfetchers
  Builder,   // straylight builder vs derivation-builder
  GC,        // io_uring GC vs legacy gc.cpp
};

/// Implementation variants
enum class Implementation {
  Legacy,     // upstream nix implementation
  Straylight, // our implementation
};

// ============================================================================
// Runtime registry
// ============================================================================

/// Get the active implementation for a component.
/// Thread-safe, lock-free after initialization.
[[nodiscard]] auto active(Component c) noexcept -> Implementation;

/// Override the implementation for a component.
/// Should be called during initialization before multi-threaded operation.
void set_active(Component c, Implementation impl) noexcept;

/// Reset a component to its default (based on env/config).
void reset_to_default(Component c) noexcept;

/// Reset all components to defaults.
void reset_all_to_defaults() noexcept;

// ============================================================================
// Configuration helpers
// ============================================================================

/// Check if a straylight component is enabled via environment variable.
/// Checks: STRAYLIGHT_STORE, STRAYLIGHT_EVAL, STRAYLIGHT_FETCHER, etc.
[[nodiscard]] auto env_enabled(Component c) noexcept -> std::optional<bool>;

/// Get the environment variable name for a component.
[[nodiscard]] constexpr auto env_var_name(Component c) noexcept -> std::string_view {
  switch (c) {
    case Component::Store:
      return "STRAYLIGHT_STORE";
    case Component::Evaluator:
      return "STRAYLIGHT_EVAL";
    case Component::Fetcher:
      return "STRAYLIGHT_FETCHER";
    case Component::Builder:
      return "STRAYLIGHT_BUILDER";
    case Component::GC:
      return "STRAYLIGHT_GC";
  }
  return "";
}

/// Get the experimental feature name for a component.
[[nodiscard]] constexpr auto feature_name(Component c) noexcept -> std::string_view {
  switch (c) {
    case Component::Store:
      return "straylight-store";
    case Component::Evaluator:
      return "straylight-eval";
    case Component::Fetcher:
      return "straylight-fetcher";
    case Component::Builder:
      return "straylight-builder";
    case Component::GC:
      return "straylight-gc";
  }
  return "";
}

/// Get human-readable component name.
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

// ============================================================================
// Compile-time feature detection
// ============================================================================

/// Check if straylight store is compiled in
#ifdef STRAYLIGHT_STORE
inline constexpr bool have_straylight_store = true;
#else
inline constexpr bool have_straylight_store = false;
#endif

/// Check if straylight evaluator is compiled in
#ifdef STRAYLIGHT_EVAL
inline constexpr bool have_straylight_eval = true;
#else
inline constexpr bool have_straylight_eval = false;
#endif

/// Check if straylight fetcher is compiled in
#ifdef STRAYLIGHT_FETCHER
inline constexpr bool have_straylight_fetcher = true;
#else
inline constexpr bool have_straylight_fetcher = false;
#endif

/// Check if straylight builder is compiled in
#ifdef STRAYLIGHT_BUILDER
inline constexpr bool have_straylight_builder = true;
#else
inline constexpr bool have_straylight_builder = false;
#endif

/// Check if straylight GC is compiled in
#ifdef STRAYLIGHT_GC
inline constexpr bool have_straylight_gc = true;
#else
inline constexpr bool have_straylight_gc = false;
#endif

/// Check if any straylight component is available at compile time
[[nodiscard]] constexpr auto have_component(Component c) noexcept -> bool {
  switch (c) {
    case Component::Store:
      return have_straylight_store;
    case Component::Evaluator:
      return have_straylight_eval;
    case Component::Fetcher:
      return have_straylight_fetcher;
    case Component::Builder:
      return have_straylight_builder;
    case Component::GC:
      return have_straylight_gc;
  }
  return false;
}

// ============================================================================
// RAII scope override
// ============================================================================

/// Temporarily override a component's implementation within a scope.
/// Useful for testing or isolated operations.
class scoped_override {
public:
  scoped_override(Component c, Implementation impl) noexcept;
  ~scoped_override();

  // non-copyable, non-movable
  scoped_override(const scoped_override&) = delete;
  scoped_override& operator=(const scoped_override&) = delete;
  scoped_override(scoped_override&&) = delete;
  scoped_override& operator=(scoped_override&&) = delete;

private:
  Component component_;
  Implementation previous_;
};

} // namespace straylight::nix::core
