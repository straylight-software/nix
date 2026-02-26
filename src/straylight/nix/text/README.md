# straylight::nix::text

High-performance string operations with adaptive SIMD backend selection.

## Quick Start

```cpp
#include <straylight/nix/text/strings.h>
#include <straylight/nix/text/regex.h>
#include <straylight/nix/text/fuzzy.h>

using namespace straylight::nix::text;

// Adaptive string search (uses SIMD for large strings)
auto pos = find(haystack, needle);
bool has = contains(haystack, "pattern");
bool prefix = starts_with(path, "/nix/store/");

// Character set operations (always SIMD)
auto pos = find_first_of(s, "aeiou");

// Regex (RE2 backend)
auto re = Regex::compile(R"(\d+)");
auto matches = re->find_all(text);

// Fuzzy matching (rapidfuzz)
auto ratio = fuzzy_ratio("hello", "helo");
auto matches = fuzzy_extract("nix", candidates, 3);
```

## Features

- **Adaptive SIMD**: Uses StringZilla for large strings, std::string_view for small
- **Compile-time thresholds**: Configurable via `strings_config.h`
- **RE2 regex**: Safe, predictable performance regex engine
- **rapidfuzz**: Fuzzy string matching for typo tolerance
- **Text formatting**: std::format wrappers, markdown, XML, tables

## API Overview

| Header | Description | |--------|-------------| | `strings.h` | Adaptive find, contains,
starts_with, ends_with, split, join | | `strings_config.h` | SIMD threshold configuration | |
`regex.h` | RE2-backed regex with match/find_all/replace | | `fuzzy.h` | rapidfuzz-backed fuzzy
matching | | `format.h` | std::format convenience wrappers | | `markdown.h` | Markdown rendering
utilities | | `xml_writer.h` | Streaming XML writer | | `table.h` | ASCII table formatting | |
`split.h` | String splitting utilities |

## Configuration

Define before including `strings.h`:

```cpp
#define STRAYLIGHT_STRINGS_FIND_THRESHOLD 256    // Min bytes for SIMD find
#define STRAYLIGHT_STRINGS_PREFIX_THRESHOLD 64   // Min bytes for SIMD prefix
#define STRAYLIGHT_STRINGS_FORCE_STD 1           // Always use std backend
#define STRAYLIGHT_STRINGS_FORCE_SZ 1            // Always use StringZilla
```

## Building

```bash
buck2 build //src/straylight/nix/text:text
buck2 test //src/straylight/nix/text/tests:...
```

## See Also

- [NIH Tracking](../docs/NIH.md)
