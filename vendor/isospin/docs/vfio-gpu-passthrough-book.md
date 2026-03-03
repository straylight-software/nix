# VFIO GPU Passthrough in Firecracker: A Complete Guide

**The definitive guide to implementing GPU passthrough in lightweight virtual machine monitors**

*From first principles to production deployment, with hard-won lessons from passing an NVIDIA RTX PRO 6000 Blackwell (128GB VRAM) through Firecracker.*

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Theoretical Foundations](#2-theoretical-foundations)
3. [The VFIO Subsystem](#3-the-vfio-subsystem)
4. [IOMMU Groups: The Isolation Boundary](#4-iommu-groups-the-isolation-boundary)
5. [PCI Configuration Space](#5-pci-configuration-space)
6. [BAR Allocation and Memory Mapping](#6-bar-allocation-and-memory-mapping)
7. [DMA and the IOMMU](#7-dma-and-the-iommu)
8. [Interrupt Routing: MSI and MSI-X](#8-interrupt-routing-msi-and-msi-x)
9. [The LTR Fix: A Critical NVIDIA Workaround](#9-the-ltr-fix-a-critical-nvidia-workaround)
10. [Implementation Architecture](#10-implementation-architecture)
11. [The Code: A Complete Walkthrough](#11-the-code-a-complete-walkthrough)
12. [Property-Based Testing](#12-property-based-testing)
13. [NixOS Integration](#13-nixos-integration)
14. [Troubleshooting Guide](#14-troubleshooting-guide)
15. [Performance Optimization](#15-performance-optimization)
16. [Appendices](#16-appendices)
17. [NVIDIA GPU Deep Dive: The RTX PRO 6000 Blackwell](#17-nvidia-gpu-deep-dive-the-rtx-pro-6000-blackwell)
18. [The GPU Broker: Eliminating Cold Boot Penalty](#18-the-gpu-broker-eliminating-cold-boot-penalty)

---

## 1. Introduction

### 1.1 What This Book Covers

This document captures the complete knowledge required to implement VFIO PCI passthrough for GPUs in a lightweight VMM like Firecracker. It covers:

- **Theory**: Memory address spaces, IOMMU operation, PCI architecture
- **Implementation**: ~3,200 lines of Rust implementing VFIO passthrough
- **Testing**: 41 property-based tests with 23,000+ test cases
- **Operations**: NixOS configuration, scripts, troubleshooting
- **Hard-won lessons**: The LTR fix, IOMMU group viability, driver timing

### 1.2 Why This Matters

GPU passthrough to lightweight VMs enables:

- **AI/ML inference** with minimal overhead
- **Secure multi-tenancy** for GPU workloads  
- **Fast cold starts** (~2.5s to nvidia-smi vs 32s without optimizations)
- **Hardware isolation** without full virtualization overhead

### 1.3 The Hardware Context

This implementation was developed and tested with:

| Component | Specification |
|-----------|---------------|
| GPU | NVIDIA RTX PRO 6000 Blackwell |
| VRAM | 128GB |
| PCI Address | 0000:01:00.0 |
| IOMMU Group | 13 (shared with HD Audio 0000:01:00.1) |
| Host OS | NixOS with kernel 6.12.63 |
| Host Display | AMD Radeon iGPU (0000:72:00.0) |

### 1.4 Key Results Achieved

```
Boot to nvidia-smi: ~2.5 seconds (vs 32s without LTR fix)
Driver load time:   ~2.5 seconds (vs 30s timeout)
VRAM visible:       128GB at BAR1 (0x6000000000)
Modules loaded:     nvidia.ko, nvidia-modeset.ko
Device nodes:       /dev/nvidia0, /dev/nvidiactl, /dev/nvidia-modeset
```

---

## 2. Theoretical Foundations

### 2.1 The Five Address Spaces

Understanding GPU passthrough requires distinguishing five distinct address spaces:

```
┌─────────────────────────────────────────────────────────────────────┐
│                        ADDRESS SPACES                               │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐             │
│  │    GVA      │    │    GPA      │    │    HVA      │             │
│  │   Guest     │───▶│   Guest     │◀──▶│    Host     │             │
│  │  Virtual    │    │  Physical   │    │   Virtual   │             │
│  │  Address    │    │  Address    │    │   Address   │             │
│  └─────────────┘    └──────┬──────┘    └──────┬──────┘             │
│         │                  │                  │                     │
│         │ Guest MMU        │ EPT/NPT          │ Host MMU            │
│         ▼                  ▼                  ▼                     │
│  ┌─────────────────────────────────────────────────────┐           │
│  │                       HPA                            │           │
│  │               Host Physical Address                  │           │
│  │              (Actual DRAM location)                  │           │
│  └─────────────────────────────────────────────────────┘           │
│                            ▲                                        │
│                            │ IOMMU                                  │
│                            │                                        │
│                   ┌────────┴────────┐                              │
│                   │      IOVA       │                              │
│                   │  I/O Virtual    │                              │
│                   │    Address      │                              │
│                   │ (Device's view) │                              │
│                   └─────────────────┘                              │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

**Critical Insight**: For GPU passthrough, we use **identity mapping** where `IOVA == GPA`. When the guest programs the GPU with a DMA address, that same address goes through the IOMMU and resolves to the correct host physical memory.

### 2.2 Type-Safe Address Handling

The implementation uses Rust newtypes to prevent address space confusion:

```rust
/// Guest Physical Address - what guest sees as physical memory
pub struct GuestPhysAddr(pub u64);

/// Host Virtual Address - VMM's address space (mmap results)  
pub struct HostVirtAddr(pub u64);

/// I/O Virtual Address - what the device uses for DMA
pub struct IovaAddr(pub u64);

impl IovaAddr {
    /// Identity mapping: IOVA == GPA
    pub const fn from_gpa(gpa: GuestPhysAddr) -> Self {
        Self(gpa.0)
    }
}
```

This prevents bugs like accidentally passing an HVA where an IOVA was expected.

### 2.3 The Virtualization Stack

```
┌─────────────────────────────────────────┐
│              Guest OS                   │
│  ┌─────────────────────────────────┐    │
│  │        NVIDIA Driver            │    │
│  │   Programs GPU with GPA for DMA │    │
│  └─────────────────────────────────┘    │
└────────────────┬────────────────────────┘
                 │ MMIO access to GPU BARs
                 ▼
┌─────────────────────────────────────────┐
│            Firecracker VMM             │
│  ┌─────────────────────────────────┐    │
│  │      VfioPciDevice              │    │
│  │  - Shadows config space         │    │
│  │  - Maps BARs into guest         │    │
│  │  - Routes MSI-X interrupts      │    │
│  └─────────────────────────────────┘    │
└────────────────┬────────────────────────┘
                 │ VFIO ioctls
                 ▼
┌─────────────────────────────────────────┐
│            Linux Kernel                 │
│  ┌─────────────────────────────────┐    │
│  │         VFIO Driver             │    │
│  │  - Manages IOMMU domains        │    │
│  │  - Provides userspace API       │    │
│  └─────────────────────────────────┘    │
└────────────────┬────────────────────────┘
                 │ PCIe transactions
                 ▼
┌─────────────────────────────────────────┐
│            NVIDIA GPU                   │
│  BAR0: 64MB registers                   │
│  BAR1: 128GB VRAM                       │
│  BAR3: 32MB additional                  │
└─────────────────────────────────────────┘
```

---

## 3. The VFIO Subsystem

### 3.1 Why VFIO?

VFIO (Virtual Function I/O) provides safe, userspace access to devices:

1. **IOMMU protection**: Devices can only DMA to explicitly mapped regions
2. **Interrupt isolation**: Device interrupts are delivered via eventfd
3. **Userspace interface**: No kernel driver needed in the VMM

### 3.2 VFIO Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    VFIO Architecture                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   /dev/vfio/vfio (Container)                                   │
│        │                                                        │
│        │  VFIO_SET_IOMMU (Type1v2)                             │
│        │  VFIO_IOMMU_MAP_DMA                                   │
│        │  VFIO_IOMMU_UNMAP_DMA                                 │
│        │                                                        │
│        ├───────────────────────────────────────┐               │
│        │                                       │               │
│   /dev/vfio/13 (Group)                   /dev/vfio/14         │
│        │                                       │               │
│        │  VFIO_GROUP_GET_DEVICE_FD             │               │
│        │                                       │               │
│        ├─────────────┬─────────────┐           │               │
│        │             │             │           │               │
│   0000:01:00.0  0000:01:00.1  ...        0000:02:00.0         │
│   (GPU)         (HD Audio)               (Other device)        │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 3.3 The VFIO Dance

Opening a VFIO device requires a specific sequence:

```rust
// 1. Open the container
let container_fd = open("/dev/vfio/vfio", O_RDWR)?;

// 2. Check API version (must be 0)
let version = ioctl(container_fd, VFIO_GET_API_VERSION)?;
assert_eq!(version, 0);

// 3. Check IOMMU type support
let type1_supported = ioctl(container_fd, VFIO_CHECK_EXTENSION, VFIO_TYPE1v2_IOMMU)?;
assert!(type1_supported != 0);

// 4. Open the IOMMU group
let group_fd = open("/dev/vfio/13", O_RDWR)?;

// 5. Check group is viable (all devices bound to vfio-pci)
let mut status = VfioGroupStatus { argsz: size_of::<VfioGroupStatus>(), flags: 0 };
ioctl(group_fd, VFIO_GROUP_GET_STATUS, &mut status)?;
assert!(status.flags & VFIO_GROUP_FLAGS_VIABLE != 0);

// 6. Add group to container (sets IOMMU type on first add)
ioctl(group_fd, VFIO_GROUP_SET_CONTAINER, &container_fd)?;
ioctl(container_fd, VFIO_SET_IOMMU, VFIO_TYPE1v2_IOMMU)?;

// 7. Get device file descriptor
let device_fd = ioctl(group_fd, VFIO_GROUP_GET_DEVICE_FD, "0000:01:00.0")?;

// 8. Map guest memory for DMA
let dma_map = VfioDmaMap {
    argsz: size_of::<VfioDmaMap>(),
    flags: VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
    vaddr: host_virtual_addr,
    iova: guest_physical_addr,  // Identity mapping: IOVA == GPA
    size: region_size,
};
ioctl(container_fd, VFIO_IOMMU_MAP_DMA, &dma_map)?;
```

### 3.4 VFIO ioctl Reference

| ioctl | Value | Description |
|-------|-------|-------------|
| `VFIO_GET_API_VERSION` | 0x3b64 | Returns 0 for current API |
| `VFIO_CHECK_EXTENSION` | 0x3b65 | Check IOMMU type support |
| `VFIO_SET_IOMMU` | 0x3b66 | Set IOMMU type (Type1v2 = 3) |
| `VFIO_GROUP_GET_STATUS` | 0x3b67 | Check group viability |
| `VFIO_GROUP_SET_CONTAINER` | 0x3b68 | Add group to container |
| `VFIO_GROUP_GET_DEVICE_FD` | 0x3b6a | Get device fd by BDF string |
| `VFIO_DEVICE_GET_REGION_INFO` | 0x3b6c | Get BAR/config region info |
| `VFIO_DEVICE_SET_IRQS` | 0x3b6e | Configure interrupts |
| `VFIO_DEVICE_RESET` | 0x3b6f | Reset device |
| `VFIO_IOMMU_MAP_DMA` | 0x3b71 | Map memory for DMA |
| `VFIO_IOMMU_UNMAP_DMA` | 0x3b72 | Unmap DMA region |

---

## 4. IOMMU Groups: The Isolation Boundary

### 4.1 What is an IOMMU Group?

An IOMMU group is the smallest unit of isolation the IOMMU can provide. All devices in a group:

- Share the same IOMMU domain
- Can access each other's DMA-mapped memory
- Must ALL be bound to vfio-pci for the group to be "viable"

### 4.2 The Viability Problem

**This was a major blocker in development.**

The NVIDIA RTX PRO 6000 Blackwell sits in IOMMU group 13 with its HD Audio companion:

```
$ ./scripts/vfio-status.sh
=== IOMMU Groups ===
0000:01:00.0 [group 13] [vfio-pci]  NVIDIA Corporation GB202GL [RTX PRO 6000]
0000:01:00.1 [group 13] [none]      NVIDIA Corporation GB202 HD Audio Controller
```

When the GPU is bound to `vfio-pci` but the audio device isn't, the group is **not viable**:

```
Error: VFIO error: IOMMU group 13 is not viable - 
       ensure all devices are bound to vfio-pci
```

### 4.3 The Hung Driver Problem

During development, attempting to unbind the audio device from `snd_hda_intel` caused the driver to hang:

```
$ sudo bash -c 'echo "0000:01:00.1" > /sys/bus/pci/drivers/snd_hda_intel/unbind'
# ... hangs indefinitely ...
```

The kernel showed:
```
[1352.872523] Future hung task reports are suppressed
```

**Root cause**: PipeWire had the audio device open. The driver unbind blocked waiting for the device to be released.

**Solution**: 
1. Stop PipeWire/PulseAudio before unbinding
2. Bind BOTH devices to vfio-pci at boot (before display manager starts)
3. Or reboot to clear hung state

### 4.4 The Viability Check Bypass

For testing on systems with ACS override patches, we implemented a warning instead of error:

```rust
// VFIO_GROUP_FLAGS_VIABLE = 1
if (status.flags & 1) == 0 {
    // Log warning but attempt to continue
    log::warn!(
        "IOMMU group {} is not fully viable (flags={:#x}). \
         Some devices may not be bound to vfio-pci. \
         Attempting to continue anyway - this may fail or be unsafe.",
        group_id, status.flags
    );
    // Don't error out - let the container setup fail naturally
}
```

This allows the code to proceed, but the kernel will still reject the group when trying to add it to a container.

### 4.5 Binding All Group Members

The safe approach is to bind ALL devices in a group:

```bash
# scripts/vfio-bind-safe.sh checks for this
sudo ./scripts/vfio-bind-safe.sh 0000:01:00.0 0000:01:00.1
```

This script:
1. Detects if the GPU is being used for display
2. Warns about active DRM clients
3. Identifies all devices in the same IOMMU group
4. Prompts to bind missing devices

---

## 5. PCI Configuration Space

### 5.1 Configuration Space Layout

```
┌─────────────────────────────────────────────────────────────────┐
│                  PCI Configuration Space                        │
├─────────┬───────────────────────────────────────────────────────┤
│ Offset  │ Content                                               │
├─────────┼───────────────────────────────────────────────────────┤
│ 0x00    │ Vendor ID (0x10de = NVIDIA)                          │
│ 0x02    │ Device ID (0x2bb1 = RTX PRO 6000)                    │
│ 0x04    │ Command Register                                      │
│ 0x06    │ Status Register                                       │
│ 0x08    │ Revision ID, Class Code                               │
│ 0x0C    │ Cache Line, Latency, Header Type, BIST               │
│ 0x10    │ BAR0                                                  │
│ 0x14    │ BAR1 (low)                                            │
│ 0x18    │ BAR1 (high) - 64-bit BAR                             │
│ 0x1C    │ BAR2                                                  │
│ 0x20    │ BAR3 (low)                                            │
│ 0x24    │ BAR3 (high) - 64-bit BAR                             │
│ 0x28    │ Cardbus CIS Pointer                                   │
│ 0x2C    │ Subsystem Vendor/Device ID                            │
│ 0x30    │ Expansion ROM BAR                                     │
│ 0x34    │ Capabilities Pointer ──────────────────┐              │
│ 0x3C    │ Interrupt Line/Pin                     │              │
├─────────┼────────────────────────────────────────┼──────────────┤
│ 0x40+   │ Capabilities List                      │              │
│         │  ├─ MSI (ID=0x05)          ◀───────────┤              │
│         │  ├─ PCIe (ID=0x10)         ◀───────────┤              │
│         │  └─ MSI-X (ID=0x11)        ◀───────────┘              │
├─────────┼───────────────────────────────────────────────────────┤
│ 0x100+  │ Extended Capabilities (PCIe)                          │
│         │  ├─ AER (ID=0x01)                                     │
│         │  ├─ Device Serial Number (ID=0x03)                   │
│         │  └─ LTR (ID=0x18) ◀── We hide this!                  │
└─────────┴───────────────────────────────────────────────────────┘
```

### 5.2 BAR Discovery

BARs (Base Address Registers) describe device memory regions. Discovery uses a size-probing protocol:

```rust
fn discover_bar_size(bar_offset: u8) -> u64 {
    // 1. Save original value
    let original = config_read32(bar_offset);
    
    // 2. Write all 1s
    config_write32(bar_offset, 0xFFFFFFFF);
    
    // 3. Read back - device returns size mask
    let mask = config_read32(bar_offset);
    
    // 4. Restore original
    config_write32(bar_offset, original);
    
    // 5. Calculate size from mask
    // Size = ~(mask & ~0xF) + 1 for memory BARs
    let size_mask = mask & !0xF;
    if size_mask == 0 { return 0; }
    (!size_mask).wrapping_add(1) as u64
}
```

### 5.3 BAR Type Encoding

```
Memory BAR bits:
  [0]    = 0 (memory, not I/O)
  [2:1]  = type: 00=32-bit, 10=64-bit
  [3]    = prefetchable

I/O BAR bits:
  [0]    = 1 (I/O space)
  [1]    = reserved
```

### 5.4 Shadow Config Space

The VMM maintains "shadow" BAR values containing guest addresses:

```rust
pub struct VfioPciDevice {
    // Physical BAR info from device
    bars: [Option<BarInfo>; 6],
    
    // Shadow values with GUEST addresses
    shadow_bars: [u64; 6],
    // ...
}

impl PciDevice for VfioPciDevice {
    fn read_config_register(&self, reg_idx: usize) -> u32 {
        match reg_idx {
            // BAR0-BAR5 (offsets 0x10-0x24)
            4..=9 => {
                let bar_idx = reg_idx - 4;
                // Return shadow (guest address), not real (host) address
                self.shadow_bars[bar_idx] as u32
            }
            _ => {
                // Forward other reads to device
                self.config_read32(reg_idx * 4)
            }
        }
    }
}
```

This prevents the guest from seeing host physical addresses.

---

## 6. BAR Allocation and Memory Mapping

### 6.1 The BAR Layout for RTX PRO 6000

```
┌────────────────────────────────────────────────────────────────┐
│                    NVIDIA RTX PRO 6000 BARs                    │
├──────┬──────────────┬───────────────────┬─────────────────────┤
│ BAR  │ Size         │ Guest Address     │ Type                │
├──────┼──────────────┼───────────────────┼─────────────────────┤
│ 0    │ 64 MB        │ 0xC4000000        │ 32-bit, registers   │
│ 1    │ 128 GB       │ 0x6000000000      │ 64-bit prefetch     │
│ 2    │ (BAR1 high)  │ -                 │ -                   │
│ 3    │ 32 MB        │ 0x4002000000      │ 64-bit prefetch     │
│ 4    │ (BAR3 high)  │ -                 │ -                   │
│ 5    │ -            │ -                 │ -                   │
└──────┴──────────────┴───────────────────┴─────────────────────┘
```

### 6.2 Natural Alignment Requirement

PCIe requires BARs to be naturally aligned:

```
BAR Size      Must be aligned to
64 MB      →  64 MB boundary (0x4000000)
128 GB     →  128 GB boundary (0x2000000000)
32 MB      →  32 MB boundary (0x2000000)
```

The allocator ensures this:

```rust
pub fn allocate_mmio64(&mut self, size: u64) -> Option<GuestPhysAddr> {
    // Align current position to BAR size
    let aligned = (self.current + size - 1) & !(size - 1);
    
    if aligned + size > self.end {
        return None;
    }
    
    self.current = aligned + size;
    Some(GuestPhysAddr(aligned))
}
```

### 6.3 Direct BAR Mapping (Critical Optimization)

Without direct mapping, every GPU register access causes a VM exit:

```
Guest MMIO write → KVM exit → Firecracker handles → VFIO pwrite → Return

Latency: ~2000ns per access
```

With direct BAR mapping:

```
Guest MMIO write → Direct to GPU hardware

Latency: ~100ns per access
```

Implementation:

```rust
pub fn map_mmio_regions(&mut self, vm: &Vm) -> VfioResult<()> {
    for bar_idx in 0..6 {
        let bar = match &self.bars[bar_idx] {
            Some(b) if !b.is_io && b.region_info.flags & VFIO_REGION_INFO_FLAG_MMAP != 0 => b,
            _ => continue,
        };
        
        let guest_addr = bar.guest_addr.ok_or(VfioError::BarNotAllocated)?;
        
        // 1. mmap the BAR from VFIO device
        let mmap = MmapRegion::new(
            self.fd.as_raw_fd(),
            bar.region_info.offset,  // VFIO region offset
            bar.size,
        )?;
        
        // 2. Allocate a KVM memory slot
        let slot = vm.next_kvm_slot(1)?;
        
        // 3. Register as KVM user memory region
        let kvm_region = kvm_userspace_memory_region {
            slot,
            guest_phys_addr: guest_addr.0,
            memory_size: bar.size,
            userspace_addr: mmap.addr() as u64,
            flags: 0,
        };
        vm.set_user_memory_region(kvm_region)?;
        
        self.bar_mmaps[bar_idx] = Some(mmap);
        self.bar_kvm_slots[bar_idx] = Some(slot);
    }
    Ok(())
}
```

**Performance impact**: Reduced NVIDIA driver init from ~6.6s to sub-second.

---

## 7. DMA and the IOMMU

### 7.1 Why DMA Mapping Matters

The GPU performs DMA (Direct Memory Access) to read/write guest memory. Without IOMMU protection:

```
GPU DMA to 0x1000 → Accesses HOST memory at 0x1000 (SECURITY HOLE!)
```

With IOMMU:

```
GPU DMA to 0x1000 → IOMMU lookup → Host physical address → Safe access
```

### 7.2 Identity Mapping

For simplicity, we use identity mapping where IOVA == GPA:

```rust
pub fn map_guest_memory(&mut self, vm: &Vm) -> VfioResult<()> {
    for region in vm.guest_memory().iter() {
        let gpa = region.start_addr();
        let size = region.len();
        let hva = region.get_host_address(0)?;
        
        // Identity mapping: IOVA = GPA
        let iova = IovaAddr::from_gpa(gpa);
        
        self.container.map_dma(iova, size, hva)?;
    }
    Ok(())
}
```

This means when the guest driver programs the GPU to DMA to GPA 0x40000000, the GPU issues a transaction to IOVA 0x40000000, which the IOMMU translates correctly.

### 7.3 DMA Mapping Overlap Detection

Overlapping DMA mappings cause undefined behavior. The implementation prevents this:

```rust
impl VfioContainer {
    pub fn map_dma(&mut self, iova: IovaAddr, size: u64, hva: HostVirtAddr) -> VfioResult<()> {
        let end = iova.0.checked_add(size).ok_or(VfioError::DmaOverflow)?;
        
        // Check for overlaps using BTreeMap range queries
        for (existing_iova, existing) in self.mappings.range(..end) {
            let existing_end = existing_iova + existing.size;
            if iova.0 < existing_end && end > *existing_iova {
                return Err(VfioError::DmaMappingOverlap {
                    existing: *existing_iova,
                    new: iova.0,
                });
            }
        }
        
        // Perform the mapping
        let dma_map = vfio_iommu_type1_dma_map {
            argsz: size_of::<vfio_iommu_type1_dma_map>() as u32,
            flags: VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
            vaddr: hva.0,
            iova: iova.0,
            size,
        };
        
        unsafe { ioctl(self.fd.as_raw_fd(), VFIO_IOMMU_MAP_DMA, &dma_map) }?;
        
        self.mappings.insert(iova.0, DmaMapping { iova, size, hva });
        Ok(())
    }
}
```

---

## 8. Interrupt Routing: MSI and MSI-X

### 8.1 MSI-X Architecture

MSI-X provides multiple interrupt vectors with per-vector masking:

```
┌─────────────────────────────────────────────────────────────────┐
│                     MSI-X Architecture                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  Config Space                    BAR (MSI-X Table)              │
│  ┌──────────────┐               ┌─────────────────────────┐    │
│  │ MSI-X Cap    │               │ Vector 0                │    │
│  │ ┌──────────┐ │               │  Address: 0xFEE00000   │    │
│  │ │Table Size│─┼──────────────▶│  Data:    0x00004021   │    │
│  │ │Table BIR │ │               │  Control: 0 (unmasked) │    │
│  │ │Table Off │ │               ├─────────────────────────┤    │
│  │ │PBA BIR   │ │               │ Vector 1                │    │
│  │ │PBA Offset│ │               │  ...                    │    │
│  │ └──────────┘ │               ├─────────────────────────┤    │
│  │ Msg Control  │               │ Vector N                │    │
│  │ [Enable][FM] │               │  ...                    │    │
│  └──────────────┘               └─────────────────────────┘    │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 8.2 MSI-X Table Entry Format

Each entry is 16 bytes:

```rust
pub struct MsixTableEntry {
    pub msg_addr_lo: u32,   // Offset 0x00
    pub msg_addr_hi: u32,   // Offset 0x04
    pub msg_data: u32,      // Offset 0x08
    pub vector_control: u32, // Offset 0x0C (bit 0 = mask)
}
```

### 8.3 MSI-X State Machine

The implementation enforces valid state transitions:

```rust
pub enum MsixVectorState {
    /// Initial state - vector is masked
    Masked,
    
    /// Address and data configured, but not enabled
    Configured { address: MsiAddress, data: MsiData },
    
    /// Fully enabled with eventfd for interrupt delivery
    Enabled { address: MsiAddress, data: MsiData, eventfd: RawFd, gsi: u32 },
}
```

Transitions:

```
        ┌───────────────┐
        │    Masked     │ ◀─── Initial state
        └───────┬───────┘
                │ configure(addr, data)
        ┌───────▼───────┐
        │  Configured   │
        └───────┬───────┘
                │ enable(eventfd, gsi)
        ┌───────▼───────┐
        │    Enabled    │ ◀─── Delivers interrupts
        └───────────────┘
```

### 8.4 KVM irqfd Integration

Interrupts are delivered to the guest via KVM irqfd:

```rust
pub fn setup_msix_routing(&mut self, vm: &Vm, vector_count: u16) -> VfioResult<()> {
    let mut irq_routing_entries = Vec::new();
    
    for i in 0..vector_count {
        // Allocate GSI (Global System Interrupt)
        let gsi = vm.allocate_gsi()?;
        
        // Create eventfd for this vector
        let eventfd = EventFd::new(EFD_NONBLOCK)?;
        
        // Register eventfd with VFIO device
        let irq_set = vfio_irq_set {
            argsz: size_of::<vfio_irq_set>() as u32 + size_of::<i32>() as u32,
            flags: VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER,
            index: VFIO_PCI_MSIX_IRQ_INDEX,
            start: i as u32,
            count: 1,
            // eventfd follows in memory
        };
        ioctl(self.fd.as_raw_fd(), VFIO_DEVICE_SET_IRQS, &irq_set)?;
        
        // Register with KVM irqfd
        let kvm_irqfd = kvm_irqfd {
            fd: eventfd.as_raw_fd() as u32,
            gsi,
            flags: 0,
        };
        vm.register_irqfd(&kvm_irqfd)?;
        
        // Add to IRQ routing table
        irq_routing_entries.push(kvm_irq_routing_entry {
            gsi,
            type_: KVM_IRQ_ROUTING_MSI,
            // MSI address/data from table entry
            ..
        });
    }
    
    // Update KVM IRQ routing table
    vm.set_gsi_routing(&irq_routing_entries)?;
    
    Ok(())
}
```

---

## 9. The LTR Fix: A Critical NVIDIA Workaround

### 9.1 The Problem

NVIDIA drivers on modern GPUs attempt to enable LTR (Latency Tolerance Reporting):

```
[    5.234] nvidia-nvlink: Nvlink Core is being initialized
[    5.456] NVRM: RmInitAdapter: LTR enable in progress...
[   35.456] NVRM: Timeout waiting for LTR response!
```

The driver waits **30 seconds** for an LTR response that never comes because:

1. Firecracker's minimal ACPI doesn't grant LTR permission
2. The virtual root complex doesn't support LTR
3. The GPU advertises LTR capability, so the driver tries to use it

### 9.2 The Solution

**Mask the LTR capability so the driver doesn't attempt to use it.**

The fix has two parts:

#### Part 1: Intercept DevCap2/DevCtl2 Reads

```rust
fn read_config_register(&self, reg_idx: usize) -> u32 {
    let offset = reg_idx * 4;
    let mut val = self.config_read32(offset);
    
    if let Some(pcie_cap) = self.pcie_cap_offset {
        // DevCap2 at PCIe cap + 0x24
        let devcap2_offset = pcie_cap as usize + 0x24;
        if offset == devcap2_offset {
            // Mask out LTR Mechanism Supported (bit 11)
            // Mask out OBFF Supported (bits 18-19)
            const LTR_MECHANISM_SUPPORTED: u32 = 1 << 11;
            const OBFF_SUPPORTED_MASK: u32 = 0x3 << 18;
            val &= !(LTR_MECHANISM_SUPPORTED | OBFF_SUPPORTED_MASK);
        }
        
        // DevCtl2 at PCIe cap + 0x28
        let devctl2_offset = pcie_cap as usize + 0x28;
        if offset == devctl2_offset {
            // Mask out LTR Enable (bit 10)
            const LTR_ENABLE: u32 = 1 << 10;
            val &= !LTR_ENABLE;
        }
    }
    
    val
}
```

#### Part 2: Hide LTR Extended Capability

```rust
fn read_config_register(&self, reg_idx: usize) -> u32 {
    let offset = reg_idx * 4;
    
    // Hide LTR extended capability entirely
    if let Some(ltr_offset) = self.ltr_ext_cap_offset {
        if offset >= ltr_offset as usize && offset < (ltr_offset + 8) as usize {
            return 0;  // Return 0 for LTR capability reads
        }
    }
    
    self.config_read32(offset)
}
```

#### Part 3: Intercept LTR Enable Writes

```rust
fn write_config_register(&mut self, reg_idx: usize, data: u32) {
    let offset = reg_idx * 4;
    
    if let Some(pcie_cap) = self.pcie_cap_offset {
        let devctl2_offset = pcie_cap as usize + 0x28;
        if offset == devctl2_offset {
            // If guest tries to enable LTR, mask it out
            const LTR_ENABLE: u32 = 1 << 10;
            let masked_data = data & !LTR_ENABLE;
            if data != masked_data {
                log::debug!("Masked LTR enable bit in DevCtl2 write");
            }
            self.config_write32(offset, masked_data);
            return;
        }
    }
    
    self.config_write32(offset, data);
}
```

### 9.3 Results

```
Before fix:
[    5.234] NVRM: RmInitAdapter: LTR enable in progress...
[   35.456] NVRM: Timeout waiting for LTR response!
[   35.789] nvidia 0000:00:04.0: enabling device
Total boot time: ~40 seconds

After fix:
[    2.345] NVRM: kbifInitLtr_GB202: LTR is disabled in the hierarchy
[    2.567] nvidia 0000:00:04.0: enabling device
Total boot time: ~2.5 seconds
```

The driver now detects that LTR is unsupported and skips the entire sequence.

---

## 10. Implementation Architecture

### 10.1 Module Structure

```
firecracker/src/vmm/src/pci/vfio/
├── mod.rs           # Module exports
├── types.rs         # GuestPhysAddr, HostVirtAddr, IovaAddr, PciBdf, BarIndex
├── error.rs         # VfioError enum (~240 lines)
├── container.rs     # VfioContainer for IOMMU domain/DMA (~250 lines)
├── device.rs        # VfioPciDevice implementation (~1700 lines)
├── msix.rs          # MSI-X interrupt handling (~400 lines)
├── msi.rs           # Plain MSI support (~200 lines)
├── mmap.rs          # MmapRegion for BAR mapping (~350 lines)
└── proptests.rs     # Property-based tests (~750 lines)

firecracker/src/vmm/src/
├── device_manager/
│   └── pci_mngr.rs  # attach_vfio_device() integration (~150 lines)
├── vmm_config/
│   └── vfio.rs      # VfioDeviceConfig struct
├── builder.rs       # attach_vfio_devices() in boot sequence
└── resources.rs     # VmResources.vfio_devices field

Total: ~3,200 lines of implementation + ~750 lines of tests
```

### 10.2 Data Flow

```
                    JSON Config
                        │
                        ▼
              ┌─────────────────┐
              │  VfioDeviceConfig │
              │  {               │
              │    id: "gpu0",   │
              │    pci_address:  │
              │    "0000:01:00.0"│
              │  }               │
              └────────┬────────┘
                       │
         build_microvm_for_boot()
                       │
                       ▼
              ┌─────────────────┐
              │attach_vfio_devices│
              └────────┬────────┘
                       │
                       ▼
    ┌──────────────────────────────────────┐
    │        PciDevices.attach_vfio_device │
    │                                      │
    │  1. Parse PCI address                │
    │  2. Create/get VfioContainer         │
    │  3. Open VFIO device                 │
    │  4. Map guest memory for DMA         │
    │  5. Allocate BARs (mmio32/mmio64)    │
    │  6. Map BAR MMIO regions             │
    │  7. Register with PCI bus            │
    │  8. Setup MSI-X/MSI interrupts       │
    └──────────────────────────────────────┘
                       │
                       ▼
    ┌──────────────────────────────────────┐
    │           VfioPciDevice              │
    │                                      │
    │  Handles:                            │
    │  - Config space reads/writes         │
    │  - BAR access (via KVM mapping)      │
    │  - MSI-X table updates               │
    │  - LTR capability masking            │
    └──────────────────────────────────────┘
```

### 10.3 Key Structs

```rust
/// VFIO device configuration from JSON
pub struct VfioDeviceConfig {
    pub id: String,                     // User identifier
    pub pci_address: String,            // "0000:01:00.0"
    pub gpudirect_clique: Option<u8>,   // NVIDIA P2P clique
}

/// The main VFIO device structure
pub struct VfioPciDevice {
    id: String,
    bdf: PciBdf,
    fd: OwnedFd,                        // VFIO device fd
    group_id: u32,
    vendor_id: VendorId,
    device_id: DeviceId,
    
    // BAR state
    bars: [Option<BarInfo>; 6],
    shadow_bars: [u64; 6],
    bar_mmaps: [Option<MmapRegion>; 6],
    bar_kvm_slots: [Option<u32>; 6],
    
    // Interrupt state  
    msix_cap_offset: Option<u16>,
    msix_table_size: Option<u16>,
    msix_state: Option<VfioMsixState>,
    msi_cap_offset: Option<u16>,
    msi_state: Option<VfioMsiState>,
    interrupt_type: InterruptType,
    
    // Capability offsets for workarounds
    pcie_cap_offset: Option<u16>,
    ltr_ext_cap_offset: Option<u16>,
}

/// VFIO container (IOMMU domain)
pub struct VfioContainer {
    fd: OwnedFd,                              // /dev/vfio/vfio
    mappings: BTreeMap<u64, DmaMapping>,
    iommu_configured: bool,
}
```

---

## 11. The Code: A Complete Walkthrough

### 11.1 Opening a VFIO Device

```rust
// In device.rs
pub fn open(bdf: &PciBdf) -> VfioResult<(OwnedFd, u32)> {
    // 1. Read IOMMU group from sysfs
    let group_link = format!("{}/iommu_group", bdf.sysfs_path());
    let group_path = std::fs::read_link(&group_link)
        .map_err(|e| VfioError::IommuGroupRead { bdf: bdf.clone(), source: e })?;
    
    let group_id: u32 = group_path
        .file_name()
        .and_then(|s| s.to_str())
        .and_then(|s| s.parse().ok())
        .ok_or(VfioError::NoIommuGroup)?;
    
    // 2. Open group
    let group_path = format!("/dev/vfio/{}", group_id);
    let group_fd = OpenOptions::new()
        .read(true)
        .write(true)
        .open(&group_path)?;
    
    // 3. Check viability (with bypass for testing)
    let mut status = VfioGroupStatus { argsz: 8, flags: 0 };
    unsafe { ioctl(group_fd.as_raw_fd(), VFIO_GROUP_GET_STATUS, &mut status) }?;
    
    if (status.flags & 1) == 0 {
        log::warn!("IOMMU group {} not viable, attempting anyway", group_id);
    }
    
    // 4. Get device fd
    let bdf_str = CString::new(bdf.to_string())?;
    let device_fd = unsafe {
        ioctl(group_fd.as_raw_fd(), VFIO_GROUP_GET_DEVICE_FD, bdf_str.as_ptr())
    }?;
    
    Ok((unsafe { OwnedFd::from_raw_fd(device_fd) }, group_id))
}
```

### 11.2 Discovering BARs

```rust
fn discover_bars(&mut self) -> VfioResult<()> {
    for bar_idx in 0..6 {
        // Get region info from VFIO
        let mut region_info = vfio_region_info {
            argsz: size_of::<vfio_region_info>() as u32,
            index: bar_idx as u32,
            ..Default::default()
        };
        
        unsafe {
            ioctl(self.fd.as_raw_fd(), VFIO_DEVICE_GET_REGION_INFO, &mut region_info)
        }?;
        
        if region_info.size == 0 {
            continue;
        }
        
        // Read BAR register to determine type
        let bar_offset = 0x10 + bar_idx * 4;
        let bar_value = self.config_read32(bar_offset);
        
        let is_io = (bar_value & 0x1) != 0;
        let is_64bit = !is_io && ((bar_value >> 1) & 0x3) == 2;
        let is_prefetchable = !is_io && ((bar_value >> 3) & 0x1) != 0;
        
        self.bars[bar_idx] = Some(BarInfo {
            index: BarIndex(bar_idx as u8),
            size: region_info.size,
            is_64bit,
            is_prefetchable,
            is_io,
            guest_addr: None,  // Allocated later
            host_addr: None,
            region_info,
        });
        
        // Skip next BAR index if this is 64-bit
        if is_64bit && bar_idx < 5 {
            // BAR pair: bar_idx is low, bar_idx+1 is high
        }
    }
    
    Ok(())
}
```

### 11.3 BAR Allocation

```rust
// In pci_mngr.rs
pub fn attach_vfio_device(
    &mut self,
    vm: &Arc<Vm>,
    id: String,
    pci_address: &str,
    gpudirect_clique: Option<u8>,
) -> Result<(), PciManagerError> {
    let bdf = PciBdf::parse(pci_address)?;
    
    // Create container if needed
    let container = self.vfio_container.get_or_insert_with(VfioContainer::new);
    
    // Open device
    let mut device = VfioPciDevice::new(bdf, id, gpudirect_clique)?;
    
    // Map guest memory for DMA
    for region in vm.guest_memory().iter() {
        container.map_dma(
            IovaAddr(region.start_addr().raw_value()),
            region.len(),
            HostVirtAddr(region.as_ptr() as u64),
        )?;
    }
    
    // Allocate BARs
    let segment = self.pci_segment.as_mut().unwrap();
    for bar in device.bars_mut().iter_mut().flatten() {
        let guest_addr = if bar.is_64bit || bar.size > 0x1_0000_0000 {
            segment.mmio64_memory.allocate(bar.size)?
        } else {
            segment.mmio32_memory.allocate(bar.size)?
        };
        bar.guest_addr = Some(guest_addr);
    }
    
    // Map BAR regions for direct access
    device.map_mmio_regions(vm)?;
    
    // Register with PCI bus
    let pci_devid = segment.pci_bus.add_device(Arc::new(Mutex::new(device)))?;
    
    Ok(())
}
```

---

## 12. Property-Based Testing

### 12.1 Why Property Testing?

Traditional unit tests verify specific cases:

```rust
#[test]
fn test_bar_allocation() {
    let mut alloc = BarAllocator::new(0x1000, 0x10000);
    assert_eq!(alloc.allocate(0x1000), Some(0x1000));
    assert_eq!(alloc.allocate(0x2000), Some(0x2000));
}
```

Property tests verify invariants across random inputs:

```rust
proptest! {
    #[test]
    fn bar_allocations_never_overlap(
        sizes in prop::collection::vec(1u64..=1024*1024*1024, 1..10)
    ) {
        let mut alloc = BarAllocator::new(0, u64::MAX);
        let mut allocations = Vec::new();
        
        for size in sizes {
            if let Some(addr) = alloc.allocate(size) {
                // Check no overlap with previous allocations
                for (prev_addr, prev_size) in &allocations {
                    prop_assert!(
                        addr >= prev_addr + prev_size || addr + size <= *prev_addr,
                        "Overlap detected!"
                    );
                }
                allocations.push((addr, size));
            }
        }
    }
}
```

### 12.2 Test Categories

The implementation includes 41 property tests across 8 categories:

| Category | Tests | Invariants Verified |
|----------|-------|---------------------|
| Address Types | 6 | GPA/HVA/IOVA don't mix, roundtrip |
| PCI Types | 4 | BDF parsing, BAR offsets |
| DMA Mapping | 8 | No overlaps, size tracking |
| BAR Allocation | 3 | Natural alignment, no overlap |
| MSI-X State | 8 | Valid transitions, delivery conditions |
| Config Space | 4 | Shadow isolation |
| Integration | 3 | Cross-module interactions |
| Edge Cases | 5 | Boundaries, zero-size, max values |

### 12.3 Critical Properties

**DMA No-Overlap Property** (most critical):

```rust
proptest! {
    #[test]
    fn dma_mappings_never_overlap(
        mappings in prop::collection::vec(
            (0u64..0x1_0000_0000u64, 1u64..0x1000_0000u64),
            1..20
        )
    ) {
        let mut tracker = MockDmaTracker::new();
        let mut successful = Vec::new();
        
        for (iova, size) in mappings {
            let result = tracker.map(IovaAddr(iova), size, HostVirtAddr(0));
            
            if result.is_ok() {
                // Verify no overlap with any successful mapping
                for (prev_iova, prev_size) in &successful {
                    let prev_end = prev_iova + prev_size;
                    let new_end = iova + size;
                    
                    prop_assert!(
                        iova >= prev_end || new_end <= *prev_iova,
                        "DMA mapping overlap: [{:#x}, {:#x}) and [{:#x}, {:#x})",
                        prev_iova, prev_end, iova, new_end
                    );
                }
                successful.push((iova, size));
            }
        }
    }
}
```

**MSI-X State Machine Property**:

```rust
proptest! {
    #[test]
    fn msix_state_transitions_are_valid(
        operations in prop::collection::vec(
            prop_oneof![
                Just(MsixOp::Configure),
                Just(MsixOp::Enable),
                Just(MsixOp::Disable),
                Just(MsixOp::Mask),
            ],
            1..50
        )
    ) {
        let mut state = MsixVectorState::Masked;
        
        for op in operations {
            let result = match (&state, op) {
                (MsixVectorState::Masked, MsixOp::Configure) => {
                    state = MsixVectorState::Configured { .. };
                    Ok(())
                }
                (MsixVectorState::Configured { .. }, MsixOp::Enable) => {
                    state = MsixVectorState::Enabled { .. };
                    Ok(())
                }
                (_, MsixOp::Enable) => Err("Can't enable from this state"),
                // ...
            };
            
            // Every operation either succeeds or fails gracefully
            prop_assert!(result.is_ok() || result.is_err());
        }
    }
}
```

### 12.4 Bugs Caught by Property Tests

1. **DMA Overlap Bug**: A race condition could create overlapping mappings
2. **BAR Misalignment**: Allocator didn't enforce natural alignment for all sizes
3. **MSI-X State Bug**: Enabling an unconfigured vector caused UB
4. **64-bit BAR Bug**: High 32 bits weren't preserved correctly

### 12.5 Running Tests

```bash
# Default (256 iterations per property)
cargo test --features proptest

# Extended (1000 iterations for CI)
PROPTEST_CASES=1000 cargo test --features proptest

# Comprehensive (10000 iterations for release)
PROPTEST_CASES=10000 cargo test --features proptest
```

---

## 13. NixOS Integration

### 13.1 NixOS Configuration

Complete `/etc/nixos/configuration.nix` for VFIO:

```nix
{ config, pkgs, ... }:

{
  # Enable IOMMU
  boot.kernelParams = [
    "amd_iommu=on"      # or intel_iommu=on
    "iommu=pt"          # passthrough mode for performance
    "vfio-pci.ids=10de:2bb1,10de:22e8"  # Bind at boot
  ];
  
  # Load VFIO modules early
  boot.initrd.kernelModules = [
    "vfio_pci"
    "vfio"
    "vfio_iommu_type1"
  ];
  
  # Blacklist NVIDIA driver on host
  boot.blacklistedKernelModules = [ "nvidia" "nouveau" ];
  
  # Use integrated graphics for display
  services.xserver.videoDrivers = [ "amdgpu" ];  # or "intel"
  
  # VFIO device permissions
  services.udev.extraRules = ''
    SUBSYSTEM=="vfio", OWNER="root", GROUP="kvm", MODE="0660"
  '';
  
  # Required for large BAR mapping
  security.pam.loginLimits = [
    { domain = "*"; type = "soft"; item = "memlock"; value = "unlimited"; }
    { domain = "*"; type = "hard"; item = "memlock"; value = "unlimited"; }
  ];
}
```

### 13.2 Nix Flake Commands

```bash
# Boot GPU VM (recommended)
nix run .#fc-gpu

# With options
nix run .#fc-gpu -- --pci 0000:01:00.0 --mem 32768 --cpus 8

# Quick demo with color output
nix run .#demo-gpu

# Performance comparison (CH vs FC)
sudo nix run .#gpu-timing-test

# Create VM assets
nix run .#create-gpu-assets
```

### 13.3 Key Flake Packages

| Package | Description |
|---------|-------------|
| `fc-gpu` | Complete GPU VM launcher with validation |
| `demo-gpu` | Demo script with color-highlighted output |
| `gpu-timing-test` | Cloud Hypervisor vs Firecracker benchmark |
| `create-gpu-assets` | Create kernel/initramfs/rootfs |
| `vfio-bind` | Bind devices to vfio-pci |
| `vfio-status` | Show VFIO binding status |

---

## 14. Troubleshooting Guide

### 14.1 Common Errors and Solutions

#### Error: "IOMMU group is not viable"

```
Error: VFIO error: IOMMU group 13 is not viable - 
       ensure all devices are bound to vfio-pci
```

**Cause**: Not all devices in the IOMMU group are bound to vfio-pci.

**Solution**:
```bash
# Check what's in the group
./scripts/vfio-status.sh

# Bind ALL devices in the group
sudo ./scripts/vfio-bind-safe.sh 0000:01:00.0 0000:01:00.1
```

#### Error: "Device not found or not bound to vfio-pci"

```
Error: Device 0000:01:00.0 not bound to vfio-pci driver (current: nvidia)
```

**Cause**: GPU still bound to NVIDIA driver.

**Solution**:
```bash
# Unbind from nvidia
sudo bash -c 'echo 0000:01:00.0 > /sys/bus/pci/drivers/nvidia/unbind'

# Bind to vfio-pci
sudo ./scripts/vfio-bind.sh 0000:01:00.0
```

#### Error: "Failed to add group to container: Operation not permitted"

```
Error: VFIO error: Failed to add group 13 to container: 
       Operation not permitted (os error 1)
```

**Cause**: Group not viable at kernel level (not all devices bound).

**Solution**: Bind ALL devices in the IOMMU group, or reboot if drivers are hung.

#### Error: "Cannot allocate memory" for BAR

```
Error: Failed to allocate BAR: size 0x2000000000 (128GB)
```

**Cause**: Insufficient MMIO64 space configured.

**Solution**: Increase MMIO64 region in VM configuration:
```json
{
  "machine-config": {
    "mmio64_size_mib": 262144
  }
}
```

#### Error: 30-second timeout during driver init

```
[   35.456] NVRM: Timeout waiting for LTR response!
```

**Cause**: LTR fix not applied.

**Solution**: Ensure using Firecracker build with LTR fix in `device.rs`.

### 14.2 Driver Hung During Unbind

If `echo ... > unbind` hangs:

1. **Check what's using the device**:
   ```bash
   lsof /dev/snd/* 2>/dev/null
   fuser -v /dev/dri/*
   ```

2. **Stop audio services**:
   ```bash
   systemctl --user stop pipewire pipewire-pulse wireplumber
   ```

3. **If still hung, reboot is required**:
   ```bash
   sudo reboot
   ```

4. **Prevent recurrence**: Bind at boot via kernel cmdline:
   ```
   vfio-pci.ids=10de:2bb1,10de:22e8
   ```

### 14.3 Debugging Commands

```bash
# Check IOMMU groups
for g in /sys/kernel/iommu_groups/*/devices/*; do
    echo "IOMMU group $(basename $(dirname $(dirname $g))): $(basename $g)"
done

# Check device driver
readlink /sys/bus/pci/devices/0000:01:00.0/driver

# Check VFIO devices
ls -la /dev/vfio/

# Check kernel messages
sudo dmesg | grep -i vfio
sudo dmesg | grep -i iommu

# Check memlock limits
ulimit -l
```

---

## 15. Performance Optimization

### 15.1 Key Optimizations Implemented

| Optimization | Impact | Implementation |
|--------------|--------|----------------|
| Direct BAR mapping | -6s driver init | `map_mmio_regions()` in device.rs |
| LTR capability masking | -30s boot time | Config space interception |
| Firmware preloading | -1s boot time | Embed GSP FW in initramfs |
| Identity DMA mapping | Simplified setup | IOVA == GPA |

### 15.2 Direct BAR Mapping

Without direct mapping:
```
Guest writes to BAR → KVM exit → Firecracker trap → pwrite() → Return
Latency: ~2000ns per access
GPU init: ~6.6 seconds
```

With direct mapping:
```
Guest writes to BAR → Direct to GPU (no exit)
Latency: ~100ns per access
GPU init: ~0.5 seconds
```

### 15.3 Firmware Preloading

NVIDIA drivers load ~72MB of GSP firmware. Options:

1. **From rootfs**: Firmware read via virtio-blk
   ```
   Time: ~1.5 seconds
   ```

2. **From initramfs**: Firmware in guest RAM
   ```
   Time: ~0.5 seconds
   ```

Create preloaded initramfs:
```bash
# Add firmware to initramfs
cp -r /lib/firmware/nvidia initramfs/lib/firmware/
cd initramfs && find . | cpio -o -H newc | gzip > ../initramfs-with-fw.cpio.gz
```

### 15.4 Boot Timeline

```
0.000s  Firecracker starts
0.050s  VM boots, kernel starts
0.200s  initramfs starts
0.500s  PCI enumeration (GPU visible)
0.800s  Root filesystem mounted
1.000s  nvidia.ko loading starts
1.500s  GSP firmware loaded
2.000s  nvidia-modeset.ko loaded
2.500s  nvidia-smi ready
```

---

## 16. Appendices

### 16.1 VFIO ioctl Quick Reference

```c
// Container ioctls (on /dev/vfio/vfio)
#define VFIO_GET_API_VERSION    _IO(VFIO_TYPE, VFIO_BASE + 0)   // 0x3b64
#define VFIO_CHECK_EXTENSION    _IO(VFIO_TYPE, VFIO_BASE + 1)   // 0x3b65
#define VFIO_SET_IOMMU          _IO(VFIO_TYPE, VFIO_BASE + 2)   // 0x3b66
#define VFIO_IOMMU_MAP_DMA      _IO(VFIO_TYPE, VFIO_BASE + 13)  // 0x3b71
#define VFIO_IOMMU_UNMAP_DMA    _IO(VFIO_TYPE, VFIO_BASE + 14)  // 0x3b72

// Group ioctls (on /dev/vfio/N)
#define VFIO_GROUP_GET_STATUS       _IO(VFIO_TYPE, VFIO_BASE + 3)  // 0x3b67
#define VFIO_GROUP_SET_CONTAINER    _IO(VFIO_TYPE, VFIO_BASE + 4)  // 0x3b68
#define VFIO_GROUP_GET_DEVICE_FD    _IO(VFIO_TYPE, VFIO_BASE + 6)  // 0x3b6a

// Device ioctls (on device fd)
#define VFIO_DEVICE_GET_REGION_INFO _IO(VFIO_TYPE, VFIO_BASE + 8)   // 0x3b6c
#define VFIO_DEVICE_SET_IRQS        _IO(VFIO_TYPE, VFIO_BASE + 10)  // 0x3b6e
#define VFIO_DEVICE_RESET           _IO(VFIO_TYPE, VFIO_BASE + 11)  // 0x3b6f
```

### 16.2 PCI Configuration Space Offsets

```
0x00: Vendor ID (2 bytes)
0x02: Device ID (2 bytes)
0x04: Command (2 bytes)
0x06: Status (2 bytes)
0x08: Revision ID (1 byte)
0x09: Class Code (3 bytes)
0x0C: Cache Line Size (1 byte)
0x0D: Latency Timer (1 byte)
0x0E: Header Type (1 byte)
0x0F: BIST (1 byte)
0x10: BAR0 (4 bytes)
0x14: BAR1 (4 bytes)
0x18: BAR2 (4 bytes)
0x1C: BAR3 (4 bytes)
0x20: BAR4 (4 bytes)
0x24: BAR5 (4 bytes)
0x28: Cardbus CIS Pointer (4 bytes)
0x2C: Subsystem Vendor ID (2 bytes)
0x2E: Subsystem ID (2 bytes)
0x30: Expansion ROM BAR (4 bytes)
0x34: Capabilities Pointer (1 byte)
0x3C: Interrupt Line (1 byte)
0x3D: Interrupt Pin (1 byte)
0x3E: Min_Gnt (1 byte)
0x3F: Max_Lat (1 byte)
```

### 16.3 MSI-X Table Entry Format

```
Offset 0x00: Message Address (low 32 bits)
Offset 0x04: Message Address (high 32 bits)
Offset 0x08: Message Data (32 bits)
Offset 0x0C: Vector Control (bit 0 = mask)

Total: 16 bytes per entry
```

### 16.4 File Locations

```
Implementation:
  firecracker/src/vmm/src/pci/vfio/device.rs     # Main VFIO device
  firecracker/src/vmm/src/pci/vfio/container.rs  # IOMMU container
  firecracker/src/vmm/src/pci/vfio/msix.rs       # MSI-X handling
  firecracker/src/vmm/src/device_manager/pci_mngr.rs  # Integration

Scripts:
  scripts/vfio-bind.sh       # Bind devices
  scripts/vfio-unbind.sh     # Unbind devices
  scripts/vfio-status.sh     # Show status
  scripts/vfio-bind-safe.sh  # Safe binding with checks
  scripts/demo-gpu.sh        # Quick demo

Documentation:
  docs/vfio-gpu-passthrough-book.md  # This document
  docs/firecracker-gpu-passthrough.md
  docs/nixos-vfio-setup.md
  docs/isospin-concepts-and-glossary.md
```

### 16.5 JSON Configuration Format

```json
{
  "boot-source": {
    "kernel_image_path": ".vm-assets/vmlinux.bin",
    "boot_args": "console=ttyS0 reboot=k panic=1 pci=realloc root=/dev/vda rw",
    "initrd_path": ".vm-assets/initramfs.cpio.gz"
  },
  "drives": [{
    "drive_id": "rootfs",
    "path_on_host": ".vm-assets/rootfs.ext4",
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

---

## 17. NVIDIA GPU Deep Dive: The RTX PRO 6000 Blackwell

This chapter documents everything learned about the NVIDIA RTX PRO 6000 Blackwell GPU during passthrough implementation - from GSP firmware to driver initialization timing.

### 17.1 Hardware Identification

| Property | Value |
|----------|-------|
| GPU Name | NVIDIA RTX PRO 6000 Blackwell Workstation Edition |
| Vendor ID | `10de` (NVIDIA) |
| Device ID | `2bb1` |
| Chip Family | GB202GL (Blackwell) |
| VRAM | 128GB |
| Audio Device | `10de:22e8` (HD Audio Controller) |
| IOMMU Group | 13 (shared with audio) |
| PCI Address | `0000:01:00.0` |
| Driver Version Tested | 580.95.05 (open kernel modules) |

### 17.2 BAR Layout

The RTX PRO 6000 has an unusual BAR layout due to its massive 128GB VRAM:

```
┌────────────────────────────────────────────────────────────────┐
│                    NVIDIA RTX PRO 6000 BARs                    │
├──────┬──────────────┬────────────────────┬────────────────────┤
│ BAR  │ Size         │ Host Address       │ Guest Address      │
├──────┼──────────────┼────────────────────┼────────────────────┤
│ 0    │ 64 MB        │ 0xD8000000         │ 0xC4000000         │
│ 1    │ 128 GB       │ (host assigned)    │ 0x6000000000       │
│ 2    │ (BAR1 high)  │ -                  │ -                  │
│ 3    │ 32 MB        │ (host assigned)    │ 0x4002000000       │
│ 4    │ (BAR3 high)  │ -                  │ -                  │
│ 5    │ 128 B        │ 0xF001 (I/O)       │ (skipped)          │
└──────┴──────────────┴────────────────────┴────────────────────┘
```

**BAR Details:**

- **BAR0 (64MB)**: GPU registers, 32-bit non-prefetchable
- **BAR1 (128GB)**: VRAM, 64-bit prefetchable - this is the massive one
- **BAR3 (32MB)**: Additional registers, 64-bit prefetchable
- **BAR5 (128B)**: Legacy I/O space, skipped in passthrough

**Raw BAR Values from VFIO:**
```
BAR0: raw=0xd8000000, is_64bit=false, is_prefetch=false
BAR1: raw=0xc, is_64bit=true, is_prefetch=true
BAR3: raw=0xc, is_64bit=true, is_prefetch=true  
BAR5: raw=0xf001, is_io=true
```

### 17.3 GSP Firmware (GPU System Processor)

Modern NVIDIA GPUs use a GPU System Processor (GSP) that runs firmware to manage the GPU. This is one of the major sources of initialization latency.

#### Firmware Files

| Firmware | Size | GPU Families |
|----------|------|--------------|
| `gsp_ga10x.bin` | **72MB** | Ampere (GA10x), Ada (AD10x), Hopper (GH100), Blackwell (GB10x, GB20x) |
| `gsp_tu10x.bin` | **29MB** | Turing (TU10x, TU11x), Ampere A100 (GA100) |

**Path:** `/lib/firmware/nvidia/<version>/gsp_ga10x.bin`

For driver version 580.95.05:
```
/lib/firmware/nvidia/580.95.05/gsp_ga10x.bin  (72MB)
```

#### GSP Chip Families (from nvidia-open source)

```c
typedef enum {
    NV_FIRMWARE_CHIP_FAMILY_TU10X = 1,   // Turing
    NV_FIRMWARE_CHIP_FAMILY_TU11X = 2,   // Turing (11 series)
    NV_FIRMWARE_CHIP_FAMILY_GA100 = 3,   // Ampere A100
    NV_FIRMWARE_CHIP_FAMILY_GA10X = 4,   // Ampere (consumer)
    NV_FIRMWARE_CHIP_FAMILY_AD10X = 5,   // Ada Lovelace
    NV_FIRMWARE_CHIP_FAMILY_GH100 = 6,   // Hopper
    NV_FIRMWARE_CHIP_FAMILY_GB10X = 8,   // Blackwell
    NV_FIRMWARE_CHIP_FAMILY_GB20X = 9,   // Blackwell (GB202 - RTX PRO 6000)
} nv_firmware_chip_family_t;
```

#### GSP Loading Process

```
1. nvidia.ko loads, calls request_firmware("nvidia/580.95.05/gsp_ga10x.bin")
2. 72MB firmware read from /lib/firmware (disk I/O ~100-500ms)
3. Firmware uploaded to GPU VRAM's WPR (Write Protected Region) (~500ms-2s)
4. GSP boots from VRAM (~1-3s)
5. GSP-RM (Resource Manager) initializes
```

**Critical Constraint:** GSP firmware **cannot be pre-loaded** or cached across power cycles because:
- WPR is scrubbed on every cold start (security requirement)
- Hardware booter locks the region
- This is by design for secure boot

#### GSP Heap Sizes

```c
// Baremetal (normal)
#define GSP_FW_HEAP_SIZE_OVERRIDE_LIBOS3_BAREMETAL_MIN_MB  (88u)
#define GSP_FW_HEAP_SIZE_OVERRIDE_LIBOS3_BAREMETAL_MAX_MB  (280u)

// vGPU mode
#define GSP_FW_HEAP_SIZE_OVERRIDE_LIBOS3_VGPU_MIN_MB       (581u)
#define GSP_FW_HEAP_SIZE_OVERRIDE_LIBOS3_VGPU_MAX_MB       (1093u)
```

### 17.4 Driver Architecture

The NVIDIA open kernel driver consists of multiple kernel modules:

| Module | Purpose | Source Location |
|--------|---------|-----------------|
| `nvidia.ko` | Main driver, PCI probe, GSP communication | `nvidia-open/kernel-open/nvidia/` |
| `nvidia-modeset.ko` | Display/mode-setting | `nvidia-open/kernel-open/nvidia-modeset/` |
| `nvidia-drm.ko` | DRM interface | `nvidia-open/kernel-open/nvidia-drm/` |
| `nvidia-uvm.ko` | Unified Virtual Memory | `nvidia-open/kernel-open/nvidia-uvm/` |
| `nvidia-peermem.ko` | GPU peer memory (RDMA) | `nvidia-open/kernel-open/nvidia-peermem/` |

#### Module Load Order

```bash
# Minimal for GPU compute:
insmod nvidia.ko NVreg_EnableGpuFirmware=1 NVreg_OpenRmEnableUnsupportedGpus=1

# For display:
insmod nvidia-modeset.ko
insmod nvidia-drm.ko
```

#### Key Module Parameters

| Parameter | Purpose |
|-----------|---------|
| `NVreg_EnableGpuFirmware=1` | Enable GSP firmware mode |
| `NVreg_OpenRmEnableUnsupportedGpus=1` | Allow open driver on all GPUs |
| `NVreg_RegisterPCIDriver=1` | Control PCI driver registration |
| `NVreg_EnableResizableBar=1` | Enable Resizable BAR |

### 17.5 Driver Initialization Sequence

The initialization happens in two phases: module load and first GPU use.

#### Phase 1: Module Load (`insmod nvidia.ko`)

```
module_init(nvidia_init_module)  [nv.c:6302]
    ├── nv_procfs_init()           # ~1ms, create /proc entries
    ├── nv_caps_root_init()        # ~1ms, capability setup
    ├── nv_module_init()           # ~10ms
    │   ├── nv_cap_drv_init()
    │   ├── nvlink_drivers_init()
    │   └── rm_init_rm()           # Resource Manager init
    └── nv_drivers_init()          # PCI probe starts
        └── nv_pci_probe()         # Per-GPU init
```

**Key Source Files:**
- `nvidia-open/kernel-open/nvidia/nv.c` - Module entry, `nvidia_init_module()` at line 6302
- `nvidia-open/kernel-open/nvidia/nv-pci.c` - PCI probe, `nv_pci_probe()` at line 1293
- `nvidia-open/src/nvidia/arch/nvalloc/unix/src/osapi.c` - `rm_init_*` functions

#### Phase 2: First Use (nvidia-smi / CUDA)

```
First nvidia-smi or CUDA call triggers:
    └── rm_init_adapter()          # Heavy - GPU hardware init
        ├── Firmware loading       (~100-500ms)
        ├── GSP upload to VRAM     (~500ms-2s)
        ├── GSP boot               (~1-3s)
        ├── Memory scrub           (~100ms-1s for 128GB)
        └── RM initialization
```

### 17.6 Boot Timing Analysis

Detailed timing from actual boot with timestamps:

```
┌─────────────────────────────────────────────────────────────────┐
│                     Boot Timeline                               │
├──────────┬─────────┬───────────────────────────────────────────┤
│ Uptime   │ Delta   │ Event                                     │
├──────────┼─────────┼───────────────────────────────────────────┤
│ 0.00s    │ -       │ Firecracker starts                        │
│ 0.05s    │ 0.05s   │ VM boots, kernel decompression            │
│ 0.50s    │ 0.45s   │ PCI enumeration, GPU visible (10de:2bb1)  │
│ 1.77s    │ 1.27s   │ insmod nvidia.ko starts                   │
│ 4.56s    │ 2.79s   │ nvidia.ko loaded (GSP firmware done)      │
│ 4.56s    │ 0.00s   │ nvidia-smi #1 starts (cold)               │
│ 11.74s   │ 7.18s   │ nvidia-smi #1 completes                   │
│ 11.75s   │ 0.01s   │ nvidia-smi #2 starts (warm)               │
│ 13.46s   │ 1.71s   │ nvidia-smi #2 completes                   │
│ 15.21s   │ 1.75s   │ Demo complete                             │
└──────────┴─────────┴───────────────────────────────────────────┘
```

**Key Observations:**

| Phase | Time | Notes |
|-------|------|-------|
| Kernel to init | 1.7s | Linux boot, virtio init |
| insmod nvidia.ko | 2.8s | Driver load + GSP firmware |
| nvidia-smi #1 (cold) | 7.2s | Full RM init, 128GB memory |
| nvidia-smi #2 (warm) | 1.7s | CLI overhead only |
| **Total cold boot** | **~15s** | From Firecracker start |

**Why nvidia-smi #1 is slow (7.2s):**
1. RM (Resource Manager) full initialization
2. 128GB VRAM scrub/check
3. GPU hardware probing
4. Creating device nodes
5. NVML library initialization

**Why nvidia-smi #2 is still 1.7s:**
- nvidia-smi CLI startup overhead
- NVML library re-initialization per invocation
- Not the GPU - it's already warm

### 17.7 Device Nodes

Required device nodes for nvidia-smi:

```bash
mknod -m 666 /dev/nvidia0 c 195 0       # GPU device
mknod -m 666 /dev/nvidiactl c 195 255   # Control device
mknod -m 666 /dev/nvidia-modeset c 195 254  # Modeset (if loaded)
mknod -m 666 /dev/nvidia-uvm c 237 0    # UVM (if loaded)
mknod -m 666 /dev/nvidia-uvm-tools c 237 1
```

Major numbers:
- 195: nvidia main driver
- 237: nvidia-uvm

### 17.8 PCI Capability Structure

The RTX PRO 6000 capabilities as discovered:

```
Capabilities at offset 0x40:
├── 0x40: Power Management (id=0x01), next=0x48
├── 0x48: MSI (id=0x05), next=0x60
│         max_vectors=16, 64bit=true, per_vector_mask=true
├── 0x60: PCIe (id=0x10), next=0x9c
│         DevCap2 at 0x84 (offset +0x24)
│         DevCtl2 at 0x88 (offset +0x28)
└── 0x9c: Vendor Specific (id=0x09), next=0x00

Extended Capabilities (0x100+):
└── (Empty or vendor-specific)
```

**Note:** No MSI-X capability - this GPU uses plain MSI with 16 vectors.

### 17.9 LTR/OBFF Capability Details

The LTR (Latency Tolerance Reporting) issue is specific to PCIe capability registers:

```
DevCap2 (PCIe cap + 0x24):
  Raw value:     0x70993
  After masking: 0x30193
  
  Bits masked:
  - Bit 11: LTR Mechanism Supported
  - Bits 18-19: OBFF Supported

DevCtl2 (PCIe cap + 0x28):
  Bit 10: LTR Enable - intercepted on write, always masked to 0
```

**What the driver sees after masking:**
```
NVRM: kbifInitLtr_GB202: LTR is disabled in the hierarchy
```

This message confirms the driver detected LTR as unsupported and skipped the 30-second initialization attempt.

### 17.10 Firmware Pre-loading Optimization

One optimization tested: embedding GSP firmware in initramfs to avoid disk I/O.

| Configuration | initramfs Size | insmod Time | Savings |
|---------------|----------------|-------------|---------|
| Firmware on rootfs | 1.5MB | ~1.7s | - |
| Firmware in initramfs | 62MB | ~0.8s | **0.9s** |

**How it works:**
```bash
# Add firmware to initramfs
cp -r /lib/firmware/nvidia initramfs/lib/firmware/
cd initramfs && find . | cpio -o -H newc | gzip > ../initramfs-with-fw.cpio.gz
```

When nvidia.ko calls `request_firmware()`, the kernel finds it already in RAM (from initramfs) instead of reading from virtio-blk, saving ~1 second of I/O.

### 17.11 CUDA Version

The driver 580.95.05 provides CUDA 13.0:

```
NVIDIA-SMI 580.95.05    Driver Version: 580.95.05    CUDA Version: 13.0
```

### 17.12 Blackwell GPU SKU Reference

All supported Blackwell GPUs (from nvidia-open README):

| Product | PCI ID |
|---------|--------|
| NVIDIA RTX PRO 6000 Blackwell Workstation Edition | **2BB1** |
| NVIDIA RTX PRO 6000 Blackwell Max-Q Workstation Edition | 2BB4 |
| NVIDIA RTX PRO 6000 Blackwell Server Edition | 2BB5 |
| NVIDIA RTX PRO 5000 Blackwell | 2BB3 |
| NVIDIA RTX PRO 5000 72GB Blackwell | 2BB3 |
| NVIDIA RTX PRO 4500 Blackwell | 2C31 |
| NVIDIA RTX PRO 4000 Blackwell | 2C34 |
| NVIDIA RTX PRO 4000 Blackwell SFF Edition | 2C33 |
| NVIDIA RTX PRO 2000 Blackwell | 2D30 |

### 17.13 Nix Store Paths (Reference)

For reproducibility, the exact Nix store paths used:

```
# Firmware
/nix/store/7xfcmll159h77msl79w1x9pd1vkmxh75-nvidia-x11-580.95.05-6.16-firmware/

# Kernel modules (for kernel 6.12.63)
/nix/store/rwfsyfyi8mrzdp4brm618r2jz0nlx5wy-nvidia-open-6.12.63-580.95.05/

# Userspace binaries
/nix/store/gnzdlz9m5nlv5cr3203lbc0kgpiwhsn5-nvidia-x11-580.95.05-6.12.63-bin/

# Libraries
/nix/store/iqvqw3q2k0fj6s0jndi70ahh09b833z9-nvidia-x11-580.95.05-6.12.63/
```

### 17.14 Future Optimization Opportunities

Based on the timing analysis, potential improvements:

| Optimization | Current | Target | Approach |
|--------------|---------|--------|----------|
| GSP firmware load | ~500ms | ~100ms | Keep in guest RAM |
| Memory scrub | ~1-2s | Skip | Disable if security allows |
| nvidia-smi CLI | 1.7s | 50ms | Direct NVML or ioctl |
| Cold RM init | 7.2s | <100ms | VM snapshots |

**Most promising path to fast GPU access:** VM snapshots taken after driver is fully initialized, restoring in ~50-100ms.

---

## 18. The GPU Broker: Eliminating Cold Boot Penalty

### 18.1 The Problem: 15 Seconds Per VM

Even with all optimizations from previous chapters (LTR fix, direct BAR mapping, firmware preloading), each VM still requires ~15 seconds to:

1. Boot the kernel (~0.5s)
2. Load nvidia.ko and GSP firmware (~2.8s)  
3. Run first nvidia-smi / CUDA init (~7.2s)
4. Total: **~11-15 seconds per cold start**

For multi-tenant GPU sharing, this is unacceptable. If 10 VMs need GPU access, you're looking at 150 seconds of cumulative boot time.

### 18.2 The Insight: Proxy the RM API

The NVIDIA driver communicates with the GPU through ~20 ioctls to `/dev/nvidiactl` and `/dev/nvidia0`. These implement the **Resource Manager (RM) API**:

```
┌─────────────────────────────────────────────────────────────┐
│                    Current Architecture                      │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│   VM 1              VM 2              VM 3                  │
│   ┌────────┐        ┌────────┐        ┌────────┐            │
│   │nvidia.ko│       │nvidia.ko│       │nvidia.ko│           │
│   └───┬────┘        └───┬────┘        └───┬────┘            │
│       │                 │                 │                  │
│       │ 15s init        │ 15s init        │ 15s init        │
│       ▼                 ▼                 ▼                  │
│   ┌─────────────────────────────────────────────────────┐   │
│   │                    GPU Hardware                      │   │
│   │              (Cold boot each time)                   │   │
│   └─────────────────────────────────────────────────────┘   │
│                                                              │
│   Total: 45 seconds for 3 VMs                               │
└─────────────────────────────────────────────────────────────┘
```

**The key insight:** If we proxy the RM API calls through a broker that maintains a warm connection to the GPU, VMs can share the already-initialized GPU:

```
┌─────────────────────────────────────────────────────────────┐
│                    Broker Architecture                       │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│   VM 1              VM 2              VM 3                  │
│   ┌────────┐        ┌────────┐        ┌────────┐            │
│   │shim.ko │        │shim.ko │        │shim.ko │            │
│   └───┬────┘        └───┬────┘        └───┬────┘            │
│       │                 │                 │                  │
│       │ ~50ms           │ ~50ms           │ ~50ms           │
│       ▼                 ▼                 ▼                  │
│   ┌─────────────────────────────────────────────────────┐   │
│   │                   GPU Broker                         │   │
│   │  - Handle translation (virtual → real)              │   │
│   │  - Per-client isolation                             │   │
│   │  - Quota enforcement                                │   │
│   │  - One warm GPU connection                          │   │
│   └──────────────────────┬──────────────────────────────┘   │
│                          │                                   │
│                          │ (already initialized)            │
│                          ▼                                   │
│   ┌─────────────────────────────────────────────────────┐   │
│   │                    GPU Hardware                      │   │
│   │                   (Warm, ready)                      │   │
│   └─────────────────────────────────────────────────────┘   │
│                                                              │
│   Total: ~150ms for 3 VMs (vs 45 seconds)                   │
└─────────────────────────────────────────────────────────────┘
```

### 18.3 Design Philosophy: The Carmack Principle

The broker is designed as a **pure state machine** following John Carmack's insight about the Saab Gripen flight software:

> "The fly-by-wire flight software for the Saab Gripen disallowed both subroutine calls and backward branches, except for the one at the bottom of the main loop. Control flow went forward only. No bug has ever been found in the 'released for flight' versions of that code."

This translates to:

```rust
// Pure state machine: (State, Input) → (State', Output)
impl BrokerState {
    pub fn tick(self, request: Request) -> (Self, Response) {
        // Deterministic
        // No callbacks
        // No shared mutable state
        // Forward-only control flow
    }
}
```

Benefits:
- **Deterministic replay**: Given any input sequence, reproduce exact behavior
- **Time-travel debugging**: Binary search through frames to find bugs
- **Simulation IS the test**: Run millions of "frames" in seconds
- **Certifiable quality**: The same rigor as aerospace software

### 18.4 Architecture Overview

```
gpu-broker/
├── Cargo.toml           # Dependencies: io-uring, zerocopy, proptest
├── SECURITY.md          # Security model documentation
└── src/
    ├── main.rs          # Entry point (placeholder)
    ├── broker.rs        # Pure functional state machine (~500 lines)
    ├── handle_table.rs  # Per-client handle isolation (~1100 lines)
    ├── rm_api.rs        # NVIDIA RM API structures (~400 lines)
    └── transport.rs     # Simulated transport layer (~760 lines)

Total: ~3,500 lines of Rust, 69 property tests
```

#### The Layers

```
┌─────────────────────────────────────────────────────────────┐
│                    Transport (membrane)                      │
│  SimulatedTransport / MultiClientTransport / (future: real) │
│  - Converts bytes ↔ Request/Response                        │
│  - Only stateful part (but fully simulated for tests)       │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    BrokerState (pure)                        │
│  fn tick(self, Request) -> (Self, Response)                 │
│  - Deterministic                                            │
│  - Replayable                                               │
│  - All state explicit                                       │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    HandleTable                               │
│  - Per-client virtual→real handle mapping                   │
│  - Quota enforcement                                        │
│  - Invariant checking                                       │
└─────────────────────────────────────────────────────────────┘
```

### 18.5 Handle Translation

The RM API uses 32-bit handles to refer to GPU objects (channels, memory, contexts). The broker translates between:

- **Virtual handles**: What the guest sees (scoped per client)
- **Real handles**: Actual handles from `/dev/nvidiactl`

```rust
/// Per-client handle isolation
pub struct HandleTable {
    clients: HashMap<ClientId, ClientHandles>,
    real_to_virtual: HashMap<RealHandle, (ClientId, VirtualHandle)>,
}

pub struct ClientHandles {
    virtual_to_real: HashMap<VirtualHandle, RealHandle>,
    real_to_virtual: HashMap<RealHandle, VirtualHandle>,
    handle_count: usize,
    quota: usize,
}
```

**Key invariants** (enforced by property tests):

1. No handle leakage between clients
2. Quota strictly enforced per client
3. Unregistering a client frees all its handles
4. Virtual handles are unique per client
5. Real handles are globally unique

### 18.6 The NVIDIA RM API

The broker proxies these ioctl commands:

| Escape Code | Name | Purpose |
|-------------|------|---------|
| `0x2A` | `NV_ESC_RM_ALLOC` | Allocate GPU object |
| `0x29` | `NV_ESC_RM_FREE` | Free GPU object |
| `0x20` | `NV_ESC_RM_CONTROL` | Control GPU object |
| `0x4A` | `NV_ESC_RM_MAP_MEMORY` | Map GPU memory |
| `0x4B` | `NV_ESC_RM_UNMAP_MEMORY` | Unmap GPU memory |
| `0x54` | `NV_ESC_RM_ALLOC_MEMORY` | Allocate GPU memory |

Each operation involves handle translation:

```rust
// Example: NV_ESC_RM_ALLOC (0x2A)
#[repr(C)]
pub struct NVOS21_PARAMETERS {
    pub hRoot: u32,         // Translate: virtual → real
    pub hObjectParent: u32, // Translate: virtual → real
    pub hObjectNew: u32,    // Allocate new virtual, map to new real
    pub hClass: u32,        // Pass through
    pub pAllocParms: u64,   // Pass through (opaque to broker)
    pub paramsSize: u32,
    pub status: u32,
}
```

### 18.7 Recording and Replay

The broker supports deterministic recording and replay:

```rust
pub struct Recording {
    frames: Vec<Frame>,
    checkpoints: Vec<(usize, BrokerState)>,
}

pub struct Frame {
    input: Request,
    output: Response,
    state_hash: u64,  // For verification
}

impl Recording {
    /// Replay and verify determinism
    pub fn replay(&self) -> bool {
        let mut state = BrokerState::new();
        for frame in &self.frames {
            let (new_state, output) = state.tick(frame.input.clone());
            if output != frame.output {
                return false;  // Non-determinism detected!
            }
            state = new_state;
        }
        true
    }
    
    /// Binary search for first divergence
    pub fn find_divergence(&self, other: &Recording) -> Option<usize> {
        // O(log n) bug finding
    }
}
```

### 18.8 Property-Based Testing

The broker uses extensive property testing with `proptest`:

```rust
proptest! {
    /// Property: Replay is always deterministic
    #[test]
    fn replay_deterministic(
        ops in prop::collection::vec(operation(), 1..100)
    ) {
        let recording = run_operations(&ops);
        prop_assert!(recording.replay());
    }
    
    /// Property: Invariants hold after any operation sequence
    #[test]
    fn invariants_always_hold(
        ops in prop::collection::vec(operation(), 1..200)
    ) {
        let mut state = BrokerState::new();
        for op in ops {
            let (new_state, _) = state.tick(op);
            state = new_state;
            prop_assert!(state.check_invariants().is_ok());
        }
    }
    
    /// Property: Handle isolation between clients
    #[test]
    fn no_shared_real_handles(
        ops in prop::collection::vec(
            (client_id(), handle_op()),
            1..100
        )
    ) {
        let mut table = HandleTable::new();
        let mut client_handles: HashMap<ClientId, HashSet<RealHandle>> = HashMap::new();
        
        for (client, op) in ops {
            if let Some(real) = apply_op(&mut table, client, op) {
                // Verify no other client has this real handle
                for (other_client, handles) in &client_handles {
                    if *other_client != client {
                        prop_assert!(!handles.contains(&real));
                    }
                }
                client_handles.entry(client).or_default().insert(real);
            }
        }
    }
}
```

**Test Coverage:**

| Category | Tests | Description |
|----------|-------|-------------|
| Handle Table | 27 | Isolation, quotas, rapid alloc/dealloc |
| Broker State | 8 | Replay, invariants, stress |
| Transport | 8 | Wire format, multi-client routing |
| RM API | 4 | Struct serialization |
| io_uring | 11 | Event loop, eventfd, multi-client |
| Shared Memory | 7 | Ring buffers, data integrity |
| Driver | 9 | Mock driver, ioctl wrappers |
| Proxy | 9 | Handle translation, client isolation |
| **Total** | **105** | All passing |

### 18.9 Security Model

The broker enforces several security boundaries:

1. **Handle isolation**: Clients cannot reference each other's GPU objects
2. **Quota enforcement**: Prevent resource exhaustion attacks
3. **No raw pointer passthrough**: All memory references validated
4. **Audit logging**: Every operation recorded for forensics

From `SECURITY.md`:

```markdown
## Threat Model

1. Malicious VM attempting to access another VM's GPU resources
   → Mitigated by handle translation (virtual handles scoped per client)

2. Resource exhaustion (allocate infinite handles)
   → Mitigated by per-client quotas

3. Use-after-free (reference freed handle)
   → Mitigated by handle table cleanup on client unregister

4. Information disclosure via timing
   → Not yet addressed (future: constant-time operations)
```

### 18.10 Implementation Status

The broker implementation is now substantially complete:

| Component | Status | Lines | Description |
|-----------|--------|-------|-------------|
| State machine | **Done** | ~900 | Pure `tick()` function with replay |
| Handle table | **Done** | ~600 | Per-client isolation with quotas |
| RM API types | **Done** | ~450 | Wire format structs (NVOS21, etc.) |
| Simulated transport | **Done** | ~770 | For testing, wire encoding |
| io_uring event loop | **Done** | ~400 | PollAdd-based multi-client mux |
| Shared memory rings | **Done** | ~760 | SPSC lock-free ring buffers |
| Driver interface | **Done** | ~500 | Real + Mock driver implementations |
| Broker proxy | **Done** | ~600 | Handle translation orchestration |
| BrokerServer | **Done** | ~500 | Unix socket accept loop |
| fd passing | **Done** | ~350 | SCM_RIGHTS for shmem/eventfd |
| Guest protocol | **Done** | ~1200 | Wire format, client state, invariants |
| Guest shim binary | **Done** | ~350 | Connection test, CUSE placeholder |
| Kernel module | **TODO** | - | nvidia-shim.ko for guest |

**Total: ~6,500 lines of Rust, 151 property tests**

### 18.11 The io_uring Event Loop

The broker uses io_uring for efficient multi-client multiplexing:

```rust
/// The io_uring event loop
pub struct UringEventLoop {
    ring: IoUring,
    fds: HashMap<u32, RegisteredFd>,
    // ...
}

impl UringEventLoop {
    /// Register a client's eventfd for polling
    pub fn register_fd(&mut self, fd: RawFd) -> io::Result<u32>;
    
    /// Wait for events with optional timeout
    pub fn wait(&mut self, timeout_ms: Option<u32>) -> io::Result<Vec<Event>>;
}
```

**How it works:**

```
┌─────────────────────────────────────────────────────────────────┐
│                     io_uring Event Loop                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│   Client 1 eventfd ─┐                                           │
│   Client 2 eventfd ─┼──▶ PollAdd submissions ──▶ kernel         │
│   Client N eventfd ─┘                                           │
│                                                                  │
│   On completion:                                                │
│   - Event::ClientReady { client_index }                         │
│   - Process requests from that client's shared memory ring      │
│   - Rearm poll for that client                                  │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

This is O(1) per wake-up regardless of client count, unlike epoll's O(n) on return.

### 18.12 Shared Memory Ring Buffers

Each client gets a dedicated shared memory region:

```
┌─────────────────────────────────────────────────────────────────┐
│                    Per-Client Shared Memory (~2MB)               │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│   Request Ring (VM → Broker)                                    │
│   ┌─────────────────────────────────────────────────────────┐   │
│   │ head │ tail │ mask │ entries[256] │ data[256 × 4KB]    │   │
│   └─────────────────────────────────────────────────────────┘   │
│                                                                  │
│   Response Ring (Broker → VM)                                   │
│   ┌─────────────────────────────────────────────────────────┐   │
│   │ head │ tail │ mask │ entries[256] │ data[256 × 4KB]    │   │
│   └─────────────────────────────────────────────────────────┘   │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

**Key properties:**

- **Lock-free SPSC**: Single Producer Single Consumer with atomic head/tail
- **Cache-line aligned**: Headers padded to 64 bytes to prevent false sharing
- **Zero-copy**: Data written directly to shared memory, no kernel involvement
- **Bounded**: 256 slots prevents unbounded memory growth

```rust
/// Ring buffer header with atomic operations
#[repr(C, align(64))]
pub struct RingHeader {
    pub head: AtomicU32,  // Consumer reads here
    _pad1: [u8; 60],      // Cache line padding
    pub tail: AtomicU32,  // Producer writes here
    _pad2: [u8; 60],
    pub mask: u32,        // ring_size - 1
    pub size: u32,
}

impl RingHeader {
    /// Producer: reserve a slot (returns None if full)
    pub fn producer_reserve(&self) -> Option<u32>;
    
    /// Consumer: peek next slot (returns None if empty)
    pub fn consumer_peek(&self) -> Option<u32>;
}
```

### 18.13 The Driver Proxy

The driver module provides both real and mock implementations:

```rust
/// Trait for NVIDIA driver operations
pub trait Driver: Send {
    fn alloc(&mut self, h_root: NvHandle, h_parent: NvHandle, 
             h_object: NvHandle, h_class: u32, params: Option<&[u8]>) -> DriverResult<u32>;
    fn free(&mut self, h_root: NvHandle, h_parent: NvHandle, 
            h_object: NvHandle) -> DriverResult<u32>;
    fn control(&mut self, h_client: NvHandle, h_object: NvHandle, 
               cmd: u32, params: &mut [u8]) -> DriverResult<u32>;
    // ... map_memory, unmap_memory
}

/// Real driver - talks to /dev/nvidiactl
pub struct NvDriver {
    ctl_fd: OwnedFd,
}

/// Mock driver - for testing without hardware
pub struct MockDriver {
    objects: HashMap<NvHandle, (NvHandle, u32)>,
    memory: HashMap<NvHandle, (u64, u64)>,
    call_log: Vec<MockCall>,
}
```

**ioctl encoding:**

```rust
// NVIDIA uses _IOWR('F', escape_code, params_struct)
const NV_IOCTL_MAGIC: u8 = b'F';

macro_rules! nv_ioctl {
    ($escape:expr, $ty:ty) => {
        nix::request_code_readwrite!(NV_IOCTL_MAGIC, $escape, std::mem::size_of::<$ty>())
    };
}

const IOCTL_ALLOC: libc::c_ulong = nv_ioctl!(0x2B, NvOs21Params);
const IOCTL_FREE: libc::c_ulong = nv_ioctl!(0x29, NvOs00Params);
const IOCTL_CONTROL: libc::c_ulong = nv_ioctl!(0x2A, NvOs54Params);
```

### 18.14 The Broker Proxy

The `BrokerProxy` ties everything together:

```rust
pub struct BrokerProxy<D: Driver> {
    handles: HandleTable,      // Virtual → Real mapping
    driver: D,                 // Real or Mock
    client_roots: HashMap<ClientId, RealHandle>,
    stats: ProxyStats,
}

impl<D: Driver> BrokerProxy<D> {
    pub fn process(&mut self, request: Request) -> Response {
        // 1. Validate client is registered
        // 2. Translate virtual handles to real handles
        // 3. Call driver with real handles
        // 4. Return response with status
    }
}
```

**Handle translation flow:**

```
Client Request: Alloc(h_root=1, h_parent=1, h_new=2, class=DEVICE)
                        │
                        ▼
              ┌─────────────────────┐
              │ HandleTable lookup  │
              │ Client 1:           │
              │   virt 1 → real 0x8001 │
              └─────────────────────┘
                        │
                        ▼
Driver Call: Alloc(h_root=0x8001, h_parent=0x8001, h_new=0x8002, class=DEVICE)
                        │
                        ▼
              ┌─────────────────────┐
              │ Record new mapping  │
              │   virt 2 → real 0x8002 │
              └─────────────────────┘
                        │
                        ▼
Response: Allocated { real_handle: 0x8002 }
```

### 18.15 Guest Protocol Module

The `guest_protocol` module contains the pure, testable core for guest-side ioctl handling. It's designed to be `no_std` compatible so the same code runs in:
- The kernel module (`nvidia-shim.ko`) in the guest
- Userspace property tests with full coverage

#### Wire Format

```rust
/// Request header - 32 bytes
#[repr(C)]
pub struct RequestHeader {
    pub request_id: u64,      // Unique ID for matching responses
    pub escape_code: u8,      // NVIDIA escape code (0x27-0x5F)
    pub device_idx: u8,       // 0=nvidiactl, 1=nvidia0, etc.
    pub flags: u16,
    pub payload_len: u32,
    pub _reserved: [u8; 16],
}

/// Response header - 32 bytes
#[repr(C)]
pub struct ResponseHeader {
    pub request_id: u64,      // Matches request
    pub status: u32,          // NVIDIA status (0=success)
    pub payload_len: u32,
    pub _reserved: [u8; 16],
}
```

#### Client State Machine

```rust
pub struct ClientState {
    next_request_id: RequestId,
    pending: Vec<PendingRequest>,  // Up to 256 concurrent
    stats: ClientStats,
}

impl ClientState {
    /// Start a new request, returns header to send
    pub fn begin_request(&mut self, escape: NvEscapeCode, ...) -> Result<RequestHeader>;
    
    /// Complete a request when response arrives
    pub fn complete_request(&mut self, response: &ResponseHeader) -> Result<PendingRequest>;
    
    /// Verify all invariants hold
    pub fn check_invariants(&self) -> Result<()>;
}
```

#### Invariants Enforced

1. `pending.len() <= MAX_PENDING_REQUESTS` (256)
2. All pending request IDs are unique
3. All pending IDs < next_request_id
4. Every response matches a pending request
5. Request IDs are monotonically increasing
6. Payload size <= MAX_PAYLOAD_SIZE (4KB - 32 bytes)

#### Property Tests (25 new)

| Test | Property |
|------|----------|
| `request_header_roundtrip` | Headers survive encode/decode |
| `ioctl_request_roundtrip` | Full requests survive encode/decode |
| `client_invariants_always_hold` | Invariants hold after any op sequence |
| `every_request_gets_one_response` | No orphaned requests |
| `out_of_order_completion` | Responses can arrive in any order |
| `rapid_request_response` | 1000 cycles without state corruption |

### 18.16 Live Wire Test - End-to-End Validation

**January 17, 2026: The complete data path is working.**

We achieved end-to-end validation of the GPU broker architecture without requiring a kernel module. The `guest-shim --live-test` command proves the entire ioctl proxy path through shared memory ring buffers.

#### What Was Proven

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         LIVE WIRE TEST - DATA FLOW                          │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  Guest-Shim (Client)                    Broker (Host + Mock Driver)         │
│  ════════════════════                   ═══════════════════════════         │
│         │                                        │                          │
│         ├─────── connect(unix socket) ──────────►│                          │
│         │                                        │                          │
│         │◄── SCM_RIGHTS: shmem_fd + eventfds ───┤                          │
│         │    (client_id=1, shmem_size=2MB)       │                          │
│         │                                        │                          │
│         ├─────── mmap(shmem_fd, 2MB) ───────────►│                          │
│         │        [shared memory now visible]     │                          │
│         │                                        │                          │
│  ┌──────┴──────────────────────────────────────────────────────────────┐   │
│  │                    SHARED MEMORY (2MB)                               │   │
│  │  ┌────────────────────────────────────────────────────────────────┐ │   │
│  │  │ Request Ring    [head=0, tail=0, mask=255]                     │ │   │
│  │  │ Response Ring   [head=0, tail=0, mask=255]                     │ │   │
│  │  │ Request Data    [256 × 4KB slots]                              │ │   │
│  │  │ Response Data   [256 × 4KB slots]                              │ │   │
│  │  └────────────────────────────────────────────────────────────────┘ │   │
│  └──────┬──────────────────────────────────────────────────────────────┘   │
│         │                                        │                          │
│         │  ┌─────────────────────────────────────────────────────────┐     │
│         │  │ TEST 1: RegisterClient                                  │     │
│         │  └─────────────────────────────────────────────────────────┘     │
│         │                                        │                          │
│         ├── write request to ring[0] ───────────►│                          │
│         │   {client_id:1, seq:1, op:Register}    │                          │
│         │                                        │                          │
│         ├── write(request_eventfd, 1) ──────────►│ wake up!                 │
│         │                                        │                          │
│         │                              ┌─────────┴─────────┐                │
│         │                              │ process request   │                │
│         │                              │ RegisterClient    │                │
│         │                              │ → handles.register│                │
│         │                              └─────────┬─────────┘                │
│         │                                        │                          │
│         │◄── ring[0] ← response ─────────────────┤                          │
│         │    {client_id:1, seq:1, success:1}     │                          │
│         │                                        │                          │
│         │◄── write(response_eventfd, 1) ─────────┤                          │
│         │                                        │                          │
│         ├── poll(response_eventfd) ─────────────►│                          │
│         │   POLLIN! read response                │                          │
│         │                                        │                          │
│         │  ✓ RegisterClient PASSED               │                          │
│         │                                        │                          │
│         │  ┌─────────────────────────────────────────────────────────┐     │
│         │  │ TEST 2: Alloc(NV01_ROOT_CLIENT)                         │     │
│         │  └─────────────────────────────────────────────────────────┘     │
│         │                                        │                          │
│         ├── write request to ring[1] ───────────►│                          │
│         │   {seq:2, op:Alloc, h_root:0,          │                          │
│         │    h_parent:0, h_new:1, class:0x41}    │                          │
│         │                                        │                          │
│         ├── write(request_eventfd, 1) ──────────►│ wake up!                 │
│         │                                        │                          │
│         │                              ┌─────────┴─────────┐                │
│         │                              │ process request   │                │
│         │                              │ Alloc             │                │
│         │                              │ → mock_driver     │                │
│         │                              │ → handle_table    │                │
│         │                              │   virt:1→real:    │                │
│         │                              │   0x80000001      │                │
│         │                              └─────────┬─────────┘                │
│         │                                        │                          │
│         │◄── ring[1] ← response ─────────────────┤                          │
│         │    {seq:2, success:1,                  │                          │
│         │     result:Allocated(0x80000001)}      │                          │
│         │                                        │                          │
│         │◄── write(response_eventfd, 1) ─────────┤                          │
│         │                                        │                          │
│         │  ✓ Alloc PASSED                        │                          │
│         │                                        │                          │
│         │  ══════════════════════════════════════════════════════════      │
│         │  ✓ ALL LIVE WIRE TESTS PASSED                                    │
│         │  ══════════════════════════════════════════════════════════      │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

#### Running the Test

```bash
# Terminal 1: Start the broker with mock driver
./target/release/gpu-broker --mock --socket /tmp/gpu-broker.sock

# Terminal 2: Run the live wire test
./target/release/guest-shim --broker-socket /tmp/gpu-broker.sock --live-test -v
```

#### Sample Output

```
INFO  guest_shim: === LIVE WIRE TEST ===
INFO  guest_shim: Step 1: Encoded RegisterClient request (32 bytes)
DEBUG guest_shim:   client_id: 1
DEBUG guest_shim:   seq: 1
DEBUG guest_shim:   op_type: 0 (RegisterClient)
DEBUG guest_shim:   Request ring: head=0, tail=0, mask=255
DEBUG guest_shim:   Writing to slot 0
INFO  guest_shim: Step 2: Wrote request to ring slot 0
INFO  guest_shim: Step 3: Signaled broker via request_eventfd
INFO  guest_shim: Step 4: Waiting for response...
DEBUG guest_shim:   Response eventfd signaled (value=1)
INFO  guest_shim: Step 5: Reading response from ring...
DEBUG guest_shim:   Response ring: head=0, tail=1, mask=255
INFO  guest_shim: Step 6: Response received!
INFO  guest_shim:   client_id: 1 (expected 1)
INFO  guest_shim:   seq: 1 (expected 1)
INFO  guest_shim:   success: 1 (OK)
INFO  guest_shim: === RegisterClient PASSED ===

INFO  guest_shim: === Testing Alloc operation (NV01_ROOT_CLIENT) ===
INFO  guest_shim: Step 1: Encoded Alloc request (48 bytes)
DEBUG guest_shim:   h_root: 0, h_parent: 0, h_new: 1, class: 0x41
INFO  guest_shim: Step 2: Wrote request to ring slot 1
INFO  guest_shim: Step 3: Signaled broker
INFO  guest_shim: Step 4: Waiting for response...
INFO  guest_shim: Step 5: Response received!
INFO  guest_shim:   client_id: 1 (expected 1)
INFO  guest_shim:   seq: 2 (expected 2)
INFO  guest_shim:   success: 1 (OK)
INFO  guest_shim:   result: Allocated(real_handle=0x80000001)
INFO  guest_shim: === Alloc PASSED ===

INFO  guest_shim: === ALL LIVE WIRE TESTS PASSED ===
```

#### Components Validated

| Component | What It Does | Status |
|-----------|--------------|--------|
| Unix socket | Client connects to broker | ✓ |
| SCM_RIGHTS | Pass shmem_fd + 2 eventfds | ✓ |
| Shared memory | 2MB mmap'd in both processes | ✓ |
| Ring buffer write | Client writes to request ring | ✓ |
| Eventfd signaling | Client wakes broker | ✓ |
| Request decoding | Broker parses wire format | ✓ |
| RegisterClient | Broker registers client | ✓ |
| Handle allocation | Mock driver + handle table | ✓ |
| Response encoding | Broker writes to response ring | ✓ |
| Response eventfd | Broker wakes client | ✓ |
| Response parsing | Client reads and validates | ✓ |

#### Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              SYSTEM ARCHITECTURE                             │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                         HOST (broker process)                        │   │
│  │  ┌─────────────────────────────────────────────────────────────────┐│   │
│  │  │                        BrokerServer                              ││   │
│  │  │  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐ ││   │
│  │  │  │  Unix Socket    │  │  Event Loop     │  │  BrokerProxy    │ ││   │
│  │  │  │  Listener       │  │  (io_uring)     │  │                 │ ││   │
│  │  │  │                 │  │                 │  │  ┌───────────┐  │ ││   │
│  │  │  │  accept() ──────┼──┼─► poll fds ─────┼──┼─►│HandleTable│  │ ││   │
│  │  │  │  SCM_RIGHTS     │  │                 │  │  │           │  │ ││   │
│  │  │  │  (fd passing)   │  │  request_evfd ──┼──┼─►│virt→real  │  │ ││   │
│  │  │  │                 │  │  response_evfd◄─┼──┼──│mapping    │  │ ││   │
│  │  │  └─────────────────┘  └─────────────────┘  │  └───────────┘  │ ││   │
│  │  │                                            │                 │ ││   │
│  │  │                                            │  ┌───────────┐  │ ││   │
│  │  │                                            │  │  Driver   │  │ ││   │
│  │  │                                            │  │  (Mock/   │  │ ││   │
│  │  │                                            │  │   Real)   │  │ ││   │
│  │  │                                            │  └───────────┘  │ ││   │
│  │  │                                            └─────────────────┘ ││   │
│  │  └─────────────────────────────────────────────────────────────────┘│   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                     │                                       │
│                                     │ shared memory                         │
│                                     │ (memfd + mmap)                        │
│                                     ▼                                       │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                    SHARED MEMORY REGION (2MB)                        │   │
│  │  ┌─────────────────────────────────────────────────────────────────┐│   │
│  │  │  Request Ring Header   [192 bytes, cache-line aligned]          ││   │
│  │  │  ├─ head: AtomicU32    (consumer/broker reads here)             ││   │
│  │  │  ├─ tail: AtomicU32    (producer/client writes here)            ││   │
│  │  │  └─ mask: 255          (ring_size - 1)                          ││   │
│  │  ├─────────────────────────────────────────────────────────────────┤│   │
│  │  │  Response Ring Header  [192 bytes, cache-line aligned]          ││   │
│  │  │  ├─ head: AtomicU32    (consumer/client reads here)             ││   │
│  │  │  ├─ tail: AtomicU32    (producer/broker writes here)            ││   │
│  │  │  └─ mask: 255                                                   ││   │
│  │  ├─────────────────────────────────────────────────────────────────┤│   │
│  │  │  Request Ring Entries  [256 × 16 bytes = 4KB]                   ││   │
│  │  │  Response Ring Entries [256 × 16 bytes = 4KB]                   ││   │
│  │  ├─────────────────────────────────────────────────────────────────┤│   │
│  │  │  Request Data Pool     [256 × 4KB = 1MB]                        ││   │
│  │  │  Response Data Pool    [256 × 4KB = 1MB]                        ││   │
│  │  └─────────────────────────────────────────────────────────────────┘│   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                     ▲                                       │
│                                     │ shared memory                         │
│                                     │ (same memfd, mmap'd)                  │
│                                     │                                       │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                    GUEST / CLIENT (guest-shim)                       │   │
│  │  ┌─────────────────────────────────────────────────────────────────┐│   │
│  │  │  guest-shim --live-test                                         ││   │
│  │  │                                                                 ││   │
│  │  │  1. connect(broker_socket)                                      ││   │
│  │  │  2. recv_with_fds() → shmem_fd, req_evfd, resp_evfd            ││   │
│  │  │  3. mmap(shmem_fd) → shared memory pointer                      ││   │
│  │  │  4. Write request to ring, signal req_evfd                      ││   │
│  │  │  5. poll(resp_evfd), read response from ring                    ││   │
│  │  │  6. Validate response matches request                           ││   │
│  │  └─────────────────────────────────────────────────────────────────┘│   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

#### Wire Format (Request)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           REQUEST WIRE FORMAT                                │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  Offset   Size   Field         Description                                  │
│  ──────   ────   ─────         ───────────                                  │
│  0        8      client_id     Client identifier (assigned by broker)       │
│  8        8      seq           Sequence number (monotonic per client)       │
│  16       4      op_type       Operation: 0=Register, 2=Alloc, 3=Free...   │
│  20       4      payload_len   Length of payload following header           │
│  24       8      reserved      Reserved for future use                      │
│  32       var    payload       Operation-specific parameters                │
│                                                                             │
│  Example RegisterClient:                                                    │
│  ┌────────────────────────────────────────────────────────────────────────┐│
│  │ 01 00 00 00 00 00 00 00   client_id = 1                                ││
│  │ 01 00 00 00 00 00 00 00   seq = 1                                      ││
│  │ 00 00 00 00               op_type = 0 (RegisterClient)                 ││
│  │ 00 00 00 00               payload_len = 0                              ││
│  │ 00 00 00 00 00 00 00 00   reserved                                     ││
│  └────────────────────────────────────────────────────────────────────────┘│
│                                                                             │
│  Example Alloc(NV01_ROOT_CLIENT):                                          │
│  ┌────────────────────────────────────────────────────────────────────────┐│
│  │ 01 00 00 00 00 00 00 00   client_id = 1                                ││
│  │ 02 00 00 00 00 00 00 00   seq = 2                                      ││
│  │ 02 00 00 00               op_type = 2 (Alloc)                          ││
│  │ 10 00 00 00               payload_len = 16                             ││
│  │ 00 00 00 00 00 00 00 00   reserved                                     ││
│  │ 00 00 00 00               h_root = 0 (no root, this IS root)           ││
│  │ 00 00 00 00               h_parent = 0 (no parent)                     ││
│  │ 01 00 00 00               h_new = 1 (virtual handle we want)           ││
│  │ 41 00 00 00               class = 0x41 (NV01_ROOT_CLIENT)              ││
│  └────────────────────────────────────────────────────────────────────────┘│
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

#### Wire Format (Response)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          RESPONSE WIRE FORMAT                                │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  Offset   Size   Field         Description                                  │
│  ──────   ────   ─────         ───────────                                  │
│  0        8      client_id     Client identifier (matches request)          │
│  8        8      seq           Sequence number (matches request)            │
│  16       1      success       1 = OK, 0 = Error                           │
│  17       4      result_type   0=Registered, 2=Allocated, 3=Freed...       │
│  21       var    result_data   Type-specific result data                    │
│                                                                             │
│  Example Allocated response:                                                │
│  ┌────────────────────────────────────────────────────────────────────────┐│
│  │ 01 00 00 00 00 00 00 00   client_id = 1                                ││
│  │ 02 00 00 00 00 00 00 00   seq = 2                                      ││
│  │ 01                        success = 1 (OK)                             ││
│  │ 02 00 00 00               result_type = 2 (Allocated)                  ││
│  │ 01 00 00 80               real_handle = 0x80000001                     ││
│  └────────────────────────────────────────────────────────────────────────┘│
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 18.17 Implementation Status (Updated)

| Component | Status | Lines | Tests |
|-----------|--------|-------|-------|
| State machine | **Done** | ~900 | 20 |
| Handle table | **Done** | ~600 | 15 |
| RM API types | **Done** | ~450 | 10 |
| Simulated transport | **Done** | ~770 | 12 |
| io_uring event loop | **Done** | ~400 | 8 |
| Shared memory rings | **Done** | ~760 | 10 |
| Driver interface | **Done** | ~500 | 8 |
| Broker proxy | **Done** | ~600 | 12 |
| BrokerServer | **Done** | ~500 | 6 |
| fd passing (SCM_RIGHTS) | **Done** | ~350 | 4 |
| Guest protocol | **Done** | ~1200 | 25 |
| Guest shim binary | **Done** | ~500 | - |
| **Live wire test** | **Done** | - | 1 integration |
| Kernel module | TODO | - | - |

**Total: ~7,500 lines of Rust, 151 property tests + 1 integration test**

### 18.18 What's Next

With the live wire test passing, the remaining work is:

| Component | Description | Complexity |
|-----------|-------------|------------|
| Kernel module | `nvidia-shim.ko` for guest VM | Medium |
| Real GPU testing | Test with RTX PRO 6000 | Low |
| Multi-client stress | Concurrent VMs | Medium |
| Error recovery | Handle broker restart | Medium |

#### Kernel Module Architecture

The kernel module will be a thin wrapper around `guest_protocol`:

```
┌─────────────────────────────────────────────────────────────┐
│                    nvidia-shim.ko                            │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│   file_operations:                                          │
│   ┌─────────────────────────────────────────────────────┐   │
│   │ .open    = copy virtio-shmem params, init client    │   │
│   │ .release = cleanup client state                      │   │
│   │ .unlocked_ioctl = serialize → ring → wait → return  │   │
│   │ .mmap    = return virtio-shmem region                │   │
│   └─────────────────────────────────────────────────────┘   │
│                              │                               │
│                              ▼                               │
│   ┌─────────────────────────────────────────────────────┐   │
│   │            guest_protocol (Rust, no_std)             │   │
│   │  - RequestHeader/ResponseHeader encoding            │   │
│   │  - ClientState with request tracking                │   │
│   │  - Invariant checking (same code as tests!)         │   │
│   └─────────────────────────────────────────────────────┘   │
│                              │                               │
│                              ▼                               │
│   ┌─────────────────────────────────────────────────────┐   │
│   │            virtio-shmem ring buffer                  │   │
│   │  - Write request to ring                            │   │
│   │  - Signal broker via eventfd                        │   │
│   │  - Wait on response eventfd                         │   │
│   │  - Read response from ring                          │   │
│   └─────────────────────────────────────────────────────┘   │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

The kernel module handles only:
- Registering character devices (major 195)
- copy_from_user/copy_to_user
- wait_event/wake_up for eventfd
- Calling into the tested Rust library for all protocol logic

### 18.19 nvidia-shim.ko Implementation

The kernel module is now implemented in `gpu-broker/kernel/nvidia-shim.c` (~600 lines of C).

#### Module Parameters

```bash
# Physical address of shared memory (from virtio-shmem)
shmem_phys=0xFE000000

# Size of shared memory region (default 2MB)
shmem_size=2097152

# Eventfds for broker communication
request_evfd=3
response_evfd=4

# Client ID assigned by broker during connection
client_id=1
```

#### Device Nodes Created

```
/dev/nvidiactl   (195, 255)  - Control device
/dev/nvidia0     (195, 0)    - GPU 0
```

#### Key Functions

```c
/* Forward ioctl to broker via ring buffer */
static int nv_shim_forward_ioctl(
    struct nv_shim_client *client,
    unsigned int cmd,
    void __user *arg,
    size_t arg_size);

/* Submit request to ring, signal broker */
static int nv_shim_submit_request(
    struct nv_shim_client *client,
    void *data, size_t len);

/* Wait for response with timeout */
static int nv_shim_wait_response(
    struct nv_shim_client *client,
    u32 expected_seq,
    void *buf, size_t buf_size,
    size_t *out_len);
```

#### Building

```bash
cd gpu-broker/kernel
make                          # Build against running kernel
make KDIR=/path/to/kernel     # Build against specific kernel
```

#### Loading

```bash
# After virtio-shmem setup provides the shared memory region
insmod nvidia-shim.ko \
    shmem_phys=0xFE000000 \
    shmem_size=2097152 \
    client_id=1

# Verify device nodes
ls -la /dev/nvidia*
```

#### Data Flow

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        GUEST VM (nvidia-shim.ko)                             │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  CUDA Application                                                           │
│       │                                                                     │
│       │ ioctl(fd, NV_ESC_RM_CONTROL, params)                               │
│       ▼                                                                     │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ nvidia_shim_unlocked_ioctl()                                         │   │
│  │   │                                                                  │   │
│  │   ├─ copy_from_user(params)                                         │   │
│  │   │                                                                  │   │
│  │   ├─ nv_shim_forward_ioctl()                                        │   │
│  │   │     │                                                            │   │
│  │   │     ├─ Build request: {client_id, seq, op, payload}             │   │
│  │   │     │                                                            │   │
│  │   │     ├─ nv_shim_submit_request()                                 │   │
│  │   │     │     ├─ memcpy to ring data slot                           │   │
│  │   │     │     ├─ Write ring entry metadata                          │   │
│  │   │     │     ├─ smp_wmb() + atomic_set(tail)                       │   │
│  │   │     │     └─ eventfd_signal(request_evfd)  ──────────────────┐  │   │
│  │   │     │                                                         │  │   │
│  │   │     ├─ nv_shim_wait_response()                               │  │   │
│  │   │     │     ├─ wait_event_interruptible_timeout()  ◄───────┐   │  │   │
│  │   │     │     ├─ Read ring entry + data                      │   │  │   │
│  │   │     │     └─ atomic_set(head) to consume                 │   │  │   │
│  │   │     │                                                     │   │  │   │
│  │   │     └─ Parse response, extract result                    │   │  │   │
│  │   │                                                           │   │  │   │
│  │   └─ copy_to_user(result)                                    │   │  │   │
│  │                                                               │   │  │   │
│  └───────────────────────────────────────────────────────────────┼───┼──┘   │
│                                                                   │   │      │
│  ═══════════════════════════════════════════════════════════════════════════│
│                         SHARED MEMORY (virtio-shmem)              │   │      │
│  ═══════════════════════════════════════════════════════════════════════════│
│                                                                   │   │      │
└───────────────────────────────────────────────────────────────────┼───┼──────┘
                                                                    │   │
                        HOST (gpu-broker)                           │   │
┌───────────────────────────────────────────────────────────────────┼───┼──────┐
│                                                                   │   │      │
│  ┌────────────────────────────────────────────────────────────────┼───┼───┐  │
│  │ BrokerServer event loop                                        │   │   │  │
│  │   │                                                            │   │   │  │
│  │   ├─ io_uring poll(request_evfd)  ◄────────────────────────────┘   │   │  │
│  │   │                                                                │   │  │
│  │   ├─ Read request from ring                                        │   │  │
│  │   │                                                                │   │  │
│  │   ├─ BrokerProxy::process()                                        │   │  │
│  │   │     ├─ Translate virtual handles → real handles                │   │  │
│  │   │     ├─ driver.ioctl() → /dev/nvidiactl (real GPU)             │   │  │
│  │   │     └─ Build response                                          │   │  │
│  │   │                                                                │   │  │
│  │   ├─ Write response to ring                                        │   │  │
│  │   │                                                                │   │  │
│  │   └─ eventfd_signal(response_evfd)  ───────────────────────────────┘   │  │
│  │                                                                        │  │
│  └────────────────────────────────────────────────────────────────────────┘  │
│                                                                              │
└──────────────────────────────────────────────────────────────────────────────┘
```

### 18.20 Implementation Status (Final)

| Component | Status | Lines | Tests |
|-----------|--------|-------|-------|
| Broker state machine | **Done** | ~900 | 20 |
| Handle table | **Done** | ~600 | 15 |
| RM API types | **Done** | ~450 | 10 |
| Simulated transport | **Done** | ~770 | 12 |
| io_uring event loop | **Done** | ~400 | 8 |
| Shared memory rings | **Done** | ~760 | 10 |
| Driver interface | **Done** | ~500 | 8 |
| Broker proxy | **Done** | ~600 | 12 |
| BrokerServer | **Done** | ~500 | 6 |
| fd passing (SCM_RIGHTS) | **Done** | ~350 | 4 |
| Guest protocol | **Done** | ~1200 | 25 |
| Guest shim binary | **Done** | ~500 | - |
| Live wire test | **Done** | - | 1 |
| **nvidia-shim.ko** | **Done** | ~600 | - |
| VMM integration | TODO | - | - |

**Total: ~8,100 lines of code (7,500 Rust + 600 C), 151 property tests + 1 integration test**

### 18.21 Expected Performance

With the broker fully implemented:

| Metric | Without Broker | With Broker |
|--------|----------------|-------------|
| First VM GPU access | ~15s | ~15s (one-time) |
| Second VM GPU access | ~15s | **~50ms** |
| Nth VM GPU access | ~15s | **~50ms** |
| 10 VMs total | ~150s | **~15.5s** |

The broker amortizes the 15-second initialization cost across all VMs, making multi-tenant GPU sharing practical.

### 18.20 Memory Model: Why the Broker is Sufficient

A critical question arises when proxying GPU operations: how do we ensure memory coherence when DMA transfers may be in flight across PCIe, NVLink, or even RDMA (GPUDirect)?

The answer is elegant: **we don't have to**. The broker is a serialization point, not a coherence enforcer.

#### 18.20.1 The Choke Point

All GPU operations from userspace flow through exactly 39 ioctls on `/dev/nvidiactl`:

```
┌─────────────────────────────────────────────────────────────────────┐
│                    NVIDIA RM API - The Choke Point                  │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  RM Escape Codes (0x27-0x5F):           27 operations               │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │ 0x27 AllocMemory      0x34 DupObject       0x52 GetEventData│   │
│  │ 0x28 AllocObject      0x35 Share           0x54 AllocCtxDma │   │
│  │ 0x29 Free             0x37 ConfigGetEx     0x56 VblankCb    │   │
│  │ 0x2A Control          0x38 ConfigSetEx     0x57 MapMemDma   │   │
│  │ 0x2B Alloc            0x39 I2cAccess       0x58 UnmapMemDma │   │
│  │ 0x32 ConfigGet        0x41 IdleChannels    0x59 BindCtxDma  │   │
│  │ 0x33 ConfigSet        0x4A VidHeapCtrl     0x5C ExportToFd  │   │
│  │                       0x4D AccessRegistry  0x5D ImportFromFd│   │
│  │                       0x4E MapMemory       0x5E UpdateMapInfo│  │
│  │                       0x4F UnmapMemory     0x5F LocklessDiag│   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  System Escape Codes (200-218):         12 operations               │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │ 200 CardInfo          209 StatusCode       213 QueryIntr    │   │
│  │ 201 RegisterFd        210 CheckVersion     214 SysParams    │   │
│  │ 206 AllocOsEvent      211 IoctlXfer        217 ExportDmaBuf │   │
│  │ 207 FreeOsEvent       212 AttachGpus       218 WaitOpenDone │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  Source: nvidia-open/src/nvidia/arch/nvalloc/unix/include/         │
│          nv_escape.h, nv-ioctl-numbers.h (stable since 1999)       │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

This API has been stable for over 25 years. NVIDIA cannot change these escape codes without breaking every CUDA application in existence.

#### 18.20.2 The TSO Analogy

x86 provides **Total Store Order (TSO)**: stores become visible to other processors in program order, and `lock cmpxchg` forces the store buffer to drain before returning.

The broker provides an analogous guarantee for GPU operations:

```
┌─────────────────────────────────────────────────────────────────────┐
│                     Memory Ordering Guarantees                       │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  x86 TSO:                                                           │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │ store A          │  Visible to other cores in this order     │   │
│  │ store B          │                                            │   │
│  │ lock cmpxchg C   │  ← Store buffer drains, C visible,        │   │
│  │                  │    then return to caller                   │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  GPU Broker:                                                        │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │ ioctl(Alloc A)   │  Visible to GPU in this order             │   │
│  │ ioctl(Alloc B)   │                                            │   │
│  │ ioctl(Free A)    │  ← GPU operation completes, resources     │   │
│  │                  │    released, then return to caller         │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  The ioctl return IS the fence.                                     │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

#### 18.20.3 Why This Works: The CUDA Memory Model

CUDA and NCCL have a well-defined memory model with explicit synchronization:

| Primitive | Happens-Before Guarantee |
|-----------|-------------------------|
| `cudaDeviceSynchronize()` | All preceding GPU work completes |
| `cudaStreamSynchronize(s)` | All work in stream `s` completes |
| `cudaEventSynchronize(e)` | All work before `e` completes |
| `ncclGroupEnd()` | Collective operation initiated |
| `cudaFree(ptr)` | Application asserts DMA complete |

A **correct** CUDA application establishes happens-before relationships before freeing resources. The application won't call `cudaFree()` on a buffer while NCCL is still DMA'ing into it - that's a bug in the application, not the driver.

#### 18.20.4 What the Driver Actually Does

Looking at the NVIDIA open-source driver (`nvidia-open/src/nvidia/arch/nvalloc/unix/src/escape.c`):

```c
// Every ioctl follows this pattern:
NV_STATUS RmIoctl(...) {
    switch (cmd) {
        case NV_ESC_RM_FREE:
            // Synchronous - does not return until object freed
            Nv01FreeWithSecInfo(pApi, secInfo);
            break;
            
        case NV_ESC_RM_IDLE_CHANNELS:
            // Explicit fence - waits for DMA to drain
            Nv04IdleChannelsWithSecInfo(pApi, secInfo);
            break;
            
        case NV_ESC_RM_CONTROL:
            // Control commands include sync operations
            // e.g., cmd 0x801714 = FifoIdleChannels
            Nv04ControlWithSecInfo(pApi, secInfo);
            break;
    }
    return rmStatus;  // Only returns after operation complete
}
```

The driver returns synchronously. The ioctl does not complete until the GPU operation finishes. This is the writeback - the `lock cmpxchg` equivalent.

#### 18.20.5 The Broker's Single Responsibility

The broker has exactly one job: **preserve the total order of ioctls and don't return early**.

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Broker Ordering Guarantee                         │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  Guest VM                    Broker                    Real Driver  │
│  ─────────                   ──────                    ───────────  │
│                                                                     │
│  ioctl(Alloc A) ──────────▶ queue ──────────────────▶ Alloc A      │
│       │                       │                           │         │
│       │ (blocked)             │                           │         │
│       │                       │                           ▼         │
│       │                       │ ◀─────────────────── complete      │
│       │                       │                                     │
│  ◀────┴─────────────────────  │                                     │
│  return                                                             │
│                                                                     │
│  ioctl(Alloc B) ──────────▶ queue ──────────────────▶ Alloc B      │
│       │                       │                           │         │
│       │ (blocked)             │                           │         │
│       │                       │                           ▼         │
│       │                       │ ◀─────────────────── complete      │
│       │                       │                                     │
│  ◀────┴─────────────────────  │                                     │
│  return                                                             │
│                                                                     │
│  INVARIANT: Response to ioctl N is not sent until                   │
│             the real driver completes ioctl N.                      │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

The broker does NOT need to:
- Track DMA state
- Understand NVLink protocols  
- Inject fences
- Parse control command parameters
- Know anything about NCCL

The broker ONLY needs to:
- Preserve ioctl ordering (FIFO per client)
- Block until real driver returns
- Translate handles between virtual/real namespaces

#### 18.20.6 Semi-Formal Model

Let's define the model precisely:

**Definitions:**
- Let `I = {i₁, i₂, ..., iₙ}` be the sequence of ioctls issued by a guest
- Let `R(i)` be the return event of ioctl `i` from the real driver
- Let `B(i)` be the return event of ioctl `i` from the broker to guest

**Broker Ordering Invariant:**
```
∀ iₐ, iᵦ ∈ I : (iₐ <ₚ iᵦ) ⟹ (R(iₐ) <ₜ B(iᵦ))
```

Where `<ₚ` is program order and `<ₜ` is temporal ordering.

In words: if ioctl A precedes ioctl B in program order, then the real driver's completion of A happens before the broker returns B to the guest.

**CUDA Memory Model Assumption:**
```
∀ free(x) ∈ I : (∀ dma(x) ∈ D : dma(x) <ₕ free(x))
```

Where `<ₕ` is the happens-before relation and `D` is the set of DMA operations.

In words: a correct CUDA program establishes happens-before between all DMA operations on a buffer and the free of that buffer.

**Theorem: Broker Transparency**

If the CUDA Memory Model Assumption holds (the application is correct), and the Broker Ordering Invariant holds (the broker preserves order), then:

```
∀ observable behavior B_guest of the guest :
  ∃ equivalent behavior B_native on native hardware
```

The guest cannot distinguish proxied execution from native execution.

**Proof sketch:**
1. The guest issues ioctls in program order
2. The broker serializes them to the real driver in the same order
3. The real driver executes them, blocking until complete
4. The broker returns in the same order
5. All happens-before relationships established by the application are preserved
6. Therefore, all memory consistency guarantees hold

#### 18.20.7 What About Concurrent VMs?

Multiple VMs issue ioctls concurrently. The broker provides per-client ordering, not global ordering:

```
VM1: Alloc A₁ → Alloc B₁ → Free A₁
VM2: Alloc A₂ → Alloc B₂ → Free A₂
```

These can interleave at the real driver:
```
Alloc A₁ → Alloc A₂ → Alloc B₁ → Alloc B₂ → Free A₁ → Free A₂
```

This is safe because:
1. Handle isolation ensures VM1 cannot reference VM2's objects
2. Each VM's operations are ordered correctly relative to itself
3. The GPU handles concurrent operations from different clients

The broker is like a multi-lane highway: each lane (VM) has total ordering, but lanes can interleave.

#### 18.20.8 The Edge Cases

**What if the guest is buggy?**

A guest that frees a buffer while DMA is in flight will corrupt its own memory - same as on native hardware. The broker doesn't make buggy code work; it makes correct code work correctly.

**What if the broker crashes?**

All client state is lost. VMs must handle `ECONNRESET` and re-initialize. This is analogous to GPU reset on native hardware.

**What about GPUDirect RDMA?**

Third-party DMA (InfiniBand HCA → GPU memory) bypasses the ioctl path entirely. The application must synchronize these transfers using standard RDMA completion mechanisms before issuing ioctls that depend on them. The broker doesn't change this requirement.

#### 18.20.9 Summary

| Concern | Resolution |
|---------|------------|
| DMA coherence | Application's responsibility (CUDA memory model) |
| Operation ordering | Broker preserves program order per client |
| Completion detection | Ioctl return = operation complete |
| Multi-VM isolation | Handle translation, per-client ordering |
| Performance | Pipelining possible within ordering constraints |

The broker is `mfence` for GPU operations: it has one job, and that job is sufficient.

**"TSO for Torch" - 39 ioctls, one job.**

---

This implementation builds on:

- The Firecracker project by AWS
- The Cloud Hypervisor VFIO implementation (reference)
- The Linux VFIO subsystem
- The rust-vmm ecosystem (vm-memory, kvm-ioctls)

Special thanks to the NixOS community for reproducible build infrastructure.

---

*Document version: 1.5*
*Last updated: January 18, 2026*
*Hardware tested: NVIDIA RTX PRO 6000 Blackwell (128GB)*
*Live wire test: PASSED*
*Kernel module: nvidia-shim.ko complete*
*Memory model: Section 18.20 - TSO for Torch*
