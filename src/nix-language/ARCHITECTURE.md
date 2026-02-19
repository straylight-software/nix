# nix-language Architecture

A world-class C++23 implementation of the Nix expression language, designed for ahead-of-time (AOT) compilation to WebAssembly.

**License Note**: The PEGTL grammar (`parse/grammar.hh`) is substantially derived from the Lix project (LGPL-2.1).

---

## Design Philosophy

1. **Modern C++23**: Variants over inheritance, `[[nodiscard]]`, concepts, spaceship operator, RAII everywhere
2. **AOT Compilation**: Compile Nix expressions to WASM instead of tree-walking interpretation
3. **Clean Separation**: Parse → AST → Compile pipeline with well-defined intermediate representations
4. **Header-Only Where Sensible**: Most modules are header-only for simplicity and inlining

---

## Module Overview

```
nix-language/
├── ast/                    # Abstract Syntax Tree types
│   ├── expression.hh       # Core expression types (variant-based)
│   └── symbol_table.hh     # String interning for identifiers
├── parse/                  # Parsing pipeline
│   ├── grammar.hh          # PEGTL grammar rules (from Lix, LGPL-2.1)
│   ├── selector.hh         # Parse tree node selection
│   ├── tree.hh             # Intermediate tree types
│   ├── convert.hh          # PEGTL tree → our tree
│   ├── lower.hh            # Our tree → AST
│   ├── parser.hh           # Main parse() entry point
│   ├── actions.hh          # Alternative PEGTL actions (direct AST build)
│   └── state.hh            # Parser state utilities
├── compile/                # WASM compilation
│   ├── compiler.hh         # AST → WASM via Binaryen (~2700 lines)
│   └── wasm_types.hh       # WASM value representation
├── eval/                   # Tree-walking interpreter (reference impl)
│   ├── eval.hh             # Evaluator (~1500 lines)
│   └── value.hh            # Runtime value types
├── runtime/                # WASM execution runtime
│   ├── memory_layout.hh    # Memory layout constants (shared w/ compiler)
│   ├── runtime.hh          # Runtime context and value operations
│   ├── runtime.cpp         # Runtime function implementations
│   ├── wasm_executor.hh    # Wasmtime-based WASM executor
│   └── wasm_executor.cpp   # Executor implementation
├── cli/                    # Command-line tools
│   └── nix_eval.cpp        # REPL and file evaluator
└── tests/                  # Test suite (~8200 lines)
    ├── ast_test.cpp        # AST type construction/invariants
    ├── grammar_test.cpp    # PEGTL grammar unit tests
    ├── parse_test.cpp      # Parse → AST round-trips
    ├── wasm_types_test.cpp # Value packing/unpacking
    ├── compiler_test.cpp   # Compilation + validation
    ├── eval_test.cpp       # Interpreter correctness
    ├── integration_test.cpp # Full pipeline (parse → compile → WASM)
    ├── execution_test.cpp  # End-to-end with wasmtime execution
    ├── runtime_test.cpp    # Direct runtime function testing
    ├── property_test.cpp   # Property-based tests (RapidCheck)
    ├── adversarial_test.cpp # Edge cases, boundary conditions
    ├── bench.cpp           # Benchmarks (nanobench)
    ├── fuzz_parse.cpp      # libFuzzer harness for parser
    └── fuzz_compile.cpp    # libFuzzer harness for compiler
```

---

## 1. AST Layer (`ast/`)

### `expression.hh` - Core Types

The AST uses `std::variant` for expression types, avoiding inheritance hierarchies:

```cpp
using expression_variant = std::variant<
    expression_identifier,     // x
    expression_integer,        // 42
    expression_float,          // 3.14
    expression_string,         // "hello"
    expression_string_interpolated,  // "hello ${name}"
    expression_path,           // ./foo/bar
    expression_path_interpolated,    // ./foo/${name}
    expression_list,           // [ 1 2 3 ]
    expression_attribute_set,  // { a = 1; }
    expression_select,         // x.y.z or default
    expression_has_attribute,  // x ? y
    expression_lambda,         // x: x + 1
    expression_application,    // f x y
    expression_let,            // let x = 1; in x
    expression_with,           // with pkgs; [ foo ]
    expression_if,             // if c then a else b
    expression_assert,         // assert c; e
    expression_binary_operation,
    expression_unary_operation
>;

struct expression_node {
    expression_variant data;
};

using expression = std::unique_ptr<expression_node>;
```

