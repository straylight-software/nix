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
│   ├── compiler.hh         # AST → WASM via Binaryen
│   └── wasm_types.hh       # WASM value representation
├── eval/                   # Tree-walking interpreter (reference impl)
│   ├── eval.hh             # Evaluator
│   └── value.hh            # Runtime value types
├── cli/                    # Command-line tools
│   └── nix_eval.cpp        # REPL and file evaluator
└── tests/                  # Test suite
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

2300+ lines of WASM codegen via Binaryen. Key components:

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

The compiler imports host functions for operations that can't be inlined:

```cpp
// Arithmetic
__add, __sub, __mul, __div

// Comparison
__lessThan, __lessEq, __eq, __neq

// Collections
__makeList, __makeAttrs, __select, __hasAttr, __update, __concat

// Runtime
__apply, __force, __throw, __lookupVar, __makeClosure, __makeThunk

// String operations
__toString, __concatStrings
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

## 4. Eval Layer (`eval/`)

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

## 5. Build System

Uses Buck2 with header-only libraries:

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
```

---

## Current Status

### Working

| Component | Status |
|-----------|--------|
| **Grammar** | Complete - handles full Nix lexical grammar |
| **Parsing** | Complete - all expression types supported |
| **AST Types** | Complete - full expression coverage |
| **Tree-Walking Eval** | ~90% - most builtins, import, lazy eval |
| **WASM Compiler** | ~90% - core expressions, closures, lazy eval, path merging |

### Expression Coverage (Compiler)

| Expression | Status | Notes |
|------------|--------|-------|
| Integer literals | Complete | |
| Float literals | Complete | |
| String literals | Complete | |
| String interpolation | Complete | |
| Path literals | Complete | |
| Path interpolation | Complete | |
| Identifiers | Complete | |
| Lists | Complete | |
| Attribute sets | Complete | Static keys |
| Dynamic attr keys | Complete | Single-segment only |
| Recursive attrsets | Complete | |
| Select (a.b.c) | Complete | |
| Has attribute (a ? b) | Complete | |
| Lambdas (simple) | Complete | |
| Lambdas (attrset) | Complete | |
| Closures | Complete | Free variable capture |
| Application | Complete | Curried |
| Let expressions | Complete | |
| With expressions | Complete | Dynamic scope lookup |
| If expressions | Complete | |
| Assert | Complete | |
| Binary operators | Complete | All operators |
| Unary operators | Complete | |
| Multi-segment paths | Complete | Path merging supported |
| Lazy evaluation | Complete | Thunks for lists, attrsets, let |

### Not Yet Implemented (Compiler)

1. **Ancient let syntax**: `let { body = ...; x = 1; }` (deprecated)
2. **Search paths**: `<nixpkgs>` requires runtime NIX_PATH lookup
3. **Flakes**: Not in scope for language-level implementation

---

## Plan to Complete

### Phase 1: Compiler Completeness (DONE)

1. ~~**Multi-segment path merging**~~ ✓ COMPLETE
   - Implemented `group_bindings_by_first_segment()` and `compile_merged_attrset_value()`
   - Supports arbitrary nesting: `{ a.b.c = 1; a.b.d = 2; a.e = 3; }`
   - Works for both attribute sets and let bindings

2. ~~**Thunk generation**~~ ✓ COMPLETE
   - `compile_as_thunk()` wraps expressions in lazy thunks
   - List elements wrapped in thunks (lazy lists)
   - Attribute set values wrapped in thunks (lazy attrsets)
   - Let binding values wrapped in thunks
   - `__force` calls at evaluation points (if/assert/logical operators)
   - `__makeThunk` runtime import added

3. **Error positions** (MEDIUM) - TODO
   - Source positions are tracked but not propagated to runtime errors
   - Need to emit source maps or inline position data

### Phase 2: Runtime Implementation

The WASM module imports runtime functions. These need implementations:

```cpp
// runtime.cpp (host side)
extern "C" {
    auto __add(nix_value a, nix_value b) -> nix_value;
    auto __apply(nix_value fn, nix_value arg) -> nix_value;
    auto __force(nix_value v) -> nix_value;
    auto __makeList(uint32_t offset, uint32_t count) -> nix_value;
    // ... ~30 more functions
}
```

**Implementation Options**:
1. **C++ host**: Implement in C++, link with WASM runtime (wasmtime, wasm3)
2. **Nix builtins in Nix**: Compile builtins.nix to WASM, link at load time
3. **Hybrid**: Core ops in C++, higher-level builtins in Nix

### Phase 3: Integration

1. **WASM execution harness**
   - Load compiled module
   - Provide runtime imports
   - Execute main function
   - Extract result

2. **Store integration**
   - `builtins.derivation` needs store access
   - Content-addressed paths
   - Build scheduling

3. **IFD (Import From Derivation)**
   - Requires build-time evaluation
   - Significant architectural consideration

### Phase 4: Performance

1. **String deduplication** in data segments
2. **Escape analysis** for closure elision
3. **Inlining** of small functions
4. **WASM GC** for proper garbage collection (when available)

---

## Testing Strategy

```
tests/
├── ast_test.cpp        # AST type construction/invariants
├── grammar_test.cpp    # PEGTL grammar unit tests
├── parse_test.cpp      # Parse → AST round-trips
├── wasm_types_test.cpp # Value packing/unpacking
├── compiler_test.cpp   # Compilation + validation
└── eval_test.cpp       # Interpreter correctness
```

**Testing Approach**:
1. Unit tests for each layer
2. Property-based tests via RapidCheck
3. Round-trip: `parse(source) → eval → expected_value`
4. Compare: `eval(source) == run_wasm(compile(source))`

---

## Dependencies

| Dependency | Version | Purpose |
|------------|---------|---------|
| PEGTL | 3.x | PEG parser generator |
| Binaryen | 125 | WASM code generation |
| Boost | 1.87 | `small_vector` for parse state |
| Catch2 | 3.x | Test framework |
| RapidCheck | - | Property-based testing |

---

## Code Style

- `snake_case` for everything (types, functions, variables)
- `[[nodiscard]]` on all pure functions
- `noexcept` where guaranteed
- Trailing return types: `auto foo() -> int`
- `std::` prefix always (no `using namespace std`)
- Namespaces: `nix::language::{ast,parse,compile,eval}`
