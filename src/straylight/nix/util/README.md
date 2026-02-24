# straylight::nix::util

Small utility types and helpers used throughout straylight.

## Quick Start

```cpp
#include <straylight/nix/util/ref.h>
#include <straylight/nix/util/finally.h>
#include <straylight/nix/util/checked_arithmetic.h>

using namespace straylight::nix::util;

// Non-nullable shared_ptr
auto r = make_ref<Widget>(args...);
r->do_thing();  // Never null

// Cast between types
auto derived = ref_cast<Derived>(base);

// Scope guard
{
    auto cleanup = finally([] { restore_state(); });
    do_work();
}  // cleanup runs here

// Checked arithmetic (overflow detection)
auto result = checked_add(a, b);  // std::expected
if (result) {
    use(*result);
}
```

## Features

- **Ref<T>**: Non-nullable shared_ptr wrapper
- **finally**: Scope guard for cleanup actions
- **checked_arithmetic**: Overflow-safe integer operations
- **comparator**: Spaceship operator helpers

## API Overview

| Type/Function | Description |
|---------------|-------------|
| `Ref<T>` | Non-nullable reference-counted pointer |
| `make_ref<T>()` | Create a Ref<T> |
| `ref_cast<To>()` | Cast between Ref types |
| `finally()` | Create scope guard |
| `checked_add/sub/mul` | Overflow-checked arithmetic |

## Building

```bash
buck2 build //src/straylight/nix/util:util
buck2 test //src/straylight/nix/util/tests:...
```

## See Also

- [Architecture](docs/ARCHITECTURE.md)
- [Testing](docs/TESTING.md)
