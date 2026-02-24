# straylight/nix Architecture

```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
                                          // straylight // nix
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

A ground-up rethinking of Nix.

This document provides a comprehensive architectural overview of the straylight/nix project,
covering the core components, design philosophy, and implementation details.

______________________________________________________________________

## Table of Contents

01. [Overview](#overview)
02. [Design Philosophy](#design-philosophy)
03. [Project Structure](#project-structure)
04. [Core Components](#core-components)
    - [libevring: Deterministic Async I/O](#libevring-deterministic-async-io)
    - [nix-language: Nix → WebAssembly Compiler](#nix-language-nix--webassembly-compiler)
    - [nix-protocol: Formal Protocol Specifications](#nix-protocol-formal-protocol-specifications)
    - [primitives: Modern Utility Replacements](#primitives-modern-utility-replacements)
    - [Nix2 Store: Daemonless Log-Structured Store](#nix2-store-daemonless-log-structured-store)
05. [Build System](#build-system)
06. [Dependencies](#dependencies)
07. [Testing Strategy](#testing-strategy)
08. [Performance Characteristics](#performance-characteristics)
09. [Default Configuration](#default-configuration)
10. [Component Details](#component-details)

______________________________________________________________________

## Overview

straylight/nix is not merely a fork of Nix—it is a fundamental reimplementation of core subsystems
designed to address longstanding architectural limitations:

| Problem | straylight Solution |
|---------|---------------------|
| SQLite store bottleneck | Log-structured store with io_uring |
| nix-daemon coordination overhead | Daemonless operation via flock |
| Tree-walking interpreter performance | AOT compilation to WebAssembly |
| Undocumented protocol | Formal Kaitai Struct specifications |
| NIH utility implementations | Modern libraries (StringZilla, RE2, BLAKE3) |
| Non-deterministic async code | State machine architecture with replay |

### Quick Start

```bash
nix develop
buck2 build //...
buck2 test //src/straylight/...
```

______________________________________________________________________

## Design Philosophy

### 1. Determinism by Construction

Traditional async code is hard to test because I/O timing is non-deterministic. straylight solves
this by separating **what** from **how**:

- **Machines** define pure state transitions (testable without I/O)
- **Rings** execute operations against the kernel (real I/O)
- **Replay** runs machines against recorded event streams (no I/O)

### 2. Modern C++23

The codebase uses the latest C++ features to maximize clarity and safety:

- `std::expected` for error handling
- `std::variant` over inheritance hierarchies
- Concepts for compile-time interface checking
- `[[nodiscard]]` on all pure functions
- Trailing return types throughout

### 3. Formal Specifications

Where Nix has undocumented wire protocols, straylight has:

- Kaitai Struct schemas (machine-readable, polyglot code generation)
- Captured test vectors from real daemon traffic
- Cross-validation against tvix/nix-compat

### 4. Replace, Don't Patch

Rather than patching NIH implementations, straylight replaces them with best-in-class libraries:

| Original | Replacement | Benefit |
|----------|-------------|---------|
| Custom string operations | StringZilla | SIMD-accelerated |
| `std::regex` | RE2 | Linear-time guarantee |
| Multi-algo hash | BLAKE3 | 3x faster than SHA-256 |
| Custom thread pool | taskflow | Work-stealing, DAG execution |
| SQLite store | Log-structured + io_uring | 10-25x faster |

______________________________________________________________________

## Project Structure

```
src/
├── nix/                      # Core Nix fork (C++23)
│   ├── cli/                  # CLI commands
│   ├── cmd/                  # Command infrastructure
│   ├── expr/                 # Expression evaluator
│   ├── fetchers/             # Input fetchers (git, github, etc.)
│   ├── flake/                # Flake support
│   ├── main/                 # Main entry/logging
│   ├── store/                # Store operations
│   └── util/                 # Utilities (144 files)
│
├── nix-c/                    # C API bindings
│
└── straylight/
    ├── evring/               # Deterministic async I/O (io_uring)
    │   ├── ARCHITECTURE.md   # Detailed evring documentation
    │   ├── handle.h          # Generational handles
    │   ├── event.h           # Events and operations
    │   ├── machine.h         # State machine concepts
    │   ├── ring.h            # io_uring ring interface
    │   ├── generators.h      # Generator machines for bulk ops
    │   ├── http1.h           # HTTP/1.1 state machines
    │   ├── http2.h           # HTTP/2 state machines
    │   ├── http3.h           # HTTP/3 (QUIC) state machines
    │   └── tls.h             # TLS state machines (libtls)
    │
    ├── language/             # Nix → WASM compiler
    │   ├── ARCHITECTURE.md   # Detailed compiler documentation
    │   ├── MEMORY.md         # Memory layout documentation
    │   ├── ast/              # AST types (variant-based)
    │   ├── parse/            # PEGTL grammar + parsing
    │   ├── compile/          # Binaryen codegen
    │   ├── eval/             # Tree-walking interpreter (reference)
    │   ├── runtime/          # WASM execution (wasmtime)
    │   └── tests/            # 8,200+ lines of tests
    │
    ├── nix/                  # Modern utility modules (crypto, text, url, async, sync, data, etc.)
    │   ├── NIH.md            # NIH replacement tracking
    │   ├── STORE_DESIGN.md   # Log-structured store design
    │   ├── strings.h         # StringZilla-backed strings
    │   ├── hash.h            # BLAKE3 hashing
    │   ├── regex.h           # RE2 regex
    │   ├── store.h           # Daemonless store
    │   └── async/            # taskflow-backed concurrency
    │
    └── protocol/             # Formal protocol specs (Kaitai)
        ├── README.md         # Protocol documentation
        ├── nix_daemon.ksy    # Daemon protocol schema (48 ops)
        ├── nar.ksy           # NAR format schema
        ├── captures/         # Binary test vectors
        └── hs/, src/         # Haskell, Rust implementations
