# Isospin: Deep Glossary and Conceptual Refresher

## For the Hardware Engineer Returning to the Metal

You've done device drivers. You know the dance. But the abstractions have shifted - Rust's type system, the VFIO subsystem, KVM's device model. This document rebuilds the mental model from first principles.

---

## Part I: The Memory Hierarchy (What Changed)

### Physical vs Virtual vs Guest Physical vs Guest Virtual

```
┌─────────────────────────────────────────────────────────────────┐
│                        HOST KERNEL                               │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────────────┐  │
│  │ Host Virtual│───▶│Host Physical│◀───│ Device Physical     │  │
│  │   (HVA)     │    │   (HPA)     │    │ (DMA addresses)     │  │
│  └─────────────┘    └─────────────┘    └─────────────────────┘  │
│         │                 ▲                      ▲               │
│         │                 │                      │               │
│         │           ┌─────┴─────┐          ┌─────┴─────┐        │
│         │           │   MMU     │          │   IOMMU   │        │
│         │           └─────┬─────┘          └─────┬─────┘        │
│         │                 │                      │               │
│  ┌──────┴──────┐    ┌─────┴─────┐    ┌─────────┴───────────┐   │
│  │ KVM Memory  │    │Guest Phys │    │ IOVA (I/O Virtual   │   │
│  │   Slots     │───▶│  (GPA)    │◀───│    Address)         │   │
│  └─────────────┘    └─────┬─────┘    └─────────────────────┘   │
│                           │                                      │
│                     ┌─────┴─────┐                                │
│                     │ Guest MMU │ (EPT/NPT)                      │
│                     └─────┬─────┘                                │
│                           │                                      │
│                    ┌──────┴──────┐                               │
│                    │Guest Virtual│                               │
│                    │   (GVA)     │                               │
│                    └─────────────┘                               │
│                           │                                      │
│                      GUEST KERNEL                                │
└─────────────────────────────────────────────────────────────────┘
```

**The Key Insight:** When a GPU does DMA, it issues addresses. Those addresses must resolve to actual RAM. Without IOMMU, the GPU uses raw physical addresses (dangerous - GPU can read any memory). With IOMMU, we create a translation layer: GPU thinks it's accessing address X, IOMMU translates to physical address Y.

**VFIO's job:** Set up these IOMMU translations so the guest's view of memory matches what the GPU will DMA to/from.

### Address Types Reference

| Abbreviation | Full Name | Who Uses It | Translation Layer |
|--------------|-----------|-------------|-------------------|
| GVA | Guest Virtual Address | Guest userspace | Guest page tables |
| GPA | Guest Physical Address | Guest kernel | EPT/NPT (second-level paging) |
| HVA | Host Virtual Address | Host userspace (QEMU/Firecracker) | Host page tables |
| HPA | Host Physical Address | Hardware, DMA | None - this is real |
| IOVA | I/O Virtual Address | Devices via IOMMU | IOMMU page tables |

**Critical relationship for VFIO:**
```
GPA == IOVA  (we set this up deliberately)
     │
     ▼
    HPA (IOMMU translates IOVA → HPA)
```

When guest driver tells GPU "DMA to address 0x8000_0000", that's a GPA. We configure IOMMU so IOVA 0x8000_0000 maps to the same HPA that GPA 0x8000_0000 maps to. The GPU's DMA lands in the right guest memory.

---

## Part II: PCI Deep Dive

### The PCI Configuration Space

Every PCI device has a 256-byte (legacy) or 4096-byte (PCIe) configuration space. This is the device's "control panel."

```
Offset  Size  Field
──────────────────────────────────────────────────
0x00    2     Vendor ID (who made it)
0x02    2     Device ID (what model)
0x04    2     Command (enable memory, bus mastering)
0x06    2     Status (capabilities present, etc.)
0x08    1     Revision ID
0x09    3     Class Code (network, display, storage...)
0x0C    1     Cache Line Size
0x0D    1     Latency Timer
0x0E    1     Header Type (0=device, 1=bridge)
0x0F    1     BIST
0x10    4     BAR0 (Base Address Register)
0x14    4     BAR1
0x18    4     BAR2
0x1C    4     BAR3
0x20    4     BAR4
0x24    4     BAR5
0x28    4     CardBus CIS Pointer
0x2C    2     Subsystem Vendor ID
0x2E    2     Subsystem ID
0x30    4     Expansion ROM Base Address
0x34    1     Capabilities Pointer  ← linked list starts here
0x35    7     Reserved
0x3C    1     Interrupt Line
0x3D    1     Interrupt Pin
0x3E    1     Min Grant
0x3F    1     Max Latency
0x40+         Capabilities / PCIe Extended Config
```

