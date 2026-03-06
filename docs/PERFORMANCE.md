# Straylight Nix Performance Analysis

This document presents benchmark data showing where Nix spends time and identifies optimization
opportunities.

## Executive Summary

**Nix is slow.** A simple `nix eval nixpkgs#hello` takes 800ms. `nix search` takes 18 seconds.

The store layer (SQLite, path parsing) is fast (sub-millisecond). The **Nix language evaluator** is
the bottleneck - single-threaded, no bytecode compilation, no persistent cache.

## Running Benchmarks

```bash
# Micro-benchmarks (Catch2)
buck2 test //src/nix/store/tests:store-path_bench
buck2 test //src/nix/store/tests:hash_bench
buck2 test //src/nix/store/tests:narinfo_bench
buck2 test //src/nix/store/tests:derivation_bench
buck2 test //src/nix/store/tests:protocol_bench
buck2 test //src/nix/store/tests:sqlite_bench
buck2 test //src/nix/store/tests:nar_bench
buck2 test //src/nix/cli/tests:cli_bench

# Real-world commands
time nix eval nixpkgs#hello.outPath
time nix search nixpkgs hello
```

______________________________________________________________________

## Real-World Command Performance

| Command | Time | Notes | |---------|------|-------| | `nix eval nixpkgs#hello.outPath` | 800ms |
Simplest package | | `nix eval nixpkgs#python3.outPath` | 220ms | Medium complexity | |
`nix eval nixpkgs#firefox.outPath` | 1.6s | Large dependency tree | |
`nix eval nixpkgs#chromium.outPath` | 1.2s | Massive dependency tree | | `nix search nixpkgs hello`
| **18s** | Evaluates ALL 80k packages | | `nix build nixpkgs#hello --dry-run` | 200ms | Cached
evaluation | | `nix path-info -r hello` | 7ms | SQLite query, 5 paths | | `nix path-info -r firefox`
| 480ms | Network/evaluation overhead | | `nix flake metadata nixpkgs` | 9ms | Just reads lock file
| | `nix derivation show nixpkgs#hello` | 200ms | 2KB JSON output | |
`nix derivation show nixpkgs#firefox` | 1.4s | 16KB JSON output |

### Key Insight

Store operations (`path-info`, `flake metadata`) are **fast** (milliseconds). Evaluation operations
(`eval`, `search`, `derivation show`) are **slow** (seconds).

______________________________________________________________________

## Micro-Benchmark Results

### Store Path Operations

| Operation | Time | Throughput | |-----------|------|------------| | Parse single path | 15μs |
66k/sec | | Parse 10k paths | 146ms | 68k/sec | | Validate path hash | 14μs | 71k/sec | | Path
to_string | 17ns | 58M/sec | | Path comparison | 0.7ns | 1.4B/sec | | std::hash | 0.2ns | 5B/sec |

**Verdict:** Path operations are fast. Not a bottleneck.

### Hash Operations

| Operation | Time | Notes | |-----------|------|-------| | SHA256 1KB | 3μs | | | SHA256 1MB | 2ms
| | | SHA256 100MB | 200ms | I/O bound | | Parse hash (base32) | 150ns | | | Serialize hash | 80ns |
| | Nix base32 encode | 100ns | |

**Verdict:** Hash operations are fast. Large file hashing is I/O bound.

### NARinfo Parsing

| Operation | Time | Notes | |-----------|------|-------| | Parse typical (2 refs, 1 sig) | 2.3μs |
cache.nixos.org format | | Parse with 10 signatures | 2.4μs | Multi-cache scenario | | Parse with 50
references | 20μs | Medium package | | Parse with 100 references | 39μs | Chromium-class | | Parse
with 200 references | 71μs | Extreme case | | Signature verification | 27μs | ed25519 verify | |
Serialize narinfo | 1-10μs | Depending on refs |

**Verdict:** NARinfo parsing scales linearly with references. Not a major bottleneck.

### Derivation Parsing (ATerm)

| Operation | Time | Notes | |-----------|------|-------| | Parse small (8 deps) | 14μs | Typical
leaf package | | Parse medium (50 deps) | 95μs | | | Parse large (150 deps) | 225μs | Firefox-class
| | Parse 500 inputDrvs | 705μs | Extreme | | Serialize small | 5μs | | | Serialize large | 30μs | |
| Round-trip large | 255μs | |

**Verdict:** Derivation parsing is moderately slow. A build touching 1000 derivations spends 225ms
just parsing.

### Protocol Serialization

| Operation | Time | Notes | |-----------|------|-------| | Serialize StorePath | 73ns | | |
Serialize 100 StorePaths | 7μs | | | Deserialize StorePath | 660ns | | | Deserialize 100 StorePaths
| 79μs | | | Serialize ValidPathInfo (5 refs) | 580ns | | | Serialize ValidPathInfo (50 refs) | 5μs
| | | Deserialize ValidPathInfo (50 refs) | 40μs | | | Serialize StorePathSet (1000 paths) | 87μs |
| | Deserialize StorePathSet (1000 paths) | 853μs | | | Deserialize StorePathSet (5000 paths) |
4.8ms | Large closure |