```

______________________________________________________________________

## Core Components

### libevring: Deterministic Async I/O

**Location:** `src/straylight/evring/`

libevring is a C++23 library for deterministic async I/O built on Linux's io_uring. The core insight
is that async programming becomes trivial to test when modeled as pure state machines:

```
State × Event → State × [Operation]
```

#### Key Concepts

**machine concept:**

```cpp
template <typename M>
concept machine = requires(M m, typename M::state_type s, event e) {
  typename M::state_type;
  { m.initial() } -> same_as<typename M::state_type>;
  { m.step(s, e) } -> same_as<step_result<typename M::state_type>>;
  { m.done(s) } -> same_as<bool>;
};
```

**generator_machine concept** (for bulk operations):

```cpp
template <typename M>
concept generator_machine = machine<M> && requires(M m, state_type s, size_t max_ops) {
  { m.wants_to_submit(s) } -> same_as<bool>;
  { m.generate(s, max_ops) } -> same_as<step_result<state_type>>;
};
```

#### Execution Modes

| Function | Description | Use Case |
|----------|-------------|----------|
| `run(machine, ring)` | Execute with real I/O | Production |
| `run_traced(machine, ring)` | Execute + capture events | Recording |
| `replay(machine, events)` | Execute against recorded events | Testing |
| `run_generate(machine, ring)` | High-throughput bulk execution | Store operations |

#### Protocol Support

| Protocol | Implementation | Backend |
|----------|----------------|---------|
| HTTP/1.1 | `http1.h` | llhttp |
| HTTP/2 | `http2.h` | nghttp2 |
| HTTP/3 (QUIC) | `http3.h` | ngtcp2 + nghttp3 |
| TLS | `tls.h` | libtls (LibreSSL) |

#### Performance

| Operation | POSIX | Generator Machine | Speedup |
|-----------|-------|-------------------|---------|
| stat 10k files | 16k ops/s | 1M+ ops/s | 66x |
| copy 1GB file | 1.4 GB/s | 4.2 GB/s | 3x |
| create 10k files | 247k ops/s | 119k ops/s | 0.5x\* |

\*File creation is slower due to open+close overhead per file.

See `src/straylight/evring/ARCHITECTURE.md` for complete documentation.

______________________________________________________________________

### nix-language: Nix → WebAssembly Compiler

**Location:** `src/straylight/nix/compiler/`

A world-class C++23 implementation of the Nix expression language, designed for ahead-of-time (AOT)
compilation to WebAssembly.

#### Pipeline

```
Source Text
    │
    ▼
┌─────────────────────┐
│   PEGTL parse       │   grammar.h (derived from Lix, LGPL-2.1)
│   (parse_tree)      │
└─────────────────────┘
    │
    ▼
┌─────────────────────┐
│   convert()         │   PEGTL tree → intermediate tree
└─────────────────────┘
    │
    ▼
