# Firecracker Build Service

> **Status: Working** - End-to-end builds verified as of March 2026. The embedded Firecracker VMM
> successfully executes builds without requiring the nix-daemon.

The Firecracker build service provides daemonless, sandboxed Nix builds using Firecracker microVMs.
It eliminates the need for a root nix-daemon while providing stronger isolation than traditional
namespace-based sandboxing.

## Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              Host System                                     │
│                                                                             │
│  ┌─────────────────┐    ┌──────────────────────────────────────────────┐   │
│  │                 │    │           Firecracker microVM                │   │
│  │  straylight-nix │    │                                              │   │
│  │                 │    │  ┌────────────────────────────────────────┐  │   │
│  │  ┌───────────┐  │    │  │         nix-builder-init               │  │   │
│  │  │ build_svc │◄─┼────┼──┤                                        │  │   │
│  │  └───────────┘  │    │  │  • Mounts /nix/store (read-only)       │  │   │
│  │       │         │    │  │  • Mounts /output (read-write)         │  │   │
│  │       │ vsock   │    │  │  • Executes builders                   │  │   │
│  │       │         │    │  │  • Streams stdout/stderr               │  │   │
│  │       ▼         │    │  │  • Reports witness events              │  │   │
│  │  ┌───────────┐  │    │  └────────────────────────────────────────┘  │   │
│  │  │  witness  │  │    │                                              │   │
│  │  │  record   │  │    │  /dev/vda ← store.ext4 (inputs)              │   │
│  │  └───────────┘  │    │  /dev/vdb ← output.ext4 (outputs)            │   │
│  │                 │    │                                              │   │
│  └─────────────────┘    └──────────────────────────────────────────────┘   │
│                                                                             │
│  ~/.local/share/nix/store/  ←  Two-tier store (user writable)              │
│  /nix/store/                ←  System store (read-only fallback)           │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Why Firecracker?

### Problems with nix-daemon

1. **Requires root** - The daemon needs root for namespace sandboxing
2. **Single point of failure** - All builds go through one daemon process
3. **Limited observability** - Hard to see what builds actually do
4. **Legacy sandboxing** - Uses namespace-based isolation from 2010s

### Firecracker advantages

1. **No root required** - Just `/dev/kvm` access (kvm group membership)
2. **Hardware isolation** - Each build in its own VM with separate kernel
3. **Deep witnessing** - Full visibility into filesystem access, syscalls
4. **Fast boot** - \<100ms VM startup time
5. **Two-tier store** - Works with straylight's user + system store architecture

## Architecture

### Components

```
src/straylight/nix/build/
├── build_service.h              # Abstract interface
├── daemon_build_service.cpp     # nix-daemon backend
├── firecracker_build_service.cpp # Firecracker backend (1200+ lines)
├── reapi_build_service.cpp      # REAPI backend (stub)
├── vm_protocol.h                # Wire protocol definitions
├── vm_protocol.cpp              # Wire protocol serialization
└── guest/
    └── nix-builder-init.c       # Guest init program (PID 1)

nix/vm/
└── guest.nix                    # Nix derivation for kernel + initrd

scripts/
└── setup-firecracker.sh         # Setup script
```

### Wire Protocol

Communication between host and guest uses vsock with a simple binary protocol:

```
Wire Header (12 bytes):
┌──────────┬──────────┬──────────┬──────────────┐
│  magic   │ version  │ msg_type │ payload_len  │
│  (u32)   │  (u16)   │  (u16)   │    (u32)     │
└──────────┴──────────┴──────────┴──────────────┘
   "NIXB"      1        see below

Message Types:
  Host → Guest:
    0x0001  BUILD_EXEC   - Execute builder with environment
    0x0002  BUILD_ABORT  - Cancel current build
    0x0003  PING         - Health check

  Guest → Host:
    0x0101  BUILD_STDOUT - Builder stdout data
    0x0102  BUILD_STDERR - Builder stderr data  
    0x0103  BUILD_EXIT   - Build completed (exit code)
    0x0104  PONG         - Response to PING
    0x0105  WITNESS_EVENT - Filesystem/syscall event
```

