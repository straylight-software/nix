# Migration Guide: Nix String Functions to straylight::nix::primitives

This document describes how to migrate Nix codebase string operations to use the SIMD-accelerated
`straylight::nix::primitives` implementation.

## Overview

The `straylight::nix::primitives::strings` library provides high-performance string operations with
adaptive backend selection. It uses SIMD (via stringzilla) for large strings where it helps, and
falls back to std::string_view for small strings where SIMD overhead would hurt performance.

## Migration Strategies

### Strategy 1: Compatibility Adapter (Recommended for Initial Migration)

Use the compatibility adapter for a drop-in replacement with minimal code changes:

```cpp
// Before:
#include "nix/util/strings.h"
#include "nix/util/util.h"

// After:
#include "straylight/nix/primitives/adapters/nix_strings_adapter.h"

// In implementation files, add this after includes:
using namespace straylight::nix::primitives::nix_compat;
```

This provides Nix-compatible signatures while using the SIMD-accelerated backend.

### Strategy 2: Direct Primitives Usage (Recommended for New Code)

For new code or complete rewrites, use the primitives directly:

```cpp
#include "straylight/nix/primitives/strings.h"

namespace sp = straylight::nix::primitives;

// Use primitives API directly
auto parts = sp::split_to_views(input, "\n");
auto trimmed = sp::trim(line);  // Returns string_view (zero-copy)
bool has = sp::starts_with(path, "/nix/store/");
```

## Function Mapping

### Prefix/Suffix Checks

| Nix Function | Location | Primitives Function | Adapter Function |
|--------------|----------|---------------------|------------------| | `hasPrefix(s, prefix)` |
util.h | `starts_with(s, prefix)` | `hasPrefix(s, prefix)` | | `hasSuffix(s, suffix)` | util.h |
`ends_with(s, suffix)` | `hasSuffix(s, suffix)` |

**Example migration:**

```cpp
// Before (nix::hasPrefix)
if (hasPrefix(path, "/nix/store/")) { ... }

// After (primitives)
if (sp::starts_with(path, "/nix/store/")) { ... }

// After (adapter - no code change needed)
if (hasPrefix(path, "/nix/store/")) { ... }
```

**Notes:**

- Primitives uses adaptive SIMD: std for small strings, SIMD for large strings
- Threshold is configurable via `STRAYLIGHT_STRINGS_PREFIX_THRESHOLD`
- Default threshold: 256 bytes (SIMD overhead hurts for small strings)

### Tokenize/Split

| Nix Function | Location | Primitives Function | Adapter Function |
|--------------|----------|---------------------|------------------| | `tokenizeString<C>(s, seps)`
| strings.h | `tokenize(s, seps)` | `tokenizeString<C>(s, seps)` | | `splitString<C>(s, seps)` |
strings.h | N/A (see note) | `splitString<C>(s, seps)` |

**Important difference:**

Nix `tokenizeString` and `splitString` treat the separator parameter as a **character set** - each
character is a potential separator:

```cpp
// Nix behavior: splits on ',' OR ' ' OR '\t'
tokenizeString<std::vector<std::string>>("a,b c\td", ", \t");
// Result: ["a", "b", "c", "d"]
```

Primitives `split_to_strings` treats the delimiter as a **single string**:

```cpp
// Primitives behavior: splits on the exact string ", \t"
sp::split_to_strings("a, \tb, \tc", ", \t");
// Result: ["a", "b", "c"]
```

The **adapter preserves Nix semantics** (character set separators).

For primitives, use `tokenize()` which supports character-set separators:

```cpp
// Primitives with character-set semantics
auto parts = sp::tokenize("a,b c\td", ", \t");
// Result: ["a", "b", "c", "d"]
```

**tokenize vs split:**

- `tokenizeString` / `tokenize`: Filters out empty strings
- `splitString` / `split_to_strings`: Preserves empty strings

### Join/Concat

| Nix Function | Location | Primitives Function | Adapter Function |
|--------------|----------|---------------------|------------------| | `concatStringsSep(sep, ss)` |
strings.h | `join(sep, ss)` | `concatStringsSep(sep, ss)` | | `concatMapStringsSep(sep, c, fn)` |
strings.h | N/A | `concatMapStringsSep(sep, c, fn)` | | `dropEmptyInitThenConcatStringsSep` |
strings.h | N/A (deprecated) | Available but deprecated |

**Example migration:**

```cpp
// Before (nix::concatStringsSep)
auto result = concatStringsSep("/", pathParts);

// After (primitives)
auto result = sp::join("/", pathParts);

// After (adapter - no code change needed)
auto result = concatStringsSep("/", pathParts);
```

### Trim

| Nix Function | Location | Primitives Function | Adapter Function |
|--------------|----------|---------------------|------------------| | `trim(s, ws)` | util.h |
`trim(s, ws)` | `trim(s, ws)` | | `chomp(s)` | util.h | `trim_right(s, ws)` | `chomp(s)` |

**Important difference:**

Nix `trim` returns `std::string` (allocates). Primitives `trim` returns `std::string_view`
(zero-copy).

