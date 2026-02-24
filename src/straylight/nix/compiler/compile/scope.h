#pragma once
/// @file straylight/nix/compiler/compile/scope.h
/// Immutable scope context for variable resolution during compilation.
///
/// The Problem:
/// The current compiler has mutable state scattered across member variables:
/// - `current_scope_` - mutable pointer
/// - `current_let_binding_offsets_` - mutable map
/// - `current_rec_binding_offsets_` - mutable map
/// - `with_scopes_` - mutable vector
///
/// This makes it hard to reason about what scope is active at any point,
/// and easy to forget to restore state after entering a nested scope.
///
/// The Solution:
/// Pass scope context explicitly as an immutable value. Creating a child
/// context returns a new value, leaving the parent unchanged. This makes
/// scope handling explicit and eliminates state restoration bugs.
///
/// Usage:
/// @code
///   auto compile_let(compiler_ctx& ctx, scope_ctx scope, const ast::expression_let& expr) {
///     // Create let bindings and add them to a new scope
///     auto let_bindings = /* ... */;
///     auto inner_scope = scope.with_let(let_bindings);
///
///     // Compile body in the inner scope
///     return compile(ctx, inner_scope, expr.body_);
///     // No need to restore - scope is immutable
///   }
/// @endcode

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "straylight/nix/compiler/ast/expression.h"

namespace straylight::nix::compiler::compile {

// Forward declaration
class lexical_scope;

// =============================================================================
// Variable Location
// =============================================================================

/// Where a variable is stored at runtime.
/// This determines how to generate code to access the variable.
enum class var_location : std::uint8_t {
  /// In a WASM local variable (lambda parameters, loop variables)
  wasm_local,

  /// In the closure/thunk environment (captured free variables)
  captured,

  /// In shared memory at a fixed offset (let bindings - mutually recursive)
  let_memory,

  /// In shared memory at a fixed offset (rec attrset bindings)
  rec_memory,

  /// Dynamic lookup via `with` scope chain (__hasAttr/__select fallback)
  with_dynamic,
};

// =============================================================================
// Variable Reference
// =============================================================================

/// A resolved variable reference with its storage location.
struct var_ref {
  ast::symbol name;
  var_location location;

  // Location-specific data (union would be cleaner but this is simpler)
  std::uint32_t local_index;   // For wasm_local
  std::uint32_t capture_index; // For captured
  std::uint32_t memory_offset; // For let_memory, rec_memory

  /// Create a reference to a WASM local variable
  [[nodiscard]] static constexpr auto local(ast::symbol name, std::uint32_t index) -> var_ref {
    return {name, var_location::wasm_local, index, 0, 0};
  }

  /// Create a reference to a captured variable
  [[nodiscard]] static constexpr auto capture(ast::symbol name, std::uint32_t index) -> var_ref {
    return {name, var_location::captured, 0, index, 0};
  }

  /// Create a reference to a let-bound variable
  [[nodiscard]] static constexpr auto let_bound(ast::symbol name, std::uint32_t offset) -> var_ref {
    return {name, var_location::let_memory, 0, 0, offset};
  }

  /// Create a reference to a rec-bound variable
  [[nodiscard]] static constexpr auto rec_bound(ast::symbol name, std::uint32_t offset) -> var_ref {
    return {name, var_location::rec_memory, 0, 0, offset};
  }

  /// Create a reference to a with-scoped variable (dynamic lookup)
  [[nodiscard]] static constexpr auto with_scoped(ast::symbol name) -> var_ref {
    return {name, var_location::with_dynamic, 0, 0, 0};
  }
};

// =============================================================================
// Binding Entry
// =============================================================================

/// A binding from symbol to memory offset (for let/rec scopes).
struct binding_entry {
  ast::symbol name;
  std::uint32_t offset;
};

// =============================================================================
// Scope Context
// =============================================================================

/// Immutable scope context for variable resolution.
///
/// This replaces the mutable scope state in the compiler class.
/// All modifications create new scope_ctx values, leaving the original unchanged.
///
/// Lookup priority (highest to lowest):
/// 1. Let-bound variables (shared memory)
/// 2. Rec-bound variables (shared memory)
/// 3. Lexical scope (locals and captures)
/// 4. With scopes (dynamic lookup chain)
/// 5. Global builtins (handled separately)
struct scope_ctx {
  /// Lexical scope chain (lambda params, local vars)
  const lexical_scope* lexical = nullptr;

  /// Let-bound variables (shared memory, mutually recursive)
  std::span<const binding_entry> let_bindings{};

  /// Rec-bound variables (shared memory, for rec attrsets)
  std::span<const binding_entry> rec_bindings{};

  /// With scope stack - each entry is the local index holding the namespace attrset
  std::span<const std::uint32_t> with_locals{};

  // ===========================================================================
  // Variable Lookup
  // ===========================================================================

  /// Look up a variable by name, returning its reference if found.
  /// Searches in priority order: let -> rec -> lexical -> with
  [[nodiscard]] auto lookup(ast::symbol name) const -> std::optional<var_ref>;

  /// Check if a variable is in let scope
  [[nodiscard]] auto in_let_scope(ast::symbol name) const -> bool;

  /// Check if a variable is in rec scope
  [[nodiscard]] auto in_rec_scope(ast::symbol name) const -> bool;

  // ===========================================================================
  // Child Context Builders
  // ===========================================================================

  /// Create a child context with a new lexical scope
  [[nodiscard]] auto with_lexical(const lexical_scope& scope) const -> scope_ctx {
    auto copy = *this;
    copy.lexical = &scope;
    return copy;
  }

  /// Create a child context with let bindings
  [[nodiscard]] auto with_let(std::span<const binding_entry> bindings) const -> scope_ctx {
    auto copy = *this;
    copy.let_bindings = bindings;
    return copy;
  }

  /// Create a child context with rec bindings
  [[nodiscard]] auto with_rec(std::span<const binding_entry> bindings) const -> scope_ctx {
    auto copy = *this;
    copy.rec_bindings = bindings;
    return copy;
  }

  /// Create a child context with additional with scopes
  [[nodiscard]] auto with_with(std::span<const std::uint32_t> locals) const -> scope_ctx {
    auto copy = *this;
    copy.with_locals = locals;
    return copy;
  }

  /// Create a child context clearing let bindings (entering new let scope)
  [[nodiscard]] auto without_let() const -> scope_ctx {
    auto copy = *this;
    copy.let_bindings = {};
    return copy;
  }

  /// Create a child context clearing rec bindings (entering new rec scope)
  [[nodiscard]] auto without_rec() const -> scope_ctx {
    auto copy = *this;
    copy.rec_bindings = {};
    return copy;
  }
};

// =============================================================================
// Lexical Scope (existing, but documented here for completeness)
// =============================================================================

/// A scope in the lexical scope chain.
/// This is the existing lexical_scope class - kept for compatibility.
/// See compiler.h for the full implementation.

} // namespace straylight::nix::compiler::compile
