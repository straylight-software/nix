# straylight::nix::store

Daemonless Nix store implementations with content-addressed and input-addressed tiers.

## Quick Start

```cpp
#include <straylight/nix/store/two_tier_store.h>

using namespace straylight::nix::store;

// Create unified two-tier store
two_tier_store store("/nix/var/nix");
store.init();

// Register a CA path (goes to ca_store, lockless)
legacy_path_info info;
info.path = "/nix/store/abc123-foo";
info.ca = "fixed:sha256:...";
store.register_path(info, {});

// Register an input-addressed path (goes to legacy_store with flock)
legacy_path_info info2;
info2.path = "/nix/store/def456-bar";
store.register_path(info2, {"/nix/store/abc123-foo"});

// Query paths
auto result = store.query_path("/nix/store/abc123-foo");
```

## Architecture

The **two-tier store** combines:

- **CA tier** (`ca_store`): Content-addressed blob storage, fully parallel, no coordination needed
- **Legacy tier** (`legacy_store`): SQLite for input-addressed paths with flock coordination

Dispatch rules:

- Paths with `ca:` field → `ca_store`
- Input-addressed paths → `legacy_store`

Benefits:

- No daemon required (kernel flock + atomic rename)
- CA paths are fully parallel
- Legacy paths use SQLite ACID guarantees
- Crash-safe (WAL + atomic rename)
- Process-death safe (kernel releases flock)

## Store Implementations

| Store | Description |
|-------|-------------|
| `two_tier_store` | Unified CA + Legacy (recommended) |
| `ca_store` | Content-addressed blob storage |
| `legacy_store` | SQLite-backed input-addressed store |
| `log_store` | Experimental log-structured store |

## Building

```bash
buck2 build //src/straylight/nix/store:store
buck2 test //src/straylight/nix/store/tests:...
```

## See Also

- [Architecture](docs/ARCHITECTURE.md)
- [Testing](docs/TESTING.md)
