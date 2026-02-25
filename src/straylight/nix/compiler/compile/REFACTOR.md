# Compiler Refactoring: From Afghanistan to Switzerland

## Executive Summary

The compiler (`compiler.h`) has become a 3400-line monolith with scattered mutable state, no RAII guards, unused abstractions, and runtime checks where compile-time enforcement is possible. The existing `DESIGN.md` describes the _correct_ architecture - but it was never fully implemented. Files like `scope.h`, `value.h`, and `capture.h` contain the right abstractions that are **not used**.

This document catalogs every design flaw with line numbers, then provides a concrete migration plan.

---

## Design Philosophy: Learn from libevring

The libevring library in this same codebase demonstrates how to **make illegal states unrepresentable at compile time**. Its `stable_ref<T>` / `stable_span<T>` pattern solves the pointer stability problem by:

1. **Private constructors** - Types can only be created through blessed paths
2. **API enforcement** - Operations _require_ the safe types (won't compile otherwise)
3. **Explicit opt-in** - Caller-owned data requires `make_stable_*()` factory functions

```cpp
// libevring: Won't compile - std::span has no implicit conversion to stable_span
make_read(handle, std::span{buffer});  // ERROR

// Must explicitly opt-in to stability guarantee
make_read(handle, make_stable_span(machine_buffer_));  // OK
```

We should apply the same pattern to the compiler:

```cpp
// Current: Runtime check, compiles but crashes
if (!current_lambda_context_.has_value()) throw compilation_error("...");

// Target: Won't compile without lambda context
auto compile_let(compiler_ctx&, scope_ctx, lambda_context&, const ast::expression_let&) -> maybe_value;
//                                         ^^^^^^^^^^^^^^^ required parameter
```

The goal is to shift invariant checking from runtime to compile-time wherever possible.

---

## Flaw Catalog

### 1. SCATTERED MUTABLE STATE (No RAII)

The compiler class has **11 mutable state fields** with implicit save/restore semantics:

| Field                          | Line | Purpose                      | Problem                                        |
| ------------------------------ | ---- | ---------------------------- | ---------------------------------------------- |
| `current_scope_`               | 528  | Raw pointer to lexical scope | No RAII guard; early return = corruption       |
| `current_lambda_context_`      | 547  | Optional lambda context      | save/restore via `std::move` at lines 757, 803 |
| `current_rec_binding_offsets_` | 559  | rec attrset symbol→offset    | save/restore at lines 1047, 1054               |
| `current_let_binding_offsets_` | 564  | let binding symbol→offset    | save/restore at lines 2887-2889                |
| `with_scopes_`                 | 554  | Dynamic `with` scope stack   | push/pop without guard                         |
| `data_offset_`                 | 525  | Data segment high water mark | Mutation interleaved with codegen              |
| `lambda_counter_`              | 531  | Lambda function index        | Monotonic but global                           |
| `thunk_counter_`               | 535  | Thunk function index         | Monotonic but global                           |
| `lambda_function_names_`       | 532  | Function table names         | Append-only                                    |
| `thunk_function_names_`        | 536  | Function table names         | Append-only                                    |
| `string_offsets_`              | 524  | String intern cache          | Append-only                                    |

**Evidence of the problem** (lines 757-803, 896-940, 1020-1072):

```cpp
// Save
auto outer_lambda_context = std::move(current_lambda_context_);
current_lambda_context_ = lambda_context{};
// ... code that might throw or early return ...
// Restore (if we get here)
current_lambda_context_ = std::move(outer_lambda_context);
```

If _any_ code path between save and restore throws or returns, state is corrupted.

---

### 2. UNUSED TYPE-SAFE ABSTRACTIONS

The `DESIGN.md` describes and `scope.h`, `value.h`, `capture.h` implement:

| Abstraction                     | File              | Status                                                    |
| ------------------------------- | ----------------- | --------------------------------------------------------- |
| `scope_ctx`                     | `scope.h:134-206` | **UNUSED** - compiler still uses `current_scope_` pointer |
| `var_ref`, `var_location`       | `scope.h:52-107`  | **UNUSED** - compiler uses `variable_binding` struct      |
| `binding_entry`                 | `scope.h:114-117` | **UNUSED**                                                |
| `forced_value`, `maybe_value`   | `value.h:117-122` | **UNUSED** - compiler returns raw `BinaryenExpressionRef` |
| `wasm_value<Tag>`               | `value.h:70-113`  | **UNUSED**                                                |
| `make_forced()`, `make_maybe()` | `value.h:130-138` | **UNUSED**                                                |
| `force()`                       | `value.h:153-158` | **UNUSED** - compiler calls `compile_force()` directly    |
| `capture_policy` enum           | `capture.h:41-66` | **PARTIALLY USED** - in function signatures only          |
| `should_force_captures()`       | `capture.h:70-78` | Used but only as adapter to old `bool` code               |

The compiler still uses:

- `bool force_value = true` parameters (line 1180)
- Raw `BinaryenExpressionRef` returns (line 716)
- Manual scope pointer management (line 528)

---

### 3. TYPE-UNSAFE MAPS

```cpp
// compiler.h:559 - uses raw uint32_t instead of ast::symbol
std::unordered_map<std::uint32_t, std::uint32_t> current_rec_binding_offsets_;

// compiler.h:564 - same issue
std::unordered_map<std::uint32_t, std::uint32_t> current_let_binding_offsets_;
```

Should be:

```cpp
std::unordered_map<ast::symbol, std::uint32_t, symbol_hash> current_rec_binding_offsets_;
```

**Why it matters**: Easy to accidentally pass wrong uint32_t (capture index, local index, symbol index - all uint32_t).

---

### 4. RUNTIME CHECKS FOR COMPILE-TIME INVARIANTS

| Line      | Runtime Check                               | Should Be                             |
| --------- | ------------------------------------------- | ------------------------------------- |
| 2826-2827 | `if (current_lambda_context_.has_value())`  | Function that takes `lambda_context&` |
| 2856-2857 | Same for inherit bindings                   | Same                                  |
| 3124-3125 | `with expressions require function context` | Same                                  |
| 2797-2805 | Dynamic let binding name check              | AST type that forbids dynamic         |

Pattern repeated 4+ times:

```cpp
if (current_lambda_context_.has_value()) {
  local_index = current_lambda_context_->next_local_index++;
} else {
  throw compilation_error("top-level X expressions not supported");
}
```

**Fix**: Functions that need lambda context should take `lambda_context&`, not check optional.

---

### 5. MAGIC STRINGS

| Line                                                  | String                             | Purpose              |
| ----------------------------------------------------- | ---------------------------------- | -------------------- |
| 474, 483                                              | `"main"`                           | Main function export |
| 510, 512                                              | `"__lambda_count"`                 | Lambda count global  |
| 569, 636, 642, 653, 659, 666, 677, 686, 689, 695, 703 | `"__add"`, `"__select"`, etc.      | Builtin imports      |
| 732, 874, 999                                         | `"__thunk_" + std::to_string(...)` | Thunk function names |

**Fix**: Centralize in `constexpr` strings or strong types.

---

### 6. MONOLITHIC FILE

`compiler.h` contains:

- `symbol_hash` (23-28)
- `compilation_error` (30-34)
- `variable_location`, `variable_binding` (36-50)
- `lexical_scope` class (52-129)
- `free_variable_analyzer` class (131-378)
- `wasm_module` RAII wrapper (380-437)
- `compiler` class (439-3398)

**Should be**:

```
compile/
├── error.h           # compilation_error
├── lexical_scope.h   # lexical_scope, variable_binding
├── free_vars.h       # free_variable_analyzer
├── wasm_module.h     # wasm_module RAII wrapper
├── compiler.h        # compiler class (< 1000 lines)
├── context.h         # compiler_ctx, scope_ctx (from existing scope.h)
├── value.h           # wasm_value<Tag> (exists, unused)
├── capture.h         # capture_policy (exists, partially used)
└── expr/             # per-expression compilation functions
    ├── literal.h
    ├── identifier.h
    ├── lambda.h
    ├── attrset.h
    ├── let.h
    └── ...
```

---

### 7. INCOMPLETE IMPLEMENTATION (Dead Code)

```cpp
// compiler.h:2452-2456
// TODO: use has_attr_is_true to short-circuit when hasAttr returns false
(void)has_attr_is_true;  // DEAD CODE - variable computed but unused
```

---

### 8. PHASE CONFUSION

Data segment allocation (`data_offset_`) is interleaved with WASM codegen:

```cpp
// compiler.h:823-825
auto env_offset = data_offset_;      // compile-time state
data_offset_ += env_size;            // compile-time mutation
data_offset_ = (data_offset_ + 7) & ~7u;
// ... then immediately:
thunk_setup.push_back(BinaryenStore(...));  // WASM codegen
```

Should separate allocation (pure, returns offset) from codegen (uses offset).

---

## Migration Plan

### Phase 1: Add RAII Guards (Non-Breaking)

Create guards for all save/restore patterns:

```cpp
// compile/guard.h
class scope_guard {
  compiler& c_;
  lexical_scope* saved_;
public:
  scope_guard(compiler& c, lexical_scope& scope) : c_(c), saved_(c.current_scope_) {
    c.current_scope_ = &scope;
  }
  ~scope_guard() { c_.current_scope_ = saved_; }
  scope_guard(const scope_guard&) = delete;
  auto operator=(const scope_guard&) -> scope_guard& = delete;
};

class lambda_context_guard {
  compiler& c_;
  std::optional<lambda_context> saved_;
public:
  lambda_context_guard(compiler& c) : c_(c), saved_(std::move(c.current_lambda_context_)) {
    c.current_lambda_context_ = lambda_context{};
  }
  ~lambda_context_guard() { c_.current_lambda_context_ = std::move(saved_); }
};

class rec_bindings_guard { /* ... */ };
class let_bindings_guard { /* ... */ };
class with_scope_guard { /* ... */ };
```

**Apply guards to all save/restore sites** - purely mechanical, behavior unchanged.

### Phase 2: Use Existing Abstractions

Wire up the unused types in `value.h`:

```cpp
// Before (compiler.h:716-718)
[[nodiscard]] auto compile_expression(const ast::expression& expr) -> BinaryenExpressionRef;

// After
[[nodiscard]] auto compile_expression(const ast::expression& expr) -> maybe_value;
```

```cpp
// Before (compiler.h:1124)
[[nodiscard]] auto compile_variant(const ast::expression_integer& expr) -> BinaryenExpressionRef;

// After
[[nodiscard]] auto compile_variant(const ast::expression_integer& expr) -> forced_value;
```

### Phase 3: Introduce `compiler_ctx` and `scope_ctx`

Replace mutable member state with explicit context passing:

```cpp
// compile/context.h
struct compiler_ctx {
  BinaryenModuleRef module;
  const ast::symbol_table& symbols;

  // Output state (append-only)
  std::uint32_t data_offset = 0;
  std::uint32_t lambda_counter = 0;
  std::uint32_t thunk_counter = 0;
  std::vector<std::string> lambda_names{};
  std::vector<std::string> thunk_names{};
  std::unordered_map<ast::symbol, std::uint32_t, symbol_hash> string_cache{};

  [[nodiscard]] auto alloc_data(std::uint32_t size, std::uint32_t align = 8) -> std::uint32_t;
  [[nodiscard]] auto alloc_string(std::string_view s) -> std::uint32_t;
  [[nodiscard]] auto register_lambda(std::string name) -> std::uint32_t;
  [[nodiscard]] auto register_thunk(std::string name) -> std::uint32_t;
};
```

Use existing `scope_ctx` from `scope.h`:

```cpp
// Function signature change
[[nodiscard]] auto compile_let(compiler_ctx& ctx, scope_ctx scope,
                               const ast::expression_let& expr) -> maybe_value;
```

### Phase 4: Extract Expression Compilers

Move each `compile_variant` overload to `expr/*.h`:

```cpp
// compile/expr/literal.h
namespace straylight::nix::compiler::compile::expr {

[[nodiscard]] auto integer(compiler_ctx& ctx, const ast::expression_integer& e) -> forced_value;
[[nodiscard]] auto floating(compiler_ctx& ctx, const ast::expression_float& e) -> forced_value;
[[nodiscard]] auto string(compiler_ctx& ctx, const ast::expression_string& e) -> forced_value;

}

// compile/expr/let.h
namespace straylight::nix::compiler::compile::expr {

[[nodiscard]] auto let(compiler_ctx& ctx, scope_ctx scope,
                       const ast::expression_let& e) -> maybe_value;

}
```

### Phase 5: Strong Types for Host Functions

```cpp
// compile/builtins.h
namespace builtins {

// Strong type for function import names
struct import_name {
  const char* module;
  const char* name;
};

inline constexpr import_name add{"builtins", "__add"};
inline constexpr import_name sub{"builtins", "__sub"};
inline constexpr import_name force{"runtime", "__force"};
// ...

}
```

### Phase 6: Compile-Time Invariants

For functions that require lambda context:

```cpp
// Before: runtime check
[[nodiscard]] auto compile_let(...) {
  if (!current_lambda_context_.has_value()) {
    throw compilation_error("...");
  }
  // ...
}

// After: compile-time enforcement
[[nodiscard]] auto compile_let(compiler_ctx& ctx, scope_ctx scope,
                               lambda_context& lambda_ctx,  // REQUIRED
                               const ast::expression_let& e) -> maybe_value;
```

---

## File Structure After Refactoring

```
compile/
├── DESIGN.md           # Existing - philosophy and concepts
├── REFACTOR.md         # This document
│
├── error.h             # compilation_error
├── wasm_module.h       # wasm_module RAII wrapper
├── guard.h             # scope_guard, lambda_context_guard, etc.
│
├── context.h           # compiler_ctx (mutable output)
├── scope.h             # scope_ctx (immutable input) - EXISTS, wire up
├── value.h             # forced_value, maybe_value - EXISTS, wire up
├── capture.h           # capture_policy - EXISTS, fully use
│
├── lexical_scope.h     # lexical_scope, variable_binding
├── free_vars.h         # free_variable_analyzer
├── builtins.h          # Strong types for host function names
│
├── thunk.h             # thunk::build() and helpers
├── lambda.h            # lambda compilation logic
│
├── expr/
│   ├── dispatch.h      # expr::compile() - main visitor
│   ├── literal.h       # integer, float, string, path
│   ├── identifier.h    # identifier lookup
│   ├── lambda.h        # lambda expressions
│   ├── attrset.h       # attribute sets (rec and non-rec)
│   ├── let.h           # let expressions
│   ├── with.h          # with expressions
│   ├── control.h       # if, assert
│   ├── binary.h        # binary operations
│   ├── unary.h         # unary operations
│   ├── list.h          # list expressions
│   └── select.h        # attribute selection, hasAttr
│
└── compiler.h          # Public API: compiler class (thin wrapper)
```

---

## Invariants to Enforce

### Compile-Time (Type System) - Following libevring's Pattern

| Invariant                      | libevring Equivalent                            | Compiler Implementation                        |
| ------------------------------ | ----------------------------------------------- | ---------------------------------------------- |
| **Value provenance**           | `stable_span<T>` vs `std::span<T>`              | `forced_value` vs `maybe_value`                |
| **Capture policy**             | N/A                                             | `capture_policy` enum (exhaustive switch)      |
| **Scope immutability**         | Machine vs State separation                     | `scope_ctx` passed by value                    |
| **Context separation**         | `machine_storage<T>` (stable) vs state (copied) | `compiler_ctx` (output) vs `scope_ctx` (input) |
| **Lambda context requirement** | Operations require `stable_*` types             | Functions take `lambda_context&`               |

**The libevring insight**: Make the _type system_ enforce the invariant. If you can write code that violates the invariant, the design is wrong.

```cpp
// libevring pattern:
class stable_span {
  // Private constructor - can't create from raw pointer
  constexpr stable_span(stable_span_tag_t, T* d, std::size_t s);
};

// Compiler pattern (target):
class forced_value {
  // Private constructor - can't create from raw BinaryenExpressionRef
  explicit constexpr forced_value(BinaryenExpressionRef e);
  friend auto force(BinaryenModuleRef, maybe_value) -> forced_value;  // Only way in
};
```

### Runtime (Must Remain)

1. **Thunk state machine**: PENDING → EVALUATING → EVALUATED
2. **Memory layout**: Validated by `static_assert` in `memory_layout.h`
3. **Type tags**: Runtime dispatch for dynamic Nix types

---

## Test Coverage Gaps to Fill

| Gap                          | New Test File             |
| ---------------------------- | ------------------------- |
| RAII guards                  | `guard_test.cpp`          |
| Scope save/restore           | `scope_ctx_test.cpp`      |
| forced_value/maybe_value     | `value_test.cpp`          |
| Nested lambdas with captures | `lambda_capture_test.cpp` |
| Let binding thunks           | `let_thunk_test.cpp`      |
| Rec attrset thunks           | `rec_thunk_test.cpp`      |
| Error paths                  | `compile_error_test.cpp`  |

---

## Migration Order

1. **guard.h** - Add RAII guards, apply everywhere (no behavior change)
2. **Wire up value.h** - Change return types to `forced_value`/`maybe_value`
3. **Wire up scope.h** - Replace member state with `scope_ctx` parameter
4. **Extract wasm_module.h, error.h** - Mechanical moves
5. **Extract free_vars.h, lexical_scope.h** - Mechanical moves
6. **Add builtins.h** - Strong types for magic strings
7. **Extract expr/\*.h** - One expression type at a time
8. **Add context.h** - Replace remaining member state with `compiler_ctx`
9. **Slim compiler.h** - Should be < 300 lines

Each step is a single PR with unchanged test behavior.
