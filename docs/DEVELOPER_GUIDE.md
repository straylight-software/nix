# straylight/nix Developer Guide

```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
                                    // straylight // developer guide
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

This guide provides everything you need to understand, build, and contribute to straylight/nix.

______________________________________________________________________

## Table of Contents

1. [Quick Start](#quick-start)
2. [Understanding the Codebase](#understanding-the-codebase)
3. [Component Deep Dives](#component-deep-dives)
4. [Development Workflow](#development-workflow)
5. [Testing](#testing)
6. [Common Tasks](#common-tasks)
7. [Troubleshooting](#troubleshooting)
8. [Architecture Decision Records](#architecture-decision-records)

______________________________________________________________________

## Quick Start

### Prerequisites

- Linux (io_uring requires kernel 5.1+, recommended 5.11+)
- Nix with flakes enabled

### Setup

```bash
# Clone the repository
git clone git@github.com:straylight-software/nix.git
cd nix

# Enter the development shell
nix develop

# Build everything
buck2 build //...

# Run all straylight tests
buck2 test //src/straylight/...
```

### Verify Your Setup

```bash
# Check that clangd is working (for editor integration)
which clangd

# Verify compile_commands.json exists
ls compile_commands.json

# Run a quick test
buck2 test //src/straylight/nix/text/tests:strings_test
```

______________________________________________________________________

## Understanding the Codebase

### The Big Picture

straylight/nix is a ground-up rethinking of Nix with four major innovations:

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           straylight/nix                                │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐          │
│  │    libevring    │  │  nix-language   │  │  nix-protocol   │          │
│  │                 │  │                 │  │                 │          │
│  │  Deterministic  │  │   Nix → WASM    │  │ Formal Protocol │          │
│  │  Async I/O      │  │   Compiler      │  │ Specifications  │          │
│  │                 │  │                 │  │                 │          │
│  │  io_uring +     │  │  Binaryen +     │  │  Kaitai Struct  │          │
│  │  State Machines │  │  Wasmtime       │  │  + Test Vectors │          │
│  └────────┬────────┘  └────────┬────────┘  └────────┬────────┘          │
│           │                    │                    │                   │
│           └────────────────────┼────────────────────┘                   │
│                                │                                        │
│  ┌─────────────────────────────┴─────────────────────────────┐          │
│  │              modern utilities (crypto, text, url, etc.)   │          │
│  │                                                           │          │
│  │  34 modernized utility modules replacing NIH code         │          │
│  │  StringZilla | RE2 | BLAKE3 | taskflow | zpp_bits         │          │
│  └───────────────────────────────────────────────────────────┘          │
│                                │                                        │
│  ┌─────────────────────────────┴─────────────────────────────┐          │
│  │                       Nix2 Store                          │          │
│  │                                                           │          │
│  │  Daemonless, log-structured, io_uring-native              │          │
│  │  10-25x faster than SQLite                                │          │
│  └───────────────────────────────────────────────────────────┘          │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

### Directory Structure

```
src/
├── nix/                      # Core Nix fork (upstream + modifications)
│   ├── util/                 # Utility files
│   ├── store/                # Store implementations
│   ├── expr/                 # Expression evaluator
│   ├── fetchers/             # Git, GitHub, tarball fetchers
│   ├── flake/                # Flake support
│   ├── main/                 # Entry point, logging
│   ├── cmd/                  # Command infrastructure
│   └── cli/                  # CLI command implementations
│
└── straylight/               # straylight innovations
    ├── evring/               # Deterministic async I/O
    └── nix/
        ├── compiler/         # Nix → WASM compiler
        ├── protocol/         # Formal protocol specs
        └── {crypto,text,url,async,sync,store,...}/  # Modern utility modules
```

### Key Files to Read First

| File | Description | Why Read It | |------|-------------|-------------| | `ARCHITECTURE.md` |
Project overview | Understand the whole system | | `docs/cpp-style-guide.md` | Code conventions |
Write conformant code | | `src/straylight/evring/ARCHITECTURE.md` | evring deep dive | Understand
async I/O | | `src/straylight/nix/compiler/docs/ARCHITECTURE.md` | Compiler deep dive | Understand
WASM compilation | | `src/straylight/nix/docs/NIH.md` | NIH replacement tracking | Understand
modernization | | `src/straylight/nix/protocol/README.md` | Protocol specs | Understand daemon
communication |

______________________________________________________________________

## Component Deep Dives

### 1. libevring - Deterministic Async I/O

**Problem:** Traditional async code is impossible to test deterministically.

**Solution:** Model async operations as pure state machines.

```cpp
// The core abstraction
State × Event → State × [Operation]

