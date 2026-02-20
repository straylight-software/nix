# Nix fmt.h Migration Guide

This document describes how to migrate Nix codebase from `boost::format` to `std::format` using the
straylight primitives adapter.

## Overview

The migration replaces:

- `nix/util/fmt.h` (boost::format based)
- `nix/util/ansicolor.h` (ANSI macros)

With:

- `straylight/nix/primitives/adapters/nix_fmt_adapter.h` (std::format based)

## Quick Start

### Step 1: Replace Include

```cpp
// Before
#include "nix/util/fmt.h"
#include "nix/util/ansicolor.h"

// After
#include "straylight/nix/primitives/adapters/nix_fmt_adapter.h"
```

### Step 2: No Code Changes Required (Initially)

The adapter provides source-compatible replacements:

- `fmt_()` works with existing format strings
- `hint_fmt_t` works with existing usage patterns
- `magenta_t<T>` and `uncolored_t<T>` work unchanged
- `ANSI_*` constants work unchanged

## Format String Syntax

### Supported Patterns (Automatic Conversion)

| Boost/Printf Style | std::format Equivalent | Example |
|-------------------|------------------------|---------| | `%s` | `{}` | `fmt_("hello %s", name)` |
| `%d` | `{:d}` | `fmt_("count: %d", n)` | | `%f` | `{:f}` | `fmt_("value: %f", x)` | | `%x` / `%X`
| `{:x}` / `{:X}` | `fmt_("hex: %x", n)` | | `%1%` | `{0}` | `fmt_("%1% + %1%", x)` | | `%2%` |
`{1}` | `fmt_("%1% to %2%", a, b)` | | `%\|1$5d\|` | `{0:5d}` | Width specifier | | `%%` | `%` |
Literal percent |

### Recommended Migration (New Code)

For new code, use native std::format syntax directly:

```cpp
// Old boost-style
auto s = fmt_("path '%s' has %d references", path, count);

// New std::format style (preferred)
auto s = format("path '{}' has {} references", path, count);
```

## API Changes

### fmt\_() Function

**Behavior preserved:**

- Single argument: returns string unchanged
- Multiple arguments: formats using boost-style conversion

**Usage:**

```cpp
// These all work unchanged
fmt_("literal string");
fmt_("hello %s", name);
fmt_("x=%1%, y=%2%", x, y);
fmt_("%d items", count);
```

**Migration path:**

```cpp
// Phase 1: Keep using fmt_() with old syntax (works)
auto s = fmt_("path '%s'", path);

// Phase 2: Switch to format() with new syntax
auto s = format("path '{}'", path);
```

### hint_fmt_t Class

**Behavior preserved:**

- Arguments wrapped in magenta by default
- `uncolored_t<T>` prevents coloring
- String literal constructor
- Format string + args constructor
- `operator%` for deferred formatting
- `.str()` method

**Usage:**

```cpp
// All of these work unchanged
hint_fmt_t("literal message");
hint_fmt_t("expected %s, got %s", expected, actual);
hint_fmt_t("path: %s", uncolored_t(path));

auto hf = hint_fmt_t::from_format_string("value: %s");
hf % value;
```

**Migration path:**

```cpp
// Phase 1: Keep using hint_fmt_t (works)
hint_fmt_t("expected %s", expected);

// Phase 2: Switch to Hint with Magenta wrappers
Hint("expected {}", Magenta(expected));

// Or use magenta_hint for auto-wrapping
magenta_hint("expected {}", expected);
```

### Color Wrappers

**Aliases provided:**

```cpp
// Old names (still work)
magenta_t<T>   // wraps value in magenta
uncolored_t<T> // resets color before value

// New names (preferred)
Magenta<T>     // same as magenta_t
Uncolored<T>   // same as uncolored_t
Colored<T>     // generic color wrapper
```

**Usage:**

```cpp
// Old style
hint_fmt_t("error in %s", uncolored_t(path));

// New style
Hint("error in {}", Uncolored(path));
Hint("warning: {}", Magenta(msg));
Hint("info: {}", Colored(msg, kAnsiCyan));
```

### ANSI Constants

**Macro to constexpr migration:**