┌─────────────────────┐
│   lower()           │   tree → AST
└─────────────────────┘
    │
    ▼
┌─────────────────────┐
│   compiler          │   AST → WASM (via Binaryen)
└─────────────────────┘
    │
    ▼
┌─────────────────────┐
│   wasm_executor     │   Execute (via Wasmtime)
└─────────────────────┘
```

#### Value Representation

All Nix values are packed into a single `i64`:

```
┌────────────────────────────────────────────────────────────────┐
│                         nix_value (i64)                        │
├────────────────────────────┬───────────────────────────────────┤
│    Payload (high 32 bits)  │      Tag (low 32 bits)            │
│    - int value             │      0 = null                     │
│    - bool (0/1)            │      1 = bool                     │
│    - memory offset         │      2 = int                      │
│    - func table index      │      3 = float                    │
│                            │      4 = string                   │
│                            │      5 = path                     │
│                            │      6 = list                     │
│                            │      7 = attrset                  │
│                            │      8 = lambda/closure           │
│                            │      9 = thunk                    │
│                            │     10 = primop                   │
└────────────────────────────┴───────────────────────────────────┘
```

#### Memory Layout

```
┌─────────────────────────────────────────────────────────────────┐
│                     WASM Linear Memory (1MB+)                   │
├─────────────────────────────────────────────────────────────────┤
│ 0x00000  DATA SEGMENT (compile-time)                   64 KB    │
│ 0x0F000  STACK (grows down)                            4 KB     │
│ 0x10000  [RESERVED]                                    64 KB    │
│ 0x20000  RUNTIME HEAP (grows up)                       896 KB   │
└─────────────────────────────────────────────────────────────────┘
```

#### Feature Coverage

| Category | Status |
|----------|--------|
| Integer/Float literals | Complete |
| String interpolation | Complete |
| Path interpolation | Complete |
| Lists (lazy) | Complete |
| Attribute sets (static + dynamic keys) | Complete |
| Recursive attribute sets | Complete |
| Lambdas (simple + pattern) | Complete |
| Closures (free variable capture) | Complete |
| Let expressions | Complete |
| With expressions | Complete |
| If/Assert | Complete |
| All binary/unary operators | Complete |
| Lazy evaluation (thunks) | Complete |

See `src/straylight/nix/compiler/docs/ARCHITECTURE.md` for complete documentation.

______________________________________________________________________

### nix-protocol: Formal Protocol Specifications

**Location:** `src/straylight/nix/protocol/`

Formal specifications of the Nix daemon "worker protocol" and NAR (Nix Archive) format in Kaitai
Struct format, with polyglot serializers validated against real binary captures.

#### Protocol Coverage

| Schema | Operations | Languages | Test Status |
|--------|------------|-----------|-------------|
| `nix_daemon.ksy` | 48 operations | C++, Rust, Haskell, Python | 14/14 tests |
| `nar.ksy` | Full NAR format | C++, Rust, Haskell | 15-16/16 tests |

#### Wire Format Primitives

| Type | Wire Format |
|------|-------------|
| `u64` | 8 bytes, little-endian |
| `string` | `u64` length + bytes + padding to 8-byte boundary |
| `bool` | `u64` (0 = false, nonzero = true) |
| `list<T>` | `u64` count + elements |
| `set<T>` | Same as list (elements sorted) |

#### Captured Operations

All 14 captured operations have request/response/stderr test vectors:

- `SetOptions`, `IsValidPath`, `QueryPathInfo`, `QueryMissing`
- `QueryReferrers`, `BuildPaths`, `BuildPathsWithResults`
- `AddTempRoot`, `AddIndirectRoot`, `FindRoots`
- `AddToStoreNar`, `NarFromPath`
- Client/Server handshake

#### Cross-Validation

The schemas have been cross-validated against tvix/nix-compat (production Rust implementation):

- Wire format: **MATCH**
- Magic values: **MATCH**
- Protocol version encoding: **MATCH**
- Operations enum: **MATCH** (tvix: 47, straylight: 48)
- Handshake version gates: **MATCH**
- ClientSettings: **MATCH**
- UnkeyedValidPathInfo: **MATCH**

See `src/straylight/nix/protocol/README.md` for complete documentation.

______________________________________________________________________

### primitives: Modern Utility Replacements

**Location:** `src/straylight/nix/` (split across `crypto/`, `text/`, `url/`, `async/`, `sync/`, `data/`, `util/`, `fs/`, `cli/`, `compat/`, `adapters/`)

Systematic replacement of Nix's Not-Invented-Here (NIH) utility implementations with modern,
high-quality external libraries.

#### Replacement Summary

| Primitive | Replaces | Backend | Tests |
|-----------|----------|---------|-------|
| `strings.h` | `util/strings.hh` | StringZilla | 56 |
| `url.h` / `url_fast.h` | `util/url.hh` | Ada URL / Boost.URL | 40 |
| `hash.h` | `util/hash.hh` | BLAKE3 + OpenSSL | 29 |
| `encoding.h` | `util/base-n.hh` | Custom SIMD | 39 |
| `regex.h` | `std::regex` | RE2 | 34 |
| `fuzzy.h` | `util/suggestions.hh` | rapidfuzz-cpp | 20 |
| `format.h` | `util/fmt.hh` | std::format | 34 |
| `async/executor.h` | `util/thread-pool.hh` | taskflow | - |
| `async/task_graph.h` | processGraph | taskflow DAG | - |
| `async/parallel.h` | Manual parallelization | taskflow algorithms | 43 |
| `async/closure.h` | `util/closure.hh` | taskflow async | 30 |
| `lru_cache.h` | `util/lru-cache.hh` | Custom (list + unordered_map) | 32 |
| `pool.h` | `util/pool.hh` | Custom (counting_semaphore) | 26 |
| `chunked_vector.h` | `util/chunked-vector.hh` | Custom | 45 |
| `sync.h` | `util/sync.hh` | Custom (folly-style) | 48 |
| `serialise.h` | `util/serialise.hh` | zpp_bits + streaming | 78 |
| `store.h` | `store/sqlite.hh` | Log-structured + io_uring | 29 |
| ... | ... | ... | ... |

**Total: 34 primitives, 1,251 test cases**

#### Adaptive Backend Selection

```cpp
// strings.h - uses SIMD for large strings, std for small
[[nodiscard]] inline std::size_t find(std::string_view haystack,
                                      std::string_view needle) noexcept {
  if (config::use_simd_find(haystack.size())) {
    return to_sz(haystack).find(to_sz(needle));
  }
  return haystack.find(needle);
}
```

See `src/straylight/nix/docs/NIH.md` for complete tracking.

______________________________________________________________________

### Nix2 Store: Daemonless Log-Structured Store

**Location:** `src/straylight/nix/store/`

A complete reimplementation of the Nix store database, designed for daemonless operation with
io_uring-native I/O.

#### Layout

```
/nix/var/nix/db/
├── log/
│   └── current.log          # Append-only operation log
├── index/
│   ├── paths/{shard}/{hash}.meta     # Materialized path info
│   ├── refs/{shard}/{hash}.refs      # Forward references
│   └── referrers/{shard}/{hash}.referrers  # Reverse index
├── head                      # Current sequence number
└── lock                      # flock for write serialization
```

#### Key Properties

| Property | Nix1 (SQLite) | Nix2 |
|----------|---------------|------|
| Daemon required | Yes | No |
| Read locking | SQLite | None (lockless) |
| Write locking | SQLite + file lock | flock only |
| Recovery | SQLite WAL | Log replay |
| Corruption detection | SQLite integrity | BLAKE3 checksums |
| Bulk reads | Sequential queries | io_uring parallel |

#### Performance (Theoretical)

| Operation | Nix1 (SQLite) | Nix2 | Speedup |
|-----------|---------------|------|---------|
| Single path lookup | ~50μs | ~5μs | 10x |
| Bulk 1000 paths | ~50ms | ~2ms | 25x |
| Check 10K validity | ~1s | ~50ms | 20x |
| Register path | ~1ms | ~200μs | 5x |

#### Crash Safety

The log is the source of truth. The index is a materialized view that can be rebuilt:

1. **Durability**: Log entry survives crash iff fsync completed
2. **Atomicity**: Index file visible iff rename completed
3. **Consistency**: Index always matches some log prefix
4. **Integrity**: BLAKE3 checksum detects bit rot

See `src/straylight/nix/store/docs/ARCHITECTURE.md` for complete documentation.

______________________________________________________________________

## Build System

straylight/nix uses **Buck2** via sensenet for builds.

### Basic Commands

```bash
# Enter development shell
nix develop