### Base Address Registers (BARs)

BARs define memory or I/O regions the device exposes. A GPU typically has:

- **BAR0:** Control registers (MMIO) - 16-256 MB
- **BAR1:** Framebuffer / VRAM aperture - up to 256 MB visible window
- **BAR2:** (Optional) Additional registers
- **BAR3:** (Optional) I/O ports (legacy, rare on GPUs)

**BAR encoding:**

```
Bit 0:     0 = Memory BAR, 1 = I/O BAR
Bits 2-1:  Memory type (00 = 32-bit, 10 = 64-bit)
Bit 3:     Prefetchable (can be cached/combined)
Bits 31-4: Base address (aligned to size)
```

**64-bit BAR:** Uses two consecutive BAR registers. BAR0 holds lower 32 bits, BAR1 holds upper 32 bits.

**Size discovery protocol:**
1. Save original BAR value
2. Write all 1s (0xFFFFFFFF)
3. Read back - zeros indicate size bits, ones indicate address bits
4. Size = ~(readback & mask) + 1
5. Restore original value

```rust
// Example: discovering a 256 MB BAR
original = read_bar(0);           // 0xE000_000C (64-bit, prefetchable, at 0xE000_0000)
write_bar(0, 0xFFFF_FFFF);
readback = read_bar(0);           // 0xF000_000C
size = !(readback & 0xFFFF_FFF0) + 1;  // 0x1000_0000 = 256 MB
write_bar(0, original);
```

### PCI Capabilities

The capabilities pointer (offset 0x34) starts a linked list of optional features:

```
┌────────────────┐
│ Cap Pointer    │──▶ Offset of first capability
└────────────────┘
        │
        ▼
┌────────────────┐     ┌────────────────┐     ┌────────────────┐
│ Cap ID = 0x05  │────▶│ Cap ID = 0x11  │────▶│ Cap ID = 0x10  │
│ Next = 0x68    │     │ Next = 0x78    │     │ Next = 0x00    │
│ MSI Data...    │     │ MSI-X Data...  │     │ PCIe Data...   │
└────────────────┘     └────────────────┘     └────────────────┘
     MSI Cap              MSI-X Cap            PCIe Capability
```

**Key capability IDs:**
- 0x05: MSI (Message Signaled Interrupts)
- 0x10: PCIe (PCI Express)
- 0x11: MSI-X (Extended Message Signaled Interrupts)
- 0x09: Vendor Specific

### MSI vs MSI-X

**Legacy Interrupts (INTx):**
- 4 shared interrupt lines (INTA, INTB, INTC, INTD)
- Level-triggered, requires ACK
- Routing through interrupt controller
- Slow, shared, problematic

**MSI (Message Signaled Interrupts):**
- Device writes to special memory address
- Up to 32 vectors
- Edge-triggered (no ACK needed)
- Single message address for all vectors

**MSI-X (Extended MSI):**
- Up to 2048 vectors
- Per-vector message address
- Table stored in device BAR
- Pending Bit Array (PBA) for deferred delivery

```
MSI-X Capability Structure (in config space):
┌─────────────────────────────────────────┐
│ Cap ID (0x11) │ Next Cap │ Message Ctrl │  Offset in config space
├─────────────────────────────────────────┤
│           Table Offset/BIR              │  Where table lives in BAR
├─────────────────────────────────────────┤
│            PBA Offset/BIR               │  Where PBA lives in BAR
└─────────────────────────────────────────┘

MSI-X Table Entry (in BAR, 16 bytes each):
┌─────────────────────────────────────────┐
│         Message Address Low (4B)        │  Where to write
├─────────────────────────────────────────┤
│         Message Address High (4B)       │  Upper 32 bits (x86_64)
├─────────────────────────────────────────┤
│           Message Data (4B)             │  What to write (vector ID)
├─────────────────────────────────────────┤
│          Vector Control (4B)            │  Bit 0 = masked
└─────────────────────────────────────────┘
```