// As a C++ concept
template <typename M>
concept machine = requires(M m, typename M::state_type s, event e) {
  typename M::state_type;
  { m.initial() } -> same_as<typename M::state_type>;
  { m.step(s, e) } -> same_as<step_result<typename M::state_type>>;
  { m.done(s) } -> same_as<bool>;
};
```

**Key insight:** The `step` function is **pure** — given the same state and event, it always
produces the same new state and operations. This means you can:

1. **Run with real I/O:** `run(machine, ring)` — for production
2. **Replay recorded events:** `replay(machine, events)` — for testing
3. **Capture events:** `run_traced(machine, ring)` — for recording

**When to use evring:**

- Any network I/O (HTTP, TLS, QUIC)
- Bulk file operations (stat thousands of files)
- Anything that would traditionally use callbacks or futures

**Example: HTTP GET request**

```cpp
// The machine handles TLS handshake → HTTP/2 connection → request/response
auto ring = evring::make_io_uring_ring(256);
auto config = evring::tls_client_config::create_default();
config.set_alpn("h2,http/1.1");

// Connect, handshake, send request, receive response
// All as pure state transitions!
http_client_machine client{"example.com", 443, "/api/data"};
auto final_state = evring::run(client, *ring);

if (final_state.ok()) {
  std::cout << final_state.response.body << std::endl;
}
```

**For more:** See `src/straylight/evring/ARCHITECTURE.md`

______________________________________________________________________

### 2. nix-language - Nix → WASM Compiler

**Problem:** Nix's tree-walking interpreter is slow and hard to optimize.

**Solution:** AOT compile Nix expressions to WebAssembly.

**Pipeline:**

```
Nix Source
    │
    ▼ PEGTL grammar
┌──────────┐
│  Parse   │  grammar.h (from Lix)
└──────────┘
    │
    ▼ parse tree
┌──────────┐
│ Convert  │  PEGTL tree → our tree
└──────────┘
    │
    ▼ intermediate tree
┌──────────┐
│  Lower   │  tree → AST
└──────────┘
    │
    ▼ AST
┌──────────┐
│ Compile  │  AST → WASM (Binaryen)
└──────────┘
    │
    ▼ WASM binary
┌──────────┐
│ Execute  │  Run (Wasmtime)
└──────────┘
    │
    ▼
  Result
```

**Key design decisions:**

1. **Variant-based AST** (not inheritance hierarchies)
2. **Lazy evaluation via thunks** (memoized, with cycle detection)
3. **Closures capture free variables** (analyzed at compile time)
4. **Values packed into i64** (tag in low 32 bits, payload in high 32)

**Example: End-to-end**

```cpp
// Parse
ast::symbol_table symbols;
auto expr = parse::parse("let x = 1; y = 2; in x + y", symbols);

// Compile
compile::compiler comp(symbols);
auto module = comp.compile(expr);

// Execute
wasm_executor executor;
auto result = executor.execute(module.emit_binary());

assert(result.success);
assert(get_int_value(result.value) == 3);
```

**For more:** See `src/straylight/nix/compiler/docs/ARCHITECTURE.md`

______________________________________________________________________

### 3. nix-protocol - Formal Protocol Specifications

**Problem:** The Nix daemon protocol is undocumented and implementations diverge.

**Solution:** Machine-readable specifications with validated test vectors.

**Approach:**

1. **Kaitai Struct schemas** — formal grammar for wire format
2. **Captured traffic** — real request/response pairs from `nix-daemon`
3. **Polyglot implementations** — C++, Rust, Haskell all validated
4. **Cross-validation** — verified against tvix/nix-compat

**Protocol basics:**

```
Wire primitives:
- u64: 8 bytes little-endian
- string: u64 length + bytes + padding to 8-byte boundary
- bool: u64 (0 = false, nonzero = true)
- list<T>: u64 count + elements

Message flow:
Client                    Server
  │                         │
  │── WORKER_MAGIC_1 ──────▶│
  │◀── WORKER_MAGIC_2 ──────│
  │── version + features ──▶│
  │◀── version + features ──│
  │                         │
  │── Op + Request ────────▶│
  │◀── STDERR_* messages ───│
  │◀── STDERR_LAST + Resp ──│