**Key Design Decisions**:
- `unique_ptr<expression_node>` for heap allocation (expressions can be large)
- Forward declaration pattern enables recursive types
- `source_position` embedded in every expression for error reporting
- Binary/unary operators as enums, not separate types

### `symbol_table.hh` - String Interning

```cpp
struct symbol {
    std::uint32_t index;  // Index into symbol table
};

class symbol_table {
    std::deque<std::string> strings_;  // Stable references
    std::unordered_map<std::string_view, std::uint32_t> index_;
public:
    auto intern(std::string_view text) -> symbol;
    auto lookup(symbol s) const -> std::string_view;
};
```

**Why `std::deque`**: Provides stable references - strings don't move when new ones are added, allowing `string_view` keys in the index map.

---

## 2. Parse Layer (`parse/`)

The parsing pipeline has two alternative paths:

### Path A: Three-Stage Pipeline (Primary)

```
Source Text
    │
    ▼
┌─────────────────────┐
│   PEGTL parse       │  grammar.hh + selector.hh
│   (parse_tree)      │
└─────────────────────┘
    │
    ▼
┌─────────────────────┐
│   convert()         │  convert.hh
│   PEGTL tree → tree │
└─────────────────────┘
    │
    ▼
┌─────────────────────┐
│   lower()           │  lower.hh
│   tree → AST        │
└─────────────────────┘
    │
    ▼
  ast::expression
```

### Path B: Direct Actions (Alternative)

```
Source Text
    │
    ▼
┌─────────────────────┐
│   PEGTL parse       │  grammar.hh + actions.hh
│   with actions      │  (builds AST directly)
└─────────────────────┘
    │
    ▼
  ast::expression
```

### `grammar.hh` - PEGTL Grammar

Adapted from Lix (LGPL-2.1). Handles Nix's notoriously tricky lexical grammar:

```cpp
// Keywords that aren't keywords if followed by path/uri continuation
template <typename S>
struct keyword_ : p::sor<
    p::seq<S, p::not_at<character::identifier_rest>,
           p::not_at<extend_as_path_>, p::not_at<extend_as_uri_>>,
    p::failure
> {};

// The "or" keyword appears in both select expressions and as identifier
struct keyword_or : keyword_<TAO_PEGTL_STRING("or")> {};
```

**Lexical Challenges Handled**:
- `./foo/bar` (path) vs `a/b` (division)
- `http://example.com` (URI literal)
- `or` as keyword vs identifier
- Indented strings with `''` delimiters
- String interpolation `${expr}`

### `tree.hh` - Intermediate Tree

Simple data structures with no templates or SFINAE:

```cpp
struct node_integer { std::int64_t value; source_span span; };
struct node_string { std::string value; source_span span; };
struct node_binary_op { binary_op_kind op; tree left; tree right; source_span span; };
// ... etc

using tree_variant = std::variant<
    node_integer, node_float, node_string, /* ... */
>;

struct tree_node {
    tree_variant data;
};
using tree = std::unique_ptr<tree_node>;
```

### `convert.hh` - PEGTL → Tree

Walks PEGTL's parse tree once, building our tree types:
- Extracts literal values from source spans
- Handles operator precedence climbing for binary expressions
- Collects list elements, bindings, formals

### `lower.hh` - Tree → AST

Final lowering pass:
- Interns symbol names via `symbol_table`
- Converts `source_span` to `ast::source_position`
- Transforms tree nodes to AST expression types

---

## 3. Compile Layer (`compile/`)

### `wasm_types.hh` - Value Representation

Nix values are represented as tagged unions:

```cpp
// Packed into i64: low 32 bits = tag, high 32 bits = payload
enum class value_tag : std::uint8_t {
    null_value = 0,
    boolean = 1,      // payload: 0 or 1
    integer = 2,      // payload: value (truncated) or pointer
    floating = 3,     // payload: pointer to f64
    string = 4,       // payload: pointer to string data
    path = 5,         // payload: pointer to path data
    list = 6,         // payload: pointer to list header
    attribute_set = 7,// payload: pointer to attrset data
    lambda = 8,       // payload: func index or closure pointer
    thunk = 9,        // payload: funcref (lazy evaluation)
    primop = 10,      // payload: builtin function index
};
```

**Memory Layout**:
```
String:  [length: i32][data: bytes...]
List:    [length: i32][elements: nix_value*...]
Attrset: [count: i32][entries: (hash, value_ptr, name_ptr)...]
Closure: [func_index: i32][capture_count: i32][captures: nix_value...]
```