### Build Flow

```
1. Collect Inputs
   ├── Resolve derivation inputs from both stores
   ├── Categorize: system store vs user store
   └── Calculate total size needed

2. Create Images
   ├── store.ext4: Input paths (read-only)
   │   └── Mounted via fuse2fs (no root needed)
   └── output.ext4: Build outputs (read-write)

3. Start VM
   ├── Generate Firecracker config JSON
   ├── Fork/exec firecracker process
   └── Wait for vsock socket to appear

4. Execute Build
   ├── Connect to guest via vsock
   ├── Send BUILD_EXEC with builder, args, env
   ├── Stream stdout/stderr back to host
   ├── Collect WITNESS_EVENTs
   └── Receive BUILD_EXIT with result

5. Extract Outputs
   ├── Mount output.ext4 via fuse2fs
   ├── Copy outputs to user store
   └── Unmount

6. Cleanup
   ├── Shutdown VM
   ├── Save witness record (optional)
   └── Remove work directory
```

## Installation

### Prerequisites

1. **KVM access** - Add user to kvm group:

   ```bash
   sudo usermod -aG kvm $USER
   # Log out and back in
   ```

2. **Firecracker binary** - Install from:

   - Package manager: `apt install firecracker`
   - GitHub releases: https://github.com/firecracker-microvm/firecracker/releases
   - Or set `NIX_FIRECRACKER_BIN` environment variable

3. **FUSE support** - For unprivileged image mounting:

   ```bash
   # Usually already available, but if not:
   sudo apt install fuse
   ```

### Setup Guest Components

Run the setup script to build and install the guest kernel and initrd:

```bash
# Install to user directory (recommended)
./scripts/setup-firecracker.sh --user

# Or install system-wide (requires sudo)
./scripts/setup-firecracker.sh --system

# Or via nix run
nix run .#setup-firecracker
```

This installs:

- `vmlinux` - Minimal Linux kernel for Firecracker
- `initrd.img` - Init system with nix-builder-init
- `bin/nix-builder-init` - Standalone init binary (for debugging)

### Verify Installation

```bash
# Check KVM access
ls -la /dev/kvm

# Check firecracker binary
which firecracker
firecracker --version

# Check guest components
ls -la ~/.local/share/nix/firecracker/
```

## Configuration

The `nix-embedded` binary uses Firecracker exclusively - there is no fallback to the traditional
nix-daemon. This is intentional: the embedded binary is designed for environments where the daemon
is unavailable or undesirable.

### Build Service Selection

Build service selection uses nix's standard `--builders` flag, not environment variables:

```bash
# Use embedded Firecracker (default for nix-embedded)
nix build --builders ''

# Use remote builders (standard nix behavior)
nix build --builders 'ssh://builder@host x86_64-linux'
```

When `--builders ''` is specified (empty string), nix invokes the `__build-remote` hook which uses
the embedded Firecracker VMM.

### Auto-Detection Paths

The service searches for components in these locations:

**Firecracker binary:**

1. `$NIX_FIRECRACKER_BIN` (if set)
2. `/usr/bin/firecracker`
3. `/usr/local/bin/firecracker`
4. `./result/bin/firecracker`

**Guest kernel:**

1. `$NIX_FIRECRACKER_KERNEL` (if set)
2. `./result/vmlinux`
3. `./.firecracker-guest/vmlinux`
4. `~/.local/share/nix/firecracker/vmlinux`
5. `~/.nix-firecracker/vmlinux`
6. `/nix/var/nix/firecracker/vmlinux`

**Guest initrd:**

1. `$NIX_FIRECRACKER_INITRD` (if set)
2. `./result/initrd.img`
3. `./.firecracker-guest/initrd.img`
4. `~/.local/share/nix/firecracker/initrd.img`
5. `~/.nix-firecracker/initrd.img`
6. `/nix/var/nix/firecracker/initrd.img`

## Usage

### Basic Usage

