# NIH Replacement Tracking

Replace Not-Invented-Here (NIH) utility implementations in Nix with high-quality external libraries.

## Guidelines

- Modern C++23, fully qualify all `std::` types
- Namespace: `straylight::nix::primitives::`
- Use `#define` to switch backends via `constexpr` where applicable
- Tests: Catch2 v3 + rapidcheck (property-based)
- Benchmarks: ankerl::nanobench
- Build with `-march=znver5` (AVX-512, Zen 5)
- Prefer `std::format` over `{fmt}`

---

## Completed Primitives

| Primitive | Replaces | Backend | Tests |
|-----------|----------|---------|-------|
| `strings.h` | `util/strings.hh` | StringZilla | 49 cases, 131 assertions |
| `url.h` / `url_fast.h` | `util/url.hh` | Ada URL / Boost.URL | 40 cases, 159 assertions |
| `hash.h` | `util/hash.hh` | BLAKE3 + OpenSSL | 29 cases, 52 assertions |
| `encoding.h` | `util/base-n.hh` | Custom SIMD | 28 cases, 20,886 assertions |
| `regex.h` | `std::regex` usage | RE2 | 27 cases, 926 assertions |
| `fuzzy.h` | `util/suggestions.hh` | rapidfuzz-cpp | 20 cases, 57 assertions |
| `format.h` | `util/fmt.hh` | std::format | 26 cases, 64 assertions |
| `filesystem/file_lock.h` | `util/file-system.hh` locking | POSIX flock | Part of filesystem_test |
| `filesystem/temp.h` | `util/file-system.hh` temp | POSIX mkstemp/mkdtemp | Part of filesystem_test |
| `filesystem/mmap.h` | Ad-hoc mmap | mio | 23 cases, 84 assertions |
| `async/executor.h` | `util/thread-pool.hh` | taskflow | Part of async_test |
| `async/task_graph.h` | `util/thread-pool.hh` processGraph | taskflow DAG | Part of async_test |
| `async/parallel.h` | Manual parallelization | taskflow algorithms | 36 cases, 112 assertions |

**Total: 13 primitives, 318 test cases, 22,630 assertions**

---

## Full NIH Inventory

### Category 1: Data Structures

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------|
| `util/lru-cache.h` | LRU cache (std::map + std::list) | ~130 | HIGH | abseil LRU / Boost.LRU | TODO |
| `util/pool.h` | Thread-safe resource pool | ~188 | HIGH | Boost.Pool / custom | TODO |
| `util/chunked-vector.h` | Stable-reference chunked container | ~77 | HIGH | boost::deque / custom | TODO |
| `util/thread-pool.h` | Work queue thread pool + processGraph | ~176 | HIGH | taskflow | **DONE** |

### Category 2: Synchronization

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------|
| `util/sync.h` | RAII mutex wrapper (monitor pattern) | ~122 | HIGH | folly::Synchronized | TODO |
| `util/callback.h` | Lambda wrapper with future | ~49 | MEDIUM | std::promise/future | TODO |

### Category 3: Smart Pointers / Memory

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------|
| `util/ref.h` | Non-nullable shared_ptr wrapper | ~80 | MEDIUM | gsl::not_null | TODO |
| `util/finally.h` | Scope guard | ~51 | MEDIUM | gsl::finally / folly::ScopeGuard | TODO |

### Category 4: String Utilities

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------|
| `util/strings.h` | tokenize, split, concat, shell split | ~175 | HIGH | StringZilla / absl | **DONE** |
| `util/split.h` | splitPrefix helpers | ~38 | MEDIUM | std::string_view ranges | TODO |
| `util/hilite.h` | String highlighting | ~21 | LOW | Keep | - |
| `util/regex-combinators.h` | Regex string builders | ~32 | LOW | Keep | - |