### `compiler.hh` - The Compiler

~2700 lines of WASM codegen via Binaryen. Key components:

#### Lexical Scope Management

```cpp
class lexical_scope {
    lexical_scope* parent_;
    std::uint32_t depth_;
    std::vector<variable_binding> variables_;
public:
    auto add_local(ast::symbol name, std::uint32_t local_index) -> std::uint32_t;
    auto add_captured(ast::symbol name, std::uint32_t capture_index) -> std::uint32_t;
    auto lookup(ast::symbol name) const -> std::optional<std::pair<variable_binding, depth>>;
};
```

#### Free Variable Analysis

```cpp
class free_variable_analyzer {
    std::unordered_set<ast::symbol> bound_;
    std::unordered_set<ast::symbol> free_;
public:
    static auto analyze(const ast::expression& expr,
                       const std::vector<ast::symbol>& bound_names)
        -> std::vector<ast::symbol>;
};
```

#### Closure Compilation

Lambdas are compiled to:
1. A WASM function `__lambda_N(env_ptr: i32, arg: nix_value) -> nix_value`
2. A closure struct with captured variables
3. Indirect calls via function table

#### Runtime Imports

The compiler imports host functions split across two WASM modules:

**`runtime` module** - Core runtime operations:
```cpp
__throw(msg_offset: i32, line: i32, col: i32) -> i64  // with position
__force(value: i64) -> i64                            // evaluate thunks
__apply(fn: i64, arg: i64) -> i64                     // function application
__lookupVar(name_offset: i32) -> i64                  // dynamic var lookup
__makeClosure(func_index: i32, env_offset: i32, env_size: i32) -> i64
__makeThunk(func_index: i32, env_offset: i32, env_size: i32) -> i64
```

**`builtins` module** - Operations on values:
```cpp
// Arithmetic (with source position for type error reporting)
__add(a: i64, b: i64, line: i32, col: i32) -> i64
__sub(a: i64, b: i64, line: i32, col: i32) -> i64
__mul(a: i64, b: i64, line: i32, col: i32) -> i64
__div(a: i64, b: i64, line: i32, col: i32) -> i64
__negate(v: i64) -> i64

// Comparison (no position needed)
__lessThan(a: i64, b: i64) -> i64
__lessEq(a: i64, b: i64) -> i64
__eq(a: i64, b: i64) -> i64
__neq(a: i64, b: i64) -> i64

// Boolean
__not(v: i64) -> i64
__isBool(v: i64) -> i32

// Collections
__makeList(offset: i32, count: i32) -> i64
__makeAttrs(offset: i32, count: i32) -> i64
__makeAttrsDynamic(offset: i32, count: i32) -> i64
__select(set: i64, key_offset: i32, line: i32, col: i32) -> i64
__selectDynamic(set: i64, key: i64, line: i32, col: i32) -> i64
__hasAttr(set: i64, key_offset: i32) -> i64
__hasAttrDynamic(set: i64, key: i64) -> i64
__update(a: i64, b: i64) -> i64
__concat(a: i64, b: i64) -> i64

// String operations
__toString(v: i64) -> i64
__concatStrings(offset: i32, count: i32) -> i64
```

#### Compilation Example

```nix
let x = 1; y = 2; in x + y
```

Compiles to (conceptually):
```wasm
(func $main (result i64)
    (local $x i64)
    (local $y i64)
    
    ;; x = 1
    (local.set $x (i64.const 0x100000002))  ;; tag=2 (int), value=1
    
    ;; y = 2  
    (local.set $y (i64.const 0x200000002))  ;; tag=2 (int), value=2
    
    ;; x + y
    (call $__add (local.get $x) (local.get $y))
)
```

---

## 4. Runtime Layer (`runtime/`)

The runtime provides host-side implementations of the functions imported by compiled WASM modules, plus a wasmtime-based executor.

### `memory_layout.hh` - Shared Constants

Constants for memory layout shared between compiler and runtime:

```cpp
namespace memory_layout {
  constexpr std::uint32_t DATA_SEGMENT_LIMIT = 0x10000;  // 64 KB
  constexpr std::uint32_t HEAP_BASE = 0x20000;           // 128 KB
  constexpr std::uint32_t DEFAULT_MEMORY_SIZE = 0x100000; // 1 MB
  constexpr std::uint32_t ALIGNMENT = 8;
  
  // Structure sizes and offsets
  constexpr std::uint32_t THUNK_SIZE = 20;
  constexpr std::uint32_t CLOSURE_HEADER_SIZE = 8;
  // ... etc (see MEMORY.md for full details)
}
```