**In virtualization:** Guest writes to MSI-X table → VMM intercepts → VMM configures KVM irqfd → Device interrupt arrives → KVM injects to guest vCPU

---

## Part III: VFIO Subsystem

### Why VFIO Exists

Before VFIO, device passthrough meant:
1. Giving VM direct access to device (unsafe)
2. Using UIO (no IOMMU integration)
3. Kernel patches for each device type

VFIO provides:
- **Safe device access** via IOMMU isolation
- **Userspace driver interface** (no kernel modules needed)
- **Group-based security** (IOMMU groups)
- **Standardized interrupt/DMA handling**

### VFIO Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                     USERSPACE (Firecracker)                     │
│                                                                 │
│   ┌──────────────┐    ┌──────────────┐    ┌──────────────┐     │
│   │ VfioContainer│    │  VfioGroup   │    │  VfioDevice  │     │
│   │  /dev/vfio/  │    │/dev/vfio/N   │    │  (from group)│     │
│   │    vfio      │    │              │    │              │     │
│   └──────┬───────┘    └──────┬───────┘    └──────┬───────┘     │
│          │                   │                   │              │
└──────────┼───────────────────┼───────────────────┼──────────────┘
           │                   │                   │
           ▼                   ▼                   ▼
┌──────────────────────────────────────────────────────────────────┐
│                        KERNEL (vfio.ko)                          │
│                                                                  │
│   ┌──────────────┐    ┌──────────────┐    ┌──────────────┐      │
│   │VFIO Container│◀──▶│  IOMMU       │    │ VFIO PCI     │      │
│   │  (DMA domain)│    │  Domain      │    │   Driver     │      │
│   └──────────────┘    └──────────────┘    └──────────────┘      │
│                              │                    │              │
│                              ▼                    ▼              │
│                       ┌──────────────┐    ┌──────────────┐      │
│                       │    IOMMU     │    │  PCI Device  │      │
│                       │  Hardware    │    │   Hardware   │      │
│                       └──────────────┘    └──────────────┘      │
└──────────────────────────────────────────────────────────────────┘
```

### IOMMU Groups

**The fundamental isolation unit.** All devices in an IOMMU group share the same IOMMU translation context and must be assigned together.

Why groups exist:
- PCIe topology determines isolation boundaries
- Devices behind a non-ACS switch can spoof each other
- Group ensures all potentially-interacting devices are controlled

```
Example: GPU with HDMI audio
┌─────────────────────────────────────┐
│          IOMMU Group 14             │
│                                     │
│   ┌─────────┐      ┌─────────┐     │
│   │  GPU    │      │  HDMI   │     │
│   │0000:01: │      │  Audio  │     │
│   │ 00.0    │      │0000:01: │     │
│   │         │      │ 00.1    │     │
│   └─────────┘      └─────────┘     │
│                                     │
└─────────────────────────────────────┘

Both must be bound to vfio-pci and passed through together.
```

### VFIO Ioctls Reference

```rust
// Container operations (/dev/vfio/vfio)
VFIO_GET_API_VERSION        // Returns VFIO_API_VERSION
VFIO_CHECK_EXTENSION        // Check for TYPE1 IOMMU, etc.
VFIO_SET_IOMMU              // Attach IOMMU type to container

// Group operations (/dev/vfio/N)
VFIO_GROUP_GET_STATUS       // Check if viable (all devices bound)
VFIO_GROUP_SET_CONTAINER    // Associate group with container
VFIO_GROUP_GET_DEVICE_FD    // Get fd for specific device

// Device operations (fd from GROUP_GET_DEVICE_FD)
VFIO_DEVICE_GET_INFO        // Num regions, num irqs, flags
VFIO_DEVICE_GET_REGION_INFO // Size, offset, flags for region
VFIO_DEVICE_GET_IRQ_INFO    // Interrupt capabilities
VFIO_DEVICE_SET_IRQS        // Configure interrupt delivery
VFIO_DEVICE_RESET           // Reset device to known state

