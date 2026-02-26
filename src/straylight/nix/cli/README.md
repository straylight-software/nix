# straylight::nix::cli

Modern C++23 CLI argument parser with CLI11-style fluent API.

## Quick Start

```cpp
#include <straylight/nix/cli/args.h>

using namespace straylight::nix::cli;

int main(int argc, char* argv[]) {
    ArgumentParser parser("myapp", "My application description");
    
    std::string name;
    int count = 0;
    bool verbose = false;

    parser.add_option("--name,-n", "User name", name)
          .required();
    parser.add_option("--count,-c", "Item count", count)
          .default_value(10);
    parser.add_flag("--verbose,-v", "Enable verbose output", verbose);

    auto result = parser.parse(argc, argv);
    if (!result) {
        std::cerr << result.error() << std::endl;
        return 1;
    }
    
    // Use name, count, verbose...
}
```

## Features

- **Fluent builder API**: Chain `.required()`, `.default_value()`, etc.
- **Type-safe binding**: Automatic conversion to bound variables
- **Subcommands**: Nested argument parsing
- **Automatic help**: `--help` generation
- **Flexible syntax**: `-f`, `--flag`, `-abc`, `--opt=value`, `--opt value`

## API Overview

| Type/Function | Description | |---------------|-------------| | `ArgumentParser` | Main parser
with program name/description | | `add_option()` | Add option with value (`--name value`) | |
`add_flag()` | Add boolean flag (`--verbose`) | | `add_positional()` | Add positional argument | |
`add_subcommand()` | Add subcommand | | `parse()` | Parse argc/argv, returns ParseResult |

## Option Syntax

| Pattern | Description | |---------|-------------| | `-f` | Short flag | | `--flag` | Long flag | |
`-abc` | Combined short flags | | `--opt=value` | Option with = | | `--opt value` | Option with
space | | `-o value` | Short option with value |

## Building

```bash
buck2 build //src/straylight/nix/cli:cli
buck2 test //src/straylight/nix/cli/tests:...
```

## See Also

- [Architecture](docs/ARCHITECTURE.md)
- [Testing](docs/TESTING.md)
