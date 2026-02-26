# Building straylight/nix

straylight/nix uses Buck2 via sensenet for builds.

## Quick Start

Enter the development shell:

```console
$ nix develop
```

Build a target:

```console
$ buck2 build //src/nix/util:util
```

Build all core libraries:

```console
$ buck2 build //src/nix/...
```

Run tests:

```console
$ buck2 test //src/nix/util/tests/...
```

## Build Targets

| Target | Description | |--------|-------------| | `//src/nix/util:util` | Core utilities library |
| `//src/nix/store:store` | Store operations | | `//src/nix/fetchers:fetchers` | Input fetchers
(git, github, etc) | | `//src/nix/expr:expr` | Expression evaluator | | `//src/nix/flake:flake` |
Flake support | | `//src/nix/main:main` | Main entry/logging | | `//src/nix/cmd:cmd` | Command
infrastructure | | `//src/nix/cli:cli` | CLI commands | | `//src/straylight/evring:evring` |
Deterministic async I/O | | `//src/straylight/nix/...:...` | Modernized utilities | |
`//src/straylight/nix/compiler:compiler` | Nix → WASM compiler |

## Remote Execution

The project is configured for NativeLink remote execution. To use:

```console
$ buck2 build --prefer-remote //src/nix/util:util
```

Remote execution uses the sense-scheduler and sense-cas servers on fly.dev.

## Dhall Build Definitions

Build targets are defined in Dhall for type safety. See `dhall/package.dhall` for the typed build
graph.

Generate Buck2 targets from Dhall:

```console
$ dhall text <<< '(./dhall/package.dhall).targets'
```

## Editor Integration

The `clangd` LSP server is available in the development shell. Buck2 generates
`compile_commands.json` automatically.

Configure your editor to use clangd from the development shell, or use
[nix-direnv](https://github.com/nix-community/nix-direnv) with your editor's direnv plugin.

## Formatting

Format all code:

```console
$ nix fmt
```

Or use the formatters directly:

```console
$ clang-format -i src/nix/util/*.cpp
$ nixfmt flake.nix
```

### Pre-commit Hooks

Install pre-commit hooks:

```console
$ pre-commit install --install-hooks
```

The hooks run:

- `nix fmt` on commit (formatting)
- `ast-grep` on commit (pattern rules for C++)
- `clang-tidy` on push (semantic lint, requires compile_commands.json)

Configuration is generated from `dhall/pre-commit.dhall`:

```console
$ dhall-to-yaml --file dhall/pre-commit.dhall > .pre-commit-config.yaml
```

### Lint Checks

`nix flake check` runs ast-grep on `src/straylight/` and fails on errors:

```console
$ nix flake check
```

For full clang-tidy lint (requires build first):

```console
$ buck2 build //src/nix/cli:nix  # generates compile_commands.json
$ ./scripts/lint                  # lint changed files
$ ./scripts/lint --all           # lint everything
```

## Platforms

Supported platforms:

- `x86_64-linux` (primary)
- `aarch64-linux`
- `aarch64-darwin`

## Project Structure

```
src/
├── nix/              # Core Nix implementation
│   ├── util/         # Utilities
│   ├── store/        # Store operations
│   ├── fetchers/     # Input fetchers
│   ├── expr/         # Expression evaluator
│   ├── flake/        # Flake support
│   ├── main/         # Main/logging
│   ├── cmd/          # Command infrastructure
│   └── cli/          # CLI commands
└── straylight/
    ├── evring/       # Async I/O (io_uring)
    └── nix/
        ├── compiler/ # Nix → WASM compiler
        ├── protocol/ # Formal protocol specs
        └── {crypto,text,url,async,sync,store,...}/  # Modernized utilities
```

## Dependencies

Dependencies are managed via Nix and injected into Buck2 via `.buckconfig.local`. See `nix/deps.nix`
for the full list.

Key dependencies:

- LibreSSL (not OpenSSL) for TLS/crypto
- BLAKE3 for hashing
- Ada for URL parsing
- RE2 for regex
- Binaryen/Wasmtime for WASM
- liburing for async I/O

## Toolchain

- LLVM/Clang 19
- C++23
- Buck2

All toolchain components come from Nix with absolute store paths — no PATH lookups.
