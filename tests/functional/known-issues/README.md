# Known Issues Tests

This directory contains tests that **reveal known broken behavior** from upstream Nix GitHub issues.
These tests are designed to demonstrate that bugs exist by showing the actual (broken) behavior vs
the expected behavior.

When a bug is fixed, the corresponding test should be updated to verify the fix.

## Test Categories

### Eval Cache Bugs

| Test | Issues | Description | |------|--------|-------------| | `eval-cache-sqlite-lock.sh` |
#3794, #6847 | SQLite lock contention during concurrent evals | |
`eval-cache-cached-errors-lose-trace.sh` | #3872, #9165 | Cached errors lose stack trace information
|

**Root cause**: `src/nix/expr/eval-cache.cpp`

- Line 398: Attrsets not cached (`; // FIXME: do something?`)
- Line 250: O(n) table scan on every attribute lookup
- SQLite default locking (exclusive during writes)

### Fetcher Cache Bugs

| Test | Issues | Description | |------|--------|-------------| | `fetchgit-wrong-tag-caching.sh` |
#7146 | fetchGit caches wrong results when tags move | | `tarball-cache-invalidation.sh` | #9814 |
Tarball cache cannot be manually invalidated |

**See also**: `tests/functional/git/packed-refs-no-cache.sh` has inverted test logic at line 74-81
revealing a similar caching bug.

### Remote Builder Bugs

| Test | Issues | Description | |------|--------|-------------| | `remote-builder-stderr-pipe.sh` |
#5701 | stderr pipe filling causes build hangs |

**Evidence**: `tests/functional/binary-cache-build-remote.sh` is skipped with comment: "remote
builders disabled - build hook is unsound"

### CA Derivations Bugs

| Test | Issues | Description | |------|--------|-------------| | `ca-derivations-ssh-ng.sh` | - |
Log streaming broken for fixed-output CA via ssh-ng |

**Evidence**: `tests/functional/build-remote-content-addressed-fixed.sh` is skipped with comment:
"log streaming for fixed-output CA via ssh-ng needs investigation"

## GitHub Issue References

| Issue | Title | Status | |-------|-------|--------| |
[#3794](https://github.com/NixOS/nix/issues/3794) | Eval cache lock contention | Open | |
[#3872](https://github.com/NixOS/nix/issues/3872) | Cached errors lose trace | Open | |
[#5261](https://github.com/NixOS/nix/issues/5261) | Eval cache subdir wrong results | Open | |
[#5701](https://github.com/NixOS/nix/issues/5701) | Remote builder stderr pipe | Open | |
[#6574](https://github.com/NixOS/nix/issues/6574) | String context corruption | Open | |
[#6666](https://github.com/NixOS/nix/issues/6666) | CA derivations schema deadlock | Open | |
[#6847](https://github.com/NixOS/nix/issues/6847) | nix search blocks other evals | Open | |
[#7146](https://github.com/NixOS/nix/issues/7146) | fetchGit wrong tag caching | Open | |
[#9165](https://github.com/NixOS/nix/issues/9165) | Cached errors lose trace (dup) | Open | |
[#9814](https://github.com/NixOS/nix/issues/9814) | Tarball cache invalidation | Open | |
[#11393](https://github.com/NixOS/nix/issues/11393) | CA realisations without sigs | Open |

## Running Tests

```bash
# Run a specific test
./tests/functional/known-issues/eval-cache-sqlite-lock.sh

# Run all known-issues tests
for f in tests/functional/known-issues/*.sh; do bash "$f"; done

# Run with common.sh sourced (from functional tests directory)
cd tests/functional
./known-issues/eval-cache-sqlite-lock.sh
```

## Test Conventions

Each test should:

1. **Document the GitHub issue number(s)** in header comments
2. **Explain the root cause** with file/line references where known
3. **Describe expected vs actual behavior**
4. **Exit code 0** when demonstrating the known broken behavior
5. **Exit code 0** when the bug appears to be fixed (with message)
6. **Exit code 1** only for unexpected failures
7. **Exit code 77** to skip (when prerequisites unavailable)

## Adding New Tests

When you discover a new bug:

1. Create a test file: `tests/functional/known-issues/<descriptive-name>.sh`
2. Add the standard header with issue numbers and description
3. Write a minimal reproduction case
4. Document what "fixed" behavior would look like
5. Update this README with the new test

## Related Files

These existing tests also document known issues:

- `tests/functional/git/packed-refs-no-cache.sh` - Has **inverted** assertions at lines 74-81 to
  pass despite the bug existing
- `tests/functional/build-remote-content-addressed-fixed.sh` - **Skipped**
- `tests/functional/binary-cache-build-remote.sh` - **Skipped**