```bash
# Build with embedded Firecracker VMM (use empty --builders to trigger build hook)
nix build .#mypackage --builders ''

# Simple derivation example (verified working)
nix build --expr 'derivation { name = "hello"; builder = "/nix/store/.../bash"; 
    args = ["-c" "echo hello > $out"]; system = "x86_64-linux"; }' --builders ''
```

### Programmatic Usage

```cpp
#include "straylight/nix/build/build_service.h"

using namespace straylight::nix::build;

// Auto-detect best available service
auto svc = make_default_build_service();

// Or explicitly create firecracker service
auto fc_svc = make_firecracker_build_service();

// Or with explicit paths
auto fc_svc = make_firecracker_build_service(
    "/usr/bin/firecracker",
    "/path/to/vmlinux",
    "/path/to/initrd.img"
);

// Check availability
if (fc_svc->is_available()) {
    // Use for builds
    fc_svc->build_derivation(store, drv_path, drv, BuildMode::Normal);
}
```

## Deep Witnessing

The Firecracker build service records detailed information about what each build does, enabling
reproducibility verification and build attestation.

### Witness Record

Each build produces a witness record (JSON):

```json
{
  "drv_path": "/nix/store/abc123-foo.drv",
  "start_time": 1709472000,
  "end_time": 1709472060,
  "inputs_read": [
    "/nix/store/def456-gcc",
    "/nix/store/ghi789-glibc"
  ],
  "outputs_written": [
    "/nix/store/xyz000-foo"
  ],
  "syscall_counts": {
    "read": 10234,
    "write": 5678,
    "open": 1234
  },
  "network_attempts": [],
  "log_hash": "sha256:..."
}
```

### Witness Events

The guest init sends witness events for:

- `FILE_READ` - File read access
- `FILE_WRITE` - File write access
- `FILE_STAT` - File stat operations
- `NET_CONNECT` - Network connection attempts (should be empty!)
- `SYSCALL` - Syscall usage statistics

### Enabling Witness Recording

```cpp
// In firecracker_config:
config.enable_witness = true;
config.witness_log_dir = "/var/log/nix-witness/";
```

## Two-Tier Store Support

The Firecracker build service fully supports straylight's two-tier store architecture:

```
User Store:    ~/.local/share/nix/store/   (writable)
System Store:  /nix/store/                  (read-only fallback)
```

### How It Works

1. **Input Resolution**: When collecting build inputs, the service checks both stores and resolves
   each path to its actual location.

2. **Image Population**: All resolved inputs are copied into the store.ext4 image, preserving their
   original `/nix/store/...` paths.

3. **Output Extraction**: After a successful build, outputs are extracted from the VM and written to
   the user store.

4. **Overlay Filesystem**: Inside the VM, an overlay filesystem allows writes to `/nix/store` while
   keeping the base store read-only:

   ```
   /nix/store = overlay(
     lower=/dev/vda/nix/store,   # Input paths (read-only)
     upper=/output/nix/store,     # New outputs (read-write)
     work=/nix-work
   )
   ```

## Guest Init (nix-builder-init)

The guest init program runs as PID 1 inside the microVM. It's a minimal C program (~700 lines) that:

1. Sets up the filesystem hierarchy
2. Mounts the store and output block devices
3. Configures overlay for `/nix/store`
4. Connects to the host via vsock
5. Receives and executes BUILD_EXEC commands
6. Streams output back to the host
7. Reports build results

### Building Manually

```bash
# Build with musl for static linking
musl-gcc -static -O2 -o nix-builder-init nix-builder-init.c

# Or via nix
nix build .#nix-builder-init
```

### Debugging

The init binary is also installed standalone for debugging:

```bash
# Run in a test environment
~/.local/share/nix/firecracker/bin/nix-builder-init

# Or check the logs in a running VM
# (connect to serial console)
```

## Guest Kernel

The guest kernel is a minimal Linux kernel configured for Firecracker:

### Key Features Enabled

- `CONFIG_VIRTIO_*` - Virtio drivers for block, console
- `CONFIG_VSOCKETS` - vsock for host communication
- `CONFIG_EXT4_FS` - ext4 filesystem support
- `CONFIG_OVERLAY_FS` - Overlay filesystem for store
- `CONFIG_FUSE_FS` - FUSE support (optional)

