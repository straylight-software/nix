# Straylight Nix Compatibility

Straylight Nix is a hardened fork of [Nix](https://nixos.org/nix/) that must maintain full compatibility with upstream Nix. This document describes our compatibility testing strategy and current status.

## Executable Specification

We use **test-first development** where failing tests define what needs to be implemented. Our test suite serves as an executable specification that:

1. **Documents** what compatibility means
2. **Verifies** we match upstream behavior
3. **Prevents** regressions
4. **Guides** implementation of missing features

### Running the Tests

```bash
# Run all compatibility tests
buck2 test //src/nix/store/tests: //src/nix/cli/tests:

# Run specific test suite
buck2 test //src/nix/store/tests:schema-compatibility_test
buck2 test //src/nix/store/tests:protocol-compatibility_test
```

## Test Coverage Summary

| Test Suite | Assertions | Status | Coverage Area |
|------------|------------|--------|---------------|
| schema-compatibility_test | 255 | PASS | Database schema |
| protocol-compatibility_test | 286 | PASS | Worker protocol |
| store-path-format_test | 118 | PASS | Store path format |
| hash-format_test | 184 | PASS | Hash encoding |
| derivation-format_test | 154 | PASS | .drv file format |
| narinfo-format_test | 88 | PASS | Binary cache format |
| authorization-settings_test | 20 | PASS | Daemon auth |
| legacy-commands_test | 173 | PASS | Legacy CLI |
| nix-daemon-integration_test | 277 | PASS | Daemon protocol |
| nix-env-operations_test | 10 | PASS* | nix-env ops |
| nix-store-operations_test | 7 | FAIL | nix-store ops |
| build-remote_test | 2 | FAIL | Distributed builds |
| **Total** | **1,574** | **99.4%** | |

*\* 4 assertions fail as expected (documenting unimplemented operations)*

---

## Database Schema Compatibility

**File:** `src/nix/store/tests/schema-compatibility_test.cpp`

Nix stores metadata in SQLite databases. Column names must match exactly for compatibility with existing installations.

### Store Database (`/nix/var/nix/db/db.sqlite`)

#### ValidPaths Table
| Column | Type | Description |
|--------|------|-------------|
| id | INTEGER | Primary key |
| path | TEXT | Store path |
| hash | TEXT | NAR hash |
| registrationTime | INTEGER | Unix timestamp |
| deriver | TEXT | Derivation that built this |
| narSize | INTEGER | Size of NAR |
| ultimate | INTEGER | Locally built flag |
| sigs | TEXT | Signatures |
| ca | TEXT | Content address |

#### Refs Table
| Column | Type | Description |
|--------|------|-------------|
| referrer | INTEGER | FK to ValidPaths |
| reference | INTEGER | FK to ValidPaths |

#### DerivationOutputs Table
| Column | Type | Description |
|--------|------|-------------|
| drv | INTEGER | FK to ValidPaths |
| id | TEXT | Output name |
| path | TEXT | Output path |

### CA Derivations Database

#### Realisations Table
| Column | Type | Description |
|--------|------|-------------|
| id | INTEGER | Primary key |
| drvPath | TEXT | Derivation path |
| outputName | TEXT | Output name |
| outputPath | TEXT | Realised path |
| signatures | TEXT | Signatures |

### Binary Cache Database

#### BinaryCaches Table
| Column | Type | Description |
|--------|------|-------------|
| id | INTEGER | Primary key |
| url | TEXT | Cache URL |
| timestamp | INTEGER | Last access |
| storeDir | TEXT | Store directory |
| wantMassQuery | INTEGER | Supports mass query |
| priority | INTEGER | Cache priority |

#### NARs Table
| Column | Type | Description |
|--------|------|-------------|
| cache | INTEGER | FK to BinaryCaches |
| hashPart | TEXT | Path hash |
| namePart | TEXT | Path name |
| url | TEXT | NAR URL |
| compression | TEXT | Compression type |
| fileHash | TEXT | Compressed hash |
| fileSize | INTEGER | Compressed size |
| narHash | TEXT | NAR hash |
| narSize | INTEGER | NAR size |
| refs | TEXT | References |
| deriver | TEXT | Deriver path |
| sigs | TEXT | Signatures |
| ca | TEXT | Content address |
| timestamp | INTEGER | Cache time |
| present | INTEGER | Exists flag |

---

## Worker Protocol Compatibility

**File:** `src/nix/store/tests/protocol-compatibility_test.cpp`

The worker protocol enables communication between nix clients and the nix-daemon.

### Protocol Version
- **Current:** 1.38
- **Minimum:** 1.18

### Magic Numbers
```
WORKER_MAGIC_1 = 0x6e697863  ("nixc" - client greeting)
WORKER_MAGIC_2 = 0x6478696f  ("dxio" - daemon response)
```

### Operations (WorkerProto::Op)

| Op | Value | Description |
|----|-------|-------------|
| IsValidPath | 1 | Check if path exists |
| BuildPaths | 9 | Build derivations |
| QueryPathInfo | 26 | Get path metadata |
| AddToStore | 7 | Add path to store |
| AddToStoreNar | 39 | Add NAR to store |
| BuildDerivation | 30 | Build single derivation |
| QueryValidPaths | 31 | Batch path validation |
| CollectGarbage | 20 | Run garbage collector |
| ... | ... | (48 total operations) |

### Stderr Protocol
| Constant | Value | Purpose |
|----------|-------|---------|
| STDERR_NEXT | 0x6f6c6d67 | Log message follows |
| STDERR_READ | 0x64617461 | Read data request |
| STDERR_WRITE | 0x64617416 | Write data request |
| STDERR_LAST | 0x616c7473 | Final message |
| STDERR_ERROR | 0x63787470 | Error follows |
| STDERR_START_ACTIVITY | 0x53545254 | Activity start |
| STDERR_STOP_ACTIVITY | 0x53544f50 | Activity end |
| STDERR_RESULT | 0x52534c54 | Activity result |

---

## Store Path Format

**File:** `src/nix/store/tests/store-path-format_test.cpp`

### Path Structure
```
/nix/store/<hash>-<name>
          └──────┘ └────┘
          32 chars  1-211 chars
```

### Hash Format
- 32 characters
- Nix base32 alphabet: `0123456789abcdfghijklmnpqrsvwxyz`
- (Omits: e, o, u, t to avoid offensive words)

### Name Restrictions
- Max length: 211 characters
- Allowed: `a-z`, `A-Z`, `0-9`, `+`, `-`, `_`, `?`, `=`, `.`
- Cannot start with `.`
- Derivations end with `.drv`

---

## Hash Format

**File:** `src/nix/store/tests/hash-format_test.cpp`

### Supported Algorithms
| Algorithm | Bytes | Base16 | Base32 | Base64 |
|-----------|-------|--------|--------|--------|
| MD5 | 16 | 32 | 26 | 24 |
| SHA1 | 20 | 40 | 32 | 28 |
| SHA256 | 32 | 64 | 52 | 44 |
| SHA512 | 64 | 128 | 103 | 88 |

### Encoding Formats
- **Base16:** `sha256:e3b0c44298fc1c14...`
- **Base32 (Nix):** `sha256:0mdqa9w1p6cmli6976v4wi0sw9r4p5prkj7lzfd1877wk11c9c73`
- **Base64:** `sha256:47DEQpj8HBSa+/TImW+5JCeu...`
- **SRI:** `sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=`

---

## Derivation Format

**File:** `src/nix/store/tests/derivation-format_test.cpp`

### ATerm Format
```
Derive(
  [("out", "/nix/store/...", "", "")],     # outputs
  [("/nix/store/....drv", ["out"])],        # inputDrvs
  ["/nix/store/..."],                       # inputSrcs
  "x86_64-linux",                           # system
  "/nix/store/.../bash",                    # builder
  ["-e", "builder.sh"],                     # args
  [("name", "value"), ...]                  # env
)
```

### Output Types
| Type | Description |
|------|-------------|
| InputAddressed | Traditional output |
| CAFixed | Content-addressed, fixed hash |
| CAFloating | Content-addressed, computed hash |
| Deferred | Hash computed at build time |
| Impure | Non-deterministic |

---

## NARinfo Format

**File:** `src/nix/store/tests/narinfo-format_test.cpp`

### Required Fields
```
StorePath: /nix/store/...-name
URL: nar/....nar.xz
Compression: xz
FileHash: sha256:...
FileSize: 12345
NarHash: sha256:...
NarSize: 67890
```

### Optional Fields
```
References: /nix/store/...-dep1 /nix/store/...-dep2
Deriver: /nix/store/....drv
Sig: cache.example.com:base64signature...
CA: fixed:r:sha256:...
```

### Supported Compression
- `none`
- `xz`
- `bzip2`
- `zstd`
- `br` (brotli)
- `lzip`

---

## Legacy Commands

**File:** `src/nix/cli/tests/legacy-commands_test.cpp`

Legacy commands are invoked via argv[0] (symlink dispatch).

### Implemented

| Command | Status | Modern Equivalent |
|---------|--------|-------------------|
| nix-env | Partial | `nix profile` |
| nix-daemon | Partial | `nix daemon` |
| nix-hash | Full | `nix hash` |
| nix-prefetch-url | Full | `nix store prefetch-file` |

### Not Implemented (Specs Written)

| Command | Status | Modern Equivalent |
|---------|--------|-------------------|
| nix-store | TODO | `nix store` |
| nix-build | TODO | `nix build` |
| nix-shell | TODO | `nix develop` |
| nix-instantiate | TODO | `nix eval` |
| nix-collect-garbage | TODO | `nix store gc` |
| nix-copy-closure | TODO | `nix copy` |
| nix-channel | TODO | Flakes |
| build-remote | TODO | (internal) |

---

## NixOS Compatibility

### Critical for Boot
- `nix-env --list-generations` - Bootloader installer
- `nix-daemon --stdio` - Multi-user mode

### Critical for Operation
- Database schema compatibility - Read existing store
- Worker protocol - Client/daemon communication
- Binary cache format - Fetch from cache.nixos.org

### Current Status
Straylight Nix can:
- Boot and operate NixOS (with `replaceSystemNix = false` workaround)
- Read existing Nix stores
- Communicate with upstream Nix daemons
- Fetch from binary caches

---

## Contributing

### Adding New Compatibility Tests

1. Create test file in `src/nix/{store,cli}/tests/`
2. Use Catch2: `#include <catch2/catch_test_macros.hpp>`
3. Add INFO() messages explaining WHY compatibility matters
4. Add to BUCK file
5. Run tests: `buck2 test //src/nix/.../tests:new_test`

### Test-First Development

For unimplemented features:
1. Write failing test documenting expected behavior
2. Use `[!shouldfail]` tag for expected failures
3. Implement feature
4. Remove `[!shouldfail]` tag
5. Verify test passes

---

## References

- [Nix Manual](https://nixos.org/manual/nix/stable/)
- [Nix Protocol](https://github.com/NixOS/nix/blob/master/src/libstore/worker-protocol.hh)
- [NARinfo Format](https://nixos.org/manual/nix/stable/protocols/store-path.html)
- [Derivation Format](https://nixos.org/manual/nix/stable/protocols/derivation-format.html)