```

**For more:** See `src/straylight/nix/protocol/README.md`

______________________________________________________________________

### 4. Modern Utility Modules

**Problem:** Nix has many Not-Invented-Here implementations that are slower and buggier than
well-tested libraries.

**Solution:** Systematic replacement with modern, high-quality libraries.

**The replacement philosophy:**

1. **Profile first** — identify actual performance bottlenecks
2. **Test equivalence** — property-based tests ensure identical behavior
3. **Adaptive backends** — use SIMD for large inputs, std for small
4. **Zero-cost when unused** — header-only where possible

**Example: Adaptive string search**

```cpp
// Uses SIMD for large strings, std for small
[[nodiscard]] inline std::size_t find(std::string_view haystack,
                                      std::string_view needle) noexcept {
  if (config::use_simd_find(haystack.size())) {
    return to_sz(haystack).find(to_sz(needle));  // StringZilla
  }
  return haystack.find(needle);  // std
}
```

**For more:** See `src/straylight/nix/docs/NIH.md`

______________________________________________________________________

### 5. Nix2 Store - Daemonless Log-Structured Store

**Problem:** SQLite store requires nix-daemon for coordination, adding latency and complexity.

**Solution:** Log-structured store with flock coordination.

**Key insight:** The kernel's `flock` is process-death safe. If a writer crashes, the kernel
releases the lock. No daemon needed.

**Layout:**

```
/nix/var/nix/db/
├── log/current.log              # Append-only source of truth
├── index/
│   ├── paths/{shard}/{hash}.meta    # Materialized path info
│   ├── refs/{shard}/{hash}.refs     # Forward references
│   └── referrers/{shard}/{hash}.referrers
├── head                         # Current sequence number
└── lock                         # flock for write serialization
```

**Write path:**

```
1. flock(lock, LOCK_EX)
2. Append to log (zpp_bits serialized)
3. BLAKE3 checksum
4. fsync(log)
5. Update index files (atomic rename)
6. funlock
```

**Read path:**

```
1. Direct read from index/ (no lock needed)
2. io_uring for bulk operations
```

**For more:** See `src/straylight/nix/store/docs/ARCHITECTURE.md`

______________________________________________________________________

## Development Workflow

### Editor Setup

**VS Code / Cursor:**

```json
// .vscode/settings.json
{
  "clangd.path": "${workspaceFolder}/result/bin/clangd",
  "clangd.arguments": ["--compile-commands-dir=${workspaceFolder}"]
}
```

**Emacs:**

```elisp
;; .dir-locals.el is already configured
;; Just ensure you're in nix develop shell
```

**Neovim:**

```lua
-- clangd should work automatically with compile_commands.json
```

### Building

```bash
# Full build
buck2 build //...

# Specific target
buck2 build //src/straylight/evring:evring

# With remote execution (faster for large builds)
buck2 build --prefer-remote //...
```

### Testing

```bash
# All straylight tests
buck2 test //src/straylight/...

# Specific component
buck2 test //src/straylight/test/unit/compiler:...

# Single test file
buck2 test //src/straylight/test/unit/compiler:compiler_test

# Run with output
buck2 test //src/straylight/test/unit/compiler:compiler_test -- --verbose
```

### Formatting

```bash
# Format all code
nix fmt

# Or directly
clang-format -i src/straylight/**/*.cpp
nixfmt flake.nix
```

### Pre-commit Hooks

```bash
# Install
pre-commit install --install-hooks

# Run manually
pre-commit run --all-files
```

______________________________________________________________________

## Testing

### Test Categories

| Category | Framework | Purpose | |----------|-----------|---------| | Unit tests | Catch2 |
Per-function correctness | | Property tests | RapidCheck | Algebraic invariants | | Adversarial
tests | Catch2 | Edge cases (INT_MIN, overflow) | | Integration tests | Catch2 | Multi-component
pipelines | | End-to-end tests | Catch2 | Full parse → execute | | Fuzzing | libFuzzer |
Crash/undefined behavior | | Capture tests | Custom | Protocol validation |

### Writing Tests

```cpp
// Catch2 basics
TEST_CASE("descriptive name", "[tag]") {
  REQUIRE(expected == actual);
  CHECK(condition);  // continues on failure
}

// Property-based with RapidCheck
TEST_CASE("roundtrip preserves value", "[property]") {
  rc::check([](int value) {
    auto serialized = serialize(value);
    auto deserialized = deserialize(serialized);
    RC_ASSERT(deserialized == value);
  });
}

