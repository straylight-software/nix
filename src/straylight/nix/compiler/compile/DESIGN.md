# Compiler Core Design

## Philosophy

**Make illegal states unrepresentable at compile time.**

Every invariant that can be enforced by the C++23 type system MUST be. Runtime checks are failures
of imagination.

## Core Abstractions

### 1. Value Provenance

A Nix value during compilation can be in one of two states:

- **Forced**: Definitely not a thunk. Safe to use directly.
- **MaybeThunk**: Could be a thunk. Must be forced before use in strict contexts.

```cpp
// A WASM expression that produces a nix_value
// The template parameter encodes whether the value might be a thunk
template <bool IsForced>
struct wasm_value {
  BinaryenExpressionRef expr;
  
  // Implicit conversion: forced -> maybe_thunk is always safe
  constexpr operator wasm_value<false>() const noexcept requires IsForced {
    return {.expr = expr};
  }
};

using forced_value = wasm_value<true>;   // Guaranteed not a thunk
using maybe_value = wasm_value<false>;   // Might be a thunk

// The ONLY way to convert maybe -> forced
[[nodiscard]] auto force(compiler_ctx&, maybe_value) -> forced_value;
```

### 2. Capture Policy

When creating a thunk, we capture variables from the enclosing scope. The capture policy determines
whether those captures are forced at capture-time or stored as-is.

```cpp
// Strong enum - no implicit conversions, exhaustive switch required
enum class capture_policy : std::uint8_t {
  // Force captures at thunk creation time.
  // Use when: thunk is for lazy evaluation of a non-recursive expression
  force_eager,
  
  // Store captures without forcing (may be thunks themselves).
  // Use when: thunk is in a recursive context (let, rec, inherit-from)
  preserve_lazy,
};
```

**The invariant**: In recursive contexts, captures MUST use `preserve_lazy` to avoid infinite
recursion during thunk construction.

### 3. Variable Location

Variables are stored in different places depending on their binding context:

```cpp
enum class var_location : std::uint8_t {
  wasm_local,      // WASM local variable (lambda params, temporaries)
  captured,        // Closure environment (free vars in lambdas/thunks)
  let_memory,      // Shared memory (let bindings - mutually recursive)
  rec_memory,      // Shared memory (rec attrset bindings)
  with_dynamic,    // Dynamic lookup via __hasAttr/__select chain
};
```

### 4. Scope Context (Immutable)

All scope information is passed explicitly, never read from mutable class state.

```cpp
struct scope_ctx {
  // Lexical scope chain (lambda params, local vars)
  const lexical_scope* scope = nullptr;
  
  // Shared memory bindings (immutable spans, not owned)
  std::span<const std::pair<ast::symbol, uint32_t>> let_bindings{};
  std::span<const std::pair<ast::symbol, uint32_t>> rec_bindings{};
  
  // With scope stack
  std::span<const uint32_t> with_locals{};
  
  // Query: where is this variable?
  [[nodiscard]] auto locate(ast::symbol) const -> std::optional<var_ref>;
  
  // Builders: create child contexts (return new immutable structs)
  [[nodiscard]] auto with_scope(const lexical_scope&) const -> scope_ctx;
  [[nodiscard]] auto with_let(std::span<const std::pair<ast::symbol, uint32_t>>) const -> scope_ctx;
  [[nodiscard]] auto with_rec(std::span<const std::pair<ast::symbol, uint32_t>>) const -> scope_ctx;
  [[nodiscard]] auto with_with(std::span<const uint32_t>) const -> scope_ctx;
};
```

### 5. Compiler Context (Mutable Output Only)

The compiler context holds output-only state: allocations, function registrations, etc.

```cpp
struct compiler_ctx {
  // Module being built
  BinaryenModuleRef module;
  
  // Symbol table (read-only reference)
  const ast::symbol_table& symbols;
  
  // Output state (append-only)
  uint32_t data_offset = 0;
  uint32_t lambda_counter = 0;
  uint32_t thunk_counter = 0;
  std::vector<std::string> lambda_names{};
  std::vector<std::string> thunk_names{};
  std::unordered_map<uint32_t, uint32_t> string_cache{};
  
  // Allocation
  [[nodiscard]] auto alloc_data(uint32_t size, uint32_t align = 8) -> uint32_t;
  [[nodiscard]] auto alloc_string(std::string_view) -> uint32_t;
  
  // Registration
  [[nodiscard]] auto register_lambda(std::string name) -> uint32_t;
  [[nodiscard]] auto register_thunk(std::string name) -> uint32_t;
};
```