// IOMMU operations (on container fd, after SET_IOMMU)
VFIO_IOMMU_MAP_DMA          // Map GPA range for device DMA
VFIO_IOMMU_UNMAP_DMA        // Remove DMA mapping
```

### The VFIO Dance (Pseudocode)

```rust
// 1. Open container
let container_fd = open("/dev/vfio/vfio");
ioctl(container_fd, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU);

// 2. Find IOMMU group for device
let group_id = read_link("/sys/bus/pci/devices/0000:01:00.0/iommu_group")
    .file_name();  // e.g., "14"

// 3. Open group
let group_fd = open(format!("/dev/vfio/{}", group_id));
ioctl(group_fd, VFIO_GROUP_GET_STATUS);  // Must be viable

// 4. Associate group with container
ioctl(group_fd, VFIO_GROUP_SET_CONTAINER, container_fd);
ioctl(container_fd, VFIO_SET_IOMMU, VFIO_TYPE1v2_IOMMU);

// 5. Get device fd
let device_fd = ioctl(group_fd, VFIO_GROUP_GET_DEVICE_FD, "0000:01:00.0");

// 6. Query device
let info = ioctl(device_fd, VFIO_DEVICE_GET_INFO);
// info.num_regions, info.num_irqs

// 7. Map guest memory for DMA
for region in guest_memory.regions() {
    let dma_map = vfio_iommu_type1_dma_map {
        vaddr: region.host_addr,
        iova: region.guest_addr,   // IOVA == GPA
        size: region.size,
        flags: VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
    };
    ioctl(container_fd, VFIO_IOMMU_MAP_DMA, &dma_map);
}

// 8. Map device BARs into guest
for bar_idx in 0..6 {
    let region_info = ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, bar_idx);
    if region_info.size == 0 { continue; }
    
    // mmap the BAR from device
    let host_addr = mmap(
        NULL, region_info.size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        device_fd,
        region_info.offset,
    );
    
    // Create KVM memory slot for guest access
    kvm_set_user_memory_region(guest_bar_addr, region_info.size, host_addr);
    
    // Map for device DMA (if device might DMA to its own BAR)
    ioctl(container_fd, VFIO_IOMMU_MAP_DMA, ...);
}

// 9. Configure interrupts
let irq_info = ioctl(device_fd, VFIO_DEVICE_GET_IRQ_INFO, VFIO_PCI_MSIX_IRQ_INDEX);
for vec in 0..irq_info.count {
    let eventfd = eventfd(0, 0);
    
    // Wire eventfd to KVM irqfd for injection
    kvm_irqfd(eventfd, guest_vector_gsi);
    
    // Tell VFIO to signal this eventfd
    let irq_set = vfio_irq_set {
        index: VFIO_PCI_MSIX_IRQ_INDEX,
        start: vec,
        count: 1,
        flags: VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER,
        data: [eventfd],
    };
    ioctl(device_fd, VFIO_DEVICE_SET_IRQS, &irq_set);
}
```

---

## Part IV: KVM Integration

### Memory Slots

KVM uses "memory slots" to define guest physical memory layout:

```rust
struct kvm_userspace_memory_region {
    slot: u32,              // Slot ID (0-509 typically)
    flags: u32,             // KVM_MEM_READONLY, etc.
    guest_phys_addr: u64,   // GPA start
    memory_size: u64,       // Size in bytes
    userspace_addr: u64,    // HVA (host pointer)
}
```

Each slot maps a contiguous GPA range to a contiguous HVA range. The mapping is linear:
```
GPA 0x8000_0000 → HVA 0x7f12_3456_0000
GPA 0x8000_0001 → HVA 0x7f12_3456_0001
...
```

### irqfd - Interrupt Injection

KVM's irqfd mechanism connects an eventfd to guest interrupt injection:

```rust
struct kvm_irqfd {
    fd: u32,        // eventfd file descriptor
    gsi: u32,       // Guest System Interrupt number
    flags: u32,     // KVM_IRQFD_FLAG_DEASSIGN, etc.
}
```

When eventfd is signaled:
1. KVM sees the event
2. KVM injects interrupt to appropriate vCPU
3. Guest interrupt handler runs

For MSI/MSI-X, we also set up the routing:

```rust
struct kvm_irq_routing_entry {
    gsi: u32,
    type_: u32,     // KVM_IRQ_ROUTING_MSI
    u: union {
        msi: kvm_irq_routing_msi {
            address_lo: u32,    // 0xFEE0_0000 | (dest_apic << 12)
            address_hi: u32,
            data: u32,          // vector | (delivery_mode << 8)
        }
    }
}
```

### ioeventfd - MMIO/PIO Notification

ioeventfd does the reverse - guest MMIO write triggers eventfd:

```rust
struct kvm_ioeventfd {
    datamatch: u64,     // Optional: only trigger on specific value
    addr: u64,          // GPA to monitor
    len: u32,           // 1, 2, 4, or 8 bytes
    fd: i32,            // eventfd to signal
    flags: u32,         // KVM_IOEVENTFD_FLAG_DATAMATCH, etc.
}
```

Used for virtio notifications - guest writes to notification BAR, eventfd signals VMM.

---

## Part V: Rust Type System Exploitation

### Ownership for Resource Safety

```rust
// VFIO container owns the DMA mappings
struct VfioContainer {
    fd: OwnedFd,
    // Dropping VfioContainer unmaps DMA automatically
}