### Category 5: Formatting / Output

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------|
| `util/fmt.h` | boost::format wrapper + colors | ~175 | MEDIUM | std::format | **DONE** |
| `util/xml-writer.h` | Simple XML generation | ~53 | HIGH | pugixml | TODO |
| `util/table.h` | Terminal table formatting | ~25 | MEDIUM | tabulate | TODO |
| `util/english.h` | Pluralization | ~16 | LOW | Keep | - |
| `util/ansicolor.h` | ANSI escape macros | ~23 | LOW | Keep | - |

### Category 6: Parsing / Encoding

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------|
| `util/url.h` | RFC3986 URL parsing | ~412 | HIGH | Ada URL / Boost.URL | **DONE** |
| `util/base-n.h` | Base16/Base64 encoding | ~52 | HIGH | Custom SIMD | **DONE** |
| `util/json-utils.h` | nlohmann::json helpers | ~127 | LOW | Keep (thin wrapper) | - |

### Category 7: Hashing / Crypto

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------|
| `util/hash.h` | Multi-algo hash (MD5/SHA/BLAKE3) | ~252 | MEDIUM | BLAKE3 + OpenSSL | **DONE** |
| `util/signature/signer.h` | Signing interface | varies | MEDIUM | Keep (wrapper) | - |
| `util/signature/local-keys.h` | Local key management | varies | MEDIUM | Keep | - |

### Category 8: Serialization / IO

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------|
| `util/serialise.h` | Binary Source/Sink framework | ~572 | MEDIUM | Custom / cereal? | TODO |
| `util/archive.h` | NAR format | ~86 | LOW | Keep (Nix-specific) | - |
| `util/tarfile.h` | libarchive wrapper | ~48 | LOW | Keep (wrapper) | - |
| `util/compression.h` | Multi-algo compression | ~33 | LOW | Keep (wrapper) | - |

### Category 9: File System

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------|
| `util/file-system.h` | Path ops, file I/O, temp files | ~458 | MEDIUM | std::filesystem + custom | PARTIAL |
| `util/file-descriptor.h` | FD handling, AutoCloseFD | ~271 | MEDIUM | Custom RAII | **DONE** (mmap) |
| `util/canon-path.h` | Virtual canonical paths | ~271 | LOW | Keep (Nix-specific) | - |
| `util/source-accessor.h` | Abstract FS interface | ~239 | LOW | Keep (Nix-specific) | - |
| `util/fs-sink.h` | FS object sink | ~147 | LOW | Keep (Nix-specific) | - |

### Category 10: Algorithms

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------|
| `util/topo-sort.h` | Topological sort + cycle detection | ~69 | HIGH | Boost.Graph | TODO |
| `util/closure.h` | Async transitive closure | ~73 | MEDIUM | Custom (async-integrated) | TODO |
| `util/suggestions.h` | Levenshtein + suggestions | ~81 | HIGH | rapidfuzz-cpp | **DONE** |

### Category 11: Error Handling / Logging

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------|
| `util/error.h` | Structured errors, ErrorInfo | ~289 | LOW | Keep (Nix-specific) | - |
| `util/logging.h` | Multi-level logging + activities | ~305 | LOW | Keep (Nix-specific) | - |

### Category 12: Configuration

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------|
| `util/configuration.h` | Settings management | ~425 | LOW | Keep (Nix-specific) | - |
| `util/experimental-features.h` | Feature flags | ~111 | LOW | Keep (Nix-specific) | - |

### Category 13: Command Line

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------|
| `util/args.h` | CLI argument parsing | ~422 | MEDIUM | CLI11 | TODO |

### Category 14: Process / Signals

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------|
| `util/processes.h` | Process spawning, Pid RAII | ~129 | LOW | Keep (platform-specific) | - |
| `util/signals.h` | Signal handling, interrupts | ~63 | LOW | std::stop_token? | TODO |
| `util/terminal.h` | TTY detection, window size | ~62 | LOW | Keep | - |

### Category 15: Database

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------|
| `store/sqlite.h` | RAII SQLite wrappers | ~188 | MEDIUM | SQLiteCpp | TODO |

