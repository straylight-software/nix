# Firecracker Build Service Architecture

This document describes the design and implementation of the embedded Firecracker
build service for nix-embedded, a self-contained nix binary that performs sandboxed
builds using Firecracker microVMs without requiring the nix daemon.

> **Status (2026-03-06)**: The Firecracker build service is **WORKING**. VM boot,
> vsock communication, build execution, and output registration are all functional.
> See [Current Status](#current-status) for details.

## Table of Contents

1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Components](#components)
4. [Build Flow](#build-flow)
5. [Wire Protocol](#wire-protocol)
6. [Embedding Strategy](#embedding-strategy)
7. [Current Status](#current-status)
8. [Known Issues](#known-issues)
9. [File Reference](#file-reference)

---

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

---

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

---

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

---

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

---

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

---

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

---

## Current Status

### What Works

1. **VMM integration**: Firecracker VMM links and initializes correctly
2. **Kernel/initrd embedding**: Data decompresses and loads into memfds
3. **VM boot**: Guest kernel boots in ~100ms
4. **Block devices**: virtio-blk devices visible to guest kernel
5. **vsock configuration**: Host-side vsock UDS created
6. **Store image creation**: Input closure packaged into ext4

### What's Working (Fixed 2026-03-06)

The following components are now **fully functional**:

1. **VM boot and event loop**: The `vmm_start_event_loop()` function runs the
   VMM event loop in a background thread, which is required for virtio devices
   to process MMIO accesses and interrupts.

2. **vsock communication**: The host successfully connects to the guest's vsock
   listener on port 5000. PING/PONG test shows ~144µs RTT.

3. **Build execution**: Builders execute in the guest with proper environment
   variables and output to the overlay filesystem.

4. **Output extraction**: Outputs are extracted from the VM's ext4 image and
   copied to `/nix/store`.

5. **Store registration**: Outputs are registered in the nix store database
   via `nix-store --register-validity`.

6. **Build hook integration**: The `nix build` command properly invokes
   `__build-remote` which uses the firecracker build service when no remote
   machines are configured.

### Verified Test

```bash
$ nix build --expr 'derivation { 
    name = "hello"; 
    builder = "/nix/store/.../bash"; 
    args = ["-c" "echo hello > $out"]; 
    system = "x86_64-linux"; 
  }' --builders ''
# Completes successfully, output in /nix/store
```

### Key Fixes Applied

1. **Event loop for virtio** (`vmm-ffi/src/lib.rs`):
   - Added `vmm_start_event_loop()` to spawn background thread
   - Event loop checks `shutdown_exit_code()` for proper termination

2. **Binary protocol handling** (`build-remote.cpp`):
   - Settings read via `read_num`/`read_string` (binary, not text)
   - Build requests parsed via CommonProto serialization
   - Additional input_paths/missing_outputs read after accept

3. **Output registration** (`build-remote.cpp`):
   - Uses `nix-store --register-validity` to register outputs
   - Format: `printf 'path\n\n0\n' | nix-store --register-validity`

4. **Mount mode** (`firecracker_build_service.cpp`):
   - Changed output mount from read-only to read-write (ext4 journal)

---

## Known Issues

### 1. Permission Errors on Work Directory Cleanup

**Symptom**: `filesystem error: cannot remove all: Permission denied`

**Cause**: fuse2fs with `fakeroot` option creates files that appear root-owned.
When the build fails or is interrupted, these files cannot be removed by the
non-root user.

**Workaround**: The code now falls back to a unique directory name if cleanup fails.

**Proper fix**: Run builds in a user namespace where we have CAP_FOWNER.

### 2. fuse2fs Dependency

**Symptom**: `error: failed to mount store image - need fuse2fs or root for loop mount`

**Cause**: Creating ext4 images requires mounting them to copy files. Without
root, we need fuse2fs (from e2fsprogs).

**Workaround**: Run with fuse2fs in PATH: `nix-shell -p fuse2fs --run 'nix build ...'`

**Proper fix**: Bundle fuse2fs into the binary, or use a different image format
(e.g., erofs, squashfs) that can be created without mounting.

### 3. vsock Connection Failure (CURRENT BLOCKER)

**Symptom**: `vsock: failed to connect to guest port 5000 after 10 attempts`

**Cause**: Under investigation. The VM boots and kernel initializes virtio devices,
but the connection to the guest's vsock listener fails.

**Debug info from last run**:
```
vmm-ffi: MMIO[2] @ 0xc0003000: found=true, magic=0x74726976, version=2, device_id=19
firecracker: VM started (embedded), vsock=/tmp/.../vsock.sock
[kernel] virtio_blk virtio0: [vda] 2040226 512-byte logical blocks
vsock: failed to connect to guest port 5000 after 10 attempts
```

The vsock device (device_id=19) is registered at the correct MMIO address.
The kernel sees the block device. But we never see output from nix-builder-init,
suggesting it either:
- Doesn't start (init= not found)
- Crashes early (before reaching vsock listen)
- Is blocked on something (filesystem mount failure)

---

## File Reference

### C++ Build Service

| File | Purpose |
|------|---------|
| `src/straylight/nix/build/firecracker_build_service.cpp` | Main build orchestrator |
| `src/straylight/nix/build/daemon_build_service.cpp` | Build service factory (returns firecracker) |
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

---

## Next Steps

1. **Debug vsock connection issue**
   - Add serial console output capture
   - Verify guest init actually runs
   - Check vsock device initialization in guest

2. **Add integration tests**
   - Test vsock protocol separately
   - Test image creation/extraction
   - End-to-end build test with trivial derivation

3. **Improve error handling**
   - Better error messages when VM fails
   - Capture guest kernel logs on failure
   - Timeout handling for hung builds

4. **Performance optimization**
   - Reuse store images across builds when possible
   - Parallel image population
   - Memory-mapped I/O for large builds

5. **Documentation**
   - API documentation for vmm_ffi
   - User guide for nix-embedded
   - Troubleshooting guide