// Device borrows container - can't outlive it
struct VfioDevice<'a> {
    fd: OwnedFd,
    container: &'a VfioContainer,
}

// This won't compile - device can't outlive container
fn bad() {
    let device = {
        let container = VfioContainer::new();
        VfioDevice::new(&container)  // ERROR: container dropped here
    };
    device.do_something();  // device would have dangling reference
}
```

### Typestate for Protocol Enforcement

```rust
// Enforce the VFIO initialization sequence at compile time
struct Container<S>(PhantomData<S>);

struct Uninitialized;
struct IommuSet;
struct Ready;

impl Container<Uninitialized> {
    fn new() -> Self { ... }
    
    fn set_iommu(self) -> Container<IommuSet> { ... }
}

impl Container<IommuSet> {
    fn add_group(self, group: Group) -> Container<Ready> { ... }
}

impl Container<Ready> {
    fn map_dma(&self, ...) { ... }  // Only available in Ready state
}

// Can't call map_dma before set_iommu and add_group - compile error
```

### Newtype for Address Safety

```rust
// Prevent mixing up address types
#[derive(Copy, Clone, Debug)]
struct GuestPhysAddr(u64);

#[derive(Copy, Clone, Debug)]
struct HostVirtAddr(u64);

#[derive(Copy, Clone, Debug)]
struct IovaAddr(u64);

// DMA mapping requires correct types
fn map_dma(iova: IovaAddr, hva: HostVirtAddr, size: u64);

// This won't compile - type mismatch
let gpa = GuestPhysAddr(0x8000_0000);
let hva = HostVirtAddr(0x7fff_0000_0000);
map_dma(gpa, hva, 0x1000);  // ERROR: expected IovaAddr, found GuestPhysAddr
```

---

## Part VI: GPU-Specific Concepts

### NVIDIA GPU Architecture (Relevant Parts)

```
┌─────────────────────────────────────────────────────────────────┐
│                         GPU Package                              │
│                                                                 │
│   ┌─────────────────────────────────────────────────────────┐  │
│   │                     PCIe Interface                       │  │
│   │  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐    │  │
│   │  │  BAR0   │  │  BAR1   │  │  BAR2   │  │Config   │    │  │
│   │  │ Control │  │ VRAM    │  │ Doorbell│  │ Space   │    │  │
│   │  │ Regs    │  │ Aperture│  │         │  │         │    │  │
│   │  └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘    │  │
│   └───────┼────────────┼────────────┼────────────┼──────────┘  │
│           │            │            │            │              │
│   ┌───────┴────────────┴────────────┴────────────┘              │
│   │                     GPU Engine                              │
│   │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐   │
│   │  │  SMs     │  │  Copy    │  │  Video   │  │  Display │   │
│   │  │ (CUDA)   │  │  Engines │  │  Decode  │  │  Engine  │   │
│   │  └──────────┘  └──────────┘  └──────────┘  └──────────┘   │
│   │                                                            │
│   │  ┌─────────────────────────────────────────────────────┐  │
│   │  │                    VRAM (HBM/GDDR)                   │  │
│   │  │                      8-80 GB                         │  │
│   │  └─────────────────────────────────────────────────────┘  │
│   └────────────────────────────────────────────────────────────┘
└─────────────────────────────────────────────────────────────────┘
```

### GPUDirect and P2P

**GPUDirect Storage:** GPU DMA directly to NVMe, bypassing CPU

**GPUDirect RDMA:** GPU DMA directly to network card

**GPUDirect P2P:** GPU-to-GPU DMA without going through CPU/system memory

```
Without P2P:                    With P2P:
GPU A → System RAM → GPU B      GPU A → GPU B (direct PCIe)
       ~50 GB/s                      ~300 GB/s (NVLink)
                                     ~64 GB/s (PCIe 5.0)
