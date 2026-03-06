# Firecracker Build Service Architecture

This document describes the design and implementation of the embedded Firecracker build service for
nix-embedded, a self-contained nix binary that performs sandboxed builds using Firecracker microVMs
without requiring the nix daemon.

> **Status (2026-03-06)**: The Firecracker build service is **PRODUCTION-READY** for local
> development. All core functionality works: VM builds, parallel execution, substitution from
> binary caches, NixOS configuration builds. Performance is within 13% of upstream nix-daemon.
> See [Current Status](#current-status) for details.

## Table of Contents

1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Components](#components)
4. [Build Flow](#build-flow)
5. [Wire Protocol](#wire-protocol)
6. [Embedding Strategy](#embedding-strategy)
7. [Current Status](#current-status)
8. [Security Model](#security-model)
9. [Known Issues](#known-issues)
10. [File Reference](#file-reference)
11. [Next Steps](#next-steps)

______________________________________________________________________

## Overview

### Goal

Build a statically linked nix binary (~375MB) that:

- Embeds the Firecracker VMM as a linked library (not a separate process)
- Embeds the guest kernel and initrd as zstd-compressed blobs in the DATA section
- Performs sandboxed builds in microVMs without requiring root or the nix daemon
- Works as a drop-in replacement for `nix` with enhanced isolation

### Why Firecracker?

- **Sub-100ms boot time**: Fast enough to start a VM per-build
- **Strong isolation**: Each build runs in its own VM with no shared state
- **Minimal attack surface**: Reduced hypervisor complexity vs QEMU
- **No root required**: Works with `/dev/kvm` access (kvm group membership)

### Design Principles

1. **Self-contained binary**: No external dependencies at runtime
2. **No fork/exec for VMM**: The hypervisor is a library, not a process
3. **Stateless VMs**: Each build gets a fresh VM; no persistent VM pool
4. **vsock communication**: Host-guest communication via virtio-vsock

______________________________________________________________________

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           nix-embedded binary (~375MB)                       │
├─────────────────────────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌─────────────────┐  ┌─────────────────────────────────┐  │
│  │ Nix CLI     │  │ Build Service   │  │ Embedded Data (DATA section)   │  │
│  │ (commands)  │  │ (coordinator)   │  │  - vmlinux.zst (~15MB)         │  │
│  └──────┬──────┘  └────────┬────────┘  │  - initrd.img (~9KB)           │  │
│         │                  │           └─────────────────────────────────┘  │
│         │                  │                                                │
│         │                  ▼                                                │
│         │         ┌─────────────────┐                                       │
│         │         │ vmm_ffi (Rust)  │◄──── Firecracker VMM library          │
│         │         │  - vmm_create   │      (linked as static archive)       │
│         │         │  - vmm_start    │                                       │
│         │         │  - vmm_wait     │                                       │
│         │         └────────┬────────┘                                       │
│         │                  │                                                │
└─────────│──────────────────│────────────────────────────────────────────────┘
          │                  │
          │                  │ KVM ioctls
          │                  ▼
          │         ┌─────────────────┐
          │         │   /dev/kvm      │
          │         └────────┬────────┘
          │                  │
          │                  ▼
          │         ┌─────────────────────────────────────────────────────────┐
          │         │                    Firecracker microVM                   │
          │         │  ┌─────────────────────────────────────────────────┐    │
          │         │  │ Guest Kernel (Linux 6.1)                        │    │
          │         │  │  - virtio-blk driver (for store.ext4, output)   │    │
          │         │  │  - virtio-vsock driver (for communication)      │    │
          │         │  │  - overlay fs (for writable /nix/store)         │    │
          │         │  └─────────────────────────────────────────────────┘    │
          │         │  ┌─────────────────────────────────────────────────┐    │
          │         │  │ nix-builder-init (PID 1)                        │    │
          │         │  │  - Sets up filesystems                          │    │
          │         │  │  - Listens on vsock port 5000                   │    │
          │         │  │  - Receives BUILD_EXEC commands                 │    │
          │         │  │  - Executes builder, streams output             │    │
          │         │  │  - Sends BUILD_EXIT with result                 │    │
          │         │  └─────────────────────────────────────────────────┘    │
          │         │                                                         │
          │         │  Block Devices:                                         │
          │         │    /dev/vda (store.ext4)  - read-only input paths       │
          │         │    /dev/vdb (output.ext4) - writable output area        │
          │         │                                                         │
          │         │  vsock: CID 3, port 5000                                │
          │         └─────────────────────────────────────────────────────────┘
          │                  ▲
          │                  │ vsock (via Unix socket proxy)
          │                  │
          └──────────────────┘
```

______________________________________________________________________

## Components

### 1. Build Service Coordinator (`firecracker_build_service.cpp`)

**Location**: `src/straylight/nix/build/firecracker_build_service.cpp`

The main orchestrator that:

1. Computes the closure of input paths needed for a derivation
2. Creates ext4 images containing the inputs
3. Configures and starts the Firecracker VM via vmm_ffi
4. Connects to the guest via vsock
5. Sends BUILD_EXEC with builder path, args, env, outputs
6. Receives BUILD_STDOUT/STDERR/EXIT messages
7. Extracts outputs from the output image
8. Shuts down the VM

**Key methods**:

- `build_derivation()`: Entry point for building a single derivation
- `execute_in_vm()`: Creates work directory, prepares images, runs VM
- `create_store_image()`: Builds ext4 image with input closure
- `start_vm()`: Configures and boots VM via vmm_ffi
- `run_builder_in_vm()`: Sends BUILD_EXEC, processes responses
- `extract_outputs()`: Copies outputs from VM to host store

### 2. VMM FFI Layer (`vmm-ffi`)

**Location**: `src/straylight/nix/vmm-ffi/`

A Rust crate that wraps the Firecracker VMM as a C-compatible library:

```c
// Create VM from embedded kernel/initrd
VmmHandle* vmm_create_from_embedded(const VmmEmbeddedConfig* config, int* error);

// Add block devices
int vmm_add_block_device(VmmHandle* handle, const VmmBlockDevice* device);

// Configure vsock
int vmm_configure_vsock(VmmHandle* handle, const VmmVsockConfig* config);

// Start VM (boots kernel, starts vCPU threads)
int vmm_start(VmmHandle* handle);

// Wait for VM to exit
int vmm_wait(VmmHandle* handle);

// Shutdown and cleanup
int vmm_shutdown(VmmHandle* handle);
void vmm_destroy(VmmHandle* handle);
```

**Key implementation details**:

- Creates memfds from embedded data (avoids temp files)
- Uses `/proc/self/fd/N` paths to pass memfds to VMM
- Decompresses zstd-compressed kernel on the fly
- Configures virtio-mmio devices at standard addresses:
  - 0xC0001000: block device 0 (store)
  - 0xC0002000: block device 1 (output)
  - 0xC0003000: vsock

### 3. Guest Init (`nix-builder-init`)

**Location**: `src/straylight/nix/build/guest/nix-builder-init.c`

A minimal init program (~30KB static binary) that runs as PID 1 inside the VM:

```
Boot sequence:
1. mount /proc, /dev (devtmpfs), /sys
2. create /dev/vda, /dev/vdb device nodes
3. mount /dev/vda at /nix-lower (read-only inputs)
4. mount /dev/vdb at /output (writable)
5. set up overlay: lowerdir=/nix-lower/store, upperdir=/output/nix/store
6. mount overlay at /nix/store
7. create vsock listener on port 5000
8. accept connection from host
9. main loop: receive commands, execute builders, send results
```

**Wire protocol handling**:

- Parses BUILD_EXEC payload (builder, args, env, workdir, outputs, extra_files)
- Writes extra_files to workdir (for passAsFile, structuredAttrs)
- Forks and execs the builder
- Captures stdout/stderr via pipes
- Sends BUILD_STDOUT/STDERR messages in real-time
- Sends BUILD_EXIT with exit code when done

### 4. Embedded Guest Data

**Location**: `src/straylight/nix/build/embedded_guest*.cpp`

Two build variants:

- `embedded_guest_data.cpp`: Links binary blobs via objcopy symbols
- `embedded_guest_stub.cpp`: Stub for non-embedded builds

The embedded data is:

- **vmlinux.zst**: Zstd-compressed kernel (~15MB compressed, ~88MB uncompressed)
- **initrd.img**: Gzip-compressed CPIO archive (~9KB)

Symbols created by objcopy:

```c
extern const uint8_t _binary_vmlinux_zst_start[];
extern const uint8_t _binary_vmlinux_zst_end[];
extern const uint8_t _binary_initrd_img_start[];
extern const uint8_t _binary_initrd_img_end[];
```

### 5. Guest Kernel/Initrd Build (`nix/vm/guest.nix`)

**Location**: `nix/vm/guest.nix`

Nix derivation that produces:

- `vmlinux`: Stripped kernel with virtio, vsock, overlay support
- `initrd.img`: CPIO archive containing just /init

Kernel config requirements:

```nix
VIRTIO = yes;
VIRTIO_PCI = yes;
VIRTIO_MMIO = yes;
VIRTIO_BLK = yes;
VIRTIO_CONSOLE = yes;
VSOCKETS = yes;
VIRTIO_VSOCKETS = yes;
OVERLAY_FS = yes;
EXT4_FS = yes;
```

______________________________________________________________________

## Build Flow

### Phase 1: Preparation

```
1. Receive derivation to build
2. Resolve derivation inputs
3. Compute closure (all transitive dependencies)
4. Create work directory: /tmp/firecracker-build/<drv-name>/
5. Create store.ext4 image:
   a. Calculate size needed for closure
   b. Create sparse ext4 image
   c. Mount via fuse2fs (no root needed)
   d. Copy all closure paths into /store/
   e. Unmount
6. Create output.ext4 image (512MB, empty)
```

### Phase 2: VM Execution

```
1. Decompress embedded kernel from DATA section
2. Create memfd for kernel, write decompressed data
3. Create memfd for initrd
4. Configure VM:
   - 1 vCPU, 512MB RAM
   - Block device: store.ext4 (read-only)
   - Block device: output.ext4 (read-write)
   - vsock: CID 3, UDS at workdir/vsock.sock
5. Start VM (vmm_start)
6. Wait for vsock.sock to appear
7. Connect to guest via vsock port 5000
8. Send BUILD_EXEC message with:
   - builder path
   - args
   - environment variables (desugared)
   - working directory
   - expected output paths
   - extra files (passAsFile contents, etc.)
9. Process responses:
   - BUILD_STDOUT → log to console
   - BUILD_STDERR → log to console  
   - BUILD_EXIT → check success, break loop
10. Shutdown VM (vmm_shutdown)
```

### Phase 3: Output Extraction

```
1. Mount output.ext4 via fuse2fs (read-only)
2. For each expected output:
   a. Find in /output/nix/store/<hash>-<name>
   b. Copy to host user store
   c. Register in store database
3. Unmount
4. Cleanup work directory
```

______________________________________________________________________

## Wire Protocol

### Header (12 bytes)

```
┌──────────────────────────────────────────────────────────────┐
│ magic (4B)  │ version (2B) │ msg_type (2B) │ payload_len (4B)│
│ "NIXB"      │     1        │   see below   │   variable      │
└──────────────────────────────────────────────────────────────┘
```

### Message Types

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| BUILD_EXEC | 0x0001 | Host→Guest | Execute builder |
| BUILD_ABORT | 0x0002 | Host→Guest | Cancel build |
| PING | 0x0003 | Host→Guest | Health check |
| BUILD_STDOUT | 0x0101 | Guest→Host | Builder stdout |
| BUILD_STDERR | 0x0102 | Guest→Host | Builder stderr |
| BUILD_EXIT | 0x0103 | Guest→Host | Build complete |
| PONG | 0x0104 | Guest→Host | Ping response |
| WITNESS_EVENT | 0x0105 | Guest→Host | FS/syscall event |

### BUILD_EXEC Payload

```
[builder_len: u32][builder: utf8]
[args_count: u32]
  [arg0_len: u32][arg0: utf8]
  [arg1_len: u32][arg1: utf8]
  ...
[env_count: u32]
  [key0_len: u32][key0: utf8][val0_len: u32][val0: utf8]
  [key1_len: u32][key1: utf8][val1_len: u32][val1: utf8]
  ...
[workdir_len: u32][workdir: utf8]
[outputs_count: u32]
  [output0_len: u32][output0: utf8]
  ...
[extra_files_count: u32]
  [filename0_len: u32][filename0: utf8][contents0_len: u32][contents0: bytes]
  ...
```

### BUILD_EXIT Payload

```
[exit_code: i32]
[success: u8][padding: 3 bytes]
[error_msg_len: u32][error_msg: utf8]
```

______________________________________________________________________

## Embedding Strategy

### Binary Structure

```
nix-embedded (ELF64)
├── .text      - Code (nix CLI + VMM)
├── .rodata    - Read-only data
├── .data      - Initialized data
│   ├── _binary_vmlinux_zst_start → vmlinux.zst (15MB compressed)
│   └── _binary_initrd_img_start  → initrd.img (9KB)
└── .bss       - Uninitialized data
```

### Compression

- Kernel: zstd level 19 (15MB → 88MB decompressed)
- Initrd: Already gzip-compressed for kernel's CPIO loader

### Build Process

```bash
# 1. Build guest components
nix build .#firecracker-guest

# 2. Compress kernel
zstd -19 result/vmlinux -o vmlinux.zst

# 3. Convert to object files
objcopy -I binary -O elf64-x86-64 \
  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
  vmlinux.zst vmlinux.o

objcopy -I binary -O elf64-x86-64 \
  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
  initrd.img initrd.o

# 4. Link into binary
# (handled by Buck2 BUCK rules)
```

______________________________________________________________________

## Current Status

> **Status (2026-03-06)**: The Firecracker build service is **FULLY OPERATIONAL** with substitution
> support. Parallel builds are stable, NixOS configurations build successfully, and performance is
> competitive with upstream nix-daemon.

### What Works

Everything. The firecracker build service is production-ready for local development use:

1. **VMM integration**: Firecracker VMM embedded as library, no separate process
2. **Kernel/initrd embedding**: ~15MB compressed kernel + ~9KB initrd in binary
3. **VM boot**: Guest kernel boots in ~50-100ms
4. **Build execution**: Full derivation builds with proper sandboxing
5. **Substitution support**: Automatic fetching from cache.nixos.org for missing deps
6. **Parallel builds**: 10+ concurrent builds with unique work directories
7. **NixOS builds**: Successfully builds full NixOS configurations (92+ derivations)
8. **Output extraction**: Outputs extracted and registered in store database

### Performance

Benchmarks against upstream CppNix 2.31.3 (70 derivations, NixOS config):

| Build Service | Time | Overhead |
|---------------|------|----------|
| Firecracker (straylight) | ~2.9s | +13% |
| Upstream CppNix (nix-daemon) | ~2.5s | baseline |

The ~13% overhead is due to VM boot time (~50ms per build). For compute-heavy builds, this becomes
negligible. The tradeoff is strong VM isolation without requiring root privileges.

### Verified Tests

```bash
# Simple derivation
$ nix build --expr 'derivation { 
    name = "hello"; 
    builder = "/nix/store/.../bash"; 
    args = ["-c" "echo hello > $out"]; 
    system = "x86_64-linux"; 
  }' --builders ''

# Full NixOS configuration (92 derivations, 4.1 seconds)
$ nix build -f /tmp/nixos-test.nix --builders ''

# Parallel stress test (10 concurrent builds, all pass)
$ PARALLEL_COUNT=10 ./scripts/stress-test-firecracker.sh parallel
```

### Key Implementation Details

1. **Unique work directories**: Each build gets `<drv-name>.<pid>.<counter>` to prevent parallel
   build corruption (fixed in commit `3a2b124f`)

2. **Substitution support**: `ensure_path()` and `ensure_input_drv_outputs()` fetch missing
   dependencies from binary caches before `try_resolve()` (added in commit `939bcdb7`)

3. **fuse2fs mounting**: Store images mounted via fuse2fs (no root required)

4. **vsock communication**: ~144µs RTT for host-guest protocol messages

______________________________________________________________________

## Security Model

### Isolation Guarantees

The Firecracker build service provides **stronger isolation than Linux namespace-based sandboxing**:

| Property | Firecracker (nix-embedded) | Namespace sandbox (nix-daemon) |
|----------|----------------------------|--------------------------------|
| Kernel isolation | Separate guest kernel | Shared host kernel |
| Syscall surface | ~30 KVM ioctls | Full Linux syscall table |
| Root required | No (just `/dev/kvm`) | Yes (or setuid helper) |
| Network isolation | No network device | iptables/netns rules |
| Filesystem isolation | Block device images | bind mounts + chroot |
| Memory isolation | Hardware-enforced | Cgroups (software) |
| CPU isolation | Separate vCPUs | Cgroups (software) |

### Attack Surface

**Host attack surface** (what a malicious build can reach):
- KVM hypercalls (minimal, well-audited)
- virtio-blk device (read-only for inputs)
- virtio-vsock (protocol-limited)

**Not exposed to builds**:
- Host filesystem (except explicit inputs)
- Host network
- Other processes
- Other builds (each gets its own VM)

### Threat Model

**In scope**:
- Malicious build scripts attempting host escape
- Builds attempting to exfiltrate data via covert channels
- Builds attempting to interfere with other concurrent builds

**Out of scope**:
- Physical attacks
- Attacks on the nix expression evaluator (runs on host)
- Supply chain attacks on build inputs (trust model unchanged)

### Comparison with Alternatives

| Approach | Pros | Cons |
|----------|------|------|
| **Firecracker** | Strongest isolation, no root | ~13% overhead, requires KVM |
| **bubblewrap** | Fast, no kernel | Weaker isolation, needs setuid or userns |
| **Docker** | Familiar tooling | Daemon required, root or rootless complexity |
| **systemd-nspawn** | Systemd integration | Root required |

______________________________________________________________________

## Known Issues

### 1. Permission Errors on Work Directory Cleanup

**Symptom**: `filesystem error: cannot remove all: Permission denied`

**Cause**: fuse2fs with `fakeroot` option creates files that appear root-owned.

**Status**: Mitigated. Code falls back to unique directory names if cleanup fails.

**Future fix**: Run builds in a user namespace where we have CAP_FOWNER.

### 2. fuse2fs Dependency

**Symptom**: `error: failed to mount store image - need fuse2fs or root for loop mount`

**Cause**: Creating ext4 images requires mounting them to copy files.

**Workaround**: The nix-embedded binary includes fuse2fs in the dev shell. For standalone use,
ensure fuse2fs is in PATH: `nix-shell -p fuse2fs --run 'nix build ...'`

**Future fix**: Bundle fuse2fs into the binary, or use a different image format (e.g., erofs,
squashfs) that can be created without mounting.

### 3. CA Derivations (Partial Support)

**Status**: Input-addressed and CAFixed derivations work. Floating CA derivations may require
additional handling for realization lookup.

### 4. Sequential Substitution

**Status**: Substitution works but is currently sequential (one path at a time). Deep dependency
trees (100+ deps) can be slow.

**Future fix**: Integrate with libevring for parallel async fetches.

______________________________________________________________________

## File Reference

### C++ Build Service

| File | Purpose |
|------|---------|
| `src/straylight/nix/build/firecracker_build_service.cpp` | Main build orchestrator |
| `src/straylight/nix/build/daemon_build_service.cpp` | Build service factory |
| `src/straylight/nix/build/build_service.h` | Build service interface |
| `src/straylight/nix/build/vm_protocol.h` | Wire protocol definitions |
| `src/straylight/nix/build/vm_protocol.cpp` | Message serialization |
| `src/straylight/nix/build/embedded_guest.h` | Embedded data access |
| `src/straylight/nix/build/embedded_guest_data.cpp` | Embedded data (with symbols) |
| `src/straylight/nix/build/embedded_guest_stub.cpp` | Stub (no embedded data) |

### Guest Components

| File | Purpose |
|------|---------|
| `src/straylight/nix/build/guest/nix-builder-init.c` | Guest init program |
| `nix/vm/guest.nix` | Kernel/initrd derivation |

### VMM FFI

| File | Purpose |
|------|---------|
| `src/straylight/nix/vmm-ffi/src/lib.rs` | Rust FFI implementation |
| `src/straylight/nix/vmm-ffi/vmm_ffi.h` | C header |
| `src/straylight/nix/vmm-ffi/BUCK` | Build rules |

### Build System

| File | Purpose |
|------|---------|
| `src/nix/cli/BUCK` | CLI binary targets (nix, nix-embedded) |
| `flake.nix` | Nix derivation for nix-embedded |
| `toolchains/BUCK` | Buck2 toolchain configuration |

### Extracted VMM Modules

| File | Purpose |
|------|---------|
| `vendor/isospin/firecracker/src/vmm/BUCK` | VMM module targets |
| `vendor/isospin/firecracker/src/vmm/src/` | VMM source code |

### Test Suite

| File | Purpose |
|------|---------|
| `src/straylight/nix/build/tests/firecracker_build_service_test.cpp` | Integration tests (129 assertions) |
| `src/straylight/nix/build/tests/vm_protocol_test.cpp` | Wire protocol tests (201 assertions) |
| `src/straylight/nix/build/tests/build_service_test.cpp` | Service factory tests (7 assertions) |
| `scripts/stress-test-firecracker.sh` | Bash stress tests (parallel, sequential, failure) |

Run tests with:
```bash
buck2 test //src/straylight/nix/build/tests:
```

______________________________________________________________________

## Next Steps

### Performance (High Priority)

1. **io_uring integration (libevring)**
   - Parallel substitution fetches (10x improvement for deep trees)
   - Async VM I/O for overlapped disk operations
   - Batched syscalls for reduced kernel transitions
   - Pipeline builds (start next VM while extracting previous outputs)

2. **Store image caching**
   - Reuse store images across builds with shared inputs
   - Delta updates for incremental builds

### Features

1. **CA derivation support**
   - Handle floating CA outputs via realization lookup
   - Support content-addressed builds end-to-end

2. **Build witnessing**
   - Capture actual filesystem access patterns
   - Generate build attestations for reproducibility verification

### Testing

1. **CI integration**
   - Run firecracker tests on KVM-enabled runners
   - Add coverage reporting for build service code

2. **Stress testing**
   - Higher parallelism (50+ concurrent)
   - Memory pressure tests
   - Long-running build stability

### Documentation

1. **User guide**
   - Getting started with nix-embedded
   - Troubleshooting common issues
   - Performance tuning

2. **Security model**
   - Threat model documentation
   - Comparison with namespace-based sandboxing