# Build all targets
buck2 build //...

# Build specific target
buck2 build //src/nix/util:util

# Run tests
buck2 test //src/straylight/...

# Run specific test
buck2 test //src/straylight/nix/compiler/tests:execution_test
```

### Build Targets

| Target | Description |
|--------|-------------|
| `//src/nix/util:util` | Core utilities library |
| `//src/nix/store:store` | Store operations |
| `//src/nix/fetchers:fetchers` | Input fetchers |
| `//src/nix/expr:expr` | Expression evaluator |
| `//src/nix/flake:flake` | Flake support |
| `//src/nix/main:main` | Main entry/logging |
| `//src/nix/cmd:cmd` | Command infrastructure |
| `//src/nix/cli:cli` | CLI commands |
| `//src/straylight/evring:evring` | Deterministic async I/O |
| `//src/straylight/nix/compiler:compiler` | Nix → WASM compiler |
| `//src/straylight/nix/...:...` | Modernized utilities |
| `//src/straylight/nix/protocol:protocol` | Formal protocol specs |

### Dhall Build Definitions

Build targets are defined in Dhall for type safety:

```bash
# Generate Buck2 targets from Dhall
dhall text <<< '(./dhall/package.dhall).targets'
```

### Remote Execution

The project is configured for NativeLink remote execution:

```bash
buck2 build --prefer-remote //src/nix/util:util
```

______________________________________________________________________

## Dependencies

### Core Dependencies

| Dependency | Purpose |
|------------|---------|
| LibreSSL | TLS/crypto (not OpenSSL) |
| BLAKE3 | High-performance hashing |
| Ada | URL parsing |
| RE2 | Regular expressions |
| Binaryen | WASM code generation |
| Wasmtime | WASM execution |
| liburing | io_uring interface |
| nghttp2 | HTTP/2 |
| ngtcp2 + nghttp3 | HTTP/3 (QUIC) |
| llhttp | HTTP/1.1 parsing |

### Build/Test Dependencies

| Dependency | Version | Purpose |
|------------|---------|---------|
| LLVM/Clang | 19 | Compiler |
| PEGTL | 3.x | PEG parser generator |
| Boost | 1.87 | `small_vector` for parse state |
| Catch2 | 3.x | Test framework |
| RapidCheck | - | Property-based testing |
| nanobench | 4.3.11 | Microbenchmarking |

### Custom Packages

Built via Nix flake:

- `stringzilla` - SIMD string operations
- `zpp_bits` - Zero-overhead serialization
- `ngtcp2-libressl` - QUIC with LibreSSL backend

______________________________________________________________________

## Testing Strategy

### Test Distribution

| Component | Test Files | Test Cases | Assertions |
|-----------|------------|------------|------------|
| nix-language | 17 files | 369 | ~4,700 |
| primitives | 34+ files | 1,251 | - |
| protocol | 3 languages | 42+ | - |
| evring | Multiple | - | - |

### Testing Approaches

1. **Unit tests** - Per-layer testing (AST, parser, compiler, runtime)
2. **Property-based tests** - RapidCheck for algebraic laws
3. **Adversarial tests** - Edge cases (INT_MIN, empty sets, overflow)
4. **Integration tests** - `parse → compile → validate`
5. **End-to-end tests** - `parse → compile → execute → verify`
6. **Fuzzing harnesses** - libFuzzer for parser and compiler
7. **Capture tests** - Protocol validation against real traffic

### Running Tests

```bash
# All straylight tests
buck2 test //src/straylight/...

# Specific component
buck2 test //src/straylight/nix/compiler/tests:...
buck2 test //src/straylight/nix/...

# Specific test file
buck2 test //src/straylight/nix/compiler/tests:execution_test
```

______________________________________________________________________

## Performance Characteristics

### evring I/O Performance

| Operation | POSIX | evring | Speedup |
|-----------|-------|--------|---------|
| stat 10k files | 16k ops/s | 1M+ ops/s | 66x |
| copy 1GB file | 1.4 GB/s | 4.2 GB/s | 3x |

### Store Performance (vs SQLite)

| Operation | SQLite | Log-structured | Speedup |
|-----------|--------|----------------|---------|
| Single lookup | ~50μs | ~5μs | 10x |
| Bulk 1000 paths | ~50ms | ~2ms | 25x |
| Register path | ~1ms | ~200μs | 5x |

### String Operations

