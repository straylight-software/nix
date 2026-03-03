# Firecracker GPU Passthrough Guide

This guide covers using Firecracker with NVIDIA GPU passthrough via VFIO. Isospin includes
a critical fix for the **30-second NVIDIA driver timeout** caused by LTR (Latency Tolerance
Reporting) capability.

## Quick Start

```bash
# From isospin repo root
cd /path/to/isospin

# 1. Enter dev shell and build
nix develop
buck2 build //firecracker/src/firecracker:firecracker

# 2. Run the demo (requires root, GPU bound to vfio-pci)
sudo ./scripts/demo-gpu.sh

# Or via nix app:
sudo nix run .#demo-gpu
```

## Prerequisites

### Hardware Requirements

- NVIDIA GPU (tested with RTX PRO 6000 Blackwell, should work with any)
- IOMMU-capable CPU (Intel VT-d or AMD-Vi)
- Separate display output (iGPU or second GPU) for host

### Software Requirements

- Linux kernel with VFIO support
- GPU and associated devices bound to `vfio-pci` driver
- VM assets (kernel, initramfs, rootfs with NVIDIA driver)

### NixOS Setup

See [nixos-vfio-setup.md](nixos-vfio-setup.md) for complete NixOS configuration.

Quick check:
```bash
# Verify GPU bound to vfio-pci
lspci -nnk -s 01:00
# Should show: Kernel driver in use: vfio-pci

# Verify VFIO group
ls -la /dev/vfio/
# Should show /dev/vfio/<group-number>
```

## The LTR Timeout Problem

### Symptom

Without the fix, NVIDIA driver initialization takes ~30 seconds with messages like:
```
NVRM: The NVIDIA GPU at PCI:0000:00:02.0 is not responding
NVRM: GPU at PCI:0000:00:02.0 has fallen off the bus
```

Eventually the driver loads, but boot is slow.

### Root Cause

Modern NVIDIA GPUs advertise **LTR (Latency Tolerance Reporting)** capability in their
PCIe DevCap2 register. The driver tries to enable LTR, but Firecracker's minimal ACPI
tables don't implement `_OSC` (Operating System Capabilities) to grant LTR permission.

The driver tries to enable LTR, waits for acknowledgment (which never comes), then times out.

### The Fix

Isospin masks the LTR and OBFF bits in the PCIe DevCap2 register when presenting the
device to the guest:

```
Original DevCap2: 0x70993  (LTR supported, OBFF supported)
Masked DevCap2:   0x30193  (LTR hidden, OBFF hidden)
```

The guest driver sees "LTR not supported" and skips the enable attempt entirely.

**Result:** Driver loads in ~2.5s instead of ~32s.

### Implementation Location

The fix is in `firecracker/src/vmm/src/pci/vfio/device.rs`:
- `find_pcie_capability()` - Locates PCIe cap in config space
- `config_space_read()` - Masks DevCap2 bits 11 (LTR) and 13-14 (OBFF)
- `config_space_write()` - Prevents guest from enabling LTR (DevCtl2 bit 10)

## VM Assets

### Required Files

Place these in `.vm-assets/`:

| File | Description |
|------|-------------|
| `vmlinux-<version>.bin` | Uncompressed Linux kernel matching host |
| `initramfs.cpio.gz` | Minimal initramfs with virtio drivers |
| `gpu-rootfs.ext4` | Root filesystem with NVIDIA driver |

### Creating VM Assets

```bash
# Option 1: Use the create-gpu-assets script
nix run .#create-gpu-assets

# Option 2: Manual creation (see scripts/create-gpu-image.sh)
```

### Kernel Requirements

The guest kernel must:
- Match host kernel version (for NVIDIA driver compatibility)
- Include virtio-blk, virtio-net, virtio-console drivers
- Include PCIe and VFIO support

### Rootfs Requirements

The rootfs must include:
- NVIDIA driver `.ko` files matching the kernel version
- `nvidia-smi` tool
- Basic userspace (busybox or full distro)

## Configuration

### VM Config Format

