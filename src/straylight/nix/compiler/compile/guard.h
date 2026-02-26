#pragma once
/// @file straylight/nix/compiler/compile/guard.h
/// RAII guards for compiler state save/restore.
///
/// The compiler has multiple pieces of mutable state that must be saved before
/// entering a nested context (lambda, thunk, let, rec, with) and restored after.
/// The old code did this manually, which is error-prone: any early return or
/// exception leaves the state corrupted.
///
/// These guards ensure state is always restored, regardless of control flow.
///
/// Usage:
/// @code
///   auto compile_lambda(...) {
///     lexical_scope lambda_scope;
///     auto guard = scope_guard(*this, lambda_scope);  // saves current_scope_
///     // ... compile body ...
///     // guard destructor restores current_scope_ automatically
///   }
/// @endcode

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace straylight::nix::compiler::ast {
struct symbol; // Forward declaration
}

namespace straylight::nix::compiler::compile {

// Forward declarations - compiler class and its nested types
class compiler;
class lexical_scope;

// =============================================================================
// Scope Guard
// =============================================================================

/// RAII guard for lexical scope pointer.
/// Saves `current_scope_` on construction, restores on destruction.
class scope_guard {
  compiler& compiler_;
  lexical_scope* saved_scope_;

public:
  /// Construct guard, setting compiler's current scope to `new_scope`.
  /// @param c The compiler instance
  /// @param new_scope The new scope to activate
  scope_guard(compiler& c, lexical_scope& new_scope);

  /// Restore the saved scope.
  ~scope_guard();

  // Non-copyable, non-movable
  scope_guard(const scope_guard&) = delete;
  auto operator=(const scope_guard&) -> scope_guard& = delete;
  scope_guard(scope_guard&&) = delete;
  auto operator=(scope_guard&&) -> scope_guard& = delete;
};

// =============================================================================
// Lambda Context Guard
// =============================================================================

/// RAII guard for lambda compilation context.
/// Saves `current_lambda_context_` on construction, restores on destruction.
///
/// Also initializes a fresh lambda context for the new lambda/thunk being compiled.
///
/// IMPORTANT: Call `take_captures()` before the guard is destroyed if you need
/// the captures list for building the closure environment.
class lambda_context_guard {
  compiler& compiler_;
  // Stored as pointer to optional to avoid including full compiler definition
  void* saved_context_;
  // Captures extracted before restore (for use after guard destruction)
  std::vector<ast::symbol> captured_symbols_;
  bool captures_taken_ = false;

public:
  /// Construct guard, saving current context and initializing a fresh one.
  /// @param c The compiler instance
  explicit lambda_context_guard(compiler& c);

  /// Restore the saved context.
  ~lambda_context_guard();

  /// Extract captures from the current context before restoration.
  /// Must be called before the guard is destroyed if you need the captures.
  /// Can only be called once.
  [[nodiscard]] auto take_captures() -> std::vector<ast::symbol>;

  /// Get the captures (after take_captures was called).
  [[nodiscard]] auto captures() const -> const std::vector<ast::symbol>& {
    return captured_symbols_;
  }

  // Non-copyable, non-movable
  lambda_context_guard(const lambda_context_guard&) = delete;
  auto operator=(const lambda_context_guard&) -> lambda_context_guard& = delete;
  lambda_context_guard(lambda_context_guard&&) = delete;
  auto operator=(lambda_context_guard&&) -> lambda_context_guard& = delete;
};

// =============================================================================
// Rec Bindings Guard
// =============================================================================

/// RAII guard for recursive attrset binding offsets.
/// Saves `current_rec_binding_offsets_` on construction, restores on destruction.
class rec_bindings_guard {
  compiler& compiler_;
  std::unordered_map<std::uint32_t, std::uint32_t> saved_offsets_;

public:
  /// Construct guard, saving current offsets and replacing with new ones.
  /// @param c The compiler instance
  /// @param new_offsets The new rec binding offsets to use
  rec_bindings_guard(compiler& c,
                     const std::unordered_map<std::uint32_t, std::uint32_t>& new_offsets);