```

**In virtualization:** The `x_nv_gpudirect_clique` parameter tells NVIDIA driver which GPUs can do P2P. GPUs in same clique assume P2P works.

### NVFP4 and Quantization

Not directly relevant to VFIO, but context for why you're doing this:

- FP32: 32-bit floating point (baseline)
- FP16: 16-bit floating point (2x throughput, slight precision loss)
- INT8: 8-bit integer (4x throughput, needs calibration)
- FP4 (NVFP4): 4-bit floating point (8x throughput, Blackwell-specific)

Blackwell GPUs have dedicated FP4 tensor cores. Your inference optimization work targets these. VFIO passthrough is how you get bare-metal FP4 performance in a microVM.

---

## Part VII: rust-vmm Crate Ecosystem

### Crate Dependency Graph

```
                    ┌─────────────────┐
                    │   firecracker   │
                    └────────┬────────┘
                             │
           ┌─────────────────┼─────────────────┐
           │                 │                 │
           ▼                 ▼                 ▼
    ┌──────────────┐  ┌──────────────┐  ┌──────────────┐
    │  kvm-ioctls  │  │  vm-memory   │  │ vm-allocator │
    └──────┬───────┘  └──────┬───────┘  └──────────────┘
           │                 │
           ▼                 ▼
    ┌──────────────┐  ┌──────────────┐
    │ kvm-bindings │  │ vm-fdt (arm) │
    └──────────────┘  └──────────────┘
    
                    ┌─────────────────┐
                    │  vfio-ioctls    │  ← We add this
                    └────────┬────────┘
                             │
                             ▼
                    ┌─────────────────┐
                    │ vfio-bindings   │
                    └─────────────────┘
```

### Key Types from rust-vmm

**vm-memory:**
```rust
trait GuestMemory {
    fn read(&self, addr: GuestAddress, buf: &mut [u8]) -> Result<()>;
    fn write(&self, addr: GuestAddress, buf: &[u8]) -> Result<()>;
    fn get_host_address(&self, addr: GuestAddress) -> Result<*mut u8>;
}

struct GuestAddress(u64);  // Newtype for GPA
```

**kvm-ioctls:**
```rust
struct Kvm;  // /dev/kvm
struct VmFd;  // VM instance
struct VcpuFd;  // vCPU instance

impl VmFd {
    fn set_user_memory_region(&self, ...) -> Result<()>;
    fn register_irqfd(&self, ...) -> Result<()>;
    fn register_ioeventfd(&self, ...) -> Result<()>;
}
```

**vfio-ioctls:**
```rust
struct VfioContainer;  // DMA domain
struct VfioDevice;     // Physical device

impl VfioContainer {
    fn new(fd: Option<Arc<dyn AsRawFd>>) -> Result<Self>;
    fn vfio_dma_map(&self, iova: u64, size: u64, user_addr: u64) -> Result<()>;
}