### `runtime.hh` / `runtime.cpp` - Runtime Functions

~1100 lines total (462 hh + 637 cpp) implementing all imported functions:

```cpp
/// packed nix_value: i64 with tag in low 32 bits, payload in high 32 bits
using nix_value = std::int64_t;

/// runtime context holding memory and function table
class runtime_context {
  std::vector<std::uint8_t> memory;  // linear memory
  heap_allocator heap;                // bump allocator for runtime
  wasm_func_t call_wasm_func;         // callback for indirect calls
  wasm_thunk_func_t call_wasm_thunk;  // callback for thunk evaluation
  std::unordered_map<std::string, nix_value> builtins;
  // ...
};

/// force a value (evaluate thunks recursively)
auto rt_force(runtime_context& ctx, nix_value v) -> nix_value;

/// apply a function to an argument
auto rt_apply(runtime_context& ctx, nix_value fn, nix_value arg) -> nix_value;

// ... arithmetic, comparison, collection operations
```

**Key Features**:
- Bump allocator for heap (`heap_allocator`)
- Memory read/write helpers (little-endian i32/i64/string)
- Thunk evaluation with infinite recursion detection
- Error types: `runtime_error`, `type_error`, `attr_error`, `oom_error`

### `wasm_executor.hh` / `wasm_executor.cpp` - WASM Execution

~950 lines total (132 hh + 822 cpp) providing wasmtime-based execution:

```cpp
struct execution_result {
  bool success;
  nix_value value;      // result if success
  std::string error;    // error message if !success
  std::uint32_t line;   // error position
  std::uint32_t column;
};

class wasm_executor {
public:
  /// execute a WASM binary, returns the result of calling main()
  auto execute(std::span<const std::uint8_t> wasm_binary) -> execution_result;
  
  /// format a value for display
  auto format_value(nix_value v) const -> std::string;
  
private:
  std::unique_ptr<wasmtime::Engine> engine_;
  std::unique_ptr<wasmtime::Store> store_;
  runtime_context ctx_;
  // ...
};
```

**Execution Flow**:
1. Create fresh store and memory
2. Setup linker with all runtime imports
3. Compile and instantiate module
4. Setup indirect call callbacks for closures/thunks
5. Call `main()`, force the result
6. Return formatted result or error

---

## 5. Eval Layer (`eval/`)

A tree-walking interpreter serving as reference implementation and for testing.

### `value.hh` - Runtime Values

```cpp
using value_variant = std::variant<
    value_null,
    bool,
    std::int64_t,
    double,
    std::string,
    value_path,
    value_list,
    attr_set,
    closure,
    builtin,
    thunk
>;

struct value {
    value_variant data;
    
    static auto make_int(std::int64_t i) -> value_ptr;
    static auto make_string(std::string s) -> value_ptr;
    static auto make_thunk(const ast::expression& expr, env_ptr env) -> value_ptr;
    // ...
};
```

### `eval.hh` - The Evaluator

~1500 lines implementing:
- Lazy evaluation via thunks
- Memoization of forced values
- Cycle detection
- Full builtins set (~50 functions)
- `import` support with cycle detection

**Lazy Evaluation**:
```cpp
auto force(value_ptr val) -> value_ptr {
    while (is_thunk(val)) {
        auto& t = as_thunk(val);
        if (t.cached) { val = t.cached; continue; }
        if (t.evaluating) throw eval_error("infinite recursion");
        t.evaluating = true;
        t.cached = eval_expr(*t.expr, t.env);
        t.evaluating = false;
        val = t.cached;
    }
    return val;
}
```

---

## 6. Build System

Uses Buck2 with header-only libraries where sensible:

```python
cxx_library(
    name = "ast",
    exported_headers = ["ast/expression.hh", "ast/symbol_table.hh"],
    # header-only, no deps
)

cxx_library(
    name = "parse",
    exported_headers = [...],
    deps = [":ast", "//third_party:pegtl", "//third_party:boost"],
)

cxx_library(
    name = "compile",
    exported_headers = [...],
    deps = [":ast", "//third_party:binaryen"],
)

cxx_library(
    name = "eval",
    exported_headers = ["eval/eval.hh", "eval/value.hh"],
    deps = [":ast"],
)

cxx_library(
    name = "runtime",
    srcs = ["runtime/runtime.cpp"],
    exported_headers = ["runtime/memory_layout.hh", "runtime/runtime.hh"],
    deps = [":compile"],
)

cxx_library(
    name = "wasm_executor",
    srcs = ["runtime/wasm_executor.cpp"],
    exported_headers = ["runtime/wasm_executor.hh"],
    deps = [":compile", ":runtime", "//third_party:wasmtime"],
)
```