```json
{
  "boot-source": {
    "kernel_image_path": ".vm-assets/vmlinux-6.12.63.bin",
    "initrd_path": ".vm-assets/initramfs.cpio.gz",
    "boot_args": "console=ttyS0 reboot=k panic=1 pci=realloc init=/init"
  },
  "drives": [{
    "drive_id": "rootfs",
    "path_on_host": ".vm-assets/gpu-rootfs.ext4",
    "is_root_device": true,
    "is_read_only": false
  }],
  "machine-config": {
    "vcpu_count": 4,
    "mem_size_mib": 8192
  },
  "vfio": [{
    "id": "gpu0",
    "pci_address": "0000:01:00.0"
  }]
}
```

### Key Boot Arguments

| Argument | Purpose |
|----------|---------|
| `pci=realloc` | Allow kernel to reassign PCI BARs if needed |
| `pcie_aspm=off` | Disable PCIe power management (stability) |
| `init=/init` | Use initramfs init script |
| `console=ttyS0` | Serial console output |

### Memory Considerations

NVIDIA GPUs with large VRAM (like the 97GB RTX PRO 6000) require careful memory management:
- Guest needs enough RAM for driver allocations
- Use `mem_size_mib: 8192` (8GB) minimum for GPU workloads
- For training/inference, consider 16-32GB

## Running

### Direct Script

```bash
# Simple demo (boots, loads driver, runs nvidia-smi, exits)
sudo ./scripts/demo-gpu.sh

# With custom GPU address
GPU_ADDR=0000:41:00.0 sudo ./scripts/demo-gpu.sh
```

### Nix App

```bash
# From repo root
sudo nix run .#demo-gpu

# With options
sudo nix run .#demo-gpu -- --gpu 0000:41:00.0
sudo nix run .#demo-gpu -- --dir /path/to/isospin

# Help
nix run .#demo-gpu -- --help
```

### Manual

```bash
# Build
nix develop --command buck2 build //firecracker/src/firecracker:firecracker

# Create config
cat > /tmp/gpu-vm.json << 'EOF'
{
  "boot-source": {
    "kernel_image_path": ".vm-assets/vmlinux-6.12.63.bin",
    "initrd_path": ".vm-assets/initramfs.cpio.gz",
    "boot_args": "console=ttyS0 reboot=k panic=1 pci=realloc init=/init"
  },
  "drives": [{"drive_id": "rootfs", "path_on_host": ".vm-assets/gpu-rootfs.ext4", "is_root_device": true, "is_read_only": false}],
  "machine-config": {"vcpu_count": 4, "mem_size_mib": 8192},
  "vfio": [{"id": "gpu0", "pci_address": "0000:01:00.0"}]
}
EOF

# Run (no-seccomp required for VFIO ioctls)
sudo ./buck-out/.../firecracker --no-api --config-file /tmp/gpu-vm.json
```

## Performance Comparison

### Boot Timing (Cold boot to nvidia-smi)

| Event | Firecracker (with LTR fix) | Cloud Hypervisor |
|-------|---------------------------|------------------|
| PCI enumeration | 0.4s | 0.5s |
| Virtio-blk ready | 0.7s | 0.8s |
| Root FS mounted | 1.7s | 2.0s |
| `insmod nvidia.ko` | 1.8s | 2.1s |
| NVIDIA driver ready | 4.5s | 32.0s* |
| `nvidia-smi` completes | ~11s | ~40s* |

*Cloud Hypervisor with LTR enabled (without masking)

### Why Firecracker is Faster

1. **LTR Masking** - Eliminates 30s timeout
2. **Minimal VMM** - Less overhead than Cloud Hypervisor
3. **No full ACPI** - Simpler device model

## Firmware Pre-loading Optimization

The NVIDIA driver requires a 72MB GSP (GPU System Processor) firmware blob that must
be loaded from disk and copied to GPU VRAM before the GPU can operate. This can be
optimized by pre-loading the firmware into the initramfs.

### How It Works

**Standard boot path:**
1. Guest kernel boots
2. `nvidia.ko` calls `request_firmware("nvidia/580.95.05/gsp_ga10x.bin")`
3. Kernel reads 72MB from ext4 via virtio-blk (disk I/O)
4. Driver copies firmware to GPU VRAM
5. GSP boots

