# Test Coverage - straylight/nix

Test infrastructure using **Catch2** (unit tests) and **RapidCheck** (property-based testing).

## Test Directory Structure

| Directory | Description |
|-----------|-------------|
| `src/straylight/test/unit/` | Unit tests (main test suite) |
| `src/straylight/test/fuzz/` | Fuzz tests |
| `src/straylight/test/property/` | Property-based tests |
| `src/straylight/test/integration/` | Integration tests |
| `src/straylight/bench/` | Benchmarks |
| `src/nix/util/tests/` | Legacy util tests |

## Test Summary - Legacy Util Tests

| Test File | Target | Assertions | Test Cases | Status |
|-----------|--------|:----------:|:----------:|:------:|
| `base-n_test.cpp` | `//src/nix/util/tests:base-n_test` | 37 | 14 | PASS |
| `checked-arithmetic_test.cpp` | `//src/nix/util/tests:checked-arithmetic_test` | 89 | 34 | PASS |
| `canon-path_test.cpp` | `//src/nix/util/tests:canon-path_test` | 129 | 56 | PASS |
| `lru-cache_test.cpp` | `//src/nix/util/tests:lru-cache_test` | 87 | 19 | PASS |
| `strings_test.cpp` | `//src/nix/util/tests:strings_test` | 206 | 84 | 82/84 |
| `topo-sort_test.cpp` | `//src/nix/util/tests:topo-sort_test` | 45 | 16 | 8/16 |
| `url_test.cpp` | `//src/nix/util/tests:url_test` | 167 | 60 | 59/60 |

**Totals: 760 assertions, 283 test cases, 272 passing**

## Unit Tests (src/straylight/test/unit/)

| Subdirectory | Test Files |
|--------------|------------|
| `adapters/` | nix_strings_adapter_test.cpp |
| `async/` | async_test.cpp, closure_test.cpp |
| `cli/` | args_test.cpp |
| `compat/` | git_test.cpp, sqlite_test.cpp |
| `compiler/` | ast_test.cpp, compiler_test.cpp, eval_test.cpp, evaluator_test.cpp, execution_test.cpp, grammar_test.cpp, parse_test.cpp, runtime_test.cpp, wasm_memory_test.cpp, wasm_types_test.cpp |
| `crypto/` | encoding_test.cpp, hash_test.cpp |
| `data/` | chunked_vector_test.cpp, lru_cache_test.cpp, serialise_test.cpp, topo_sort_test.cpp |
| `fs/` | filesystem_test.cpp |
| `store/` | ca_store_machine_test.cpp, ca_store_test.cpp, corruption_test.cpp, store_test.cpp |
| `sync/` | callback_test.cpp, lock_test.cpp, pool_test.cpp, signals_test.cpp, sync_test.cpp |
| `text/` | format_test.cpp, fuzzy_test.cpp, markdown_test.cpp, regex_test.cpp, split_test.cpp, strings_test.cpp, table_test.cpp, xml_writer_test.cpp |
| `url/` | url_test.cpp |
| `util/` | checked_arithmetic_test.cpp, comparator_test.cpp, finally_test.cpp, ref_test.cpp |

## Fuzz Tests (src/straylight/test/fuzz/)

| Test File | Description |
|-----------|-------------|
| `compiler_compile.cpp` | Fuzz compiler compilation |
| `compiler_execute.cpp` | Fuzz compiler execution |
| `compiler_parse.cpp` | Fuzz compiler parsing |
| `wasm_memory.cpp` | Fuzz WASM memory operations |

## Property Tests (src/straylight/test/property/)

| Test File | Description |
|-----------|-------------|
| `compiler_nondeterminism.cpp` | Property tests for compiler determinism |
| `compiler_property.cpp` | General compiler property tests |

## Integration Tests (src/straylight/test/integration/)

| Test File | Description |
|-----------|-------------|
| `compiler_adversarial.cpp` | Adversarial compiler tests |
| `compiler_brutal.cpp` | Stress/brutal compiler tests |
| `compiler_gc.cpp` | Compiler GC integration tests |
| `compiler_integration.cpp` | General compiler integration |

## Benchmarks (src/straylight/bench/)

| Subdirectory | Benchmark Files |
|--------------|-----------------|
| `arch/` | architectural_bench.cpp |
| `async/` | async_bench.cpp, closure_bench.cpp |
| `cmp/` | encoding_cmp.cpp, format_cmp.cpp, hash_cmp.cpp, regex_cmp.cpp, strings_cmp.cpp |
| `compiler/` | compiler_bench.cpp |
| `crypto/` | encoding_bench.cpp, hash_bench.cpp |
| `data/` | chunked_vector_bench.cpp, lru_cache_bench.cpp, topo_sort_bench.cpp |
| `evring/` | bench_evring.cpp |
| `fs/` | filesystem_bench.cpp |
| `store/` | store_bench.cpp |
| `sync/` | pool_bench.cpp, sync_bench.cpp |
| `text/` | format_bench.cpp, fuzzy_bench.cpp, regex_bench.cpp, strings_bench.cpp |
| `url/` | url_bench.cpp |
| `util/` | checked_arithmetic_bench.cpp |