---

## Current Status

### Working

| Component | Status | Confidence |
|-----------|--------|------------|
| **Grammar** | Complete - handles full Nix lexical grammar | 95% |
| **Parsing** | Complete - all expression types supported | 95% |
| **AST Types** | Complete - full expression coverage | 95% |
| **Tree-Walking Eval** | ~90% - most builtins, import, lazy eval | 85% |
| **WASM Compiler** | ~90% - core expressions, closures, lazy eval, path merging | 85% |
| **Runtime** | ~85% - arithmetic, comparison, collections, thunks, closures | 80% |
| **WASM Executor** | Complete - wasmtime integration, full execution pipeline | 90% |

### Expression Coverage (Compiler)

| Expression | Status | Notes |
|------------|--------|-------|
| Integer literals | Complete | |
| Float literals | Complete | |
| String literals | Complete | |
| String interpolation | Complete | |
| Path literals | Complete | |
| Path interpolation | Complete | |
| Identifiers | Complete | Thunks forced on access |
| Lists | Complete | Lazy elements |
| Attribute sets | Complete | Static and dynamic keys |
| Empty attrsets | Complete | Special `{}` handling |
| Recursive attrsets | Complete | |
| Select (a.b.c) | Complete | With error on missing |
| Has attribute (a ? b) | Complete | Works on empty sets |
| Lambdas (simple) | Complete | `x: body` |
| Lambdas (attrset pattern) | Complete | `{ x, y }: body` |
| Pattern defaults | Complete | `{ x, y ? 10 }: body` |
| Pattern with @ | Complete | `args@{ x }: body` |
| Closures | Complete | Free variable capture |
| Application | Complete | Curried |
| Let expressions | Complete | Thunks forced on reference |
| With expressions | Complete | Dynamic scope lookup |
| If expressions | Complete | |
| Assert | Complete | |
| Binary operators | Complete | All operators, INT_MIN/-1 handled |
| Unary operators | Complete | |
| Multi-segment paths | Complete | Path merging supported |
| Lazy evaluation | Complete | Thunks for lists, attrsets, let |

### Not Yet Implemented (Compiler)

1. **Ancient let syntax**: `let { body = ...; x = 1; }` (deprecated)
2. **Search paths**: `<nixpkgs>` requires runtime NIX_PATH lookup
3. **Flakes**: Not in scope for language-level implementation

### Known Limitations (Runtime)

1. **`rt_update`**: Returns second attrset (needs proper merge implementation)
2. **String coercion**: `rt_to_string` only handles strings, not other types
3. **Deep equality**: Lists and attrsets use reference equality, not structural

---

## Completed Work

### Phase 1: Compiler Completeness ✓

1. **Multi-segment path merging** ✓
   - `group_bindings_by_first_segment()` and `compile_merged_attrset_value()`
   - Supports arbitrary nesting: `{ a.b.c = 1; a.b.d = 2; a.e = 3; }`

2. **Thunk generation** ✓
   - `compile_as_thunk()` wraps expressions in lazy thunks
   - Thunks forced on identifier access (let bindings, captures)

3. **Error positions** ✓
   - All error-throwing imports receive source position (line, column)

4. **Attrset pattern defaults** ✓
   - `{ x, y ? 10 }: body` uses default when attribute missing

5. **Empty attrset handling** ✓
   - `{}` represented as pointer 0, handled correctly in runtime

6. **Integer overflow** ✓
   - `INT_MIN / -1` throws runtime error instead of SIGFPE

### Phase 2: Runtime Implementation ✓

All 27 runtime functions implemented in `runtime/runtime.cpp`:

