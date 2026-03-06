# Straylight Nix Build System

Comprehensive documentation of the Buck2 + Nix build system.

## Overview

This project uses **Buck2** as the build system with **Nix** providing hermetic toolchains. All tool
paths are absolute Nix store paths - no PATH lookup, fully reproducible.

```
┌─────────────────────────────────────────────────────────────────┐
│                         nix develop                             │
│  (generates .buckconfig.local with Nix store paths)             │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                          Buck2                                  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐           │
│  │   C++ Rules  │  │  Rust Rules  │  │ Rust Crate   │           │
│  │   (cxx.bzl)  │  │  (rust.bzl)  │  │(rust_crate)  │           │
│  └──────────────┘  └──────────────┘  └──────────────┘           │
└─────────────────────────────────────────────────────────────────┘
```

## Cell Structure

Defined in `.buckconfig`:

```ini
[cells]
root = .                    # Main project
prelude = build/prelude     # Buck2 prelude (standard rules)
toolchains = build/toolchains  # Custom toolchain definitions
none = none                 # Stub for unused FB cells
```

### Cell Aliases

```ini
[cell_aliases]
config = prelude            # For config// references
ovr_config = prelude
fbcode = none               # FB internal - stubbed out
fbsource = none
fbcode_macros = none
buck = none
```

## Target Platform

By default, all targets build for **musl** (static linking):

```ini
[parser]
target_platform_detector_spec = target:root//...->toolchains//:musl

[build]
execution_platforms = toolchains//:musl
```

This means:

- Rust targets use `x86_64-unknown-linux-musl`
- C++ targets link against musl libc
- Final binaries are fully static

## Toolchain Configuration

### How Nix Paths Get Into Buck2

1. `nix develop` runs the shell hook
2. Shell hook generates `.buckconfig.local` with absolute paths:

```ini
[rust]
rustc = /nix/store/xxx-rust/bin/rustc
rustdoc = /nix/store/xxx-rust/bin/rustdoc
clippy_driver = /nix/store/xxx-rust/bin/clippy-driver
target_triple = x86_64-unknown-linux-musl

[cxx]
cc = /nix/store/xxx-clang/bin/clang
cxx = /nix/store/xxx-clang/bin/clang++
# ... etc
```

3. Buck2 rules read these via `read_root_config("rust", "rustc", "rustc")`

### Toolchain Definitions (`build/toolchains/BUCK`)

| Target | Purpose | |--------|---------| | `:cxx` | Default C++ toolchain (LLVM, C23/C++23) | |
`:cxx_coverage` | C++ with coverage instrumentation | | `:rust` | Rust toolchain (uses prelude's
`system_rust_toolchain`) | | `:python_bootstrap` | Required by Buck2 internals | | `:genrule` | For
custom build rules | | `:test` | Test execution (noop - runs locally) | | `:local` | Local-only
execution platform | | `:lre` | Local execution platform | | `:musl` | Musl target platform
(default) |

## Rust Build Rules

### Two Rule Systems

There are **two** sets of Rust rules in this project:

#### 1. Custom Rules (`build/toolchains/rust.bzl`)

Simple, minimal rules that read rustc path from config:

```python
# Providers
RustLibraryInfo = provider(fields = ["rlib", "crate_name", "transitive_deps"])

# Rules
rust_library    # Builds .rlib, provides RustLibraryInfo
rust_binary     # Builds executable
rust_toolchain  # Toolchain definition (not used by custom rules)
```

**How they work:**

- First source file = crate root
- Other sources = hidden deps (tracked for rebuilds)
- Deps can provide either `RustLibraryInfo` or `RustCrateInfo`
- Transitive deps propagated via `-Ldependency=` flags

**Attributes:**

```python
rust_library(
    name = "foo",
    srcs = ["src/lib.rs", "src/bar.rs"],  # First = crate root
    deps = [":other_lib"],
    edition = "2021",
    crate_name = "foo",        # Optional, defaults to name
    proc_macro = False,        # True for proc-macro crates
    features = ["std"],        # --cfg feature="std"
)
```

#### 2. Prelude Rules (`build/prelude/rust/`)

Full-featured rules from Buck2 prelude:

- `rust_library` - with metadata pipelining, split debuginfo, etc.
- `rust_binary` - with resources, env, run info
- `rust_test` - test execution with framework support

**The prelude rules are used for `rust_test`** since custom rules don't define it.

### Crate Fetching (`build/toolchains/rust_crate.bzl`)

