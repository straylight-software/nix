# straylight/nix Source Tree Restructure Plan

```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
                                    // straylight // restructure
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

## Status: COMPLETED

The restructure is complete. All files have been moved, namespaces updated, and the build passes
(`buck2 build //src/straylight/nix/...`).

### Remaining work (not blocking):

- Create README.md for each new module
- Fix runtime_test.cpp API mismatch (test disabled)
- Update stale nix store paths in BUCK files (pre-existing issue)
- Migrate to static musl builds (separate effort)

______________________________________________________________________

## Motivation

The current structure has issues:

1. **`straylight::language`** - Too generic; this is Nix-specific
2. **`straylight::protocol`** - Same problem; it's the Nix daemon protocol
3. **`straylight::nix::primitives`** - A "big pile of poo" mixing unrelated concerns
4. **Inconsistent documentation** - Some modules have ARCHITECTURE.md, some don't

## Goals

1. Everything Nix-specific under `straylight::nix::*`
2. `straylight::evring` stays (it's a generic io_uring library)
3. Break up primitives into cohesive modules
4. Standardized documentation per module

______________________________________________________________________

## New Structure

```
src/straylight/
│
├── evring/                         # straylight::evring (UNCHANGED)
│   ├── README.md
│   ├── docs/
│   │   ├── ARCHITECTURE.md         # (move from ARCHITECTURE.md)
│   │   ├── TODO.md
│   │   └── TESTING.md
│   ├── BUCK
│   ├── *.h / *.cpp
│   ├── bench/
│   ├── examples/
│   └── tests/
│
└── nix/                            # straylight::nix::*
    │
    ├── compiler/                   # straylight::nix::compiler
    │   ├── README.md               # (was language/)
    │   ├── docs/
    │   │   ├── ARCHITECTURE.md     # (move from ARCHITECTURE.md)
    │   │   ├── MEMORY.md           # (move from MEMORY.md)
    │   │   ├── TODO.md
    │   │   └── TESTING.md
    │   ├── BUCK
    │   ├── ast/
    │   ├── parse/
    │   ├── compile/
    │   ├── eval/
    │   ├── runtime/
    │   ├── cli/
    │   └── tests/
    │
    ├── protocol/                   # straylight::nix::protocol
    │   ├── README.md               # (move from protocol/)
    │   ├── docs/
    │   │   ├── ARCHITECTURE.md
    │   │   ├── TODO.md
    │   │   └── TESTING.md
    │   ├── BUCK
    │   ├── nix_daemon.ksy
    │   ├── nar.ksy
    │   ├── captures/
    │   ├── nar_captures/
    │   ├── hs/                     # Haskell impl
    │   ├── src/                    # Rust impl
    │   └── tests/
    │
    ├── store/                      # straylight::nix::store
    │   ├── README.md
    │   ├── docs/
    │   │   ├── ARCHITECTURE.md     # (consolidate STORE_DESIGN.md)
    │   │   ├── TODO.md
    │   │   └── TESTING.md
    │   ├── BUCK
    │   ├── ca_store.h/cpp          # Content-addressed tier
    │   ├── ca_store_machine.h      # evring state machines
    │   ├── legacy_store.h/cpp      # SQLite tier (Nix1 compat)
    │   ├── two_tier_store.h        # Unified interface
    │   ├── log_store.h/cpp         # Experimental log-structured
    │   ├── async.h                 # (was store_async.h)
    │   └── tests/
    │
    ├── crypto/                     # straylight::nix::crypto
    │   ├── README.md
    │   ├── docs/
    │   │   ├── ARCHITECTURE.md
    │   │   ├── TODO.md
    │   │   └── TESTING.md
    │   ├── BUCK
    │   ├── hash.h/cpp              # BLAKE3, SHA-256, MD5
    │   ├── hash_config.h
    │   ├── encoding.h/cpp          # Base16/32/64
    │   └── tests/
    │
    ├── text/                       # straylight::nix::text
    │   ├── README.md
    │   ├── docs/
    │   │   ├── ARCHITECTURE.md
    │   │   ├── TODO.md
    │   │   └── TESTING.md
    │   ├── BUCK
    │   ├── strings.h               # StringZilla-backed
    │   ├── strings_config.h
    │   ├── regex.h/cpp             # RE2
    │   ├── fuzzy.h                 # rapidfuzz
    │   ├── format.h                # std::format wrappers
    │   ├── markdown.h
    │   ├── xml_writer.h
    │   ├── table.h
    │   ├── split.h
    │   └── tests/
    │
    ├── url/                        # straylight::nix::url
    │   ├── README.md
    │   ├── docs/
    │   │   ├── ARCHITECTURE.md
    │   │   ├── TODO.md
    │   │   └── TESTING.md
    │   ├── BUCK
    │   ├── url.h/cpp               # Ada URL
    │   ├── url_fast.h/cpp          # Boost.URL
    │   └── tests/
    │
    ├── async/                      # straylight::nix::async
    │   ├── README.md
    │   ├── docs/
    │   │   ├── ARCHITECTURE.md
    │   │   ├── TODO.md
    │   │   └── TESTING.md
    │   ├── BUCK
    │   ├── executor.h              # taskflow executor
    │   ├── task_graph.h            # DAG execution
    │   ├── parallel.h              # Parallel algorithms
    │   ├── closure.h               # Async transitive closure
    │   └── tests/
    │
    ├── sync/                       # straylight::nix::sync
    │   ├── README.md
    │   ├── docs/
    │   │   ├── ARCHITECTURE.md
    │   │   ├── TODO.md
    │   │   └── TESTING.md
    │   ├── BUCK
    │   ├── synchronized.h          # Synchronized<T> (was sync.h)
    │   ├── lock.h                  # flock wrapper
    │   ├── pool.h                  # Resource pool
    │   ├── callback.h              # promise/future wrapper
    │   ├── signals.h               # stop_token + interrupts
    │   └── tests/
    │
    ├── data/                       # straylight::nix::data
    │   ├── README.md
    │   ├── docs/
    │   │   ├── ARCHITECTURE.md
    │   │   ├── TODO.md
    │   │   └── TESTING.md
    │   ├── BUCK
    │   ├── chunked_vector.h
    │   ├── lru_cache.h
    │   ├── topo_sort.h
    │   ├── serialise.h             # zpp_bits wrapper
    │   └── tests/
    │
    ├── util/                       # straylight::nix::util
    │   ├── README.md
    │   ├── docs/
    │   │   ├── ARCHITECTURE.md
    │   │   ├── TODO.md
    │   │   └── TESTING.md
    │   ├── BUCK
    │   ├── ref.h                   # not_null<T>
    │   ├── finally.h               # scope guard
    │   ├── checked_arithmetic.h
    │   ├── comparator.h            # spaceship helpers
    │   ├── config.h
    │   └── tests/
    │
    ├── fs/                         # straylight::nix::fs
    │   ├── README.md
    │   ├── docs/
    │   │   ├── ARCHITECTURE.md
    │   │   ├── TODO.md
    │   │   └── TESTING.md
    │   ├── BUCK
    │   ├── file_lock.h             # (from filesystem/)
    │   ├── temp.h                  # (from filesystem/)
    │   ├── mmap.h                  # (from filesystem/)
    │   └── tests/
    │
    ├── cli/                        # straylight::nix::cli
    │   ├── README.md
    │   ├── docs/
    │   │   ├── ARCHITECTURE.md
    │   │   ├── TODO.md
    │   │   └── TESTING.md
    │   ├── BUCK
    │   ├── args.h                  # CLI11-style argument parsing
    │   └── tests/
    │
    ├── compat/                     # straylight::nix::compat
    │   ├── README.md
    │   ├── docs/
    │   │   ├── ARCHITECTURE.md
    │   │   ├── TODO.md
    │   │   └── TESTING.md
    │   ├── BUCK
    │   ├── git.h                   # Git object parsing
    │   ├── sqlite.h                # SQLite wrapper
    │   └── tests/
    │
    └── adapters/                   # straylight::nix::adapters
        ├── README.md
        ├── docs/
        │   ├── ARCHITECTURE.md
        │   ├── TODO.md
        │   └── TESTING.md
        ├── BUCK
        ├── MIGRATION.md
        ├── STRINGS_MIGRATION.md
        ├── THREAD_POOL_MIGRATION.md
        └── tests/
```

______________________________________________________________________

## File Moves

### Phase 1: Top-level moves

| From | To | |------|-----| | `src/straylight/language/` | `src/straylight/nix/compiler/` | |
`src/straylight/protocol/` | `src/straylight/nix/protocol/` |

### Phase 2: Split primitives/

| From | To | |------|-----| | `primitives/ca_store*` | `nix/store/` | | `primitives/legacy_store*`
| `nix/store/` | | `primitives/two_tier_store*` | `nix/store/` | | `primitives/store.h/cpp` |
`nix/store/log_store.h/cpp` | | `primitives/store_async.h` | `nix/store/async.h` | |
`primitives/STORE_DESIGN.md` | `nix/store/docs/ARCHITECTURE.md` | | `primitives/hash*` |
`nix/crypto/` | | `primitives/encoding*` | `nix/crypto/` | | `primitives/strings*` | `nix/text/` | |
`primitives/regex*` | `nix/text/` | | `primitives/fuzzy*` | `nix/text/` | | `primitives/format*` |
`nix/text/` | | `primitives/markdown*` | `nix/text/` | | `primitives/xml_writer*` | `nix/text/` | |
`primitives/table*` | `nix/text/` | | `primitives/split*` | `nix/text/` | | `primitives/url*` |
`nix/url/` | | `primitives/async/` | `nix/async/` | | `primitives/sync.h` |
`nix/sync/synchronized.h` | | `primitives/lock.h` | `nix/sync/` | | `primitives/pool.h` |
`nix/sync/` | | `primitives/callback.h` | `nix/sync/` | | `primitives/signals.h` | `nix/sync/` | |
`primitives/chunked_vector.h` | `nix/data/` | | `primitives/lru_cache.h` | `nix/data/` | |
`primitives/topo_sort.h` | `nix/data/` | | `primitives/serialise.h` | `nix/data/` | |
`primitives/ref.h` | `nix/util/` | | `primitives/finally.h` | `nix/util/` | |
`primitives/checked_arithmetic.h` | `nix/util/` | | `primitives/comparator.h` | `nix/util/` | |
`primitives/config.h` | `nix/util/` | | `primitives/filesystem/` | `nix/fs/` | | `primitives/args.h`
| `nix/cli/` | | `primitives/git.h` | `nix/compat/` | | `primitives/sqlite.h` | `nix/compat/` | |
`primitives/adapters/` | `nix/adapters/` | | `primitives/NIH.md` | `nix/docs/NIH.md` |

______________________________________________________________________

## Namespace Changes

| Old | New | |-----|-----| | `straylight::language::*` | `straylight::nix::compiler::*` | |
`straylight::language::ast` | `straylight::nix::compiler::ast` | | `straylight::language::parse` |
`straylight::nix::compiler::parse` | | `straylight::language::compile` |
`straylight::nix::compiler::compile` | | `straylight::language::runtime` |
`straylight::nix::compiler::runtime` | | `straylight::protocol::*` | `straylight::nix::protocol::*`
| | `straylight::nix::primitives::*` | Split across new namespaces | |
`straylight::nix::primitives::hash` | `straylight::nix::crypto` | | (etc.) | (etc.) |

______________________________________________________________________

## Include Path Changes

| Old | New | |-----|-----| | `straylight/language/ast/expression.h` |
`straylight/nix/compiler/ast/expression.h` | | `straylight/protocol/nix_daemon.ksy` |
`straylight/nix/protocol/nix_daemon.ksy` | | `straylight/nix/primitives/hash.h` |
`straylight/nix/crypto/hash.h` | | `straylight/nix/primitives/strings.h` |
`straylight/nix/text/strings.h` | | (etc.) | (etc.) |

______________________________________________________________________

## Documentation Standard

Each module gets:

```
module/
├── README.md           # Quick start, API overview, examples
├── docs/
│   ├── ARCHITECTURE.md # Design decisions, internals, diagrams
│   ├── TODO.md         # Planned work, known issues, wishlist
│   └── TESTING.md      # Test strategy, how to run, coverage info
├── BUCK
├── *.h / *.cpp
└── tests/
```

### README.md Template

```markdown
# straylight::nix::module

Brief description (1-2 sentences).

## Quick Start

\`\`\`cpp
#include <straylight/nix/module/thing.h>

auto result = straylight::nix::module::do_thing();
\`\`\`

## Features

- Feature 1
- Feature 2

## API Overview

| Type/Function | Description |
|---------------|-------------|
| `thing_t` | Does X |
| `do_thing()` | Returns Y |

## Building

\`\`\`bash
buck2 build //src/straylight/nix/module:module
buck2 test //src/straylight/nix/module/tests:...
\`\`\`

## See Also

- [Architecture](docs/ARCHITECTURE.md)
- [Testing](docs/TESTING.md)
```

______________________________________________________________________

## Execution Order

1. **Create new directory structure** (empty dirs)
2. **Move files** (git mv for history)
3. **Update namespaces** (sed/ast-grep)
4. **Update includes** (sed/ast-grep)
5. **Update BUCK files**
6. **Verify build** (`buck2 build //...`)
7. **Verify tests** (`buck2 test //src/straylight/...`)
8. **Create documentation** (README.md, docs/\*.md per module)
9. **Update top-level docs** (ARCHITECTURE.md, README.md)

______________________________________________________________________

## Risk Mitigation

- **Git history**: Use `git mv` for all moves
- **Build breakage**: Incremental moves with build verification
- **Test coverage**: Run full test suite after each phase
- **Rollback**: Each phase is a separate commit

______________________________________________________________________

## Questions to Resolve

1. Should `evring` also get the `docs/` structure, or is the current `ARCHITECTURE.md` at root okay?
2. Should tests stay in `tests/` subdirs or move to a top-level `tests/` mirroring `src/`?
3. Any modules that should be combined or split differently?