| Category | Functions |
|----------|-----------|
| Core | `rt_force`, `rt_apply`, `rt_lookup_var`, `rt_make_closure`, `rt_make_thunk` |
| Arithmetic | `rt_add`, `rt_sub`, `rt_mul`, `rt_div`, `rt_negate` |
| Comparison | `rt_less_than`, `rt_less_eq`, `rt_eq`, `rt_neq` |
| Boolean | `rt_not`, `rt_is_bool` |
| Collections | `rt_make_list`, `rt_make_attrs`, `rt_make_attrs_dynamic`, `rt_select`, `rt_select_dynamic`, `rt_has_attr`, `rt_has_attr_dynamic`, `rt_update`, `rt_concat` |
| Strings | `rt_to_string`, `rt_concat_strings` |

### Phase 3: Integration ✓

- `wasm_executor` class using wasmtime
- All runtime imports bound to wasmtime linker
- Indirect call support for closures and thunks

---

## Remaining Work

### Runtime Improvements (HIGH PRIORITY)

1. **`rt_update` proper merge**
   - Currently returns second attrset
   - Should merge: `{ a = 1; } // { b = 2; }` → `{ a = 1; b = 2; }`

2. **String coercion**
   - `rt_to_string` only handles strings
   - Should handle: integers, paths, booleans, null

3. **Deep equality**
   - Lists and attrsets use reference equality
   - Should compare structurally

### Features Not Implemented

1. **Ancient let syntax**: `let { body = ...; }` (deprecated, low priority)
2. **Search paths**: `<nixpkgs>` (requires NIX_PATH runtime)
3. **Flakes**: Not in scope for language-level implementation
4. **Store integration**: `builtins.derivation`, content-addressed paths
5. **IFD**: Import From Derivation (requires build-time eval)
6. **Most builtins**: Only basic operations implemented

### Performance (FUTURE)

1. String deduplication in data segments
2. Escape analysis for closure elision
3. Inlining of small functions
4. WASM GC when available

---

## Testing Strategy

### Test Files

| File | Lines | Test Cases | Assertions | Focus |
|------|-------|------------|------------|-------|
| `ast_test.cpp` | 572 | 40 | 74 | AST type construction |
| `grammar_test.cpp` | 654 | 27 | 197 | PEGTL grammar rules |
| `parse_test.cpp` | 1,109 | 21 | 526 | Parse → AST round-trips |
| `wasm_types_test.cpp` | 310 | 19 | 116 | Value packing/unpacking |
| `compiler_test.cpp` | 1,881 | 67 | 138 | WASM compilation + validation |
| `eval_test.cpp` | 417 | 55 | 129 | Tree-walking interpreter |
| `integration_test.cpp` | 343 | 34 | 71 | Parse → compile → WASM binary |
| `execution_test.cpp` | 448 | 49 | 328 | Full pipeline with wasmtime |
| `runtime_test.cpp` | 526 | 14 | 185 | Direct runtime function tests |
| `property_test.cpp` | 600 | 28 | 2,800* | Property-based tests (RapidCheck) |
| `adversarial_test.cpp` | 748 | 15 | 148 | Edge cases, boundary conditions |
| **Total** | **~8,200** | **369** | **~4,700** | |

*Property tests run 100 iterations each

### Additional Test Infrastructure

| File | Purpose |
|------|---------|
| `bench.cpp` | Microbenchmarks using nanobench |
| `fuzz_parse.cpp` | libFuzzer harness for parser |
| `fuzz_compile.cpp` | libFuzzer harness for compiler |

### Testing Approach

1. **Unit tests** for each layer (AST, parser, compiler, runtime)
2. **Property-based tests** via RapidCheck for algebraic laws
3. **Adversarial tests** for edge cases (INT_MIN, empty sets, overflow)
4. **Integration tests**: `parse → compile → validate`
5. **End-to-end tests**: `parse → compile → execute → verify result`
6. **Fuzzing harnesses** for parser and compiler (manual execution)

---

## Dependencies

| Dependency | Version | Purpose |
|------------|---------|---------|
| PEGTL | 3.x | PEG parser generator |
| Binaryen | 125 | WASM code generation |
| Boost | 1.87 | `small_vector` for parse state |
| Wasmtime | 40.0 | WASM execution runtime |
| Catch2 | 3.x | Test framework |
| RapidCheck | - | Property-based testing |
| nanobench | 4.3.11 | Microbenchmarking |

---

## Code Style

- `snake_case` for everything (types, functions, variables)
- `[[nodiscard]]` on all pure functions
- `noexcept` where guaranteed
- Trailing return types: `auto foo() -> int`
- `std::` prefix always (no `using namespace std`)
- Namespaces: `nix::language::{ast,parse,compile,eval,runtime,memory_layout}`