## Coverage by Component

| Library | Component | Unit Tests | Property Tests | Status |
|---------|-----------|:----------:|:--------------:|:------:|
| **util** | `base-n.h` (base16) | 6 | 3 | Done |
| **util** | `base-n.h` (base64) | 5 | 1 | Done |
| **util** | `checked-arithmetic.h` | 22 | 13 | Done |
| **util** | `canon-path.h` | 37 | 10 | Done |
| **util** | `lru-cache.h` | 12 | 8 | Done |
| **util** | `strings.h` / `split.h` | 44 | 7 | Done (2 failures) |
| **util** | `topo-sort.h` | 14 | 4 | Done (8 failures) |
| **util** | `url.h` | 40 | 8 | Done (1 failure) |
| **util** | `base-nix-32.h` | - | - | TODO |
| **util** | `hash.h` | - | - | TODO |
| **util** | `chunked-vector.h` | - | - | TODO |
| **util** | `closure.h` | - | - | TODO |
| **util** | `compression.h` | - | - | TODO |
| **util** | `configuration.h` | - | - | TODO |
| **util** | `english.h` | - | - | TODO |
| **util** | `error.h` | - | - | TODO |
| **util** | `executable-path.h` | - | - | TODO |
| **util** | `experimental-features.h` | - | - | TODO |
| **util** | `file-system.h` | - | - | TODO |
| **util** | `git.h` | - | - | TODO |
| **util** | `hilite.h` | - | - | TODO |
| **util** | `json-utils.h` | - | - | TODO |
| **util** | `logging.h` | - | - | TODO |
| **util** | `pool.h` | - | - | TODO |
| **util** | `references.h` | - | - | TODO |
| **util** | `regex-combinators.h` | - | - | TODO |
| **util** | `serialise.h` | - | - | TODO |
| **util** | `suggestions.h` | - | - | TODO |
| **util** | `sync.h` | - | - | TODO |
| **util** | `tarfile.h` | - | - | TODO |
| **util** | `thread-pool.h` | - | - | TODO |
| **util** | `xml-writer.h` | - | - | TODO |
| **store** | `path.h` | - | - | TODO |
| **store** | `path-info.h` | - | - | TODO |
| **store** | `content-address.h` | - | - | TODO |
| **store** | `derivations.h` | - | - | TODO |
| **store** | `nar-info.h` | - | - | TODO |
| **store** | `outputs-spec.h` | - | - | TODO |
| **store** | `realisation.h` | - | - | TODO |
| **store** | `store-api.h` | - | - | TODO |
| **expr** | `value.h` | - | - | TODO |
| **expr** | `eval.h` | - | - | TODO |
| **expr** | `nixexpr.h` | - | - | TODO |
| **expr** | `attr-set.h` | - | - | TODO |
| **expr** | `primops.h` | - | - | TODO |
| **fetchers** | `fetchers.h` | - | - | TODO |
| **fetchers** | `git.h` | - | - | TODO |
| **flake** | `flake.h` | - | - | TODO |
| **flake** | `lockfile.h` | - | - | TODO |

## Running Tests

```bash
# Run legacy util tests
buck2 build //src/nix/util/tests:base-n_test //src/nix/util/tests:checked-arithmetic_test \
  //src/nix/util/tests:canon-path_test //src/nix/util/tests:lru-cache_test \
  //src/nix/util/tests:strings_test //src/nix/util/tests:topo-sort_test \
  //src/nix/util/tests:url_test

# Run a specific legacy test
buck2 run //src/nix/util/tests:base-n_test

# Run with verbose output
buck2 run //src/nix/util/tests:base-n_test -- -v

# Run specific test case
buck2 run //src/nix/util/tests:base-n_test -- "[base16]"

# Run straylight unit tests (example)
buck2 run //src/straylight/test/unit/crypto:encoding_test
buck2 run //src/straylight/test/unit/text:strings_test

# Run benchmarks (example)
buck2 run //src/straylight/bench/crypto:encoding_bench
```

## Known Test Failures

### strings_test (2 failures)

- `stripIndentation` tests have incorrect expected output - need to verify actual `stripIndentation`
  behavior

### topo-sort_test (8 failures)

- API mismatch: tests assume different function signatures than actual implementation
- Need to audit `topoSort()` function signature and return type

### url_test (1 failure)

- Test around line 158 has incorrect expectation for URL parsing

## Priority Queue for Next Tests

High priority targets (pure functions, good for property testing):

1. **`hash.h`** - hash computation and parsing
2. **`base-nix-32.h`** - nix-specific base32 encoding
3. **`compression.h`** - compression/decompression roundtrips
4. **`serialise.h`** - serialization invariants
5. **`file-system.h`** - path utilities (non-IO parts)

## Test Framework Notes

- Tests use Catch2 v3 with `catch2/catch_test_macros.hpp`
- Property tests use RapidCheck with `rapidcheck/catch.h` integration
- Catch2 must be included BEFORE rapidcheck headers
- Linker flags for transitive deps are in `src/nix/util/tests/BUCK`