### Key Features Disabled

- `CONFIG_NET` - No networking (builds are hermetic)
- `CONFIG_MODULES` - No loadable modules
- `CONFIG_DEBUG_*` - No debugging overhead
- `CONFIG_SOUND`, `CONFIG_USB` - No unnecessary drivers

### Building Manually

```bash
# Build kernel via nix
nix build .#firecracker-guest

# The kernel config is at:
# nix/vm/guest.nix (kernelConfig variable)
```

## Troubleshooting

### "KVM not accessible"

```bash
# Check if KVM is available
ls -la /dev/kvm

# Add user to kvm group
sudo usermod -aG kvm $USER

# Log out and back in, then verify
groups | grep kvm
```

### "Firecracker binary not found"

```bash
# Install firecracker
sudo apt install firecracker

# Or download from GitHub
wget https://github.com/firecracker-microvm/firecracker/releases/download/v1.5.0/firecracker-v1.5.0-x86_64.tgz

# Or set path explicitly
export NIX_FIRECRACKER_BIN=/path/to/firecracker
```

### "Guest kernel/initrd not found"

```bash
# Run setup script
./scripts/setup-firecracker.sh --user

# Or build manually
nix build .#firecracker-guest
export NIX_FIRECRACKER_KERNEL=./result/vmlinux
export NIX_FIRECRACKER_INITRD=./result/initrd.img
```

### "Failed to mount store image"

The service uses `fuse2fs` for unprivileged ext4 access. If this fails:

```bash
# Install e2fsprogs (provides fuse2fs)
sudo apt install e2fsprogs fuse

# Check FUSE is working
fusermount --version

# If fuse2fs not available, the service falls back to loop mount
# which requires root
```

### "Build timeout"

Default timeout is 1 hour. For long builds:

```cpp
// In firecracker_config:
config.build_timeout = std::chrono::hours(4);
```

### "VM exited immediately"

Check firecracker logs:

```bash
# Look in the work directory
ls /tmp/firecracker-build/*/

# Check VM config
cat /tmp/firecracker-build/*/vm-config.json

# Try running firecracker manually
firecracker --no-api --config-file /tmp/firecracker-build/*/vm-config.json
```

## Performance

### Typical Timings

| Operation | Time | |-----------|------| | VM boot | ~50-100ms | | vsock connect | ~10ms | | Image
creation (100MB) | ~500ms | | Small build | ~2-5s overhead |

### Optimization Tips

1. **Increase memory** for large builds:

   ```cpp
   config.mem_size_mib = 2048;  // 2GB
   ```

2. **Increase CPUs** for parallel builds:

   ```cpp
   config.vcpu_count = 4;
   ```

3. **Keep work directories** for debugging:

   ```cpp
   // In execute_in_vm(), don't delete work_dir
   ```

4. **Pre-warm images** by reusing store.ext4 for similar builds

## Security Considerations

### Isolation Guarantees

- Each build runs in a separate VM with its own kernel
- No network access (CONFIG_NET disabled for most protocols)
- Filesystem access limited to provided inputs
- Syscalls filtered to minimum required

### Trust Model

- Host trusts firecracker binary
- Host trusts guest kernel and initrd
- Guest trusts nothing - all inputs verified by Nix

### Attestation

Witness records can be signed and verified:

```json
{
  "witness": { ... },
  "signature": "...",
  "public_key": "..."
}
```

## Future Work

1. **virtiofs support** - More efficient than block devices for large stores
2. **GPU passthrough** - Via isospin's GPU broker for CUDA builds
3. **Distributed builds** - Integration with nativelink via REAPI
4. **Witness verification** - Automated reproducibility checking
5. **Caching** - Reuse VM images for similar builds

## Related Documentation

- [Two-Tier Store Architecture](./two-tier-store.md)
- [Build Service Interface](./build-service.md)
- [Isospin GPU Broker](../vendor/isospin/docs/gpu-passthrough.md)
- [Firecracker Documentation](https://github.com/firecracker-microvm/firecracker/blob/main/docs/getting-started.md)
