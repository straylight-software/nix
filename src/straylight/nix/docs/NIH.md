# NIH Replacement Tracking

Replace Not-Invented-Here (NIH) utility implementations in Nix with high-quality external libraries.

## Guidelines

- Modern C++23, fully qualify all `std::` types
- Namespace: `straylight::nix::{module}::` (e.g., `straylight::nix::text::`,
  `straylight::nix::crypto::`)
- Use `#define` to switch backends via `constexpr` where applicable
- Tests: Catch2 v3 + rapidcheck (property-based)
- Benchmarks: ankerl::nanobench
- Build with `-march=znver5` (AVX-512, Zen 5)
- Prefer `std::format` over `{fmt}`

______________________________________________________________________

## Completed Primitives

| Primitive | Replaces | Backend | Tests | |-----------|----------|---------|-------| | `strings.h`
| `util/strings.hh` | StringZilla | 56 cases | | `url.h` / `url_fast.h` | `util/url.hh` | Ada URL /
Boost.URL | 40 cases | | `hash.h` | `util/hash.hh` | BLAKE3 + OpenSSL | 29 cases | | `encoding.h` |
`util/base-n.hh` | Custom SIMD | 39 cases | | `regex.h` | `std::regex` usage | RE2 | 34 cases | |
`fuzzy.h` | `util/suggestions.hh` | rapidfuzz-cpp | 20 cases | | `format.h` | `util/fmt.hh` |
std::format | 34 cases | | `filesystem/file_lock.h` | `util/file-system.hh` locking | POSIX flock |
Part of filesystem_test | | `filesystem/temp.h` | `util/file-system.hh` temp | POSIX mkstemp/mkdtemp
| Part of filesystem_test | | `filesystem/mmap.h` | Ad-hoc mmap | mio | 23 cases | |
`async/executor.h` | `util/thread-pool.hh` | taskflow | Part of async_test | | `async/task_graph.h`
| `util/thread-pool.hh` processGraph | taskflow DAG | Part of async_test | | `async/parallel.h` |
Manual parallelization | taskflow algorithms | 43 cases | | `async/closure.h` | `util/closure.hh` |
taskflow async | 30 cases | | `lru_cache.h` | `util/lru-cache.hh` | Custom (std::list +
unordered_map) | 32 cases | | `pool.h` | `util/pool.hh` | Custom (std::counting_semaphore) | 26
cases | | `chunked_vector.h` | `util/chunked-vector.hh` | Custom | 45 cases | | `sync.h` |
`util/sync.hh` | Custom (folly-style Synchronized) | 48 cases | | `callback.h` | `util/callback.hh`
| std::promise/future | 29 cases | | `ref.h` | `util/ref.hh` | Custom (gsl::not_null-style) | 45
cases | | `finally.h` | `util/finally.hh` | Custom RAII | 39 cases | | `topo_sort.h` |
`util/topo-sort.hh` | Custom (Kahn's algorithm) | 23 cases | | `signals.h` | `util/signals.hh` |
std::stop_token + custom | 34 cases | | `checked_arithmetic.h` | `util/checked-arithmetic.hh` |
Compiler builtins | 37 cases | | `xml_writer.h` | `util/xml-writer.hh` | Custom (pugixml-style API)
| 43 cases | | `split.h` | `util/split.hh` | std::string_view ranges | 65 cases | | `table.h` |
`util/table.hh` | Custom | 60 cases | | `comparator.h` | `util/comparator.hh` | C++20 spaceship | 46
cases | | `args.h` | `util/args.hh` | Custom (CLI11-style) | 55 cases | | `sqlite.h` |
`store/sqlite.hh` | Custom (SQLiteCpp-style) | 55 cases | | `git.h` | `util/git.hh` | Custom parsing
| 45 cases | | `markdown.h` | `cmd/markdown.hh` | Custom | 68 cases | | `serialise.h` |
`util/serialise.hh` | zpp_bits + streaming | 78 cases | | `store.h` | `store/sqlite.hh` +
`local-store.cpp` | Log-structured + flock + io_uring | 29 cases |

**Total: 34 primitives, 1251 test cases**

______________________________________________________________________

## Full NIH Inventory

### Category 1: Data Structures

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------| | `util/lru-cache.h` | LRU
cache (std::map + std::list) | ~130 | HIGH | Custom (std::list + unordered_map) | **DONE** | |
`util/pool.h` | Thread-safe resource pool | ~188 | HIGH | Custom (std::counting_semaphore) |
**DONE** | | `util/chunked-vector.h` | Stable-reference chunked container | ~77 | HIGH | Custom |
**DONE** | | `util/thread-pool.h` | Work queue thread pool + processGraph | ~176 | HIGH | taskflow |
**DONE** |

### Category 2: Synchronization

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------| | `util/sync.h` | RAII mutex
wrapper (monitor pattern) | ~122 | HIGH | Custom (folly-style) | **DONE** | | `util/callback.h` |
Lambda wrapper with future | ~49 | MEDIUM | std::promise/future | **DONE** |

### Category 3: Smart Pointers / Memory

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------| | `util/ref.h` | Non-nullable
shared_ptr wrapper | ~80 | MEDIUM | Custom (gsl::not_null-style) | **DONE** | | `util/finally.h` |
Scope guard | ~51 | MEDIUM | Custom RAII | **DONE** |

### Category 4: String Utilities

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------| | `util/strings.h` |
tokenize, split, concat, shell split | ~175 | HIGH | StringZilla / absl | **DONE** | |
`util/split.h` | splitPrefix helpers | ~38 | MEDIUM | std::string_view ranges | **DONE** | |
`util/hilite.h` | String highlighting | ~21 | LOW | Keep | - | | `util/regex-combinators.h` | Regex
string builders | ~32 | LOW | Keep | - |

### Category 5: Formatting / Output

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------| | `util/fmt.h` |
boost::format wrapper + colors | ~175 | MEDIUM | std::format | **DONE** | | `util/xml-writer.h` |
Simple XML generation | ~53 | HIGH | Custom (pugixml-style API) | **DONE** | | `util/table.h` |
Terminal table formatting | ~25 | MEDIUM | Custom | **DONE** | | `util/english.h` | Pluralization |
~16 | LOW | Keep | - | | `util/ansicolor.h` | ANSI escape macros | ~23 | LOW | Keep | - |

### Category 6: Parsing / Encoding

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------| | `util/url.h` | RFC3986 URL
parsing | ~412 | HIGH | Ada URL / Boost.URL | **DONE** | | `util/base-n.h` | Base16/Base64 encoding
| ~52 | HIGH | Custom SIMD | **DONE** | | `util/json-utils.h` | nlohmann::json helpers | ~127 | LOW
| Keep (thin wrapper) | - |

### Category 7: Hashing / Crypto

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------| | `util/hash.h` | Multi-algo
hash (MD5/SHA/BLAKE3) | ~252 | MEDIUM | BLAKE3 + OpenSSL | **DONE** | | `util/signature/signer.h` |
Signing interface | varies | MEDIUM | Keep (wrapper) | - | | `util/signature/local-keys.h` | Local
key management | varies | MEDIUM | Keep | - |

### Category 8: Serialization / IO

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------| | `util/serialise.h` | Binary
Source/Sink framework | ~572 | MEDIUM | zpp_bits + streaming | **DONE** | | `util/archive.h` | NAR
format | ~86 | LOW | Keep (Nix-specific) | - | | `util/tarfile.h` | libarchive wrapper | ~48 | LOW |
Keep (wrapper) | - | | `util/compression.h` | Multi-algo compression | ~33 | LOW | Keep (wrapper) |
\- |

### Category 9: File System

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------| | `util/file-system.h` | Path
ops, file I/O, temp files | ~458 | MEDIUM | std::filesystem + custom | PARTIAL | |
`util/file-descriptor.h` | FD handling, AutoCloseFD | ~271 | MEDIUM | Custom RAII | **DONE** (mmap)
| | `util/canon-path.h` | Virtual canonical paths | ~271 | LOW | Keep (Nix-specific) | - | |
`util/source-accessor.h` | Abstract FS interface | ~239 | LOW | Keep (Nix-specific) | - | |
`util/fs-sink.h` | FS object sink | ~147 | LOW | Keep (Nix-specific) | - |

### Category 10: Algorithms

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------| | `util/topo-sort.h` |
Topological sort + cycle detection | ~69 | HIGH | Custom (Kahn's algorithm) | **DONE** | |
`util/closure.h` | Async transitive closure | ~73 | MEDIUM | taskflow async | **DONE** | |
`util/suggestions.h` | Levenshtein + suggestions | ~81 | HIGH | rapidfuzz-cpp | **DONE** |

### Category 11: Error Handling / Logging

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------| | `util/error.h` | Structured
errors, ErrorInfo | ~289 | LOW | Keep (Nix-specific) | - | | `util/logging.h` | Multi-level logging
\+ activities | ~305 | LOW | Keep (Nix-specific) | - |

### Category 12: Configuration

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------| | `util/configuration.h` |
Settings management | ~425 | LOW | Keep (Nix-specific) | - | | `util/experimental-features.h` |
Feature flags | ~111 | LOW | Keep (Nix-specific) | - |

### Category 13: Command Line

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------| | `util/args.h` | CLI
argument parsing | ~422 | MEDIUM | Custom (CLI11-style) | **DONE** |

### Category 14: Process / Signals

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------| | `util/processes.h` |
Process spawning, Pid RAII | ~129 | LOW | Keep (platform-specific) | - | | `util/signals.h` | Signal
handling, interrupts | ~63 | LOW | std::stop_token + custom | **DONE** | | `util/terminal.h` | TTY
detection, window size | ~62 | LOW | Keep | - |

### Category 15: Database / Storage

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------| | `store/sqlite.h` | RAII
SQLite wrappers | ~188 | MEDIUM | Custom (SQLiteCpp-style) | **DONE** | | `store/local-store.cpp` |
SQLite store backend | ~1500 | HIGH | Log-structured + flock + io_uring | **DONE** |

### Category 16: Networking

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------| | `store/filetransfer.h` |
HTTP/S3 transfer abstraction | ~312 | LOW | Keep (libcurl wrapper) | - |

### Category 17: Metaprogramming / Utilities

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------| | `util/comparator.h` |
Comparison operator macros | ~51 | LOW | C++20 spaceship | **DONE** | | `util/variant-wrapper.h` |
Variant wrapper macros | ~32 | LOW | Keep | - | | `util/checked-arithmetic.h` | Overflow-safe
arithmetic | ~150 | MEDIUM | Compiler builtins | **DONE** |

### Category 18: Domain-Specific

| File | Description | Lines | NIH Level | Proposed Backend | Status |
|------|-------------|-------|-----------|------------------|--------| | `util/git.h` | Git object
parsing | ~204 | LOW | Custom parsing | **DONE** | | `expr/symbol-table.h` | String interning | ~278
| LOW | Keep (Nix-specific) | - | | `cmd/markdown.h` | Terminal markdown | ~18 | MEDIUM | Custom |
**DONE** |

______________________________________________________________________

## Priority Queue

**All planned replacements are now complete!**

### Keep (Nix-specific, deeply integrated)

- `util/error.h`, `util/logging.h`, `util/configuration.h`
- `util/archive.h`, `util/canon-path.h`, `util/source-accessor.h`
- `util/tarfile.h`, `util/compression.h` (already wrappers)
- `expr/symbol-table.h`

### Next Phase: Integration

All primitives are implemented and tested. Next step is integrating them into the Nix codebase to
replace the original NIH implementations.

______________________________________________________________________

## Integration Status

| Primitive | Implemented | Tested | Integrated into Nix |
|-----------|-------------|--------|---------------------| | strings | ✓ | ✓ | ✗ | | url | ✓ | ✓ | ✗
| | hash | ✓ | ✓ | ✗ | | encoding | ✓ | ✓ | ✗ | | regex | ✓ | ✓ | ✗ | | fuzzy | ✓ | ✓ | ✗ | | format
| ✓ | ✓ | ✗ | | file_lock | ✓ | ✓ | ✗ | | temp | ✓ | ✓ | ✗ | | mmap | ✓ | ✓ | ✗ | | executor | ✓ | ✓
| ✗ | | task_graph | ✓ | ✓ | ✗ | | parallel | ✓ | ✓ | ✗ | | closure | ✓ | ✓ | ✗ | | lru_cache | ✓ |
✓ | ✗ | | pool | ✓ | ✓ | ✗ | | chunked_vector | ✓ | ✓ | ✗ | | sync | ✓ | ✓ | ✗ | | callback | ✓ | ✓
| ✗ | | ref | ✓ | ✓ | ✗ | | finally | ✓ | ✓ | ✗ | | topo_sort | ✓ | ✓ | ✗ | | signals | ✓ | ✓ | ✗ |
| checked_arithmetic | ✓ | ✓ | ✗ | | xml_writer | ✓ | ✓ | ✗ | | split | ✓ | ✓ | ✗ | | table | ✓ | ✓
| ✗ | | comparator | ✓ | ✓ | ✗ | | args | ✓ | ✓ | ✗ | | sqlite | ✓ | ✓ | ✗ | | git | ✓ | ✓ | ✗ | |
markdown | ✓ | ✓ | ✗ | | serialise | ✓ | ✓ | ✗ |

______________________________________________________________________

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

All ThreadPool usages are heavy I/O or compute (NAR transfers, crypto, HTTP, Git). ~2.5μs task
submission overhead is negligible.

### boost::format Patterns

~309 usages with mixed syntax: `%s`, `%d`, `%1%`, `%2%`, `%|2$5d|`

### std::format Performance

| Operation | std::format | boost::format | Speedup |
|-----------|-------------|---------------|---------| | One string | 20ns | 101ns | 5x | | 3 args |
51ns | 230ns | 4.5x | | 6 args | 96ns | 454ns | 4.7x |
