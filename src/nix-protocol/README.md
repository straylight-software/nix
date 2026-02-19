# Nix Daemon Protocol Specification

This directory contains a formal specification of the Nix daemon "worker protocol" in
[Kaitai Struct](https://kaitai.io/) format.

## Files

- `nix_daemon.ksy` - Kaitai Struct schema for the protocol

## Protocol Overview

The worker protocol is used for communication between:

- `nix` CLI tools and local `nix-daemon` (via Unix socket)
- `nix` CLI tools and remote stores via `ssh-ng://`

### Wire Format Primitives

| Type | Wire Format | |------|-------------| | `u64` | 8 bytes, little-endian | | `string` | `u64`
length + bytes + padding to 8-byte boundary | | `bool` | `u64` (0 = false, nonzero = true) | |
`list<T>` | `u64` count + elements | | `set<T>` | Same as list (elements sorted) | | `optional<T>` |
Empty string = None (for string-like types) |

### Protocol Version

Version is encoded as `(major << 8) | minor`. Current version: **1.38**

The protocol is backwards-compatible: client and server negotiate
`min(client_version, server_version)`.

Fields are version-gated with `if: _root.protocol_version >= N` in the schema.

### Message Flow

```
Client                              Server (nix-daemon)
   |                                    |
   |--- WORKER_MAGIC_1 + version ------>|
   |<-- WORKER_MAGIC_2 + version -------|
   |--- cpu_affinity (obsolete) ------->|
   |--- reserve_space (obsolete) ------>|
   |<-- daemon_version (>= 1.33) -------|
   |<-- trust_level (>= 1.35) ----------|
   |--- features (>= 1.38) ------------>|
   |<-- features (>= 1.38) -------------|
   |                                    |
   |========= Handshake Complete =======|
   |                                    |
   |--- Op + Request ------------------>|
   |<-- STDERR_* messages --------------|  (logging, progress)
   |<-- STDERR_LAST or STDERR_ERROR ----|
   |<-- Response -----------------------|
   |                                    |
   |--- Op + Request ------------------>|
   |    ...                             |
```

### Stderr Protocol

During request processing, the server may send interleaved stderr messages:

| Message | Magic | Description | |---------|-------|-------------| | `STDERR_NEXT` | `0x6f6c6d67` |
Log line | | `STDERR_READ` | `0x64617461` | Request data from client | | `STDERR_WRITE` |
`0x64617416` | Send data to client | | `STDERR_LAST` | `0x616c7473` | Success, response follows | |
`STDERR_ERROR` | `0x63787470` | Error, no response | | `STDERR_START_ACTIVITY` | `0x53545254` |
Progress activity start | | `STDERR_STOP_ACTIVITY` | `0x53544f50` | Progress activity end | |
`STDERR_RESULT` | `0x52534c54` | Progress result |

## Using the Schema

### Generate Parsers

```bash
# Install kaitai-struct-compiler
nix-shell -p kaitai-struct-compiler

# Generate C++ parser
kaitai-struct-compiler -t cpp_stl nix_daemon.ksy

# Generate Rust parser  
kaitai-struct-compiler -t rust nix_daemon.ksy

# Generate Python parser (for testing)
kaitai-struct-compiler -t python nix_daemon.ksy
```

### Visualize Protocol

```bash
# Install ksv (Kaitai Struct Visualizer)
nix-shell -p kaitai-struct-visualizer

# Capture some traffic
sudo socat -t100 -x -v \
    UNIX-LISTEN:/tmp/nix-daemon-debug.sock,fork \
    UNIX-CONNECT:/nix/var/nix/daemon-socket/socket \
    2>&1 | tee capture.txt

# Or use the Web IDE
# https://ide.kaitai.io/
```

## Version History

| Version | Nix Version | Notable Changes | |---------|-------------|-----------------| | 1.38 |
2.24+ | Feature negotiation | | 1.37 | 2.20-2.22 | CPU time in build results | | 1.35 | 2.15-2.19 |
Trust level | | 1.33 | 2.7 | Daemon version string | | 1.32 | 2.4-2.6 | | | 1.28 | | Built outputs
in results | | 1.26 | | Structured errors | | 1.16 | | Ultimate flag, sigs, CA | | 1.10 | 1.0 |
Minimum supported |

## Operations

See `nix_daemon.ksy` for the full list. Key operations:

| Op | Code | Description | |----|------|-------------| | `IsValidPath` | 1 | Check if store path
exists | | `QueryPathInfo` | 26 | Get metadata for store path | | `AddToStore` | 7 | Add NAR to
store | | `BuildPaths` | 9 | Build derivations | | `BuildPathsWithResults` | 46 | Build with
detailed results | | `QueryMissing` | 40 | Query what needs building/fetching | | `NarFromPath` | 38
| Download NAR from store | | `SetOptions` | 19 | Configure daemon options | | `CollectGarbage` | 20
| GC operations |

## Generated Parsers

The following parsers have been generated from `nix_daemon.ksy`:

| Language | File | Status |
|----------|------|--------|
| C++ | `nix_daemon_protocol.{h,cpp}` | Validated (reader) |
| C++ | `nix_daemon_serialize.h` | Validated (writer, 14/14 tests) |
| Python | `nix_daemon_protocol.py` | Validated |
| Rust | `nix_daemon_protocol.rs` | Generated (reader) |
| Rust | `src/lib.rs` | Validated (writer, 14/14 tests) |
| Haskell | `hs/src/Nix/Protocol.hs` | Validated (writer, 14/14 tests) |

### C++ Serializer (Writer)

Header-only C++23 serializer for writing protocol messages:

```cpp
#include "nix_daemon_serialize.h"

std::vector<std::byte> buf;
nix::proto::Writer w{buf};

// Handshake
nix::proto::write_client_hello(w, 0x0126);  // version 1.38

// Request
nix::proto::write_query_path_info_request(w, "/nix/store/...");

// Complex request
nix::proto::ClientSettings settings;
settings.max_build_jobs = 8;
settings.use_substitutes = true;
nix::proto::write_set_options_request(w, settings, 38);
```

**Compile and test:**
```bash
g++ -std=c++23 -Wall -Wextra -Wpedantic -o test_serialize test_serialize.cpp
./test_serialize
# Results: 14 passed, 0 failed
```

### Rust Serializer (Writer)

The Rust crate provides type-safe serializers for writing protocol messages:

```rust
use nix_protocol::{Writer, write_client_hello, write_query_path_info_request};
use nix_protocol::{write_build_paths_request, BuildMode, ClientSettings, write_set_options_request};

// Build a message
let mut buf = Vec::new();
let mut w = Writer::new(&mut buf);

// Handshake
write_client_hello(&mut w, 0x0126).unwrap(); // version 1.38

// Simple request
write_query_path_info_request(&mut w, "/nix/store/abc...").unwrap();

// Build request with mode
write_build_paths_request(
    &mut w,
    &["/nix/store/xyz.drv"],
    BuildMode::Normal,
).unwrap();

// Complex settings
let settings = ClientSettings {
    max_build_jobs: 8,
    use_substitutes: true,
    overrides: vec![
        ("sandbox".to_string(), "true".to_string()),
    ],
    ..Default::default()
};
write_set_options_request(&mut w, &settings, 38).unwrap();
```

**Build and test:**
```bash
cargo test
# running 14 tests (captures) + 3 unit tests
# test result: ok. 17 passed
```

### Haskell Serializer (Writer)

The Haskell library uses a Writer monad for building protocol messages:

```haskell
import Nix.Protocol
import Data.Text (Text)
import qualified Data.Text as T

-- Handshake
clientHello :: ByteString
clientHello = execWriter $ writeClientHello 0x0126  -- version 1.38

-- Simple request
queryPathInfo :: Text -> ByteString
queryPathInfo path = execWriter $ writeQueryPathInfoRequest path

-- Build request
buildRequest :: [Text] -> ByteString
buildRequest paths = execWriter $ writeBuildPathsRequest paths Normal

-- Complex settings
settingsRequest :: ByteString
settingsRequest = execWriter $ writeSetOptionsRequest settings 38
  where
    settings = defaultClientSettings
      { csMaxBuildJobs = 8
      , csUseSubstitutes = True
      , csOverrides = [(T.pack "sandbox", T.pack "true")]
      }
```

**Build and test:**
```bash
# Using nix-shell for dependencies
nix-shell -p "ghc.withPackages (p: [p.bytestring p.text p.tasty p.tasty-hunit p.filepath])" cabal-install

# Build and test
cabal test
# Captures: 14 tests passed
```

### C++ Parser Validation

```bash
# Compile test harness
g++ -std=c++23 \
  -I/path/to/kaitai-struct-cpp-stl-runtime/include \
  -L/path/to/kaitai-struct-cpp-stl-runtime/lib \
  -lkaitai_struct_cpp_stl_runtime \
  test_parser.cpp nix_daemon_protocol.cpp -o test_parser

# Run tests against captured traffic
./test_parser
```
### Test Captures

Binary test vectors in `captures/`. Each operation has `*_request.bin`, `*_response.bin`, and `*_stderr.txt`.

#### Captured Operations (12 unique)

| Operation | Op Code | Request | Response | Description |
|-----------|---------|---------|----------|-------------|
| `SetOptions` | 19 | 448B | 48B | Client configuration |
| `IsValidPath` | 1 | 80B | 8B | Check path exists |
| `QueryPathInfo` | 26 | 72B | 456B | Get ValidPathInfo metadata |
| `QueryMissing` | 40 | 88B | 40B | What needs building/fetching |
| `QueryReferrers` | 6 | 72B | 4KB | What references this path |
| `BuildPaths` | 9 | 88B | 8B | Build derivations |
| `BuildPathsWithResults` | 46 | 96B | 440B | Build with detailed results |
| `AddTempRoot` | 11 | 80B | 8B | Add temporary GC root |
| `AddIndirectRoot` | 12 | 64B | 8B | Add indirect GC root |
| `FindRoots` | 14 | 8B | 65KB | List all GC roots |
| `AddToStoreNar` | 39 | 288B | 64B | Add NAR to store (streaming) |
| `NarFromPath` | 38 | 80B | 144B | Get NAR from store (synthetic) |
| `client_hello` | - | 16B | - | Handshake |
| `server_hello` | - | - | 16B | Handshake |

#### AddToStoreNar (op 39) - Streaming Capture

`AddToStoreNar` uses the framed streaming protocol (version >= 1.23):
- Request header contains: path, deriver, nar_hash, refs, registration_time, nar_size, ultimate, sigs, ca, repair, dont_check_sigs
- NAR data follows as framed chunks: `[u64 length][data]...`, terminated by `length=0`

Captured files:
- `addtostorenar_request.bin` - Request header (288B)
- `addtostorenar_nar.bin` - Framed NAR data (152B, 1 frame + end marker)
- `addtostorenar_response.bin` - Store path result (64B)

#### NarFromPath (op 38) - Synthetic Capture

`NarFromPath` is only triggered when the client **cannot access /nix/store directly** 
(e.g., SSH to remote machine). The request/response format is simple:

- **Request**: `op(8) + store_path(nix_string)` = 80 bytes
- **Response**: Raw NAR bytes (after `STDERR_LAST`)

Captured files:
- `narfrompath_request.bin` - Request (80B, synthetic)
- `narfrompath_response.bin` - NAR data (144B, real NAR from nix-store --dump)

#### Not Captured

| Operation | Reason |
|-----------|--------|
| `AddToStore` (7) | Legacy, superseded by AddToStoreNar |
| `AddMultipleToStore` (44) | Streaming protocol, similar to AddToStoreNar |

These operations use similar streaming patterns to AddToStoreNar.
## tvix nix-compat Cross-Validation

Our Kaitai schema has been cross-validated against
[tvix/nix-compat](https://github.com/tvlfyi/tvix/tree/canon/nix-compat), the production Rust
implementation used by the TVL Nix reimplementation.

### Wire Format: MATCH ✓

| Component | Our Schema | tvix | Status | |-----------|-----------|------|--------| | Integers |
`u8` (LE u64) | `read_u64_le()` | ✓ | | Strings | `u64 len + data + pad(8)` |
`u64 len + data + pad(8)` | ✓ | | Padding | `(8 - (len % 8)) % 8` | `len.wrapping_add(7) & !7` | ✓ |
| Padding validation | (implicit zeros) | Explicit zero-check | ✓ |

### Magic Values: MATCH ✓

| Constant | Our Schema | tvix | Value | |----------|-----------|------|-------| | `WORKER_MAGIC_1`
| `0x6e697863` | `0x6e697863` | "nixc" | | `WORKER_MAGIC_2` | `0x6478696f` | `0x6478696f` | "dxio" |
| `STDERR_LAST` | `0x616c7473` | `0x616c7473` | "alts" | | `STDERR_ERROR` | `0x63787470` |
`0x63787470` | "cxtp" | | `STDERR_READ` | `0x64617461` | `0x64617461` | "data" |

### Protocol Version Encoding: MATCH ✓

Both encode version as `u16` with `major << 8 | minor`, transmitted as LE u64.

| Version | Binary (u16) | Description | |---------|-------------|-------------| | 1.37 | `0x0125` |
tvix current | | 1.38 | `0x0126` | Our target/Nix 2.24+ |

### Operations Enum: MATCH ✓

tvix defines 47 operations, we define 48 (includes `query_active_builds` = 48).

| Op | Code | tvix | Our Schema | |----|------|------|------------| | `IsValidPath` | 1 | ✓ | ✓ | |
`HasSubstitutes` | 3 | ✓ | ✓ | | `QueryPathHash` | 4 | obsolete | obsolete | | `QueryReferences` | 5
| obsolete | obsolete | | `QueryReferrers` | 6 | ✓ | ✓ | | `AddToStore` | 7 | ✓ | ✓ | |
`AddTextToStore` | 8 | obsolete (1.25) | obsolete (1.25) | | `BuildPaths` | 9 | ✓ | ✓ | | ... | ...
| ... | ... | | `AddPermRoot` | 47 | ✓ | ✓ | | `QueryActiveBuilds` | 48 | - | ✓ (1.38+) |

### Handshake Version Gates: MATCH ✓

| Field | Version | tvix | Our Schema | |-------|---------|------|------------| | `cpu_affinity` |
\>= 1.14 | ✓ | ✓ | | `reserve_space` | >= 1.11 | ✓ | ✓ | | `daemon_version` | >= 1.33 | ✓ | ✓ | |
`trust_level` | >= 1.35 | ✓ | ✓ | | Feature exchange | >= 1.38 | - (1.37 max) | ✓ |

### ClientSettings: MATCH ✓

tvix uses `#[nix(version = "12..")]` for setting overrides, we use `if: >= 12`. Identical semantics.

```rust
// tvix ClientSettings
pub struct ClientSettings {
    pub keep_failed: bool,
    pub keep_going: bool,
    pub try_fallback: bool,
    pub verbosity: VerbosityLevel,
    pub max_build_jobs: u64,
    pub max_silent_time: u64,
    pub use_build_hook: bool,
    pub verbose_build: u64,
    pub log_type: u64,
    pub print_build_trace: u64,
    pub build_cores: u64,
    pub use_substitutes: bool,
    #[nix(version = "12..")]
    pub overrides: BTreeMap<String, String>,
}
```

### UnkeyedValidPathInfo: MATCH ✓

tvix struct matches our schema exactly (field names differ slightly but semantics identical):

| Field | tvix | Our Schema | |-------|------|------------| | `deriver` | `Option<StorePath>` |
`optional_store_path` | | `nar_hash` | `String` | `nix_string` | | `references` | `Vec<StorePath>` |
`store_path_set` | | `registration_time` | `u64` | `u8` (u64) | | `nar_size` | `u64` | `u8` (u64) |
| `ultimate` | `bool` | `u8` (>= 1.16) | | `signatures` | `Vec<String>` | `nix_string_set` (>= 1.16)
| | `ca` | `Option<String>` | `nix_string` (>= 1.16) |

### Key Differences

1. **Protocol Version Target**

   - tvix: 1.37 (`PROTOCOL_VERSION = ProtocolVersion::from_parts(1, 37)`)
   - Our schema: 1.38 (includes feature negotiation)
   - Impact: We support `query_active_builds` (op 48) and feature exchange

2. **Minimum Version**

   - tvix: Rejects clients < 1.10
   - Our schema: Documents 1.10 as minimum but doesn't enforce

3. **Trust Level Enum**

   - tvix: `Trust::Trusted = 1`, `Trust::NotTrusted = 2` (no 0)
   - Our schema: `unknown = 0`, `trusted = 1`, `not_trusted = 2`
   - Note: tvix comment says "legacy third option u8 0" exists but they don't implement it

4. **Async vs Sync**

   - tvix: All async (tokio)
   - Our schema: Sync read (Kaitai limitation), but wire format identical

### Conclusion

**The schemas are semantically equivalent.** Our Kaitai specification correctly describes the Nix
daemon wire protocol as implemented by tvix. The minor differences are:

- We target a newer protocol version (1.38 vs 1.37)
- Enum value naming conventions differ
- We have explicit `unknown` trust level (backwards compat)

This cross-validation confirms our specification can be used as a reference for implementing Nix
daemon protocol clients/servers.

## TODO

- [x] Complete schema for all 48 operations
- [x] Add test vectors (captured traffic)
- [x] Generate and validate C++ parser
- [x] Generate Rust parser
- [x] Cross-validate with tvix nix-compat
- [x] Capture core operations (SetOptions, BuildPaths, QueryMissing, etc.)
- [x] Capture AddToStoreNar with framed NAR streaming
- [x] Capture NarFromPath (synthetic - requires SSH/container for real capture)
- [x] Generate C++ serializers (write path) - validated against all captures
- [x] Generate Rust serializers - validated against all captures (14/14 tests)
- [x] Generate Haskell serializers - validated against all captures (14/14 tests)
- [ ] Lean4 serializers (future work)
