# Nix Daemon Protocol Specification

This directory contains a formal specification of the Nix daemon "worker protocol"
in [Kaitai Struct](https://kaitai.io/) format.

## Files

- `nix_daemon.ksy` - Kaitai Struct schema for the protocol

## Protocol Overview

The worker protocol is used for communication between:
- `nix` CLI tools and local `nix-daemon` (via Unix socket)
- `nix` CLI tools and remote stores via `ssh-ng://`

### Wire Format Primitives

| Type | Wire Format |
|------|-------------|
| `u64` | 8 bytes, little-endian |
| `string` | `u64` length + bytes + padding to 8-byte boundary |
| `bool` | `u64` (0 = false, nonzero = true) |
| `list<T>` | `u64` count + elements |
| `set<T>` | Same as list (elements sorted) |
| `optional<T>` | Empty string = None (for string-like types) |

### Protocol Version

Version is encoded as `(major << 8) | minor`. Current version: **1.38**

The protocol is backwards-compatible: client and server negotiate `min(client_version, server_version)`.

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

| Message | Magic | Description |
|---------|-------|-------------|
| `STDERR_NEXT` | `0x6f6c6d67` | Log line |
| `STDERR_READ` | `0x64617461` | Request data from client |
| `STDERR_WRITE` | `0x64617416` | Send data to client |
| `STDERR_LAST` | `0x616c7473` | Success, response follows |
| `STDERR_ERROR` | `0x63787470` | Error, no response |
| `STDERR_START_ACTIVITY` | `0x53545254` | Progress activity start |
| `STDERR_STOP_ACTIVITY` | `0x53544f50` | Progress activity end |
| `STDERR_RESULT` | `0x52534c54` | Progress result |

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

| Version | Nix Version | Notable Changes |
|---------|-------------|-----------------|
| 1.38 | 2.24+ | Feature negotiation |
| 1.37 | 2.20-2.22 | CPU time in build results |
| 1.35 | 2.15-2.19 | Trust level |
| 1.33 | 2.7 | Daemon version string |
| 1.32 | 2.4-2.6 | |
| 1.28 | | Built outputs in results |
| 1.26 | | Structured errors |
| 1.16 | | Ultimate flag, sigs, CA |
| 1.10 | 1.0 | Minimum supported |

## Operations

See `nix_daemon.ksy` for the full list. Key operations:

| Op | Code | Description |
|----|------|-------------|
| `IsValidPath` | 1 | Check if store path exists |
| `QueryPathInfo` | 26 | Get metadata for store path |
| `AddToStore` | 7 | Add NAR to store |
| `BuildPaths` | 9 | Build derivations |
| `BuildPathsWithResults` | 46 | Build with detailed results |
| `QueryMissing` | 40 | Query what needs building/fetching |
| `NarFromPath` | 38 | Download NAR from store |
| `SetOptions` | 19 | Configure daemon options |
| `CollectGarbage` | 20 | GC operations |

## Generated Parsers

The following parsers have been generated from `nix_daemon.ksy`:

| Language | File | Status |
|----------|------|--------|
| C++ | `nix_daemon_protocol.{h,cpp}` | Validated |
| Python | `nix_daemon_protocol.py` | Validated |
| Rust | `nix_daemon_protocol.rs` | Generated |

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

Binary test vectors in `captures/`:

| File | Description |
|------|-------------|
| `client_hello.bin` | Client handshake (WORKER_MAGIC_1 + version) |
| `server_hello.bin` | Server handshake (WORKER_MAGIC_2 + version) |
| `query_path_info_request.bin` | QueryPathInfo request |
| `query_path_info.bin` | QueryPathInfo response (ValidPathInfo) |

## TODO

- [x] Complete schema for all 48 operations
- [x] Add test vectors (captured traffic)
- [x] Generate and validate C++ parser
- [x] Generate Rust parser
- [ ] Cross-validate with tvix nix-compat
- [ ] Generate serializers (write path) from schema
- [ ] Add tests for more operations (BuildPaths, AddToStore)