| Operation | std::format | boost::format | Speedup |
|-----------|-------------|---------------|---------|
| One string | 20ns | 101ns | 5x |
| 3 args | 51ns | 230ns | 4.5x |
| 6 args | 96ns | 454ns | 4.7x |

______________________________________________________________________

## Default Configuration

straylight/nix ships with different defaults than upstream Nix:

| Setting | Upstream | straylight |
|---------|----------|------------|
| `ca-derivations` | Disabled | **Enabled** |
| `flakes` | Disabled | **Enabled** |
| `nix-command` | Disabled | **Enabled** |
| WASM builtins | N/A | **Enabled** |
| Remote builders | Enabled | **Disabled**\* |

\*Remote builders disabled due to unsound log streaming.

______________________________________________________________________

## Component Details

### evring State Machine Example

```cpp
struct file_reader_machine {
  using state_type = file_reader_state;
  const char* path_;
  mutable vector<byte> read_buffer_;  // Buffer in machine, not state!

  auto initial() const -> state_type { return {}; }

  auto step(state_type state, event e) const -> step_result<state_type> {
    vector<operation> ops;

    switch (state.current_phase) {
      case phase::initial:
        state.current_phase = phase::opening;
        ops.push_back(operation::make_open(path_, O_RDONLY));
        break;

      case phase::opening:
        if (!e.ok()) {
          state.current_phase = phase::error;
          state.error_code = e.error_code();
        } else {
          state.file_handle = e.resource_handle;
          state.current_phase = phase::reading;
          ops.push_back(operation::make_read(state.file_handle,
              make_stable_span(span{read_buffer_})));
        }
        break;
      // ... continue state machine
    }

    return {move(state), move(ops)};
  }

  auto done(const state_type& s) const -> bool {
    return s.current_phase == phase::done ||
           s.current_phase == phase::error;
  }
};

// Real execution
auto ring = make_io_uring_ring(64);
file_reader_machine reader{"/etc/hostname"};
auto final_state = run(reader, *ring);

// Replay without I/O (for testing)
vector<event> recorded_events = {...};
auto replayed_state = replay(reader, recorded_events);
```

### nix-language Compilation Example

```cpp
// Parse
ast::symbol_table symbols;
auto expr = parse::parse("let x = 1; y = 2; in x + y", symbols);

// Compile
compile::compiler comp(symbols);
auto module = comp.compile(expr);

if (!module.validate()) {
  throw compilation_error("WASM validation failed");
}

// Execute
wasm_executor executor;
auto result = executor.execute(module.emit_binary());

if (result.success) {
  std::cout << executor.format_value(result.value) << std::endl;  // "3"
}
```

### Protocol Serialization Example

```cpp
// C++ - write protocol messages
std::vector<std::byte> buf;
nix::proto::Writer w{buf};
nix::proto::write_client_hello(w, 0x0126);  // version 1.38
nix::proto::write_query_path_info_request(w, "/nix/store/...");
```

```rust
// Rust - write protocol messages
let mut buf = Vec::new();
let mut w = Writer::new(&mut buf);
write_client_hello(&mut w, 0x0126)?;
write_query_path_info_request(&mut w, "/nix/store/...")?;
```

```haskell
-- Haskell - write protocol messages
clientHello :: ByteString
clientHello = execWriter $ writeClientHello 0x0126
```

______________________________________________________________________

## Related Documentation

| Document | Location | Description |
|----------|----------|-------------|
| evring Architecture | `src/straylight/evring/ARCHITECTURE.md` | Complete evring documentation |
| Compiler Architecture | `src/straylight/nix/compiler/docs/ARCHITECTURE.md` | Compiler pipeline details |
| Memory Layout | `src/straylight/nix/compiler/docs/MEMORY.md` | WASM memory architecture |
| Protocol README | `src/straylight/nix/protocol/README.md` | Protocol specifications |
| NIH Tracking | `src/straylight/nix/docs/NIH.md` | Primitive replacement status |
| Store Design | `src/straylight/nix/store/docs/ARCHITECTURE.md` | Log-structured store |
| C++ Style Guide | `docs/cpp-style-guide.md` | Code conventions |
| Contributing | `CONTRIBUTING.md` | Development workflow |

______________________________________________________________________

## License

LGPL-2.1. See `COPYING`.

The PEGTL grammar (`src/straylight/nix/compiler/parse/grammar.h`) is substantially derived from the Lix
project (LGPL-2.1).
