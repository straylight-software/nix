// component.cpp - Runtime component registry implementation

#include "component.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>

namespace straylight::nix::core {

namespace {

// Number of components
constexpr auto kNumComponents = static_cast<std::size_t>(Component::GC) + 1;

// Atomic storage for each component's active implementation.
// Using relaxed ordering since we only need visibility, not synchronization.
std::array<std::atomic<Implementation>, kNumComponents> g_active_impl = {
    Implementation::Legacy, // Store
    Implementation::Legacy, // Evaluator
    Implementation::Legacy, // Fetcher
    Implementation::Legacy, // Builder
    Implementation::Legacy, // GC
};

// Whether initialization has been done
std::once_flag g_init_flag;

// Index helper
constexpr auto idx(Component c) noexcept -> std::size_t {
  return static_cast<std::size_t>(c);
}

// Initialize from environment
void init_from_env() {
  // Check each component's environment variable
  for (std::size_t i = 0; i < kNumComponents; ++i) {
    auto c = static_cast<Component>(i);
    if (auto enabled = env_enabled(c); enabled.has_value()) {
      g_active_impl[i].store(*enabled ? Implementation::Straylight : Implementation::Legacy,
                             std::memory_order_relaxed);
    }
  }
}

} // namespace

auto active(Component c) noexcept -> Implementation {
  // Ensure env is checked on first call
  std::call_once(g_init_flag, init_from_env);

  return g_active_impl[idx(c)].load(std::memory_order_relaxed);
}

void set_active(Component c, Implementation impl) noexcept {
  // Ensure env is checked first (so explicit sets override env)
  std::call_once(g_init_flag, init_from_env);

  g_active_impl[idx(c)].store(impl, std::memory_order_relaxed);
}

void reset_to_default(Component c) noexcept {
  // Reset to legacy, then re-check env
  g_active_impl[idx(c)].store(Implementation::Legacy, std::memory_order_relaxed);

  if (auto enabled = env_enabled(c); enabled.has_value() && *enabled) {
    g_active_impl[idx(c)].store(Implementation::Straylight, std::memory_order_relaxed);
  }
}

void reset_all_to_defaults() noexcept {
  for (std::size_t i = 0; i < kNumComponents; ++i) {
    reset_to_default(static_cast<Component>(i));
  }
}

auto env_enabled(Component c) noexcept -> std::optional<bool> {
  auto var_name = env_var_name(c);
  if (var_name.empty()) {
    return std::nullopt;
  }

  // Need null-terminated string for getenv
  // env_var_name returns string_view to static storage, so this is safe
  const char* value = std::getenv(var_name.data());
  if (value == nullptr) {
    return std::nullopt;
  }

  // Check for truthy values
  std::string_view sv{value};
  if (sv == "1" || sv == "true" || sv == "yes" || sv == "on") {
    return true;
  }
  if (sv == "0" || sv == "false" || sv == "no" || sv == "off") {
    return false;
  }

  // Non-empty but unrecognized value - treat as enabled
  return !sv.empty();
}

// ============================================================================
// scoped_override
// ============================================================================

scoped_override::scoped_override(Component c, Implementation impl) noexcept
    : component_(c), previous_(active(c)) {
  set_active(c, impl);
}

scoped_override::~scoped_override() {
  set_active(component_, previous_);
}

} // namespace straylight::nix::core