```cpp
// Old macros (in ansicolor.h)
#define ANSI_NORMAL "\e[0m"
#define ANSI_RED "\e[31;1m"
// etc.

// New constants (in adapter)
inline constexpr auto ANSI_NORMAL = kAnsiNormal;
inline constexpr auto ANSI_RED = kAnsiRed;
// etc.
```

The adapter provides both forms for compatibility.

## Migration Checklist

### Per-File Migration

1. [ ] Replace `#include "nix/util/fmt.h"` with
   `#include "straylight/nix/primitives/adapters/nix_fmt_adapter.h"`
2. [ ] Remove `#include "nix/util/ansicolor.h"` (included by adapter)
3. [ ] Compile and verify no errors
4. [ ] (Optional) Convert format strings to std::format syntax
5. [ ] (Optional) Replace `fmt_()` with `format()`
6. [ ] (Optional) Replace `hint_fmt_t` with `Hint`

### Codebase-Wide Migration

1. [ ] Add adapter header to build system
2. [ ] Migrate files incrementally (adapter supports mixed usage)
3. [ ] Remove boost::format dependency from affected translation units
4. [ ] Update CMakeLists.txt / meson.build to remove boost format linking

## Common Patterns

### Error Messages

```cpp
// Before
throw Error("cannot find '%s' in '%s'", name, path);

// After (with adapter, works unchanged)
throw Error("cannot find '%s' in '%s'", name, path);

// After (migrated to std::format)
throw Error("cannot find '{}' in '{}'", name, path);
```

### Logging

```cpp
// Before
printError("bad JSON from %s: %s", uncolored_t(source), e.what());

// After (with adapter, works unchanged)
printError("bad JSON from %s: %s", uncolored_t(source), e.what());

// After (migrated)
printError("bad JSON from {}: {}", Uncolored(source), e.what());
```

### Activity Descriptions

```cpp
// Before
activity_t act(*logger, lvl_info, act_copy_paths, 
               fmt_("copying %d paths", missing.size()));

// After (with adapter, works unchanged)
activity_t act(*logger, lvl_info, act_copy_paths, 
               fmt_("copying %d paths", missing.size()));

// After (migrated)
activity_t act(*logger, lvl_info, act_copy_paths, 
               format("copying {} paths", missing.size()));
```

## Known Limitations

### Runtime vs Compile-Time Format Checking

- `boost::format`: Runtime format string parsing
- `std::format`: Compile-time format string checking (with `std::format_string`)
- The adapter uses `std::vformat` for runtime compatibility with converted strings

### Performance Considerations

- Format string conversion happens at runtime in the adapter
- For hot paths, prefer migrating to native std::format syntax
- Single-argument `fmt_()` calls have no conversion overhead

### Positional Arguments

- Boost uses 1-based indexing: `%1%`, `%2%`
- std::format uses 0-based indexing: `{0}`, `{1}`
- The adapter handles this conversion automatically

## Files Requiring Migration

Based on grep analysis, the following files use `fmt_()`, `hint_fmt_t`, `magenta_t`, or
`uncolored_t`:

### High-Impact Files (>5 usages)

- `src/nix/util/error.cpp`
- `src/nix/store/local-store.cpp`
- `src/nix/store/gc.cpp`
- `src/nix/store/filetransfer.cpp`
- `src/nix/store/store-api.cpp`
- `src/nix/store/derivations.cpp`

### Medium-Impact Files (2-5 usages)

- `src/nix/util/logging.cpp`
- `src/nix/util/git.cpp`
- `src/nix/util/file-system.cpp`
- `src/nix/util/configuration.cpp`
- `src/nix/store/sqlite.cpp`
- `src/nix/store/profiles.cpp`
- `src/nix/store/parsed-derivations.cpp`
- `src/nix/store/build/*.cpp`

### Low-Impact Files (1 usage)

- Various other files (see grep output for complete list)

## Testing the Migration

```bash
# Build with adapter
nix build .#nix-util

# Run tests
nix build .#nix-util-tests
./result/bin/nix-util-tests

# Verify no boost::format usage remains (after full migration)
grep -r "boost::format" src/
```

## Rollback

If issues are encountered, rollback is straightforward:

1. Revert include changes
2. Remove adapter from build system

The adapter is designed to be drop-in compatible, so rollback should not require code changes beyond
includes.