## Expression Compilation

Each expression type maps to a pure function:

```cpp
namespace expr {

// Returns maybe_value because identifier lookup might return a thunk
[[nodiscard]] auto compile_identifier(compiler_ctx&, scope_ctx, 
                                      const ast::expression_identifier&) -> maybe_value;

// Literals are always forced (they're immediate values)
[[nodiscard]] auto compile_integer(compiler_ctx&, const ast::expression_integer&) -> forced_value;
[[nodiscard]] auto compile_float(compiler_ctx&, const ast::expression_float&) -> forced_value;
[[nodiscard]] auto compile_string(compiler_ctx&, const ast::expression_string&) -> forced_value;

// Lambdas are values (closures), not thunks
[[nodiscard]] auto compile_lambda(compiler_ctx&, scope_ctx,
                                  const ast::expression_lambda&) -> forced_value;

// Attrsets are immediate values containing (possibly thunked) attributes
[[nodiscard]] auto compile_attrset(compiler_ctx&, scope_ctx,
                                   const ast::expression_attribute_set&) -> forced_value;

// Let returns the body, which might be a thunk
[[nodiscard]] auto compile_let(compiler_ctx&, scope_ctx,
                               const ast::expression_let&) -> maybe_value;

// Binary ops force operands (for arithmetic) or short-circuit (for logic)
[[nodiscard]] auto compile_binary(compiler_ctx&, scope_ctx,
                                  const ast::expression_binary_operation&) -> maybe_value;

// Main dispatch
[[nodiscard]] auto compile(compiler_ctx&, scope_ctx, const ast::expression&) -> maybe_value;

} // namespace expr
```

## Thunk Building

Single entry point for all thunk creation:

```cpp
namespace thunk {

struct build_params {
  const ast::expression& body;
  scope_ctx scope;
  capture_policy policy;
  std::string_view debug_name;  // For error messages / debugging
};

// The ONLY way to create a thunk
[[nodiscard]] auto build(compiler_ctx&, build_params) -> forced_value;

// Internal: analyze what needs capturing
[[nodiscard]] auto analyze_captures(const ast::expression&, scope_ctx) 
    -> std::vector<ast::symbol>;

// Internal: emit capture stores according to policy
[[nodiscard]] auto emit_capture_stores(compiler_ctx&, scope_ctx,
                                       std::span<const ast::symbol> captures,
                                       capture_policy policy,
                                       uint32_t env_offset) 
    -> std::vector<BinaryenExpressionRef>;

} // namespace thunk
```

## Invariant Enforcement

### Compile-Time

1. **Value provenance**: `forced_value` vs `maybe_value` types prevent passing unforced values where
   forced required
2. **Capture policy**: Enum forces exhaustive handling, no bool confusion
3. **Scope immutability**: `scope_ctx` is passed by value, modifications return new instances
4. **No mutable global state**: All state either in `compiler_ctx` (output) or `scope_ctx` (input)

### Runtime

1. **Thunk state machine**: PENDING → EVALUATING → EVALUATED (infinite recursion =
   EVALUATING→EVALUATING)
2. **Memory layout**: Static `constexpr` offsets with `static_assert` validation

## Migration

### Phase 1: Add Types

- Add `wasm_value<bool>`, `capture_policy`, `scope_ctx`, `compiler_ctx`
- Add `thunk::build()` wrapping existing logic
- Add `expr::*` functions delegating to existing `compile_variant`

### Phase 2: Convert

- One expression type at a time
- Each PR converts one `compile_variant` overload to `expr::compile_*`
- Tests unchanged (behavioral equivalence)

### Phase 3: Remove Old API

- Delete `compile_variant` overloads
- Delete `force_captures` parameter
- Delete mutable state from `compiler` class

## File Structure

```
compile/
├── compiler.h          # Public API: compiler class
├── context.h           # compiler_ctx, scope_ctx
├── value.h             # wasm_value<bool>, forced_value, maybe_value
├── capture.h           # capture_policy, analyze_captures
├── thunk.h             # thunk::build()
├── expr/
│   ├── dispatch.h      # expr::compile() main dispatch
│   ├── literal.h       # integer, float, string, path
│   ├── identifier.h    # identifier lookup
│   ├── lambda.h        # lambda compilation
│   ├── attrset.h       # attribute set (rec and non-rec)
│   ├── let.h           # let expressions
│   ├── binary.h        # binary operations
│   ├── control.h       # if, assert, with
│   └── list.h          # list expressions
└── scope.h             # lexical_scope, var_location
```