For building vendored crates from crates.io:

```python
# Provider
RustCrateInfo = provider(fields = [
    "rlib",           # Compiled .rlib
    "rmeta",          # Metadata (same as rlib here)
    "crate_name",     # Crate name (underscores)
    "edition",
    "features",
    "is_proc_macro",
    "transitive_deps",  # All transitive rlibs
])

# Low-level rule
rust_crate(
    name = "serde",
    src = ":serde-1.0.228.crate",  # http_archive target
    crate_name = "serde",
    edition = "2021",
    features = ["derive"],
    deps = [":serde_derive"],
    env = {"CARGO_PKG_VERSION": "1.0.228"},
    generated_files = {"version.rs": "..."},  # For build.rs simulation
)

# Convenience macro (not currently used - third-party uses rust_library)
crates_io(
    name = "serde",
    version = "1.0.228",
    sha256 = "...",
    features = ["derive"],
    deps = [":serde_derive"],
)
```

## Third-Party Rust Dependencies

Located at `vendor/isospin/third-party/rust/`:

```
third-party/rust/
├── BUCK              # 8400+ lines, generated by reindeer
├── Cargo.toml        # Workspace manifest
├── Cargo.lock        # Locked versions
├── reindeer.toml     # Reindeer config
└── vendor/           # Symlink to Nix store (vendored sources)
    ├── libc-0.2.180/
    ├── serde-1.0.228/
    └── ...
```

### BUCK File Structure

Generated by `reindeer buckify`:

```python
# Full target with version
rust_library(
    name = "libc-0.2.180",
    srcs = [
        "vendor/libc-0.2.180/src/lib.rs",
        "vendor/libc-0.2.180/src/unix/mod.rs",
        # ... explicit file list
    ],
    crate = "libc",
    crate_root = "vendor/libc-0.2.180/src/lib.rs",
    edition = "2021",
    env = {
        "CARGO_PKG_NAME": "libc",
        "CARGO_PKG_VERSION": "0.2.180",
        "CARGO_PKG_VERSION_MAJOR": "0",
        "CARGO_PKG_VERSION_MINOR": "2",
        "CARGO_PKG_VERSION_PATCH": "180",
    },
    features = ["std", "extra_traits"],
    visibility = [],
    deps = [...],
)

# Convenience alias
alias(
    name = "libc",
    actual = ":libc-0.2.180",
    visibility = ["PUBLIC"],
)
```

### Using Third-Party Deps

Reference by alias name:

```python
rust_library(
    name = "my_lib",
    deps = [
        "//vendor/isospin/third-party/rust:libc",
        "//vendor/isospin/third-party/rust:thiserror",
    ],
)
```

### Available Crates (Partial List)

| Crate | Version | Notes | |-------|---------|-------| | libc | 0.2.180 | System bindings | |
thiserror | 2.0.17 | Error derive | | zerocopy | 0.8.24 | Zero-copy parsing | | io-uring | 0.7.7 |
Linux io_uring | | proptest | 1.6.0 | Property testing | | bitflags | 2.10.0 | Bitflag macros | |
cfg-if | 1.0.4 | Conditional compilation |

### Missing Crates

Not yet in third-party (blocking some targets):

- `tracing` - Logging/tracing framework
- `tracing-subscriber` - Subscriber implementation
- `nix` (0.27.1 vendored but no BUCK target) - Unix API bindings

## Building Rust Targets

### Command Patterns

```bash
# Enter nix shell first (generates .buckconfig.local)
nix develop

# Build a library
buck2 build //vendor/isospin/gpu-broker:guest_protocol

# Build with explicit platform
buck2 build --target-platforms=toolchains//:musl //...

# Run tests
buck2 test //vendor/isospin/gpu-broker:guest_protocol_test
```

### Dependency Resolution

When a target depends on another:

1. Buck2 builds dep first, gets `RustLibraryInfo` or `RustCrateInfo`
2. Adds `--extern crate_name=/path/to/lib.rlib`
3. Adds `-Ldependency=/path/to/` for the dep directory
4. Recursively adds `-Ldependency=` for all transitive deps

```
my_binary
    └── my_lib (RustLibraryInfo)
            └── libc (RustLibraryInfo from third-party)
                    └── (no deps)

rustc my_binary.rs \
    --extern my_lib=/path/to/libmy_lib.rlib \
    --extern libc=/path/to/liblibc.rlib \
    -Ldependency=/path/to/my_lib/ \
    -Ldependency=/path/to/libc/
```

