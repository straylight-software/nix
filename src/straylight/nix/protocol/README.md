# straylight::nix::protocol

Formal specifications and parsers for Nix daemon protocol and NAR format.

## Overview

This module contains:

- **Kaitai Struct** specifications (`.ksy`) for Nix protocols
- Generated parsers in multiple languages (C++, Rust, Haskell, Python)
- Protocol capture files for testing and documentation

## Protocols

### Nix Daemon Protocol (Worker Protocol)

Communication between nix CLI tools and nix-daemon over Unix sockets or SSH.

```
nix_daemon.ksy → nix_daemon_protocol.{h,cpp,rs,py}
```

Wire format:

- Integers: little-endian u64
- Strings: u64 length + bytes + padding to 8-byte boundary
- Booleans: u64 (0 = false, nonzero = true)
- Lists: u64 count + elements

### NAR Format (Nix Archive)

Deterministic archive format for Nix store paths.

```
nar.ksy → nar_serialize.h
```

## Directory Structure

```
protocol/
├── nix_daemon.ksy          # Daemon protocol spec
├── nar.ksy                 # NAR format spec
├── nix_daemon_protocol.h   # C++ generated parser
├── nix_daemon_protocol.rs  # Rust generated parser
├── nar_serialize.h         # NAR serialization
├── captures/               # Protocol captures
├── nar_captures/           # NAR test files
├── hs/                     # Haskell implementation
└── src/                    # Rust implementation
```

## Usage

### C++

```cpp
#include <straylight/nix/protocol/nix_daemon_protocol.h>
#include <straylight/nix/protocol/nar_serialize.h>

// Parse daemon protocol message
kaitai::kstream ks(data);
nix_daemon_protocol_t msg(&ks);

// Serialize NAR
auto nar = nar_serialize(store_path);
```

### Rust

```rust
use straylight_protocol::NixDaemonProtocol;

let msg = NixDaemonProtocol::from_bytes(&data)?;
```

## Building

```bash
# C++ library
buck2 build //src/straylight/nix/protocol:protocol

# Run tests
buck2 test //src/straylight/nix/protocol/tests:...

# Rust library
cd src/straylight/nix/protocol && cargo build

# Regenerate from .ksy (requires kaitai-struct-compiler)
kaitai-struct-compiler -t cpp_stl nix_daemon.ksy
```

## See Also

- [Architecture](docs/ARCHITECTURE.md)
- [Testing](docs/TESTING.md)
- [Kaitai Struct](https://kaitai.io/)