The adapter returns `std::string` for compatibility. For better performance, use primitives directly
or the `trim_view()` adapter function:

```cpp
// Before (nix::trim)
std::string cleaned = trim(input);

// After (primitives - zero copy if you only need a view)
std::string_view cleaned = sp::trim(input);
// Or if you need ownership:
std::string cleaned = sp::to_string(sp::trim(input));

// After (adapter - returns std::string for compatibility)
std::string cleaned = trim(input);

// After (adapter - zero copy variant)
std::string_view cleaned = trim_view(input);
```

**Whitespace characters:**

- Nix default: `" \n\r\t"`
- Primitives default: `" \t\n\r\f\v"` (includes form feed and vertical tab)
- Adapter uses Nix default for compatibility

### Replace

| Nix Function | Location | Primitives Function | Adapter Function |
|--------------|----------|---------------------|------------------| | `replaceStrings(s, from, to)`
| util.h | `replace_all(s, from, to)` | `replaceStrings(s, from, to)` |

**Example migration:**

```cpp
// Before (nix::replaceStrings)
auto result = replaceStrings(input, "\n", "\\n");

// After (primitives)
auto result = sp::replace_all(input, "\n", "\\n");

// After (adapter - no code change needed)
auto result = replaceStrings(input, "\n", "\\n");
```

### Contains/Find

Nix doesn't have dedicated `contains` or `find` functions in strings.h/util.h, but primitives
provides them:

```cpp
// Check if string contains substring
if (sp::contains(content, "error")) { ... }

// Find position of substring
auto pos = sp::find(content, "warning");
if (pos != std::string_view::npos) { ... }

// Find from end
auto pos = sp::rfind(content, "\n");
```

## Performance Considerations

### When SIMD Helps

SIMD acceleration is most beneficial for:

- Large strings (> 64-256 bytes depending on operation)
- Multiple substring searches in the same string
- Character set operations (find_first_of, etc.)
- Replace operations with multiple replacements

### When SIMD Hurts

SIMD has overhead that makes it slower for:

- Short strings (< 64 bytes)
- Simple prefix/suffix checks (< 256 bytes)
- Single-character operations

The primitives library uses adaptive selection based on string size. You can tune thresholds via
compile-time configuration:

```cpp
// Before including strings.h:
#define STRAYLIGHT_STRINGS_FIND_THRESHOLD 128
#define STRAYLIGHT_STRINGS_PREFIX_THRESHOLD 512

#include "straylight/nix/primitives/strings.h"
```

Or force a specific backend:

```cpp
#define STRAYLIGHT_STRINGS_FORCE_STD 1   // Always use std (no SIMD)
#define STRAYLIGHT_STRINGS_FORCE_SZ 1    // Always use SIMD
```

### Zero-Copy Operations

Primitives provides zero-copy variants that return `std::string_view`:

```cpp
// Zero-copy trim (returns view into original string)
std::string_view trimmed = sp::trim(input);

// Zero-copy split (returns vector of views)
std::vector<std::string_view> parts = sp::split_to_views(input, "\n");
```

Use these when you don't need to own the result and the source string outlives the views.

## Migration Checklist

### For each file:

1. [ ] Add include for adapter or primitives header
2. [ ] Add `using namespace` declaration if using adapter
3. [ ] Update function calls if using primitives directly
4. [ ] Consider zero-copy variants for performance-critical paths
5. [ ] Test with existing unit tests

### For the codebase:

1. [ ] Identify high-frequency string operations (hot paths)
2. [ ] Benchmark before/after with representative workloads
3. [ ] Tune thresholds based on benchmark results
4. [ ] Consider forced SIMD for known-large-string operations
5. [ ] Consider forced std for known-small-string operations

## Files to Update

Based on grep analysis, here are the Nix util files with string operations:

### Core string utilities (highest priority)

- `src/nix/util/strings.h` - tokenizeString, splitString, concatStringsSep
- `src/nix/util/strings.cpp` - Template instantiations
- `src/nix/util/strings-inline.h` - Template implementations
- `src/nix/util/util.h` - hasPrefix, hasSuffix, trim, chomp, replaceStrings
- `src/nix/util/util.cpp` - Implementation

### Consumers (to update after core)

- `src/nix/util/url.cpp` - Uses hasPrefix
- `src/nix/util/logging.cpp` - Uses hasPrefix
- `src/nix/util/split.h` - Uses hasPrefix
- `src/nix/util/args.cpp` - Uses trim
- `src/nix/util/cgroup.cpp` - Uses trim, replace_strings
- `src/nix/util/error.cpp` - Uses trim
- `src/nix/util/experimental-features.cpp` - Uses trim
- `src/nix/util/linux-namespaces.cpp` - Uses trim

## API Reference

See `src/straylight/nix/primitives/strings.h` for full API documentation.

Key namespaces:

- `straylight::nix::primitives` - Core primitives API
- `straylight::nix::primitives::simd` - Direct SIMD access (always uses stringzilla)
- `straylight::nix::primitives::nix_compat` - Nix-compatible adapter functions
- `straylight::nix::primitives::config` - Configuration constants and helpers
