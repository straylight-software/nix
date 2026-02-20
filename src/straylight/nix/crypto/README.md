# straylight::nix::crypto

High-performance cryptographic hash functions and base encodings for Nix store paths.

## Quick Start

```cpp
#include <straylight/nix/crypto/hash.h>
#include <straylight/nix/crypto/encoding.h>

// Hash a string
auto h = straylight::nix::crypto::sha256("hello world");
auto hex = h.to_hex();    // "b94d27b9..."
auto b64 = h.to_base64(); // "uU0nuZNNPg..."

// Streaming hash
straylight::nix::crypto::Hasher hasher(straylight::nix::crypto::Algorithm::BLAKE3);
hasher.update(chunk1);
hasher.update(chunk2);
auto h = hasher.finish();

// Base encodings
auto hex = straylight::nix::crypto::base16::encode(data);
auto nix = straylight::nix::crypto::nix32::encode(data);  // Nix store path format
```

## Features

- **BLAKE3**: 256-bit, AVX-512/AVX2 accelerated via official C library
- **SHA256**: 256-bit, SHA-NI accelerated via OpenSSL (primary algorithm for Nix)
- **SHA512**: 512-bit, via OpenSSL
- **SHA1**: 160-bit, legacy/git compatibility
- **MD5**: 128-bit, legacy/insecure
- **Base16**: Standard lowercase hexadecimal
- **Base64**: RFC 4648 with padding
- **Nix32**: Nix-specific base32 variant (omits e, o, u, t; LSB-first)

## API Overview

| Type/Function | Description | |---------------|-------------| | `Algorithm` | Hash algorithm enum
(MD5, SHA1, SHA256, SHA512, BLAKE3) | | `Hash` | Fixed-size hash result with encoding methods | |
`Hasher` | Streaming hash computation | | `sha256()` | One-shot SHA256 hash | | `blake3()` |
One-shot BLAKE3 hash | | `base16::encode/decode` | Hexadecimal encoding | | `base64::encode/decode`
| RFC 4648 base64 | | `nix32::encode/decode` | Nix store path base32 |

## Building

```bash
buck2 build //src/straylight/nix/crypto:crypto
buck2 test //src/straylight/nix/crypto/tests:...
```

## See Also

- [Architecture](docs/ARCHITECTURE.md)
- [Testing](docs/TESTING.md)
