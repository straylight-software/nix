# Test Coverage - straylight/nix

Test infrastructure using **Catch2** (unit tests) and **RapidCheck** (property-based testing).

## Test Summary

| Test File | Target | Assertions | Test Cases | Status |
|-----------|--------|:----------:|:----------:|:------:| | `base-n_test.cpp` |
`//src/nix/util/tests:base-n_test` | 37 | 14 | PASS | | `checked-arithmetic_test.cpp` |
`//src/nix/util/tests:checked-arithmetic_test` | 89 | 34 | PASS | | `canon-path_test.cpp` |
`//src/nix/util/tests:canon-path_test` | 129 | 56 | PASS | | `lru-cache_test.cpp` |
`//src/nix/util/tests:lru-cache_test` | 87 | 19 | PASS | | `strings_test.cpp` |
`//src/nix/util/tests:strings_test` | 206 | 84 | 82/84 | | `topo-sort_test.cpp` |
`//src/nix/util/tests:topo-sort_test` | 45 | 16 | 8/16 | | `url_test.cpp` |
`//src/nix/util/tests:url_test` | 167 | 60 | 59/60 |

**Totals: 760 assertions, 283 test cases, 272 passing**

## Coverage by Component

| Library | Component | Unit Tests | Property Tests | Status |
|---------|-----------|:----------:|:--------------:|:------:| | **util** | `base-n.h` (base16) | 6
| 3 | Done | | **util** | `base-n.h` (base64) | 5 | 1 | Done | | **util** | `checked-arithmetic.h` |
22 | 13 | Done | | **util** | `canon-path.h` | 37 | 10 | Done | | **util** | `lru-cache.h` | 12 | 8
| Done | | **util** | `strings.h` / `split.h` | 44 | 7 | Done (2 failures) | | **util** |
`topo-sort.h` | 14 | 4 | Done (8 failures) | | **util** | `url.h` | 40 | 8 | Done (1 failure) | |
**util** | `base-nix-32.h` | - | - | TODO | | **util** | `hash.h` | - | - | TODO | | **util** |
`chunked-vector.h` | - | - | TODO | | **util** | `closure.h` | - | - | TODO | | **util** |
`compression.h` | - | - | TODO | | **util** | `configuration.h` | - | - | TODO | | **util** |
`english.h` | - | - | TODO | | **util** | `error.h` | - | - | TODO | | **util** |
`executable-path.h` | - | - | TODO | | **util** | `experimental-features.h` | - | - | TODO | |
**util** | `file-system.h` | - | - | TODO | | **util** | `git.h` | - | - | TODO | | **util** |
`hilite.h` | - | - | TODO | | **util** | `json-utils.h` | - | - | TODO | | **util** | `logging.h` |
\- | - | TODO | | **util** | `pool.h` | - | - | TODO | | **util** | `references.h` | - | - | TODO | |
**util** | `regex-combinators.h` | - | - | TODO | | **util** | `serialise.h` | - | - | TODO | |
**util** | `suggestions.h` | - | - | TODO | | **util** | `sync.h` | - | - | TODO | | **util** |
`tarfile.h` | - | - | TODO | | **util** | `thread-pool.h` | - | - | TODO | | **util** |
`xml-writer.h` | - | - | TODO | | **store** | `path.h` | - | - | TODO | | **store** | `path-info.h`
| - | - | TODO | | **store** | `content-address.h` | - | - | TODO | | **store** | `derivations.h` |
\- | - | TODO | | **store** | `nar-info.h` | - | - | TODO | | **store** | `outputs-spec.h` | - | - |
TODO | | **store** | `realisation.h` | - | - | TODO | | **store** | `store-api.h` | - | - | TODO | |
**expr** | `value.h` | - | - | TODO | | **expr** | `eval.h` | - | - | TODO | | **expr** |
`nixexpr.h` | - | - | TODO | | **expr** | `attr-set.h` | - | - | TODO | | **expr** | `primops.h` | -
| - | TODO | | **fetchers** | `fetchers.h` | - | - | TODO | | **fetchers** | `git.h` | - | - | TODO
| | **flake** | `flake.h` | - | - | TODO | | **flake** | `lockfile.h` | - | - | TODO |

## Running Tests

```bash
# Build all util tests
buck2 build //src/nix/util/tests:base-n_test //src/nix/util/tests:checked-arithmetic_test \
  //src/nix/util/tests:canon-path_test //src/nix/util/tests:lru-cache_test \
  //src/nix/util/tests:strings_test //src/nix/util/tests:topo-sort_test \
  //src/nix/util/tests:url_test

# Run a specific test
buck2 run //src/nix/util/tests:base-n_test

# Run with verbose output
buck2 run //src/nix/util/tests:base-n_test -- -v

# Run specific test case
buck2 run //src/nix/util/tests:base-n_test -- "[base16]"
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