  /// Construct guard, saving current offsets and clearing to empty.
  /// @param c The compiler instance
  explicit rec_bindings_guard(compiler& c);

  /// Restore the saved offsets.
  ~rec_bindings_guard();

  // Non-copyable, non-movable
  rec_bindings_guard(const rec_bindings_guard&) = delete;
  auto operator=(const rec_bindings_guard&) -> rec_bindings_guard& = delete;
  rec_bindings_guard(rec_bindings_guard&&) = delete;
  auto operator=(rec_bindings_guard&&) -> rec_bindings_guard& = delete;
};

// =============================================================================
// Let Bindings Guard
// =============================================================================

/// RAII guard for let binding offsets.
/// Saves `current_let_binding_offsets_` on construction, restores on destruction.
class let_bindings_guard {
  compiler& compiler_;
  std::unordered_map<std::uint32_t, std::uint32_t> saved_offsets_;

public:
  /// Construct guard, saving current offsets and replacing with new ones.
  /// @param c The compiler instance
  /// @param new_offsets The new let binding offsets to use
  let_bindings_guard(compiler& c,
                     const std::unordered_map<std::uint32_t, std::uint32_t>& new_offsets);

  /// Construct guard, saving current offsets and clearing to empty.
  /// @param c The compiler instance
  explicit let_bindings_guard(compiler& c);

  /// Restore the saved offsets.
  ~let_bindings_guard();

  // Non-copyable, non-movable
  let_bindings_guard(const let_bindings_guard&) = delete;
  auto operator=(const let_bindings_guard&) -> let_bindings_guard& = delete;
  let_bindings_guard(let_bindings_guard&&) = delete;
  auto operator=(let_bindings_guard&&) -> let_bindings_guard& = delete;
};

// =============================================================================
// With Scope Guard
// =============================================================================

/// RAII guard for `with` scope stack.
/// Pushes a with scope on construction, pops on destruction.
class with_scope_guard {
  compiler& compiler_;

public:
  /// Construct guard, pushing a new with scope.
  /// @param c The compiler instance
  /// @param namespace_local_index WASM local index holding the namespace attrset
  with_scope_guard(compiler& c, std::uint32_t namespace_local_index);

  /// Pop the with scope.
  ~with_scope_guard();

  // Non-copyable, non-movable
  with_scope_guard(const with_scope_guard&) = delete;
  auto operator=(const with_scope_guard&) -> with_scope_guard& = delete;
  with_scope_guard(with_scope_guard&&) = delete;
  auto operator=(with_scope_guard&&) -> with_scope_guard& = delete;
};

// =============================================================================
// Combined Guard for Thunk/Lambda Compilation
// =============================================================================

/// Combined RAII guard for entering a new compilation context (lambda or thunk).
/// Manages: scope, lambda_context, and optionally rec_bindings.
///
/// This is the typical pattern for compiling a lambda or thunk body:
/// 1. Save outer scope, set new scope
/// 2. Save outer lambda context, create fresh context
/// 3. Optionally save/set rec bindings (for rec thunks)
/// 4. Compile body
/// 5. Restore all state (handled by destructors)
class compilation_context_guard {
  scope_guard scope_;
  lambda_context_guard lambda_;
  std::optional<rec_bindings_guard> rec_;

public:
  /// Construct guard for lambda/thunk without rec bindings.
  compilation_context_guard(compiler& c, lexical_scope& new_scope);

  /// Construct guard for rec thunk with rec bindings.
  compilation_context_guard(compiler& c, lexical_scope& new_scope,
                            const std::unordered_map<std::uint32_t, std::uint32_t>& rec_offsets);

  // Destructor is default - member guards handle cleanup
  ~compilation_context_guard() = default;

  // Non-copyable, non-movable
  compilation_context_guard(const compilation_context_guard&) = delete;
  auto operator=(const compilation_context_guard&) -> compilation_context_guard& = delete;
  compilation_context_guard(compilation_context_guard&&) = delete;
  auto operator=(compilation_context_guard&&) -> compilation_context_guard& = delete;
};

} // namespace straylight::nix::compiler::compile
