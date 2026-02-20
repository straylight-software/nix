# straylight::nix::adapters

Migration shims for transitioning from Nix util/ to straylight primitives.

## Overview

The adapters provide source-compatible replacements for Nix utility headers,
allowing gradual migration without rewriting all call sites at once.

## Available Adapters

| Adapter | Replaces | Description |
|---------|----------|-------------|
| `nix_fmt_adapter.h` | `nix/util/fmt.h` | boost::format → std::format |
| `nix_strings_adapter.h` | `nix/util/strings.h` | String operations shim |
| `thread_pool_adapter.h` | `nix/util/thread-pool.h` | Thread pool shim |

## Quick Start

```cpp
// Before
#include "nix/util/fmt.h"
#include "nix/util/ansicolor.h"

// After
#include "straylight/nix/adapters/nix_fmt_adapter.h"

// Same API works
auto s = fmt_("path '%s' has %d references", path, count);
```

## Migration Guides

- [MIGRATION.md](MIGRATION.md) - General migration overview
- [STRINGS_MIGRATION.md](STRINGS_MIGRATION.md) - String operations migration
- [THREAD_POOL_MIGRATION.md](THREAD_POOL_MIGRATION.md) - Thread pool migration

## Format String Conversion

| boost/printf | std::format | Example |
|--------------|-------------|---------|
| `%s` | `{}` | `fmt_("hello %s", name)` |
| `%d` | `{:d}` | `fmt_("count: %d", n)` |
| `%1%` | `{0}` | `fmt_("%1% + %1%", x)` |
| `%%` | `%` | Literal percent |

## Building

```bash
buck2 build //src/straylight/nix/adapters:adapters
buck2 test //src/straylight/nix/adapters/tests:...
```

## See Also

- [MIGRATION.md](MIGRATION.md) - Full migration guide
