#pragma once
/// @file straylight/nix/compiler/compile/value.h
/// Type-safe WASM value expressions with compile-time forcing guarantees.
///
/// The Problem:
/// Nix is lazy - values may be thunks that need forcing before use.
/// The current code uses `bool force` parameters which are easy to get wrong.
/// A bug where `force=true` should be `force=false` (or vice versa) compiles
/// fine but causes infinite recursion or incorrect evaluation at runtime.
///
/// The Solution:
/// Encode the forcing state in the type system:
/// - `forced_value`: Guaranteed not a thunk. Safe to use in strict contexts.
/// - `maybe_value`: Might be a thunk. Must be forced before strict use.
///
/// The ONLY way to convert `maybe_value` -> `forced_value` is via the `force()`
/// function, which emits a runtime __force call. Implicit conversion the other
/// direction (forced -> maybe) is safe and allowed.
///
/// Usage:
/// @code
///   // Literals are always forced
///   auto int_val = compile_integer(ctx, expr);  // -> forced_value
///
///   // Identifier lookup might return a thunk
///   auto id_val = compile_identifier(ctx, scope, expr);  // -> maybe_value
///
///   // Must force before arithmetic
///   auto left = force(ctx.module, left_maybe);   // -> forced_value
///   auto right = force(ctx.module, right_maybe); // -> forced_value
///   return compile_add(ctx, left, right);        // forced_value args required
///
///   // Passing to thunk capture - maybe_value is fine
///   store_capture(ctx, capture_val);  // accepts maybe_value
/// @endcode

#include <concepts>
#include <cstdint>
#include <type_traits>

#include <binaryen-c.h>

namespace straylight::nix::compiler::compile {

// =============================================================================
// Value Types
// =============================================================================

/// Tag type for forced values (definitely not thunks)
struct forced_tag {
  explicit forced_tag() = default;
};

/// Tag type for maybe-thunk values (might need forcing)
struct maybe_tag {
  explicit maybe_tag() = default;
};

/// Concept for valid value tags
template <typename T>
concept value_tag = std::same_as<T, forced_tag> || std::same_as<T, maybe_tag>;

/// A WASM expression that produces a nix_value.
///
/// The tag type encodes whether the value is guaranteed to be forced:
/// - `forced_tag`: The value is definitely not a thunk. Safe to use in strict contexts.
/// - `maybe_tag`: The value might be a thunk. Must be forced before strict use.
///
/// This is a zero-cost abstraction - at runtime it's just a BinaryenExpressionRef.
template <value_tag Tag>
class wasm_value {
  BinaryenExpressionRef expr_;

  // Private constructor - use factory functions
  explicit constexpr wasm_value(BinaryenExpressionRef e) noexcept : expr_(e) {}

  // Friends that can construct
  friend constexpr auto make_forced(BinaryenExpressionRef e) noexcept -> wasm_value<forced_tag>;
  friend constexpr auto make_maybe(BinaryenExpressionRef e) noexcept -> wasm_value<maybe_tag>;
  friend auto force(BinaryenModuleRef module, wasm_value<maybe_tag> v) noexcept
      -> wasm_value<forced_tag>;

  // Conversion friend
  template <value_tag U>
  friend class wasm_value;

public:
  // Default constructor creates null expression
  constexpr wasm_value() noexcept : expr_(nullptr) {}

  // Copy/move allowed
  constexpr wasm_value(const wasm_value&) noexcept = default;
  constexpr wasm_value(wasm_value&&) noexcept = default;
  constexpr auto operator=(const wasm_value&) noexcept -> wasm_value& = default;
  constexpr auto operator=(wasm_value&&) noexcept -> wasm_value& = default;

  /// Implicit conversion: forced -> maybe is always safe.
  /// A forced value trivially satisfies the weaker "maybe thunk" requirement.
  constexpr operator wasm_value<maybe_tag>() const noexcept
    requires std::same_as<Tag, forced_tag>
  {
    return wasm_value<maybe_tag>{expr_};
  }

  /// Get the underlying expression (for codegen)
  [[nodiscard]] constexpr auto expr() const noexcept -> BinaryenExpressionRef { return expr_; }

  /// Explicit check for null expression
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return expr_ != nullptr; }

  /// Check if the expression is null
  [[nodiscard]] constexpr auto is_null() const noexcept -> bool { return expr_ == nullptr; }
};

/// A value that is guaranteed not to be a thunk.
/// Produced by: literals, lambda expressions, forcing a maybe_value.
using forced_value = wasm_value<forced_tag>;

/// A value that might be a thunk.
/// Produced by: identifier lookup, function application, most expressions.
/// Must be forced before use in strict contexts (arithmetic, comparison, etc.)
using maybe_value = wasm_value<maybe_tag>;

// =============================================================================
// Factory Functions (the ONLY way to construct values)
// =============================================================================

/// Create a forced value from a literal expression.
/// Use this for compile-time known non-thunks: integers, floats, strings, lambdas.
[[nodiscard]] constexpr auto make_forced(BinaryenExpressionRef e) noexcept -> forced_value {
  return forced_value{e};
}

/// Create a maybe-thunk value.
/// Use this when the expression might produce a thunk at runtime.
[[nodiscard]] constexpr auto make_maybe(BinaryenExpressionRef e) noexcept -> maybe_value {
  return maybe_value{e};
}

// =============================================================================
// Force Function (the ONLY way to convert maybe -> forced)
// =============================================================================

/// Force a maybe-thunk value, producing a guaranteed forced value.
///
/// This emits a call to the runtime __force function, which:
/// - If the value is not a thunk: returns it unchanged
/// - If the value is a thunk: evaluates it, caches the result, returns it
///
/// @param module The Binaryen module (for creating the call)
/// @param v The value to force
/// @return A forced value (guaranteed not a thunk)
[[nodiscard]] inline auto force(BinaryenModuleRef module, maybe_value v) noexcept -> forced_value {
  // __force : (nix_value) -> nix_value
  // The runtime guarantees the result is not a thunk
  BinaryenExpressionRef args[] = {v.expr()};
  return forced_value{BinaryenCall(module, "__force", args, 1, BinaryenTypeInt64())};
}

/// Convenience: force if needed, no-op if already forced.
/// This allows generic code to accept either type.
[[nodiscard]] constexpr auto ensure_forced(BinaryenModuleRef /*module*/, forced_value v) noexcept
    -> forced_value {
  return v; // Already forced, no-op
}

[[nodiscard]] inline auto ensure_forced(BinaryenModuleRef module, maybe_value v) noexcept
    -> forced_value {
  return force(module, v);
}

// =============================================================================
// Type Traits
// =============================================================================

/// Check if a type is a wasm_value
template <typename T>
struct is_wasm_value : std::false_type {};

template <value_tag Tag>
struct is_wasm_value<wasm_value<Tag>> : std::true_type {};

template <typename T>
inline constexpr bool is_wasm_value_v = is_wasm_value<T>::value;

/// Check if a wasm_value is forced
template <typename T>
struct is_forced_value : std::false_type {};

template <>
struct is_forced_value<forced_value> : std::true_type {};

template <typename T>
inline constexpr bool is_forced_value_v = is_forced_value<T>::value;

/// Concept for any wasm value type
template <typename T>
concept any_wasm_value = is_wasm_value_v<T>;

/// Concept for forced wasm values only
template <typename T>
concept forced_wasm_value = is_forced_value_v<T>;

} // namespace straylight::nix::compiler::compile