// Adversarial
TEST_CASE("handles INT_MIN division", "[adversarial]") {
  // INT_MIN / -1 would overflow
  auto result = safe_divide(INT_MIN, -1);
  REQUIRE(result.has_error());
}
```

### Test File Locations

- Component tests: `src/straylight/<component>/tests/`
- Nix core tests: `src/nix/<component>/tests/`

______________________________________________________________________

## Common Tasks

### Adding a New Utility Module

1. Create header in appropriate `src/straylight/nix/<module>/` (crypto, text, url, async, sync,
   data, util, fs, cli, compat)
2. Add tests in `src/straylight/nix/<module>/tests/`
3. Update `NIH.md` with the new module
4. Add to `dhall/package.dhall` if needed

### Adding a New evring Operation

1. Add operation type to `event.h`
2. Implement in `io_uring_ring.cpp`
3. Add to `operation::make_*` builders
4. Write state machine test

### Adding a Protocol Operation

1. Update `nix_daemon.ksy`
2. Capture real traffic to `captures/`
3. Update serializers in all languages
4. Add tests validating against captures

### Adding a Language Feature

1. Update grammar in `parse/grammar.h`
2. Add AST node in `ast/expression.h`
3. Implement in `compile/compiler.h`
4. Add runtime support if needed
5. Write tests at each level

______________________________________________________________________

## Troubleshooting

### Build Fails with Missing Headers

```bash
# Regenerate compile_commands.json
buck2 build //...

# Check that you're in nix develop
echo $IN_NIX_SHELL  # should be "impure" or "pure"
```

### Tests Fail with io_uring Errors

```bash
# Check kernel version (need 5.11+)
uname -r

# Check io_uring support
cat /proc/sys/kernel/io_uring_disabled  # should be 0
```

### clangd Not Finding Files

```bash
# Ensure compile_commands.json is fresh
rm compile_commands.json
buck2 build //...
ls compile_commands.json
```

### Remote Execution Fails

```bash
# Check network connectivity
curl -s https://sense-scheduler.fly.dev/health

# Fall back to local
buck2 build --no-remote //...
```

______________________________________________________________________

## Architecture Decision Records

### ADR-001: State Machines for Async I/O

**Context:** Testing async code is traditionally difficult.

**Decision:** Model all async operations as pure state machines.

**Consequences:**

- All async code is trivially testable via replay
- Slight overhead from state copying (~2%)
- More verbose than callbacks, but more debuggable

### ADR-002: WASM as Compilation Target

**Context:** Tree-walking interpretation is slow.

**Decision:** AOT compile Nix to WebAssembly.

**Consequences:**

- Significant speedup for pure computation
- Can run sandboxed (WASM capabilities)
- Requires runtime for builtins that access store

### ADR-003: Log-Structured Store

**Context:** SQLite requires daemon for safe concurrent access.

**Decision:** Log-structured design with flock coordination.

**Consequences:**

- No daemon required
- Crash-safe via checksums + log replay
- Slightly more disk space (log + index)

### ADR-004: LibreSSL over OpenSSL

**Context:** OpenSSL has complex API and licensing.

**Decision:** Use LibreSSL for all TLS/crypto.

**Consequences:**

- Cleaner libtls API
- Smaller attack surface
- Some libraries need patches for LibreSSL

### ADR-005: Kaitai Struct for Protocol Specs

**Context:** Protocol documentation is incomplete and divergent.

**Decision:** Formal machine-readable specifications.

**Consequences:**

- Auto-generate parsers in multiple languages
- Test vectors ensure implementations agree
- Some Kaitai limitations for complex protocols

______________________________________________________________________

## Further Reading

- [ARCHITECTURE.md](../ARCHITECTURE.md) — Project overview
- [cpp-style-guide.md](cpp-style-guide.md) — Code conventions
- [evring ARCHITECTURE](../src/straylight/evring/ARCHITECTURE.md) — Async I/O details
- [Compiler ARCHITECTURE](../src/straylight/nix/compiler/docs/ARCHITECTURE.md) — Compiler details
- [protocol README](../src/straylight/nix/protocol/README.md) — Protocol specs
- [NIH.md](../src/straylight/nix/docs/NIH.md) — Primitive tracking
- [STORE_DESIGN.md](../src/straylight/nix/store/docs/ARCHITECTURE.md) — Store design