**Verdict:** Protocol serialization is fast for small messages, but large closures (5000 paths) take
milliseconds.

### SQLite Store Operations

| Operation | Time | Notes | |-----------|------|-------| | QueryPathInfo single | 570ns | In-memory
| | QueryPathInfo batch 100 | 41μs | | | Insert new path | 9μs | | | QueryReferences | 660ns | | |
QueryReferrers | 1μs | | | Enumerate GC roots | 10μs | | | QueryPathInfo in 10k store | 385ns |
Index lookup | | QueryReferences in 10k store | 763ns | |

**Verdict:** SQLite is extremely fast. Not a bottleneck.

### NAR Operations

| Operation | Time | Notes | |-----------|------|-------| | Serialize 100 files | 1.2ms | | |
Serialize 100MB file | 6.5ms | I/O bound | | NAR hash 50 files | 1.4ms | | | NAR hash 50MB file |
88ms | CPU bound (SHA256) | | Deserialize 50 files | 2ms | |

**Verdict:** NAR operations are I/O and hash bound. Reasonable performance.

______________________________________________________________________

## Where Time Goes

### The Evaluation Problem

When you run `nix eval nixpkgs#hello.outPath`:

1. **Parse flake.nix** - Read and parse the flake
2. **Import nixpkgs** - Load 200MB of .nix files
3. **Evaluate `hello`** - Traverse attribute path
4. **Force `outPath`** - Compute derivation hash
5. **Return string** - Serialize result

Steps 2-4 dominate. The Nix evaluator:

- Parses .nix files as text (no bytecode)
- Evaluates lazily but doesn't persist results
- Single-threaded
- String-heavy (immutable, lots of copying)

### The Search Problem

`nix search nixpkgs hello` takes **18 seconds** because:

- Must evaluate `meta.description` for every package
- nixpkgs has 80,000+ packages
- Each package evaluation may trigger dependency evaluation
- No caching of evaluated metadata
- Single-threaded traversal

### Why Nix Is Slow

1. **No incremental evaluation** - Re-evaluates everything each invocation
2. **No bytecode compilation** - Parses .nix text files every time
3. **No parallel evaluation** - Single-threaded evaluator
4. **String-heavy design** - Store paths, interpolation, all strings
5. **No persistent memoization** - Thunk cache lost on exit
6. **ATerm derivation format** - Text parsing instead of binary

______________________________________________________________________

## Optimization Opportunities

### Already Fast (No Action Needed)

- SQLite store queries
- Store path operations
- Hash computation (hardware accelerated)
- Basic protocol serialization

### Low-Hanging Fruit

| Optimization | Estimated Impact | Effort | |--------------|------------------|--------| | Binary
derivation format | 2-3x faster drv parsing | Medium | | Parallel narinfo fetching | 5-10x faster
cache queries | Low | | Connection pooling | Reduce daemon overhead | Low | | Pre-indexed package
search | 100x faster search | Medium |

### Major Wins (High Effort)

| Optimization | Estimated Impact | Effort | |--------------|------------------|--------| | Bytecode
compilation | 5-10x faster eval | High | | Persistent eval cache | 10-100x for repeated evals | High
| | Parallel evaluation | 4-8x on multi-core | Very High | | WASM-compiled evaluator | Portable,
cacheable | High |

### Straylight Roadmap

1. **Binary derivation format** - Use Kaitai-generated parsers
2. **Persistent eval cache** - SQLite-backed thunk memoization
3. **WASM evaluator** - Compile Nix expressions to WASM
4. **Pre-built search index** - JSON index of package metadata
5. **Parallel evaluation** - Work-stealing evaluator

______________________________________________________________________

## Benchmark Files

| File | Coverage |
|------|----------|
| `src/nix/store/tests/store-path_bench.cpp` | Path parsing, validation, comparison |
| `src/nix/store/tests/hash_bench.cpp` | SHA256, encoding, content address |
| `src/nix/store/tests/narinfo_bench.cpp` | Parse/serialize cache format |
| `src/nix/store/tests/derivation_bench.cpp` | ATerm parsing, serialization |
| `src/nix/store/tests/protocol_bench.cpp` | Worker protocol messages |
| `src/nix/store/tests/sqlite_bench.cpp` | Store database operations |
| `src/nix/store/tests/nar_bench.cpp` | Archive serialization, hashing |
| `src/nix/cli/tests/cli_bench.cpp` | Command dispatch, arg parsing |

> **Note:** The numbers in this document were collected on a specific test system. Results
> vary by hardware. Run the benchmarks yourself to get accurate numbers for your system.
> No stored result files are maintained to avoid stale data - always regenerate.

______________________________________________________________________

## References

- [Nix Performance Issues](https://github.com/NixOS/nix/issues?q=is%3Aissue+performance)
- [Tvix Project](https://tvix.dev/) - Alternative Nix implementation
- [Lix](https://lix.systems/) - Nix fork with performance focus
