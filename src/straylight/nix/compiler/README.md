# straylight::nix::compiler

Nix-to-WASM compiler and evaluator.

## Quick Start

```cpp
#include <straylight/nix/compiler/evaluator.h>

using namespace straylight::nix::compiler;

// Create evaluator
evaluator eval;

// Evaluate Nix expression
auto result = eval.eval_string("1 + 2");
if (result) {
    // result is 3
}

// Evaluate Nix file
auto result = eval.eval_file("./default.nix");
```

## Architecture

The compiler pipeline:

```
Nix Source → Parser → AST → Compiler → WASM bytecode → Executor → Value
```

### Components

| Directory | Namespace | Description |
|-----------|-----------|-------------|
| `ast/` | `compiler::ast` | Abstract syntax tree types |
| `parse/` | `compiler::parse` | Tree-sitter based parser |
| `compile/` | `compiler::compile` | AST to WASM compilation |
| `runtime/` | `compiler::runtime` | WASM execution engine |
| `eval/` | `compiler::eval` | Evaluation utilities |
| `cli/` | `compiler::cli` | Command-line interface |

## Features

- **Tree-sitter parser**: Fast, incremental parsing
- **WASM compilation**: Compile Nix to WebAssembly bytecode
- **Import resolution**: Handle imports with cycle detection
- **I/O backends**: Pluggable I/O for different environments

## API Overview

| Type | Description |
|------|-------------|
| `evaluator` | Top-level Nix evaluator |
| `eval_string()` | Evaluate Nix expression string |
| `eval_file()` | Evaluate Nix file |
| `eval_result<T>` | Result type with error info |
| `eval_error` | Error with location info |

## Building

```bash
buck2 build //src/straylight/nix/compiler:compiler
buck2 test //src/straylight/nix/compiler/tests:...
```

## See Also

- [Architecture](docs/ARCHITECTURE.md)
- [Memory](docs/MEMORY.md)
- [Testing](docs/TESTING.md)