**Optimized boot path (firmware in initramfs):**
1. Initramfs loaded by kernel (already in guest RAM)
2. Init script copies firmware from initramfs to rootfs (RAM-to-RAM)
3. Guest kernel boots, mounts rootfs
4. `nvidia.ko` finds firmware already present (no disk I/O)
5. Driver copies firmware to GPU VRAM
6. GSP boots

### Timing Results

| Metric | Original (1.5MB initramfs) | FW-Preloaded (62MB initramfs) | Savings |
|--------|---------------------------|------------------------------|---------|
| `insmod nvidia.ko` time | ~1.7s | ~0.8s | **0.9s** |
| Total boot to nvidia-smi | ~15s | ~14s | **1s** |

### Using the Optimized Initramfs

```bash
# Test with firmware pre-loading
sudo ./scripts/gpu-fw-timing-test.sh

# The initramfs with firmware is at:
# .vm-assets/initramfs-with-fw.cpio.gz (62MB)
```

### Trade-offs

- **Pro:** ~1s faster boot by eliminating virtio-blk I/O for firmware
- **Con:** Larger initramfs (62MB vs 1.5MB)
- **Note:** The kernel decompresses initramfs directly into RAM, so the larger
  size doesn't significantly impact early boot

## Troubleshooting

### "GPU not responding"

```bash
# Check GPU is bound to vfio-pci
cat /sys/bus/pci/devices/0000:01:00.0/driver_override
# Should show: vfio-pci

# Check VFIO group
ls -la /sys/bus/pci/devices/0000:01:00.0/iommu_group/devices/
# All devices must be bound to vfio-pci
```

### "Cannot open /dev/vfio/N"

```bash
# Check VFIO device exists
ls -la /dev/vfio/

# Fix permissions (temporary)
sudo chmod 666 /dev/vfio/13

# Or run as root
sudo ./scripts/demo-gpu.sh
```

### "nvidia.ko: version magic mismatch"

Kernel/driver version mismatch. The guest kernel must match the NVIDIA driver version.

```bash
# Check host kernel version
uname -r

# Rebuild VM assets with matching versions
```

### "NVRM: kbifInitLtr_GB202: LTR is disabled"

This is **expected and correct**! It means the LTR masking is working:
```
NVRM: kbifInitLtr_GB202: LTR is disabled in the hierarchy
```

### Slow Boot (30+ seconds)

If driver still takes 30s, the LTR fix isn't being applied:
```bash
# Check for "Masked DevCap2" in boot log
dmesg | grep -i "masked\|devcap2\|ltr"

# Verify you're using the Isospin Firecracker build
./buck-out/.../firecracker --version
```

## Files Reference

### Scripts

| Script | Description |
|--------|-------------|
| `scripts/demo-gpu.sh` | Quick GPU demo |
| `scripts/gpu-timing-test.sh` | Compare FC vs CH timing |
| `scripts/vfio-bind.sh` | Bind GPU to vfio-pci |
| `scripts/vfio-unbind.sh` | Unbind GPU from vfio-pci |
| `scripts/vfio-status.sh` | Show IOMMU groups and bindings |

### Nix Apps

| App | Description |
|-----|-------------|
| `nix run .#demo-gpu` | GPU passthrough demo |
| `nix run .#gpu-timing-test` | Performance comparison |
| `nix run .#fc-gpu` | Full GPU VM launcher |

### Source Files (VFIO implementation)

| File | Purpose |
|------|---------|
| `firecracker/src/vmm/src/pci/vfio/device.rs` | VFIO device, LTR masking |
| `firecracker/src/vmm/src/pci/vfio/mmap.rs` | BAR memory mapping |
| `firecracker/src/vmm/src/pci/vfio/error.rs` | Error types |
| `firecracker/src/vmm/src/device_manager/pci_mngr.rs` | PCI device manager |

## See Also

- [gpu-passthrough-setup.md](gpu-passthrough-setup.md) - General GPU passthrough overview
- [nixos-vfio-setup.md](nixos-vfio-setup.md) - NixOS VFIO configuration
- [live-gpu-test-plan.md](live-gpu-test-plan.md) - Testing procedures