### Category 16: Networking

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------|
| `store/filetransfer.h` | HTTP/S3 transfer abstraction | ~312 | LOW | Keep (libcurl wrapper) | - |

### Category 17: Metaprogramming / Utilities

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------|
| `util/comparator.h` | Comparison operator macros | ~51 | LOW | C++20 `<=>` | TODO |
| `util/variant-wrapper.h` | Variant wrapper macros | ~32 | LOW | Keep | - |
| `util/checked-arithmetic.h` | Overflow-safe arithmetic | ~150 | MEDIUM | SafeInt / builtins | TODO |

### Category 18: Domain-Specific

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------|
| `util/git.h` | Git object parsing | ~204 | LOW | libgit2? | TODO |
| `expr/symbol-table.h` | String interning | ~278 | LOW | Keep (Nix-specific) | - |
| `cmd/markdown.h` | Terminal markdown | ~18 | MEDIUM | cmark | TODO |

---

## Priority Queue

### P0: Next Up (High value, clear alternatives)
1. `util/lru-cache.h` → abseil / custom
2. `util/pool.h` → Boost.Pool / std::counting_semaphore
3. `util/sync.h` → folly::Synchronized pattern
4. `util/topo-sort.h` → Boost.Graph / custom
5. `util/xml-writer.h` → pugixml

### P1: Medium Priority
6. `util/args.h` → CLI11
7. `util/finally.h` → gsl::finally / [[nodiscard]] RAII
8. `util/ref.h` → gsl::not_null
9. `store/sqlite.h` → SQLiteCpp
10. `util/serialise.h` → investigate (cereal? custom?)

### P2: Low Priority / Investigate
11. `util/signals.h` → std::stop_token (C++20)
12. `util/comparator.h` → C++20 spaceship
13. `util/checked-arithmetic.h` → SafeInt
14. `util/git.h` → libgit2
15. `cmd/markdown.h` → cmark

### Keep (Nix-specific, deeply integrated)
- `util/error.h`, `util/logging.h`, `util/configuration.h`
- `util/archive.h`, `util/canon-path.h`, `util/source-accessor.h`
- `util/tarfile.h`, `util/compression.h` (already wrappers)
- `expr/symbol-table.h`

---

## Integration Status

| Primitive | Implemented | Tested | Integrated into Nix |
|-----------|-------------|--------|---------------------|
| strings | ✓ | ✓ | ✗ |
| url | ✓ | ✓ | ✗ |
| hash | ✓ | ✓ | ✗ |
| encoding | ✓ | ✓ | ✗ |
| regex | ✓ | ✓ | ✗ |
| fuzzy | ✓ | ✓ | ✗ |
| format | ✓ | ✓ | ✗ |
| file_lock | ✓ | ✓ | ✗ |
| temp | ✓ | ✓ | ✗ |
| mmap | ✓ | ✓ | ✗ |
| executor | ✓ | ✓ | ✗ |
| task_graph | ✓ | ✓ | ✗ |
| parallel | ✓ | ✓ | ✗ |

---

## Notes

### Catch2 v3 / rapidcheck Include Order
```cpp
// CORRECT - Catch2 MUST come first
#include <catch2/catch_test_macros.hpp>
#include <rapidcheck/catch.h>
```

### taskflow API
- Constructor takes `std::unique_ptr<WorkerInterface>` (not shared_ptr)
- `WorkerInterface::scheduler_prologue/epilogue` for GC thread registration

### Nix ThreadPool Workloads
All ThreadPool usages are heavy I/O or compute (NAR transfers, crypto, HTTP, Git).
~2.5μs task submission overhead is negligible.

### boost::format Patterns
~309 usages with mixed syntax: `%s`, `%d`, `%1%`, `%2%`, `%|2$5d|`

### std::format Performance
| Operation | std::format | boost::format | Speedup |
|-----------|-------------|---------------|---------|
| One string | 20ns | 101ns | 5x |
| 3 args | 51ns | 230ns | 4.5x |
| 6 args | 96ns | 454ns | 4.7x |