## Creating New Rust Targets

### Library Target

```python
rust_library(
    name = "my_lib",
    srcs = [
        "src/lib.rs",      # Crate root (MUST be first)
        "src/module.rs",
        "src/utils.rs",
    ],
    crate = "my_lib",
    crate_root = "src/lib.rs",  # Explicit (optional if first in srcs)
    edition = "2021",
    features = ["std"],
    deps = [
        "//vendor/isospin/third-party/rust:libc",
    ],
    visibility = ["PUBLIC"],
)
```

### Test Target

Uses prelude's `rust_test`:

```python
rust_test(
    name = "my_lib_test",
    srcs = [
        "src/lib.rs",
        "src/module.rs",
        "src/tests.rs",
    ],
    crate = "my_lib",
    crate_root = "src/lib.rs",
    edition = "2021",
    deps = [
        "//vendor/isospin/third-party/rust:proptest",
    ],
)
```

### Binary Target

```python
rust_binary(
    name = "my_binary",
    srcs = ["src/main.rs"],
    edition = "2021",
    deps = [
        ":my_lib",
        "//vendor/isospin/third-party/rust:clap",
    ],
    visibility = ["PUBLIC"],
)
```

## Granular vs Monolithic Targets

### Monolithic (Avoid)

```python
rust_library(
    name = "big_lib",
    srcs = glob(["src/**/*.rs"]),  # Bad: no granularity
    ...
)
```

### Granular (Preferred)

```python
# Low-level module, no internal deps
rust_library(
    name = "wire_protocol",
    srcs = ["src/wire.rs"],
    ...
)

# Higher-level module depends on lower
rust_library(
    name = "client",
    srcs = ["src/client.rs"],
    deps = [":wire_protocol"],
    ...
)
```

**Challenge:** Rust's `crate::` imports require modules to be in the same crate. Breaking into
separate crates requires changing to `extern crate` / `use other_crate::`.

## Musl Cross-Compilation

The default platform is musl, which means:

1. **Target platform** (`toolchains//:musl`):

   - Has `abi_configuration = "prelude//abi/constraints:musl"`
   - Rust uses `x86_64-unknown-linux-musl`

2. **Execution platform** (also `toolchains//:musl`):

   - Runs on the host (glibc)
   - Proc-macros built for host via exec transition

3. **Result**: Static binaries that run anywhere

```python
# In toolchains/BUCK
system_rust_toolchain(
    name = "rust",
    rustc_target_triple = select({
        "DEFAULT": "x86_64-unknown-linux-gnu",  # Host/proc-macros
        "config//abi:musl": "x86_64-unknown-linux-musl",  # Target
    }),
)
```

## Common Issues

### "unresolved module or unlinked crate"

Missing dependency. Add to `deps`:

```python
deps = [
    "//vendor/isospin/third-party/rust:missing_crate",
],
```

If crate not in third-party, either:

1. Add BUCK target for vendored crate
2. Inline the functionality
3. Add to Cargo.toml and re-run reindeer

### Type Mismatches (musl)

Musl has different type sizes than glibc:

- `msg_controllen`: `u32` (musl) vs `usize` (glibc)
- `cmsg_len`: `u32` (musl) vs `usize` (glibc)

Fix with explicit casts or `#[cfg(target_env = "musl")]`.

### "command not found: buck2"

Run inside nix shell:

```bash
nix develop
buck2 build //...
```

Or prefix command:

```bash
nix develop --command buck2 build //...
```

## File Locations

| Path | Purpose | |------|---------| | `.buckconfig` | Main Buck2 config | | `.buckconfig.local` |
Generated Nix paths (gitignored) | | `build/prelude/` | Buck2 prelude (git submodule) | |
`build/toolchains/BUCK` | Toolchain definitions | | `build/toolchains/rust.bzl` | Custom Rust rules
| | `build/toolchains/rust_crate.bzl` | Crate fetching rules | | `build/toolchains/cxx.bzl` | C++
toolchain rules | | `vendor/isospin/third-party/rust/BUCK` | Third-party Rust deps | |
`vendor/isospin/third-party/rust/vendor/` | Vendored sources (Nix symlink) |

## Regenerating Third-Party

```bash
# Update Cargo.lock
cd vendor/isospin/third-party/rust
cargo update

# Regenerate BUCK file
reindeer --third-party-dir=. buckify
```

Note: The vendor/ directory is a Nix symlink, managed by the flake.
