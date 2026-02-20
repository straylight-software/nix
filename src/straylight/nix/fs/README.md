# straylight::nix::fs

Filesystem primitives for Nix store operations.

## Quick Start

```cpp
#include <straylight/nix/fs/file_lock.h>
#include <straylight/nix/fs/mmap.h>
#include <straylight/nix/fs/temp.h>

using namespace straylight::nix::fs;

// File locking (POSIX flock)
auto lock = file_lock::exclusive("/nix/store/.links/.lock");
// Lock held until destruction

// Non-blocking try
if (auto lock = file_lock::try_exclusive(path)) {
    // Acquired lock
}

// Memory-mapped file
auto mapping = mmap_file(path, MapMode::ReadOnly);
std::span<const std::byte> data = mapping.span();

// Temporary files/directories
auto tmp_dir = TempDir::create();
auto tmp_file = TempFile::create_in(tmp_dir.path());
```

## Features

- **file_lock**: RAII flock() wrapper with exclusive/shared modes
- **mmap**: Memory-mapped file access
- **temp**: Temporary file/directory with automatic cleanup

## API Overview

| Type/Function | Description |
|---------------|-------------|
| `file_lock::exclusive()` | Blocking exclusive lock |
| `file_lock::shared()` | Blocking shared lock |
| `file_lock::try_exclusive()` | Non-blocking exclusive lock |
| `mmap_file()` | Memory-map a file |
| `TempDir` | RAII temporary directory |
| `TempFile` | RAII temporary file |

## Building

```bash
buck2 build //src/straylight/nix/fs:fs
buck2 test //src/straylight/nix/fs/tests:...
```

## See Also

- [Architecture](docs/ARCHITECTURE.md)
- [Testing](docs/TESTING.md)
