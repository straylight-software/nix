#pragma once
/// @file straylight/nix/compiler/compile/capture.h
/// Capture policy for thunk creation - controls when captured values are forced.
///
/// The Problem:
/// When creating a thunk, we capture variables from the enclosing scope.
/// Whether those captures should be forced at capture-time or stored as-is
/// depends on the context:
///
/// - In NON-recursive contexts (list elements, function arguments), forcing
///   at capture-time is fine - the value is already computed.
///
/// - In RECURSIVE contexts (let bindings, rec attrsets, inherit-from), forcing
///   at capture-time causes infinite recursion if the captured variable is
///   itself defined in terms of the thunk being created.
///
/// The old code used `bool force_captures` which was easy to get wrong.
/// This enum makes the intent explicit and forces exhaustive handling.
///
/// Invariant:
/// > In recursive binding contexts, captures MUST use preserve_lazy to avoid
/// > infinite recursion during thunk construction.
///
/// Usage:
/// @code
///   // Non-recursive: list element thunk - safe to force captures
///   auto thunk = build_thunk(ctx, scope, expr, capture_policy::force_eager);
///
///   // Recursive: let binding thunk - MUST NOT force captures
///   auto thunk = build_thunk(ctx, scope, expr, capture_policy::preserve_lazy);
/// @endcode

#include <cstdint>

namespace straylight::nix::compiler::compile {

/// Policy for handling captured variables when creating thunks.
///
/// This enum replaces the old `bool force_captures` parameter with
/// explicit, self-documenting values that require exhaustive switch handling.
enum class capture_policy : std::uint8_t {
  /// Force captured values at thunk creation time.
  ///
  /// Use when the thunk is in a NON-recursive context:
  /// - List elements: `[ expr1 expr2 ]`
  /// - Function arguments: `f expr`
  /// - Standalone thunks not involved in recursive bindings
  ///
  /// This is the "eager capture" mode - captured values are evaluated
  /// immediately and stored as forced values in the thunk's environment.
  force_eager,

  /// Store captured values without forcing (may be thunks themselves).
  ///
  /// Use when the thunk is in a RECURSIVE context:
  /// - Let bindings: `let x = expr; in ...`
  /// - Recursive attrset values: `{ a = expr; }` (in non-rec, but might reference outer rec)
  /// - Inherit-from bindings: `inherit (expr) x;`
  /// - Any context where the captured variable might be defined in terms of
  ///   the thunk being created (fixpoint patterns)
  ///
  /// This is the "lazy capture" mode - captured values are stored as-is,
  /// potentially still thunks. They will be forced when the thunk body
  /// actually uses them.
  preserve_lazy,
};

/// Convert capture_policy to bool for compatibility with existing code.
/// This is a transitional helper - new code should use capture_policy directly.
[[nodiscard]] constexpr auto should_force_captures(capture_policy policy) noexcept -> bool {
  switch (policy) {
    case capture_policy::force_eager:
      return true;
    case capture_policy::preserve_lazy:
      return false;
  }
  __builtin_unreachable();
}

/// Get the appropriate capture policy for a recursive context.
/// Always returns preserve_lazy - recursive contexts must not force.
[[nodiscard]] constexpr auto recursive_capture_policy() noexcept -> capture_policy {
  return capture_policy::preserve_lazy;
}

/// Get the appropriate capture policy for a non-recursive context.
/// Returns force_eager - safe to force in non-recursive contexts.
[[nodiscard]] constexpr auto nonrecursive_capture_policy() noexcept -> capture_policy {
  return capture_policy::force_eager;
}

} // namespace straylight::nix::compiler::compile