impl VfioDevice {
    fn new(path: &Path, container: Arc<VfioContainer>) -> Result<Self>;
    fn region_read(&self, index: u32, buf: &mut [u8], offset: u64);
    fn enable_msix(&self, fds: &[&EventFd]) -> Result<()>;
}
```

---

## Part VIII: Putting It All Together

### The Complete VFIO-KVM-Guest Data Path

```
GUEST DRIVER                          HOST VMM                         HARDWARE
───────────────────────────────────────────────────────────────────────────────

1. Guest allocates DMA buffer
   at GPA 0x8000_0000

2. Guest programs GPU:                                                  
   "DMA from 0x8000_0000"      
                               ────────────────────────────────────────────────
                               VFIO already set up IOMMU mapping:
                               IOVA 0x8000_0000 → HPA 0x1_0000_0000
                               ────────────────────────────────────────────────
                               
3. GPU issues DMA read         ──────────────────────────────────▶   GPU starts
   to IOVA 0x8000_0000                                                  DMA
                                                                          │
                                                                          ▼
                                                                    ┌─────────┐
                                                                    │  IOMMU  │
                                                                    │translates│
                                                                    │ to HPA  │
                                                                    └────┬────┘
                                                                         │
                                                                         ▼
                                                                    RAM at
                                                                    HPA 0x1_0000_0000
                                                                    (guest's buffer)

4. GPU completes, signals MSI-X
                               ◀────────────────────────────────────  MSI-X write
                               VFIO catches interrupt                  to APIC
                               signals eventfd
                               ◀────────────────────────────────────
                               KVM irqfd triggers
                               KVM injects interrupt to vCPU

5. Guest interrupt handler runs
   Processes completed DMA
```

### Latency Breakdown

| Stage | Typical Latency |
|-------|-----------------|
| Guest driver → GPU command | ~100 ns |
| GPU processes command | varies |
| IOMMU translation | ~50 ns |
| DMA transfer | depends on size |
| MSI-X generation | ~100 ns |
| VFIO → eventfd | ~200 ns |
| KVM irqfd → vCPU | ~1 µs |
| Guest interrupt handler | ~500 ns |

**Total interrupt overhead vs bare metal:** ~2-3 µs additional

This is why VFIO passthrough is viable for GPU compute - the DMA transfer itself dominates, and interrupt overhead is negligible for bulk operations.

---

## Quick Reference Card

### Device Binding Commands
```bash
# Find IOMMU group
readlink /sys/bus/pci/devices/0000:01:00.0/iommu_group

# Unbind from native driver  
echo "0000:01:00.0" > /sys/bus/pci/drivers/nvidia/unbind

# Bind to vfio-pci
echo "vfio-pci" > /sys/bus/pci/devices/0000:01:00.0/driver_override
echo "0000:01:00.0" > /sys/bus/pci/drivers_probe

# Verify
ls -la /sys/bus/pci/devices/0000:01:00.0/driver
# Should show -> ../../../bus/pci/drivers/vfio-pci
```

### Key File Paths
```
/dev/vfio/vfio              # Container fd
/dev/vfio/<group>           # Group fd (e.g., /dev/vfio/14)
/sys/bus/pci/devices/       # PCI device sysfs
/sys/kernel/iommu_groups/   # IOMMU group info
/sys/class/iommu/           # IOMMU driver info
```

### VFIO Region Indices
```rust
VFIO_PCI_BAR0_REGION_INDEX = 0
VFIO_PCI_BAR1_REGION_INDEX = 1
VFIO_PCI_BAR2_REGION_INDEX = 2
VFIO_PCI_BAR3_REGION_INDEX = 3
VFIO_PCI_BAR4_REGION_INDEX = 4
VFIO_PCI_BAR5_REGION_INDEX = 5
VFIO_PCI_ROM_REGION_INDEX = 6
VFIO_PCI_CONFIG_REGION_INDEX = 7
VFIO_PCI_VGA_REGION_INDEX = 8
```

### VFIO IRQ Indices
```rust
VFIO_PCI_INTX_IRQ_INDEX = 0
VFIO_PCI_MSI_IRQ_INDEX = 1
VFIO_PCI_MSIX_IRQ_INDEX = 2
VFIO_PCI_ERR_IRQ_INDEX = 3
VFIO_PCI_REQ_IRQ_INDEX = 4
```