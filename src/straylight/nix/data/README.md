# straylight::nix::data

High-performance data structures for Nix evaluation and store operations.

## Quick Start

```cpp
#include <straylight/nix/data/lru_cache.h>
#include <straylight/nix/data/chunked_vector.h>
#include <straylight/nix/data/topo_sort.h>
#include <straylight/nix/data/serialise.h>

using namespace straylight::nix::data;

// LRU cache with O(1) operations
LRUCache<std::string, int> cache(100);
cache.put("key", 42);
auto val = cache.get("key");  // std::optional<int>

// Thread-safe variant
LRUCacheSafe<std::string, int> safe_cache(100);

// Chunked vector (stable pointers)
ChunkedVector<Data> vec;
vec.push_back(data);
// Pointers remain valid after growth

// Topological sort
auto sorted = topo_sort(nodes, get_deps);

// Serialization (zpp_bits)
auto bytes = serialize(value);
auto result = deserialize<T>(bytes);
```

## Features

- **LRUCache**: O(1) get/put/erase with optional eviction callback
- **LRUCacheSafe**: Thread-safe LRU with shared_mutex
- **ChunkedVector**: Stable pointers, no reallocation invalidation
- **topo_sort**: Topological ordering for dependency graphs
- **serialise**: zpp_bits wrapper for binary serialization

## API Overview

| Type/Function | Description | |---------------|-------------| | `LRUCache<K, V>` | Non-thread-safe
LRU cache | | `LRUCacheSafe<K, V>` | Thread-safe LRU cache | | `ChunkedVector<T>` | Vector with
stable element addresses | | `topo_sort()` | Topological sort for DAGs | | `serialize()` | Binary
serialization | | `deserialize<T>()` | Binary deserialization |

## Building

```bash
buck2 build //src/straylight/nix/data:data
buck2 test //src/straylight/nix/data/tests:...
```

## See Also

- [Architecture](docs/ARCHITECTURE.md)
- [Testing](docs/TESTING.md)
