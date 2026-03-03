// Copyright 2025 Isospin Authors
// SPDX-License-Identifier: Apache-2.0
//
//! VFIO PCI device implementation.
//!
//! This implements the `PciDevice` trait for passthrough devices, handling:
//! - Config space virtualization (shadow BARs with guest addresses)
//! - BAR mapping and MMIO region setup
//! - MSI-X interrupt routing through KVM irqfd

use std::os::unix::io::{AsRawFd, OwnedFd, RawFd};
use std::path::PathBuf;
use std::sync::{Arc, Barrier};

use super::container::VfioContainer;
use super::error::{VfioError, VfioResult};
use super::mmap::MmapRegion;
use super::msi::VfioMsiState;
use super::types::*;
use crate::pci::{BarReprogrammingParams, PciDevice};
use crate::vstate::interrupts::{MsixVectorConfig, MsixVectorGroup};

// VFIO ioctl definitions
// These match the actual kernel values from <linux/vfio.h>
// VFIO_TYPE = 0x3b (';'), VFIO_BASE = 100
// Note: Linux VFIO uses _IO() which doesn't encode size, just (type << 8) | nr
mod vfio_ioctls {
    use std::os::raw::c_ulong;
    
    // Group ioctls
    pub const VFIO_GROUP_GET_STATUS: c_ulong = 0x3b67;
    pub const VFIO_GROUP_SET_CONTAINER: c_ulong = 0x3b68;
    pub const VFIO_GROUP_GET_DEVICE_FD: c_ulong = 0x3b6a;
    
    // Device ioctls
    pub const VFIO_DEVICE_GET_REGION_INFO: c_ulong = 0x3b6c;
    pub const VFIO_DEVICE_SET_IRQS: c_ulong = 0x3b6e;  // VFIO_BASE + 10
    pub const VFIO_DEVICE_RESET: c_ulong = 0x3b6f;
    
    // IRQ set flags
    pub const VFIO_IRQ_SET_DATA_NONE: u32 = 1 << 0;
    pub const VFIO_IRQ_SET_DATA_BOOL: u32 = 1 << 1;
    pub const VFIO_IRQ_SET_DATA_EVENTFD: u32 = 1 << 2;
    pub const VFIO_IRQ_SET_ACTION_MASK: u32 = 1 << 3;
    pub const VFIO_IRQ_SET_ACTION_UNMASK: u32 = 1 << 4;
    pub const VFIO_IRQ_SET_ACTION_TRIGGER: u32 = 1 << 5;
    
    // IRQ indices for PCI devices
    pub const VFIO_PCI_INTX_IRQ_INDEX: u32 = 0;
    pub const VFIO_PCI_MSI_IRQ_INDEX: u32 = 1;
    pub const VFIO_PCI_MSIX_IRQ_INDEX: u32 = 2;
}

use vfio_ioctls::*;

/// Information about a VFIO device region (BAR or config space).
#[derive(Debug, Clone)]
pub struct VfioRegionInfo {
    /// Size of the region in bytes
    pub size: u64,
    /// Offset for mmap
    pub offset: u64,
    /// Region flags
    pub flags: u32,
}

/// Information about a device BAR.
#[derive(Debug, Clone)]
pub struct BarInfo {
    /// BAR index (0-5)
    pub index: BarIndex,
    /// Size of the BAR
    pub size: u64,
    /// Whether this is a 64-bit BAR
    pub is_64bit: bool,
    /// Whether the BAR is prefetchable
    pub is_prefetchable: bool,
    /// Whether this is an I/O BAR (vs memory)
    pub is_io: bool,
    /// Guest address assigned to this BAR
    pub guest_addr: Option<GuestPhysAddr>,
    /// Host virtual address after mmap
    pub host_addr: Option<HostVirtAddr>,
    /// VFIO region info
    pub region_info: VfioRegionInfo,
}

/// MSI-X table entry (16 bytes per entry)
#[derive(Debug, Clone, Copy)]
pub struct MsixTableEntry {
    /// Message address low (bits 31:0)
    pub msg_addr_lo: u32,
    /// Message address high (bits 63:32)
    pub msg_addr_hi: u32,
    /// Message data
    pub msg_data: u32,
    /// Vector control (bit 0 = masked)
    pub vector_ctrl: u32,
}

impl Default for MsixTableEntry {
    fn default() -> Self {
        Self {
            msg_addr_lo: 0,
            msg_addr_hi: 0,
            msg_data: 0,
            // Per PCIe spec, MSI-X table entries reset with mask bit SET
            vector_ctrl: 1,
        }
    }
}

impl MsixTableEntry {
    /// Check if this vector is masked
    pub fn is_masked(&self) -> bool {
        (self.vector_ctrl & 1) != 0
    }
}

/// MSI-X state for a VFIO device
#[derive(Debug)]
pub struct VfioMsixState {
    /// Capability offset in config space
    pub cap_offset: u16,
    /// Number of table entries
    pub table_size: u16,
    /// BAR containing MSI-X table
    pub table_bar: u8,
    /// Offset within BAR to table
    pub table_offset: u32,
    /// BAR containing PBA
    pub pba_bar: u8,
    /// Offset within BAR to PBA
    pub pba_offset: u32,
    /// Local copy of MSI-X table entries
    pub table_entries: Vec<MsixTableEntry>,
    /// MSI-X enabled (from config space)
    pub enabled: bool,
    /// Function masked (from config space)
    pub function_masked: bool,
}

/// Interrupt type supported by the device
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InterruptType {
    /// No MSI support
    None,
    /// Plain MSI (capability ID 0x05)
    Msi,
    /// MSI-X (capability ID 0x11)
    MsiX,
}

/// A VFIO PCI device for passthrough.
pub struct VfioPciDevice {
    /// Device identifier
    id: String,
    /// PCI BDF
    bdf: PciBdf,
    /// File descriptor for the device
    fd: OwnedFd,
    /// IOMMU group ID
    group_id: u32,
    /// Path to device in sysfs
    sysfs_path: PathBuf,
    /// Vendor ID
    vendor_id: VendorId,
    /// Device ID
    device_id: DeviceId,
    /// BAR information
    bars: [Option<BarInfo>; 6],
    /// Shadow config space for BARs (guest addresses)
    shadow_bars: [u64; 6],
    /// MSI-X capability offset (if present)
    msix_cap_offset: Option<u16>,
    /// MSI-X table size
    msix_table_size: Option<u16>,
    /// MSI-X state (table entries, enable state)
    msix_state: Option<VfioMsixState>,
    /// MSI-X vector group for KVM interrupt routing
    msix_vectors: Option<Arc<MsixVectorGroup>>,
    /// MSI capability offset (if present)
    msi_cap_offset: Option<u16>,
    /// MSI state (config registers, enable state)
    msi_state: Option<VfioMsiState>,
    /// MSI vector group for KVM interrupt routing (reuses MsixVectorGroup)
    msi_vectors: Option<Arc<MsixVectorGroup>>,
    /// Which interrupt type is currently in use
    interrupt_type: InterruptType,
    /// PCI device ID for MSI routing (segment:bus:device:function)
    pci_devid: u32,
    /// NVIDIA GPUDirect clique ID
    gpudirect_clique: Option<u8>,
    /// PCIe capability offset (cap ID 0x10, for masking unsupported features)
    pcie_cap_offset: Option<u16>,
    /// LTR Extended Capability offset (cap ID 0x18 in extended config space)
    /// We need to hide this to prevent NVIDIA driver timeouts
    ltr_ext_cap_offset: Option<u16>,
    /// Memory-mapped BAR regions for direct guest access.
    ///
    /// These are stored separately from BarInfo because MmapRegion is not Clone.
    /// Index corresponds to BAR index (0-5).
    ///
    /// # Invariants
    /// - If `bar_mmaps[i].is_some()`, then `bars[i].is_some()` and
    ///   `bars[i].guest_addr.is_some()`
    /// - The MmapRegion remains valid as long as the VfioPciDevice exists
    /// - KVM memory regions are registered when this is set
    bar_mmaps: [Option<MmapRegion>; 6],
    /// KVM memory slot IDs for mmapped BARs.
    ///
    /// These track the slot IDs we've allocated for each BAR's KVM user memory region.
    /// Needed for cleanup (though KVM auto-cleans on VM destruction).
    bar_kvm_slots: [Option<u32>; 6],
}

impl VfioPciDevice {
    /// Create a new VFIO PCI device.
    ///
    /// # Arguments
    /// * `id` - Unique identifier for the device
    /// * `bdf` - PCI Bus:Device.Function address
    /// * `device_fd` - File descriptor obtained from VFIO group
    /// * `group_id` - IOMMU group this device belongs to
    /// * `gpudirect_clique` - Optional NVIDIA GPUDirect P2P clique ID
    pub fn new(
        id: String,
        bdf: PciBdf,
        device_fd: OwnedFd,
        group_id: u32,
        gpudirect_clique: Option<u8>,
    ) -> VfioResult<Self> {
        let sysfs_path = PathBuf::from(bdf.sysfs_path());

        // Read vendor/device ID from config space
        let mut vendor_device = [0u8; 4];
        Self::read_config_raw(device_fd.as_raw_fd(), 0, &mut vendor_device)?;
        let vendor_id = VendorId::new(u16::from_le_bytes([vendor_device[0], vendor_device[1]]));
        let device_id = DeviceId::new(u16::from_le_bytes([vendor_device[2], vendor_device[3]]));

        // Compute PCI device ID for MSI routing
        // Format: segment (16 bits) | bus (8 bits) | device (5 bits) | function (3 bits)
        let pci_devid = ((bdf.bus as u32) << 8) | ((bdf.device as u32) << 3) | (bdf.function as u32);

        let mut device = Self {
            id,
            bdf,
            fd: device_fd,
            group_id,
            sysfs_path,
            vendor_id,
            device_id,
            bars: Default::default(),
            shadow_bars: [0; 6],
            msix_cap_offset: None,
            msix_table_size: None,
            msix_state: None,
            msix_vectors: None,
            msi_cap_offset: None,
            msi_state: None,
            msi_vectors: None,
            interrupt_type: InterruptType::None,
            pci_devid,
            gpudirect_clique,
            pcie_cap_offset: None,
            ltr_ext_cap_offset: None,
            bar_mmaps: Default::default(),
            bar_kvm_slots: Default::default(),
        };

        // Reset the device to put it in a known state
        // This is what cloud-hypervisor does - may help with driver init timing
        log::info!("Resetting VFIO device {}...", bdf);
        let reset_start = std::time::Instant::now();
        if let Err(e) = device.reset() {
            log::warn!("VFIO device reset failed (may not be supported): {:?}", e);
        } else {
            log::info!("VFIO device reset took {:?}", reset_start.elapsed());
        }

        // Discover BARs
        device.discover_bars()?;

        // Find interrupt capabilities (MSI-X and/or MSI)
        device.find_interrupt_capabilities()?;

        Ok(device)
    }

    /// Read from device config space.
    fn read_config_raw(fd: RawFd, offset: u64, buf: &mut [u8]) -> VfioResult<()> {
        // VFIO_PCI_CONFIG_REGION_INDEX = 7
        // Use pread to read from config space region
        let region_offset = Self::get_region_offset(fd, 7)?;
        
        let ret = unsafe {
            libc::pread(
                fd,
                buf.as_mut_ptr() as *mut libc::c_void,
                buf.len(),
                (region_offset + offset) as i64,
            )
        };

        if ret < 0 {
            return Err(VfioError::ConfigRead {
                offset,
                source: std::io::Error::last_os_error(),
            });
        }

        Ok(())
    }

    /// Write to device config space.
    fn write_config_raw(fd: RawFd, offset: u64, buf: &[u8]) -> VfioResult<()> {
        let region_offset = Self::get_region_offset(fd, 7)?;
        
        let ret = unsafe {
            libc::pwrite(
                fd,
                buf.as_ptr() as *const libc::c_void,
                buf.len(),
                (region_offset + offset) as i64,
            )
        };

        if ret < 0 {
            return Err(VfioError::ConfigWrite {
                offset,
                source: std::io::Error::last_os_error(),
            });
        }

        Ok(())
    }

    /// Get region offset for mmap/pread/pwrite.
    fn get_region_offset(fd: RawFd, region_index: u32) -> VfioResult<u64> {
        #[repr(C)]
        struct VfioRegionInfo {
            argsz: u32,
            flags: u32,
            index: u32,
            cap_offset: u32,
            size: u64,
            offset: u64,
        }

        let mut info = VfioRegionInfo {
            argsz: std::mem::size_of::<VfioRegionInfo>() as u32,
            flags: 0,
            index: region_index,
            cap_offset: 0,
            size: 0,
            offset: 0,
        };

        let ret = unsafe { libc::ioctl(fd, VFIO_DEVICE_GET_REGION_INFO, &mut info) };
        if ret < 0 {
            let err = std::io::Error::last_os_error();
            return Err(VfioError::GetRegionInfo {
                bar: BarIndex::new(region_index as u8).unwrap_or(BarIndex::BAR0),
                source: err,
            });
        }

        Ok(info.offset)
    }

    /// Discover device BARs.
    fn discover_bars(&mut self) -> VfioResult<()> {
        let mut skip_next = false;

        for i in 0..6 {
            if skip_next {
                skip_next = false;
                log::debug!("BAR{}: skipped (upper half of 64-bit BAR)", i);
                continue;
            }

            let bar_idx = BarIndex::new(i).unwrap();
            let region_info = self.get_region_info(VfioRegionIndex::from_bar(bar_idx))?;

            if region_info.size == 0 {
                log::debug!("BAR{}: size=0, skipping", i);
                continue;
            }

            // Read BAR value to determine type
            let bar_offset = bar_idx.config_offset();
            let mut bar_value = [0u8; 4];
            Self::read_config_raw(self.fd.as_raw_fd(), bar_offset as u64, &mut bar_value)?;
            let bar_raw = u32::from_le_bytes(bar_value);

            let is_io = (bar_raw & 0x1) != 0;
            let is_64bit = !is_io && ((bar_raw >> 1) & 0x3) == 2;
            let is_prefetchable = !is_io && ((bar_raw >> 3) & 0x1) != 0;

            log::info!(
                "BAR{}: size={:#x}, raw={:#x}, is_64bit={}, is_prefetch={}, is_io={}",
                i, region_info.size, bar_raw, is_64bit, is_prefetchable, is_io
            );

            self.bars[i as usize] = Some(BarInfo {
                index: bar_idx,
                size: region_info.size,
                is_64bit,
                is_prefetchable,
                is_io,
                guest_addr: None,
                host_addr: None,
                region_info,
            });

            if is_64bit {
                skip_next = true;
            }
        }

        Ok(())
    }

    /// Get region info from VFIO.
    fn get_region_info(&self, region: VfioRegionIndex) -> VfioResult<VfioRegionInfo> {
        #[repr(C)]
        struct VfioRegionInfoRaw {
            argsz: u32,
            flags: u32,
            index: u32,
            cap_offset: u32,
            size: u64,
            offset: u64,
        }

        let mut info = VfioRegionInfoRaw {
            argsz: std::mem::size_of::<VfioRegionInfoRaw>() as u32,
            flags: 0,
            index: region.0,
            cap_offset: 0,
            size: 0,
            offset: 0,
        };

        let ret = unsafe { libc::ioctl(self.fd.as_raw_fd(), VFIO_DEVICE_GET_REGION_INFO, &mut info) };
        if ret < 0 {
            return Err(VfioError::GetRegionInfo {
                bar: BarIndex::new(region.0 as u8).unwrap_or(BarIndex::BAR0),
                source: std::io::Error::last_os_error(),
            });
        }

        Ok(VfioRegionInfo {
            size: info.size,
            offset: info.offset,
            flags: info.flags,
        })
    }

    /// Find interrupt capabilities (MSI-X and MSI) in config space.
    fn find_interrupt_capabilities(&mut self) -> VfioResult<()> {
        // Read capabilities pointer (offset 0x34)
        let mut cap_ptr = [0u8; 1];
        Self::read_config_raw(self.fd.as_raw_fd(), 0x34, &mut cap_ptr)?;
        let mut offset = cap_ptr[0] as u16;
        log::info!("find_interrupt_capabilities: cap_ptr at offset {:#x}", offset);

        while offset != 0 && offset < 0x100 {
            let mut cap_header = [0u8; 2];
            Self::read_config_raw(self.fd.as_raw_fd(), offset as u64, &mut cap_header)?;
            let cap_id = cap_header[0];
            let next_cap = cap_header[1] as u16;
            log::info!("find_interrupt_capabilities: cap at {:#x}: id={:#x}, next={:#x}", offset, cap_id, next_cap);

            match cap_id {
                // MSI capability ID = 0x05
                0x05 => {
                    self.msi_cap_offset = Some(offset);

                    // Read message control register (offset + 2)
                    let mut msi_ctrl = [0u8; 2];
                    Self::read_config_raw(self.fd.as_raw_fd(), (offset + 2) as u64, &mut msi_ctrl)?;
                    let msg_ctl = u16::from_le_bytes(msi_ctrl);

                    let max_vectors = super::msi::msi_max_vectors(msg_ctl);
                    let is_64bit = (msg_ctl & super::msi::MSI_CTL_64_BIT_CAPABLE) != 0;
                    let has_mask = (msg_ctl & super::msi::MSI_CTL_PER_VECTOR_MASK) != 0;

                    log::info!(
                        "MSI capability at {:#x}: max_vectors={}, 64bit={}, per_vector_mask={}",
                        offset, max_vectors, is_64bit, has_mask
                    );

                    // Initialize MSI state
                    self.msi_state = Some(VfioMsiState::new(offset, msg_ctl));
                }

                // PCIe capability ID = 0x10
                0x10 => {
                    self.pcie_cap_offset = Some(offset);
                    log::info!("PCIe capability at {:#x}", offset);
                }

                // MSI-X capability ID = 0x11
                0x11 => {
                    self.msix_cap_offset = Some(offset);

                    // Read message control register (offset + 2)
                    let mut msix_ctrl = [0u8; 2];
                    Self::read_config_raw(self.fd.as_raw_fd(), (offset + 2) as u64, &mut msix_ctrl)?;
                    let msg_ctrl = u16::from_le_bytes(msix_ctrl);
                    let table_size = (msg_ctrl & 0x7FF) + 1;
                    self.msix_table_size = Some(table_size);

                    // Read table offset/BIR (offset + 4)
                    let mut table_offset_bir = [0u8; 4];
                    Self::read_config_raw(self.fd.as_raw_fd(), (offset + 4) as u64, &mut table_offset_bir)?;
                    let table_val = u32::from_le_bytes(table_offset_bir);
                    let table_bar = (table_val & 0x7) as u8;
                    let table_offset = table_val & !0x7;

                    // Read PBA offset/BIR (offset + 8)
                    let mut pba_offset_bir = [0u8; 4];
                    Self::read_config_raw(self.fd.as_raw_fd(), (offset + 8) as u64, &mut pba_offset_bir)?;
                    let pba_val = u32::from_le_bytes(pba_offset_bir);
                    let pba_bar = (pba_val & 0x7) as u8;
                    let pba_offset = pba_val & !0x7;

                    log::info!(
                        "MSI-X capability at {:#x}: {} vectors, table in BAR{} offset {:#x}, PBA in BAR{} offset {:#x}",
                        offset, table_size, table_bar, table_offset, pba_bar, pba_offset
                    );

                    // Initialize MSI-X state with empty table entries
                    self.msix_state = Some(VfioMsixState {
                        cap_offset: offset,
                        table_size,
                        table_bar,
                        table_offset,
                        pba_bar,
                        pba_offset,
                        table_entries: vec![MsixTableEntry::default(); table_size as usize],
                        enabled: false,
                        function_masked: true,
                    });
                }

                _ => {}
            }

            // Protect against broken capability lists
            if next_cap == offset {
                break;
            }
            offset = next_cap;
        }

        // Log summary of interrupt support
        let has_msix = self.msix_cap_offset.is_some();
        let has_msi = self.msi_cap_offset.is_some();
        log::info!(
            "Device {}: MSI-X={}, MSI={}", 
            self.bdf, 
            if has_msix { "yes" } else { "no" },
            if has_msi { "yes" } else { "no" }
        );

        // Scan extended capabilities (>= 0x100) for LTR capability
        // Extended capabilities have a different format: [cap_id:16][version:4][next:12]
        self.find_ltr_extended_capability()?;

        Ok(())
    }

    /// Find LTR Extended Capability in extended config space (offset >= 0x100)
    fn find_ltr_extended_capability(&mut self) -> VfioResult<()> {
        // Extended capabilities start at offset 0x100
        // Format: bits 15:0 = cap ID, bits 19:16 = version, bits 31:20 = next cap offset
        let mut offset: u16 = 0x100;
        let mut iterations = 0;

        log::info!("Scanning extended config space for LTR capability...");

        while offset != 0 && offset >= 0x100 && offset < 0x1000 && iterations < 50 {
            iterations += 1;
            
            let mut ext_cap_header = [0u8; 4];
            if let Err(e) = Self::read_config_raw(self.fd.as_raw_fd(), offset as u64, &mut ext_cap_header) {
                log::warn!("Extended config read at {:#x} failed: {:?}", offset, e);
                break;
            }
            
            let header = u32::from_le_bytes(ext_cap_header);
            let cap_id = (header & 0xFFFF) as u16;
            let next_offset = ((header >> 20) & 0xFFF) as u16;

            log::debug!(
                "Extended cap at {:#x}: id={:#x}, next={:#x}",
                offset, cap_id, next_offset
            );

            // Cap ID 0 or 0xFFFF means end of list
            if cap_id == 0 || cap_id == 0xFFFF {
                log::info!("End of extended capability list at {:#x}", offset);
                break;
            }

            // LTR Extended Capability ID = 0x18
            if cap_id == 0x18 {
                self.ltr_ext_cap_offset = Some(offset);
                log::info!("LTR Extended Capability at {:#x} (will be hidden)", offset);
                // Don't break - continue scanning to log all caps
            }

            // Prevent infinite loops
            if next_offset != 0 && next_offset <= offset {
                log::warn!("Extended cap chain loop detected: {:#x} -> {:#x}", offset, next_offset);
                break;
            }
            if next_offset == 0 {
                log::info!("Extended capability chain ends at {:#x}", offset);
                break;
            }
            offset = next_offset;
        }

        if self.ltr_ext_cap_offset.is_none() {
            log::info!("LTR Extended Capability not found (scanned {} caps)", iterations);
        }

        Ok(())
    }

    /// Get device identifier.
    pub fn id(&self) -> &str {
        &self.id
    }

    /// Get PCI BDF.
    pub fn bdf(&self) -> PciBdf {
        self.bdf
    }

    /// Get vendor ID.
    pub fn vendor_id(&self) -> VendorId {
        self.vendor_id
    }

    /// Get device ID.
    pub fn device_id(&self) -> DeviceId {
        self.device_id
    }

    /// Get IOMMU group ID.
    pub fn group_id(&self) -> u32 {
        self.group_id
    }

    /// Check if device supports MSI-X.
    pub fn has_msix(&self) -> bool {
        self.msix_cap_offset.is_some()
    }

    /// Check if device supports MSI.
    pub fn has_msi(&self) -> bool {
        self.msi_cap_offset.is_some()
    }

    /// Get the preferred interrupt type for this device
    pub fn preferred_interrupt_type(&self) -> InterruptType {
        // Prefer MSI-X over MSI if available
        if self.has_msix() {
            InterruptType::MsiX
        } else if self.has_msi() {
            InterruptType::Msi
        } else {
            InterruptType::None
        }
    }

    /// Get MSI-X table size.
    pub fn msix_table_size(&self) -> Option<u16> {
        self.msix_table_size
    }

    /// Get MSI max vectors.
    pub fn msi_max_vectors(&self) -> Option<usize> {
        self.msi_state.as_ref().map(|s| s.cap.max_vectors())
    }

    /// Get MSI-X state (for interrupt routing setup)
    pub fn msix_state(&self) -> Option<&VfioMsixState> {
        self.msix_state.as_ref()
    }

    /// Get mutable MSI-X state
    pub fn msix_state_mut(&mut self) -> Option<&mut VfioMsixState> {
        self.msix_state.as_mut()
    }

    /// Get MSI state (for interrupt routing setup)
    pub fn msi_state(&self) -> Option<&VfioMsiState> {
        self.msi_state.as_ref()
    }

    /// Get mutable MSI state
    pub fn msi_state_mut(&mut self) -> Option<&mut VfioMsiState> {
        self.msi_state.as_mut()
    }

    /// Check if an offset within a BAR accesses the MSI-X table
    pub fn is_msix_table_access(&self, bar_index: u8, offset: u64) -> bool {
        if let Some(msix) = &self.msix_state {
            if bar_index == msix.table_bar {
                let table_end = msix.table_offset as u64 + (msix.table_size as u64 * 16);
                return offset >= msix.table_offset as u64 && offset < table_end;
            }
        }
        false
    }

    /// Write to MSI-X table (called when guest writes to table region in BAR)
    /// Returns the vector index that was modified, if any
    pub fn write_msix_table(&mut self, bar_offset: u64, data: &[u8]) -> Option<u16> {
        let msix = self.msix_state.as_mut()?;
        
        // Calculate offset within the table
        let table_offset = bar_offset - msix.table_offset as u64;
        let vector_index = (table_offset / 16) as u16;
        let entry_offset = (table_offset % 16) as usize;

        if vector_index >= msix.table_size {
            return None;
        }

        let entry = &mut msix.table_entries[vector_index as usize];

        // Write to the appropriate field based on offset within entry
        match entry_offset {
            0..=3 if data.len() >= 4 => {
                entry.msg_addr_lo = u32::from_le_bytes([data[0], data[1], data[2], data[3]]);
            }
            4..=7 if data.len() >= 4 => {
                entry.msg_addr_hi = u32::from_le_bytes([data[0], data[1], data[2], data[3]]);
            }
            8..=11 if data.len() >= 4 => {
                entry.msg_data = u32::from_le_bytes([data[0], data[1], data[2], data[3]]);
            }
            12..=15 if data.len() >= 4 => {
                entry.vector_ctrl = u32::from_le_bytes([data[0], data[1], data[2], data[3]]);
            }
            _ => {
                // Partial write - handle byte by byte if needed
                log::debug!("Partial MSI-X table write at offset {}, len {}", entry_offset, data.len());
            }
        }

        log::debug!(
            "MSI-X table[{}] updated: addr={:#x}:{:#x} data={:#x} ctrl={:#x}",
            vector_index, entry.msg_addr_hi, entry.msg_addr_lo, entry.msg_data, entry.vector_ctrl
        );

        Some(vector_index)
    }

    /// Read from MSI-X table
    pub fn read_msix_table(&self, bar_offset: u64, data: &mut [u8]) {
        let Some(msix) = &self.msix_state else { return };

        let table_offset = bar_offset - msix.table_offset as u64;
        let vector_index = (table_offset / 16) as usize;
        let entry_offset = (table_offset % 16) as usize;

        if vector_index >= msix.table_size as usize {
            return;
        }

        let entry = &msix.table_entries[vector_index];

        // Read from the appropriate field
        let value = match entry_offset {
            0..=3 => entry.msg_addr_lo,
            4..=7 => entry.msg_addr_hi,
            8..=11 => entry.msg_data,
            12..=15 => entry.vector_ctrl,
            _ => 0,
        };

        let bytes = value.to_le_bytes();
        let copy_len = data.len().min(4);
        data[..copy_len].copy_from_slice(&bytes[..copy_len]);
    }

    /// Get raw file descriptor.
    pub fn as_raw_fd(&self) -> RawFd {
        self.fd.as_raw_fd()
    }

    /// Set the MSI-X vector group for KVM interrupt routing
    pub fn set_msix_vectors(&mut self, vectors: Arc<MsixVectorGroup>) {
        self.msix_vectors = Some(vectors);
    }

    /// Update KVM MSI routing for a specific vector
    /// Called when MSI-X table entry is programmed by guest
    fn update_msix_routing(&self, vector_index: u16) {
        let Some(msix) = &self.msix_state else { return };
        let Some(vectors) = &self.msix_vectors else { return };

        if vector_index as usize >= msix.table_entries.len() {
            return;
        }

        let entry = &msix.table_entries[vector_index as usize];
        
        // Only update routing if MSI-X is enabled
        if !msix.enabled || msix.function_masked {
            return;
        }

        let config = MsixVectorConfig {
            high_addr: entry.msg_addr_hi,
            low_addr: entry.msg_addr_lo,
            data: entry.msg_data,
            devid: self.pci_devid,
        };

        let masked = entry.is_masked();

        if let Err(e) = vectors.update(vector_index as usize, config, masked, true) {
            log::error!("Failed to update MSI-X routing for vector {}: {:?}", vector_index, e);
        } else {
            log::debug!(
                "Updated MSI-X routing: vector={} addr={:#x}:{:#x} data={:#x} masked={}",
                vector_index, entry.msg_addr_hi, entry.msg_addr_lo, entry.msg_data, masked
            );
        }
    }

    /// Enable/disable all MSI-X vectors based on MSI-X enable state
    fn update_all_msix_routing(&self) {
        let Some(msix) = &self.msix_state else { return };
        let Some(vectors) = &self.msix_vectors else { return };

        if msix.enabled && !msix.function_masked {
            log::info!("Enabling MSI-X routing for {} vectors", msix.table_entries.len());
            for (idx, entry) in msix.table_entries.iter().enumerate() {
                let config = MsixVectorConfig {
                    high_addr: entry.msg_addr_hi,
                    low_addr: entry.msg_addr_lo,
                    data: entry.msg_data,
                    devid: self.pci_devid,
                };

                if let Err(e) = vectors.update(idx, config, entry.is_masked(), true) {
                    log::error!("Failed to enable MSI-X vector {}: {:?}", idx, e);
                }
            }
        } else {
            log::info!("Disabling MSI-X routing");
            if let Err(e) = vectors.disable() {
                log::error!("Failed to disable MSI-X vectors: {:?}", e);
            }
        }
    }

    /// Get iterator over valid BARs.
    pub fn bars(&self) -> impl Iterator<Item = &BarInfo> {
        self.bars.iter().filter_map(|b| b.as_ref())
    }

    /// Set the guest address for a BAR.
    pub fn set_bar_guest_addr(&mut self, bar: BarIndex, addr: GuestPhysAddr) {
        if let Some(ref mut bar_info) = self.bars[bar.as_usize()] {
            bar_info.guest_addr = Some(addr);

            // Build the BAR value with proper type bits
            // Memory BAR format:
            //   Bit 0: 0 (memory)
            //   Bits 1-2: Type (00=32-bit, 10=64-bit)
            //   Bit 3: Prefetchable
            //   Bits 4+: Base address (aligned to size)
            let mut bar_val = addr.raw();

            if bar_info.is_io {
                // I/O BAR: bit 0 = 1
                bar_val |= 0x1;
            } else {
                // Memory BAR
                if bar_info.is_64bit {
                    bar_val |= 0x4; // Type = 10 (64-bit)
                }
                if bar_info.is_prefetchable {
                    bar_val |= 0x8;
                }
            }

            self.shadow_bars[bar.as_usize()] = bar_val;

            // For 64-bit BARs, also set the high part
            if bar_info.is_64bit && bar.as_u8() < 5 {
                self.shadow_bars[bar.as_usize() + 1] = addr.raw() >> 32;
            }
        }
    }

    /// Map MMIO BAR regions for direct guest access (bypass VM exits).
    ///
    /// This is a critical optimization for GPU passthrough. Without this,
    /// every guest access to GPU registers causes a VM exit and a pread/pwrite
    /// syscall. With direct mapping, guest BAR accesses go directly to the
    /// device hardware at native speed.
    ///
    /// # Algorithm
    ///
    /// ```text
    /// map_mmio_regions(vm):
    ///   for each bar in bars:
    ///     1. Skip I/O BARs (only MMIO supported)
    ///     2. Skip if region doesn't support mmap (check VFIO flags)
    ///     3. Skip if no guest address assigned yet
    ///     4. mmap the BAR region from VFIO device fd
    ///     5. Allocate KVM memory slot
    ///     6. Register with KVM as user memory region
    ///     7. Store mmap and slot for cleanup
    /// ```
    ///
    /// # Arguments
    ///
    /// * `vm` - Reference to the VM for allocating KVM memory slots
    ///
    /// # Errors
    ///
    /// Returns an error if:
    /// - mmap fails (e.g., BAR not mappable, permission denied)
    /// - No KVM memory slots available
    /// - KVM_SET_USER_MEMORY_REGION fails
    ///
    /// # Performance
    ///
    /// Before this optimization, NVIDIA driver init took ~6.6s due to ~50,000+
    /// register accesses going through pread syscalls. With direct mapping,
    /// these accesses happen at native speed (sub-second init).
    ///
    /// # Safety
    ///
    /// The mapped regions remain valid as long as:
    /// - The VfioPciDevice exists (holds MmapRegion ownership)
    /// - The VFIO device fd remains open (VfioPciDevice holds it)
    ///
    /// KVM memory regions are automatically cleaned up when the VM is destroyed.
    ///
    /// # Formal Specification
    ///
    /// ```text
    /// pre:  for all i: bars[i].guest_addr.is_some() implies valid_guest_addr(bars[i])
    /// post: for all i where bars[i] is mappable:
    ///         bar_mmaps[i].is_some() &&
    ///         bar_kvm_slots[i].is_some() &&
    ///         kvm_region_registered(bars[i].guest_addr, bar_mmaps[i].addr)
    /// ```
    pub fn map_mmio_regions(&mut self, vm: &crate::vstate::vm::Vm) -> VfioResult<()> {
        // VFIO region info flags
        const VFIO_REGION_INFO_FLAG_MMAP: u32 = 4;

        for bar_idx in 0..6 {
            let bar_info = match &self.bars[bar_idx] {
                Some(info) => info,
                None => continue,
            };

            // Step 1: Skip I/O BARs (only MMIO can be mapped)
            if bar_info.is_io {
                log::debug!(
                    "BAR{}: Skipping I/O BAR for mmap (size={:#x})",
                    bar_idx,
                    bar_info.size
                );
                continue;
            }

            // Step 2: Check if region supports mmap
            if (bar_info.region_info.flags & VFIO_REGION_INFO_FLAG_MMAP) == 0 {
                log::info!(
                    "BAR{}: Region doesn't support mmap (flags={:#x})",
                    bar_idx,
                    bar_info.region_info.flags
                );
                continue;
            }

            // Step 3: Need guest address to be assigned
            let guest_addr = match bar_info.guest_addr {
                Some(addr) => addr,
                None => {
                    log::debug!("BAR{}: No guest address assigned, skipping mmap", bar_idx);
                    continue;
                }
            };

            // Step 4: mmap the BAR region
            log::info!(
                "BAR{}: Mapping MMIO region for direct access (guest={:#x}, size={:#x}, offset={:#x})",
                bar_idx,
                guest_addr.raw(),
                bar_info.size,
                bar_info.region_info.offset
            );

            let mmap_start = std::time::Instant::now();
            let mmap = MmapRegion::new(
                self.fd.as_raw_fd(),
                bar_info.region_info.offset,
                bar_info.size,
            )
            .map_err(|e| VfioError::MmapBar {
                bar: bar_info.index,
                source: e,
            })?;

            log::debug!(
                "BAR{}: mmap completed in {:?}, host_addr={:p}",
                bar_idx,
                mmap_start.elapsed(),
                mmap.addr()
            );

            // Step 5: Allocate KVM memory slot
            let slot = vm
                .next_kvm_slot(1)
                .ok_or(VfioError::NoKvmSlotAvailable { bar: bar_info.index })?;

            // Step 6: Register with KVM as user memory region
            //
            // This tells KVM that guest physical addresses in range
            // [guest_addr, guest_addr + size) should map directly to
            // the mmap'd host memory, bypassing VM exits.
            let kvm_region = kvm_bindings::kvm_userspace_memory_region {
                slot,
                guest_phys_addr: guest_addr.raw(),
                memory_size: bar_info.size,
                userspace_addr: mmap.addr() as u64,
                flags: 0,
            };

            vm.set_user_memory_region(kvm_region).map_err(|e| {
                log::error!(
                    "BAR{}: Failed to register KVM memory region: {:?}",
                    bar_idx,
                    e
                );
                VfioError::KvmMemoryRegion(std::io::Error::new(
                    std::io::ErrorKind::Other,
                    format!("{:?}", e),
                ))
            })?;

            log::info!(
                "BAR{}: Registered KVM memory region (slot={}, guest={:#x}, host={:p}, size={:#x})",
                bar_idx,
                slot,
                guest_addr.raw(),
                mmap.addr(),
                bar_info.size
            );

            // Step 7: Store mmap and slot for cleanup
            // Update host_addr in bar_info
            if let Some(ref mut bi) = self.bars[bar_idx] {
                bi.host_addr = Some(HostVirtAddr::new(mmap.addr() as u64));
            }
            self.bar_mmaps[bar_idx] = Some(mmap);
            self.bar_kvm_slots[bar_idx] = Some(slot);
        }

        Ok(())
    }

    /// Check if a BAR is directly mapped for guest access.
    ///
    /// Returns true if the BAR has been registered as a KVM user memory region,
    /// meaning guest accesses go directly to the device without VM exits.
    ///
    /// # Arguments
    ///
    /// * `bar_idx` - BAR index (0-5)
    ///
    /// # Returns
    ///
    /// `true` if BAR is directly mapped, `false` if not (requires emulation).
    #[inline]
    pub fn is_bar_directly_mapped(&self, bar_idx: usize) -> bool {
        bar_idx < 6 && self.bar_mmaps[bar_idx].is_some()
    }

    /// Reset the device.
    pub fn reset(&self) -> VfioResult<()> {
        let ret = unsafe { libc::ioctl(self.fd.as_raw_fd(), VFIO_DEVICE_RESET) };
        if ret < 0 {
            return Err(VfioError::DeviceReset {
                bdf: self.bdf,
                source: std::io::Error::last_os_error(),
            });
        }
        Ok(())
    }

    /// Setup MSI-X interrupts for the device.
    ///
    /// This configures VFIO to signal the provided eventfds when the device
    /// generates MSI-X interrupts. The eventfds should be registered with KVM
    /// via irqfd before calling this.
    ///
    /// # Arguments
    /// * `eventfds` - Slice of eventfd file descriptors, one per MSI-X vector
    pub fn setup_msix_irqs(&self, eventfds: &[RawFd]) -> VfioResult<()> {
        if eventfds.is_empty() {
            return Ok(());
        }

        // Check device supports MSI-X
        let table_size = self.msix_table_size.ok_or(VfioError::NoMsixCapability {
            bdf: self.bdf,
        })?;

        if eventfds.len() > table_size as usize {
            return Err(VfioError::TooManyMsixVectors {
                requested: eventfds.len(),
                max: table_size as usize,
            });
        }

        // Build the VFIO_DEVICE_SET_IRQS structure
        // Layout: vfio_irq_set header followed by array of i32 eventfds
        #[repr(C)]
        struct VfioIrqSetHeader {
            argsz: u32,
            flags: u32,
            index: u32,
            start: u32,
            count: u32,
        }

        let header_size = std::mem::size_of::<VfioIrqSetHeader>();
        let data_size = eventfds.len() * std::mem::size_of::<i32>();
        let total_size = header_size + data_size;

        // Allocate buffer for the structure
        let mut buf = vec![0u8; total_size];

        // Fill in header
        let header = VfioIrqSetHeader {
            argsz: total_size as u32,
            flags: VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER,
            index: VFIO_PCI_MSIX_IRQ_INDEX,
            start: 0,
            count: eventfds.len() as u32,
        };

        // Copy header to buffer
        unsafe {
            std::ptr::copy_nonoverlapping(
                &header as *const VfioIrqSetHeader as *const u8,
                buf.as_mut_ptr(),
                header_size,
            );
        }

        // Copy eventfds to buffer (as i32s)
        let data_ptr = unsafe { buf.as_mut_ptr().add(header_size) as *mut i32 };
        for (i, &fd) in eventfds.iter().enumerate() {
            unsafe {
                *data_ptr.add(i) = fd;
            }
        }

        // Call ioctl
        let ret = unsafe {
            libc::ioctl(self.fd.as_raw_fd(), VFIO_DEVICE_SET_IRQS, buf.as_ptr())
        };

        if ret < 0 {
            return Err(VfioError::SetIrqs {
                bdf: self.bdf,
                source: std::io::Error::last_os_error(),
            });
        }

        log::debug!(
            "Configured {} MSI-X vectors for VFIO device {}",
            eventfds.len(),
            self.bdf
        );

        Ok(())
    }

    /// Disable all MSI-X interrupts for the device.
    pub fn disable_msix_irqs(&self) -> VfioResult<()> {
        #[repr(C)]
        struct VfioIrqSetHeader {
            argsz: u32,
            flags: u32,
            index: u32,
            start: u32,
            count: u32,
        }

        let header = VfioIrqSetHeader {
            argsz: std::mem::size_of::<VfioIrqSetHeader>() as u32,
            flags: VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
            index: VFIO_PCI_MSIX_IRQ_INDEX,
            start: 0,
            count: 0,  // count=0 disables the IRQ index
        };

        let ret = unsafe {
            libc::ioctl(self.fd.as_raw_fd(), VFIO_DEVICE_SET_IRQS, &header)
        };

        if ret < 0 {
            return Err(VfioError::SetIrqs {
                bdf: self.bdf,
                source: std::io::Error::last_os_error(),
            });
        }

        Ok(())
    }

    /// Setup MSI interrupts for the device.
    ///
    /// This configures VFIO to signal the provided eventfds when the device
    /// generates MSI interrupts. The eventfds should be registered with KVM
    /// via irqfd before calling this.
    ///
    /// # Arguments
    /// * `eventfds` - Slice of eventfd file descriptors, one per MSI vector
    pub fn setup_msi_irqs(&mut self, eventfds: &[RawFd]) -> VfioResult<()> {
        if eventfds.is_empty() {
            return Ok(());
        }

        // Check device supports MSI
        let max_vectors = self.msi_max_vectors().ok_or(VfioError::NoMsiCapability {
            bdf: self.bdf,
        })?;

        if eventfds.len() > max_vectors {
            return Err(VfioError::TooManyMsiVectors {
                requested: eventfds.len(),
                max: max_vectors,
            });
        }

        // Build the VFIO_DEVICE_SET_IRQS structure
        #[repr(C)]
        struct VfioIrqSetHeader {
            argsz: u32,
            flags: u32,
            index: u32,
            start: u32,
            count: u32,
        }

        let header_size = std::mem::size_of::<VfioIrqSetHeader>();
        let data_size = eventfds.len() * std::mem::size_of::<i32>();
        let total_size = header_size + data_size;

        // Allocate buffer for the structure
        let mut buf = vec![0u8; total_size];

        // Fill in header - use VFIO_PCI_MSI_IRQ_INDEX instead of MSIX
        let header = VfioIrqSetHeader {
            argsz: total_size as u32,
            flags: VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER,
            index: VFIO_PCI_MSI_IRQ_INDEX,
            start: 0,
            count: eventfds.len() as u32,
        };

        // Copy header to buffer
        unsafe {
            std::ptr::copy_nonoverlapping(
                &header as *const VfioIrqSetHeader as *const u8,
                buf.as_mut_ptr(),
                header_size,
            );
        }

        // Copy eventfds to buffer (as i32s)
        let data_ptr = unsafe { buf.as_mut_ptr().add(header_size) as *mut i32 };
        for (i, &fd) in eventfds.iter().enumerate() {
            unsafe {
                *data_ptr.add(i) = fd;
            }
        }

        // Call ioctl
        let ret = unsafe {
            libc::ioctl(self.fd.as_raw_fd(), VFIO_DEVICE_SET_IRQS, buf.as_ptr())
        };

        if ret < 0 {
            return Err(VfioError::SetIrqs {
                bdf: self.bdf,
                source: std::io::Error::last_os_error(),
            });
        }

        self.interrupt_type = InterruptType::Msi;

        log::info!(
            "Configured {} MSI vectors for VFIO device {}",
            eventfds.len(),
            self.bdf
        );

        Ok(())
    }

    /// Disable all MSI interrupts for the device.
    pub fn disable_msi_irqs(&mut self) -> VfioResult<()> {
        #[repr(C)]
        struct VfioIrqSetHeader {
            argsz: u32,
            flags: u32,
            index: u32,
            start: u32,
            count: u32,
        }

        let header = VfioIrqSetHeader {
            argsz: std::mem::size_of::<VfioIrqSetHeader>() as u32,
            flags: VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
            index: VFIO_PCI_MSI_IRQ_INDEX,
            start: 0,
            count: 0,  // count=0 disables the IRQ index
        };

        let ret = unsafe {
            libc::ioctl(self.fd.as_raw_fd(), VFIO_DEVICE_SET_IRQS, &header)
        };

        if ret < 0 {
            return Err(VfioError::SetIrqs {
                bdf: self.bdf,
                source: std::io::Error::last_os_error(),
            });
        }

        self.interrupt_type = InterruptType::None;

        Ok(())
    }

    /// Set the MSI vector group for KVM interrupt routing
    pub fn set_msi_vectors(&mut self, vectors: Arc<MsixVectorGroup>) {
        self.msi_vectors = Some(vectors);
    }

    /// Update KVM MSI routing when MSI config is updated by guest
    fn update_msi_routing(&self) {
        let Some(msi) = &self.msi_state else { return };
        let Some(vectors) = &self.msi_vectors else { return };

        if !msi.cap.is_enabled() {
            return;
        }

        let num_vectors = msi.cap.num_enabled_vectors();
        log::info!(
            "Updating MSI routing: {} vectors, addr={:#x}:{:#x} data={:#x}",
            num_vectors, msi.cap.msg_addr_hi, msi.cap.msg_addr_lo, msi.cap.msg_data
        );

        for idx in 0..num_vectors {
            // MSI uses same address/data for all vectors, but data is incremented per vector
            let config = MsixVectorConfig {
                high_addr: msi.cap.msg_addr_hi,
                low_addr: msi.cap.msg_addr_lo,
                data: msi.cap.msg_data as u32 + idx as u32,
                devid: self.pci_devid,
            };

            let masked = msi.cap.is_vector_masked(idx);

            if let Err(e) = vectors.update(idx, config, masked, true) {
                log::error!("Failed to update MSI routing for vector {}: {:?}", idx, e);
            } else {
                log::debug!(
                    "Updated MSI routing: vector={} addr={:#x}:{:#x} data={:#x} masked={}",
                    idx, msi.cap.msg_addr_hi, msi.cap.msg_addr_lo, 
                    msi.cap.msg_data as u32 + idx as u32, masked
                );
            }
        }
    }

    /// Enable all MSI vectors based on current config
    fn enable_all_msi_routing(&self) {
        let Some(msi) = &self.msi_state else { return };
        let Some(vectors) = &self.msi_vectors else { return };

        if msi.cap.is_enabled() {
            log::info!("Enabling MSI routing for {} vectors", msi.cap.num_enabled_vectors());
            self.update_msi_routing();
        } else {
            log::info!("Disabling MSI routing");
            if let Err(e) = vectors.disable() {
                log::error!("Failed to disable MSI vectors: {:?}", e);
            }
        }
    }
}

impl PciDevice for VfioPciDevice {
    fn write_config_register(
        &mut self,
        reg_idx: usize,
        offset: u64,
        data: &[u8],
    ) -> Option<Arc<Barrier>> {
        let reg_offset = (reg_idx * 4) as u64 + offset;

        // Intercept BAR writes for sizing protocol
        if (0x10..=0x27).contains(&reg_offset) {
            let bar_idx = ((reg_offset - 0x10) / 4) as usize;
            if bar_idx < 6 && data.len() >= 4 {
                let write_val = u32::from_le_bytes([data[0], data[1], data[2], data[3]]);
                
                // Check if this is a sizing probe (all 1s written)
                if write_val == 0xFFFFFFFF {
                    // Check if this is the upper half of a 64-bit BAR
                    let is_upper_half = bar_idx > 0 && 
                        self.bars[bar_idx].is_none() && 
                        self.bars[bar_idx - 1].as_ref().map_or(false, |b| b.is_64bit);
                    
                    if is_upper_half {
                        // Upper half of 64-bit BAR - return upper size mask
                        if let Some(ref bar_info) = self.bars[bar_idx - 1] {
                            let size = bar_info.size;
                            // Upper 32 bits of the size mask
                            let size_mask_full = if size > 0 { !(size - 1) } else { 0 };
                            let upper_mask = (size_mask_full >> 32) as u32;
                            self.shadow_bars[bar_idx] = upper_mask as u64;
                        }
                    } else if let Some(ref bar_info) = self.bars[bar_idx] {
                        // Lower BAR or 32-bit BAR
                        let size = bar_info.size;
                        let type_bits = if bar_info.is_io {
                            0x1
                        } else {
                            let mut bits = 0u32;
                            if bar_info.is_64bit {
                                bits |= 0x4;
                            }
                            if bar_info.is_prefetchable {
                                bits |= 0x8;
                            }
                            bits
                        };
                        
                        // Size mask: ~(size - 1) gives the alignment mask
                        // Lower 32 bits only for the low BAR register
                        let size_mask_full = if size > 0 { !(size - 1) } else { 0 };
                        let size_mask = (size_mask_full as u32) | type_bits;
                        
                        self.shadow_bars[bar_idx] = size_mask as u64;
                    }
                } else {
                    // Normal write - restore the actual BAR value or set to 0 if unallocated
                    // Check if this is upper half of 64-bit BAR
                    let is_upper_half = bar_idx > 0 && 
                        self.bars[bar_idx].is_none() && 
                        self.bars[bar_idx - 1].as_ref().map_or(false, |b| b.is_64bit);
                    
                    if is_upper_half {
                        // Restore upper 32 bits of the 64-bit BAR address
                        if let Some(ref bar_info) = self.bars[bar_idx - 1] {
                            if let Some(guest_addr) = bar_info.guest_addr {
                                self.shadow_bars[bar_idx] = guest_addr.raw() >> 32;
                            } else {
                                // BAR not allocated - set to 0
                                self.shadow_bars[bar_idx] = 0;
                            }
                        } else {
                            self.shadow_bars[bar_idx] = 0;
                        }
                    } else if let Some(ref bar_info) = self.bars[bar_idx] {
                        if let Some(guest_addr) = bar_info.guest_addr {
                            let mut bar_val = guest_addr.raw();
                            if bar_info.is_io {
                                bar_val |= 0x1;
                            } else {
                                if bar_info.is_64bit {
                                    bar_val |= 0x4;
                                }
                                if bar_info.is_prefetchable {
                                    bar_val |= 0x8;
                                }
                            }
                            self.shadow_bars[bar_idx] = bar_val;
                            if bar_info.is_64bit && bar_idx < 5 {
                                self.shadow_bars[bar_idx + 1] = guest_addr.raw() >> 32;
                            }
                        } else {
                            // BAR exists but wasn't allocated (e.g., I/O BAR we skipped)
                            // Set to 0 to indicate disabled
                            self.shadow_bars[bar_idx] = 0;
                        }
                    } else {
                        // No BAR info at this index - just set to 0
                        self.shadow_bars[bar_idx] = 0;
                    }
                }
            }
            return None;
        }

        // Check for MSI-X control register write
        if let Some(msix) = &self.msix_state {
            let msix_ctrl_offset = msix.cap_offset as u64 + 2;
            if reg_offset == msix_ctrl_offset || (reg_offset == msix.cap_offset as u64 && offset == 2) {
                // MSI-X Message Control register is being written
                if data.len() >= 2 {
                    let msg_ctrl = u16::from_le_bytes([data[0], data[1]]);
                    let new_enabled = (msg_ctrl >> 15) & 1 == 1;
                    let new_masked = (msg_ctrl >> 14) & 1 == 1;
                    
                    log::info!(
                        "MSI-X control write: enabled={} masked={} (raw={:#x})",
                        new_enabled, new_masked, msg_ctrl
                    );

                    // Update our state
                    if let Some(msix) = &mut self.msix_state {
                        let state_changed = msix.enabled != new_enabled || msix.function_masked != new_masked;
                        msix.enabled = new_enabled;
                        msix.function_masked = new_masked;
                        
                        if state_changed {
                            // Forward write to device first
                            let _ = Self::write_config_raw(self.fd.as_raw_fd(), reg_offset, data);
                            // Then update KVM routing
                            self.update_all_msix_routing();
                            return None;
                        }
                    }
                }
            }
        }

        // Check for MSI capability register writes
        if let Some(msi) = &self.msi_state {
            if msi.contains_offset(reg_offset) {
                let relative_offset = msi.relative_offset(reg_offset);
                
                log::info!(
                    "MSI config write at {:#x} (relative {:#x}), len={}, data={:02x?}",
                    reg_offset, relative_offset, data.len(), data
                );

                // Update our local MSI state
                if let Some(msi) = &mut self.msi_state {
                    let old_enabled = msi.cap.is_enabled();
                    let state_changed = msi.cap.update(relative_offset, data);
                    let new_enabled = msi.cap.is_enabled();

                    log::info!(
                        "MSI state update: enabled {} -> {}, addr={:#x}:{:#x} data={:#x}",
                        old_enabled, new_enabled,
                        msi.cap.msg_addr_hi, msi.cap.msg_addr_lo, msi.cap.msg_data
                    );

                    // Forward write to device first
                    let _ = Self::write_config_raw(self.fd.as_raw_fd(), reg_offset, data);

                    // Update KVM routing if state changed
                    if state_changed {
                        self.enable_all_msi_routing();
                    } else if new_enabled {
                        // Address or data changed while enabled - update routing
                        self.update_msi_routing();
                    }
                    return None;
                }
            }
        }

        // Intercept Device Control 2 (DevCtl2) writes to prevent LTR enable attempts
        // DevCtl2 is at PCIe capability offset + 0x28
        // The NVIDIA driver tries to enable LTR (bit 10) which will fail and cause delays
        // We intercept this write, mask out the LTR enable bit, and forward the rest
        if let Some(pcie_cap) = self.pcie_cap_offset {
            let devctl2_offset = pcie_cap as u64 + 0x28;
            if reg_offset == devctl2_offset && data.len() >= 2 {
                let mut devctl2 = u16::from_le_bytes([data[0], data[1]]);
                const LTR_ENABLE: u16 = 1 << 10;
                if (devctl2 & LTR_ENABLE) != 0 {
                    log::info!(
                        "Intercepted DevCtl2 LTR enable attempt (value {:#x}), masking LTR bit",
                        devctl2
                    );
                    devctl2 &= !LTR_ENABLE;
                    let masked_data = devctl2.to_le_bytes();
                    let _ = Self::write_config_raw(self.fd.as_raw_fd(), reg_offset, &masked_data);
                    return None;
                }
            }
        }

        // Forward other config writes to device
        let _ = Self::write_config_raw(self.fd.as_raw_fd(), reg_offset, data);
        None
    }

    fn read_config_register(&mut self, reg_idx: usize) -> u32 {
        let offset = (reg_idx * 4) as u64;
        
        // For BARs, return shadow values (guest addresses)
        if (4..=9).contains(&reg_idx) {
            let bar_idx = reg_idx - 4;
            let val = self.shadow_bars[bar_idx] as u32;
            // Log first few BAR reads
            static LOGGED: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
            if LOGGED.fetch_add(1, std::sync::atomic::Ordering::Relaxed) < 20 {
                log::info!("read_config BAR{} (reg_idx={}) -> {:#x}", bar_idx, reg_idx, val);
            }
            return val;
        }

        // Read from device
        let mut data = [0u8; 4];
        let _ = Self::read_config_raw(self.fd.as_raw_fd(), offset, &mut data);
        let mut val = u32::from_le_bytes(data);

        // Mask unsupported PCIe features in Device Capabilities 2 (DevCap2)
        // DevCap2 is at PCIe capability offset + 0x24
        // We mask:
        //   - Bit 11: LTR Mechanism Supported (causes ~30s delay if advertised but not supported in hierarchy)
        //   - Bit 18-19: OBFF Supported (Optimized Buffer Flush/Fill) - not supported by virtual root
        // This prevents the NVIDIA driver from trying to enable these features and timing out.
        if let Some(pcie_cap) = self.pcie_cap_offset {
            let devcap2_offset = pcie_cap as u64 + 0x24;
            if offset == devcap2_offset {
                const LTR_MECHANISM_SUPPORTED: u32 = 1 << 11;
                const OBFF_SUPPORTED_MASK: u32 = 0x3 << 18; // Bits 18-19
                let original = val;
                val &= !(LTR_MECHANISM_SUPPORTED | OBFF_SUPPORTED_MASK);
                if val != original {
                    log::info!(
                        "Masked DevCap2: {:#x} -> {:#x} (hiding LTR/OBFF)",
                        original, val
                    );
                }
            }

            // Also mask Device Control 2 (DevCtl2) at PCIe cap + 0x28
            // Clear bit 10: LTR Enable - return it as always disabled
            let devctl2_offset = pcie_cap as u64 + 0x28;
            if offset == devctl2_offset {
                const LTR_ENABLE: u32 = 1 << 10;
                val &= !LTR_ENABLE;
            }
        }

        // Hide LTR Extended Capability (cap ID 0x18 in extended config space)
        // When the guest tries to read the LTR capability, return that it doesn't exist
        // by zeroing the capability header. This prevents the NVIDIA driver from trying
        // to configure LTR and timing out.
        if let Some(ltr_offset) = self.ltr_ext_cap_offset {
            // LTR extended capability is 8 bytes:
            //   - Offset +0: Header (cap ID, version, next)
            //   - Offset +4: Max Snoop/No-Snoop Latency
            // Return 0 for both to indicate capability is not present/functional
            if offset == ltr_offset as u64 || offset == (ltr_offset + 4) as u64 {
                log::info!("Hiding LTR Extended Capability read at offset {:#x}", offset);
                val = 0;
            }
        }

        val
    }

    fn read_bar(&mut self, base: u64, offset: u64, data: &mut [u8]) {
        // Find which BAR this is
        // Note: Directly-mapped BARs don't hit this code path - KVM handles them.
        // This is only called for unmapped BARs or sparse regions (e.g., MSI-X table).
        for (bar_idx, bar_info) in self.bars.iter().enumerate().filter_map(|(i, b)| b.as_ref().map(|b| (i, b))) {
            if let Some(guest_addr) = bar_info.guest_addr {
                if guest_addr.raw() == base {
                    // Check if this is an MSI-X table access
                    if self.is_msix_table_access(bar_idx as u8, offset) {
                        self.read_msix_table(offset, data);
                        return;
                    }

                    // Read from VFIO region
                    let region_offset = bar_info.region_info.offset + offset;
                    let _ = unsafe {
                        libc::pread(
                            self.fd.as_raw_fd(),
                            data.as_mut_ptr() as *mut libc::c_void,
                            data.len(),
                            region_offset as i64,
                        )
                    };
                    return;
                }
            }
        }
    }

    fn write_bar(&mut self, base: u64, offset: u64, data: &[u8]) -> Option<Arc<Barrier>> {
        // Find which BAR this is
        for (bar_idx, bar_info) in self.bars.iter().enumerate().filter_map(|(i, b)| b.as_ref().map(|b| (i, b))) {
            if let Some(guest_addr) = bar_info.guest_addr {
                if guest_addr.raw() == base {
                    // Check if this is an MSI-X table access
                    if self.is_msix_table_access(bar_idx as u8, offset) {
                        // Update our local MSI-X table copy
                        if let Some(vector_idx) = self.write_msix_table(offset, data) {
                            // Update KVM routing for this vector if MSI-X is enabled
                            self.update_msix_routing(vector_idx);
                        }
                        return None;
                    }

                    // Write to VFIO region
                    let region_offset = bar_info.region_info.offset + offset;
                    let _ = unsafe {
                        libc::pwrite(
                            self.fd.as_raw_fd(),
                            data.as_ptr() as *const libc::c_void,
                            data.len(),
                            region_offset as i64,
                        )
                    };
                    return None;
                }
            }
        }
        None
    }

    fn detect_bar_reprogramming(
        &mut self,
        reg_idx: usize,
        _data: &[u8],
    ) -> Option<BarReprogrammingParams> {
        // We handle BAR addressing internally, no reprogramming needed
        // since we use fixed guest addresses
        None
    }
}

/// Implement BusDevice for VfioPciDevice so it can be inserted into the MMIO bus.
impl crate::vstate::bus::BusDevice for VfioPciDevice {
    fn read(&mut self, base: u64, offset: u64, data: &mut [u8]) {
        // Forward to PciDevice::read_bar
        self.read_bar(base, offset, data);
    }

    fn write(&mut self, base: u64, offset: u64, data: &[u8]) -> Option<std::sync::Arc<std::sync::Barrier>> {
        // Forward to PciDevice::write_bar
        self.write_bar(base, offset, data)
    }
}

/// Open a VFIO device from its PCI address.
pub fn open_vfio_device(
    container: &mut VfioContainer,
    bdf: PciBdf,
    id: String,
    gpudirect_clique: Option<u8>,
) -> VfioResult<VfioPciDevice> {
    let sysfs_path = PathBuf::from(bdf.sysfs_path());

    // Check device exists
    if !sysfs_path.exists() {
        return Err(VfioError::DeviceNotFound {
            bdf,
            path: sysfs_path,
        });
    }

    // Check bound to vfio-pci
    let driver_link = sysfs_path.join("driver");
    let current_driver = std::fs::read_link(&driver_link)
        .ok()
        .and_then(|p| p.file_name().map(|s| s.to_string_lossy().into_owned()));

    if current_driver.as_deref() != Some("vfio-pci") {
        return Err(VfioError::NotVfioBound {
            bdf,
            current_driver,
        });
    }

    // Get IOMMU group
    let group_link = sysfs_path.join("iommu_group");
    let group_path = std::fs::read_link(&group_link).map_err(VfioError::IommuGroupRead)?;
    let group_id: u32 = group_path
        .file_name()
        .and_then(|s| s.to_str())
        .and_then(|s| s.parse().ok())
        .ok_or(VfioError::NoIommuGroup)?;

    // Open group
    let group_path = format!("/dev/vfio/{}", group_id);
    let group_fd = std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .open(&group_path)
        .map_err(|e| VfioError::GroupOpen {
            group: group_id,
            source: e,
        })?;

    // Check group is viable
    #[repr(C)]
    struct VfioGroupStatus {
        argsz: u32,
        flags: u32,
    }

    let mut status = VfioGroupStatus {
        argsz: std::mem::size_of::<VfioGroupStatus>() as u32,
        flags: 0,
    };

    let ret = unsafe { libc::ioctl(group_fd.as_raw_fd(), VFIO_GROUP_GET_STATUS, &mut status) };
    if ret < 0 {
        return Err(VfioError::GroupOpen {
            group: group_id,
            source: std::io::Error::last_os_error(),
        });
    }

    // VFIO_GROUP_FLAGS_VIABLE = 1
    if (status.flags & 1) == 0 {
        // Group is not viable - all devices in the group must be bound to vfio-pci.
        // Log a warning but try to continue - some setups (like ACS override) may work anyway.
        log::warn!(
            "IOMMU group {} is not fully viable (flags={:#x}). Some devices may not be bound to vfio-pci. \
             Attempting to continue anyway - this may fail or be unsafe.",
            group_id, status.flags
        );
        // Don't error out - let the container setup fail naturally if it can't work
    }

    // Set container for group
    let container_fd = container.as_raw_fd();
    let ret = unsafe { libc::ioctl(group_fd.as_raw_fd(), VFIO_GROUP_SET_CONTAINER, &container_fd) };
    if ret < 0 {
        return Err(VfioError::GroupSetContainer {
            group: group_id,
            source: std::io::Error::last_os_error(),
        });
    }

    // Configure IOMMU type (only needs to be done once per container)
    container.set_iommu_type()?;

    // Get device fd
    let bdf_str = std::ffi::CString::new(bdf.to_string()).unwrap();
    let device_fd = unsafe { libc::ioctl(group_fd.as_raw_fd(), VFIO_GROUP_GET_DEVICE_FD, bdf_str.as_ptr()) };
    if device_fd < 0 {
        return Err(VfioError::GetDeviceFd {
            bdf,
            source: std::io::Error::last_os_error(),
        });
    }

    let device_fd = unsafe { OwnedFd::from_raw_fd(device_fd) };

    VfioPciDevice::new(id, bdf, device_fd, group_id, gpudirect_clique)
}

// Need this for OwnedFd::from_raw_fd
use std::os::unix::io::FromRawFd;

/// Mock structures for testing without real VFIO hardware
#[cfg(test)]
pub mod mock {
    use super::*;

    /// Mock config space for testing shadow BAR behavior.
    /// Simulates a GPU with typical BAR layout.
    #[derive(Debug)]
    pub struct MockConfigSpace {
        /// Raw config space (4KB for PCIe)
        data: [u8; 4096],
        /// Shadow BAR values (what guest sees)
        shadow_bars: [u64; 6],
        /// Actual BAR sizes
        bar_sizes: [u64; 6],
        /// BAR types (true = 64-bit)
        bar_64bit: [bool; 6],
    }

    impl MockConfigSpace {
        /// Create a mock config space resembling an NVIDIA GPU
        pub fn new_nvidia_gpu() -> Self {
            let mut data = [0u8; 4096];
            
            // Vendor ID: NVIDIA (0x10de)
            data[0] = 0xde;
            data[1] = 0x10;
            // Device ID: RTX 6000 (0x2bb1)
            data[2] = 0xb1;
            data[3] = 0x2b;
            // Command register
            data[4] = 0x07; // I/O, Memory, Bus Master enabled
            data[5] = 0x00;
            // Status register
            data[6] = 0x10; // Capabilities list present
            data[7] = 0x00;
            // Revision ID
            data[8] = 0xa1;
            // Class code: VGA compatible controller (0x030000)
            data[9] = 0x00;  // Prog IF
            data[10] = 0x00; // Subclass
            data[11] = 0x03; // Base class
            
            // Header type: 0 (standard)
            data[14] = 0x00;
            
            // BAR0: 32-bit memory, non-prefetchable (16MB control regs)
            // Value encodes: bit 0 = 0 (memory), bits 2:1 = 00 (32-bit), bit 3 = 0 (non-prefetch)
            data[0x10] = 0x00;
            data[0x11] = 0x00;
            data[0x12] = 0x00;
            data[0x13] = 0x00;
            
            // BAR1: 64-bit memory, prefetchable (256MB VRAM aperture)
            // Value encodes: bit 0 = 0, bits 2:1 = 10 (64-bit), bit 3 = 1 (prefetchable)
            data[0x14] = 0x0c; // 64-bit, prefetchable
            data[0x15] = 0x00;
            data[0x16] = 0x00;
            data[0x17] = 0x00;
            // BAR2 is upper 32 bits of BAR1
            data[0x18] = 0x00;
            data[0x19] = 0x00;
            data[0x1a] = 0x00;
            data[0x1b] = 0x00;
            
            // BAR3: 64-bit memory, prefetchable (32MB)
            data[0x1c] = 0x0c;
            data[0x1d] = 0x00;
            data[0x1e] = 0x00;
            data[0x1f] = 0x00;
            // BAR4 is upper 32 bits of BAR3
            data[0x20] = 0x00;
            data[0x21] = 0x00;
            data[0x22] = 0x00;
            data[0x23] = 0x00;
            
            // BAR5: I/O ports (128 bytes) - uncommon on modern GPUs
            data[0x24] = 0x01; // I/O space
            data[0x25] = 0x00;
            data[0x26] = 0x00;
            data[0x27] = 0x00;
            
            // Capabilities pointer
            data[0x34] = 0x60; // First capability at offset 0x60
            
            // MSI-X capability at 0x60
            data[0x60] = 0x11; // Cap ID = MSI-X
            data[0x61] = 0x70; // Next cap at 0x70
            data[0x62] = 0xff; // Message control: 256 vectors (0xff + 1), disabled
            data[0x63] = 0x00;
            data[0x64] = 0x00; // Table offset/BIR: BAR0, offset 0
            data[0x65] = 0x00;
            data[0x66] = 0x00;
            data[0x67] = 0x00;
            data[0x68] = 0x00; // PBA offset/BIR: BAR0, offset 0x1000
            data[0x69] = 0x10;
            data[0x6a] = 0x00;
            data[0x6b] = 0x00;
            
            // PCIe capability at 0x70
            data[0x70] = 0x10; // Cap ID = PCI Express
            data[0x71] = 0x00; // Next cap = end
            
            Self {
                data,
                shadow_bars: [0; 6],
                bar_sizes: [
                    0x100_0000,   // BAR0: 16MB
                    0x1000_0000,  // BAR1: 256MB (64-bit, uses BAR2)
                    0,            // BAR2: upper bits of BAR1
                    0x200_0000,   // BAR3: 32MB (64-bit, uses BAR4)
                    0,            // BAR4: upper bits of BAR3
                    0x80,         // BAR5: 128 bytes I/O
                ],
                bar_64bit: [false, true, false, true, false, false],
            }
        }

        /// Read from config space (returns shadow BAR values for BARs)
        pub fn read(&self, offset: usize, size: usize) -> u32 {
            // BAR region: 0x10-0x27
            if offset >= 0x10 && offset < 0x28 {
                let bar_idx = (offset - 0x10) / 4;
                if bar_idx < 6 {
                    let bar_val = self.shadow_bars[bar_idx];
                    // Return appropriate portion based on offset within BAR
                    return bar_val as u32;
                }
            }
            
            // Everything else comes from actual config space
            let mut val = 0u32;
            for i in 0..size.min(4) {
                if offset + i < self.data.len() {
                    val |= (self.data[offset + i] as u32) << (i * 8);
                }
            }
            val
        }

        /// Write to config space (intercept BAR writes)
        pub fn write(&mut self, offset: usize, value: u32) {
            // BAR region: 0x10-0x27
            if offset >= 0x10 && offset < 0x28 {
                let bar_idx = (offset - 0x10) / 4;
                if bar_idx < 6 {
                    // Update shadow BAR
                    self.shadow_bars[bar_idx] = value as u64;
                    return;
                }
            }
            
            // Write to actual config space for non-BAR regions
            let bytes = value.to_le_bytes();
            for (i, &b) in bytes.iter().enumerate() {
                if offset + i < self.data.len() {
                    self.data[offset + i] = b;
                }
            }
        }

        /// Set shadow BAR value (as VMM would do during allocation)
        pub fn set_shadow_bar(&mut self, bar: usize, guest_addr: u64) {
            if bar < 6 {
                self.shadow_bars[bar] = guest_addr;
                // For 64-bit BARs, set upper bits too
                if self.bar_64bit[bar] && bar < 5 {
                    self.shadow_bars[bar + 1] = guest_addr >> 32;
                }
            }
        }

        /// Get BAR size (for allocation)
        pub fn bar_size(&self, bar: usize) -> u64 {
            if bar < 6 { self.bar_sizes[bar] } else { 0 }
        }

        /// Check if BAR is 64-bit
        pub fn is_bar_64bit(&self, bar: usize) -> bool {
            if bar < 6 { self.bar_64bit[bar] } else { false }
        }

        /// Parse MSI-X capability
        pub fn msix_info(&self) -> Option<(u16, usize, usize)> {
            // Walk capability list
            let mut cap_ptr = self.data[0x34] as usize;
            while cap_ptr != 0 && cap_ptr < 256 {
                let cap_id = self.data[cap_ptr];
                if cap_id == 0x11 {
                    // MSI-X capability
                    let msg_ctrl = u16::from_le_bytes([
                        self.data[cap_ptr + 2],
                        self.data[cap_ptr + 3],
                    ]);
                    let table_size = (msg_ctrl & 0x7ff) + 1;
                    let table_offset = u32::from_le_bytes([
                        self.data[cap_ptr + 4],
                        self.data[cap_ptr + 5],
                        self.data[cap_ptr + 6],
                        self.data[cap_ptr + 7],
                    ]);
                    let table_bar = (table_offset & 0x7) as usize;
                    return Some((table_size, table_bar, (table_offset & !0x7) as usize));
                }
                cap_ptr = self.data[cap_ptr + 1] as usize;
            }
            None
        }

        /// Get vendor ID
        pub fn vendor_id(&self) -> u16 {
            u16::from_le_bytes([self.data[0], self.data[1]])
        }

        /// Get device ID  
        pub fn device_id(&self) -> u16 {
            u16::from_le_bytes([self.data[2], self.data[3]])
        }
    }

    /// Mock BAR allocator for testing
    #[derive(Debug)]
    pub struct MockBarAllocator {
        /// Next available address
        next_addr: u64,
        /// Allocated regions: (start, size)
        allocations: Vec<(u64, u64)>,
    }

    impl MockBarAllocator {
        pub fn new(base: u64) -> Self {
            Self {
                next_addr: base,
                allocations: Vec::new(),
            }
        }

        /// Allocate space for a BAR, respecting alignment
        pub fn allocate(&mut self, size: u64) -> Option<u64> {
            if size == 0 {
                return None;
            }
            
            // Align to size (BAR addresses must be naturally aligned)
            let aligned = (self.next_addr + size - 1) & !(size - 1);
            
            // Check for overflow
            if aligned.checked_add(size).is_none() {
                return None;
            }
            
            let addr = aligned;
            self.next_addr = aligned + size;
            self.allocations.push((addr, size));
            
            Some(addr)
        }

        /// Check if an address is within any allocation
        pub fn contains(&self, addr: u64) -> bool {
            self.allocations.iter().any(|(start, size)| {
                addr >= *start && addr < start + size
            })
        }

        /// Get all allocations
        pub fn allocations(&self) -> &[(u64, u64)] {
            &self.allocations
        }
    }
}

#[cfg(test)]
mod tests {
    use super::mock::*;
    use super::*;

    #[test]
    fn test_mock_config_space_nvidia() {
        let cfg = MockConfigSpace::new_nvidia_gpu();
        
        // Check vendor/device ID
        assert_eq!(cfg.vendor_id(), 0x10de); // NVIDIA
        assert_eq!(cfg.device_id(), 0x2bb1); // RTX 6000
        
        // Check BAR sizes
        assert_eq!(cfg.bar_size(0), 0x100_0000);  // 16MB
        assert_eq!(cfg.bar_size(1), 0x1000_0000); // 256MB
        assert_eq!(cfg.bar_size(3), 0x200_0000);  // 32MB
        
        // Check 64-bit flags
        assert!(!cfg.is_bar_64bit(0));
        assert!(cfg.is_bar_64bit(1));
        assert!(cfg.is_bar_64bit(3));
    }

    #[test]
    fn test_shadow_bar_isolation() {
        let mut cfg = MockConfigSpace::new_nvidia_gpu();
        
        // Set shadow BAR values (guest addresses)
        cfg.set_shadow_bar(0, 0xE000_0000);         // BAR0 at 3.5GB
        cfg.set_shadow_bar(1, 0x4_0000_0000);       // BAR1 at 16GB (64-bit)
        cfg.set_shadow_bar(3, 0x4_1000_0000);       // BAR3 at 16GB + 256MB
        
        // Reading BARs should return shadow values, not real config
        assert_eq!(cfg.read(0x10, 4), 0xE000_0000);
        assert_eq!(cfg.read(0x14, 4), 0x0000_0000); // Low 32 bits of BAR1
        assert_eq!(cfg.read(0x18, 4), 0x0000_0004); // High 32 bits of BAR1
        
        // Reading non-BAR config should return actual values
        assert_eq!(cfg.read(0, 2), 0x10de); // Vendor ID
    }

    #[test]
    fn test_msix_capability_parsing() {
        let cfg = MockConfigSpace::new_nvidia_gpu();
        
        let msix = cfg.msix_info();
        assert!(msix.is_some());
        
        let (table_size, table_bar, table_offset) = msix.unwrap();
        assert_eq!(table_size, 256); // 0xff + 1
        assert_eq!(table_bar, 0);    // BAR0
        assert_eq!(table_offset, 0); // Offset 0
    }

    #[test]
    fn test_bar_allocator_alignment() {
        let mut alloc = MockBarAllocator::new(0x8000_0000); // Start at 2GB
        
        // Allocate 16MB BAR (must be 16MB aligned)
        let bar0 = alloc.allocate(0x100_0000).unwrap();
        assert_eq!(bar0 & 0xFF_FFFF, 0); // 16MB aligned
        
        // Allocate 256MB BAR (must be 256MB aligned)
        let bar1 = alloc.allocate(0x1000_0000).unwrap();
        assert_eq!(bar1 & 0xFFF_FFFF, 0); // 256MB aligned
        assert!(bar1 > bar0); // Must be after BAR0
        
        // Allocate 32MB BAR
        let bar3 = alloc.allocate(0x200_0000).unwrap();
        assert_eq!(bar3 & 0x1FF_FFFF, 0); // 32MB aligned
        assert!(bar3 > bar1);
    }

    #[test]
    fn test_bar_allocator_no_overlap() {
        let mut alloc = MockBarAllocator::new(0x8000_0000);
        
        let bar0 = alloc.allocate(0x100_0000).unwrap();
        let bar1 = alloc.allocate(0x1000_0000).unwrap();
        let bar3 = alloc.allocate(0x200_0000).unwrap();
        
        // Check no overlaps
        assert!(bar0 + 0x100_0000 <= bar1);
        assert!(bar1 + 0x1000_0000 <= bar3);
        
        // Check contains
        assert!(alloc.contains(bar0));
        assert!(alloc.contains(bar0 + 0x1000));
        assert!(alloc.contains(bar1 + 0x100_0000));
        assert!(!alloc.contains(0x1000)); // Before allocations
    }

    #[test]
    fn test_64bit_bar_shadow() {
        let mut cfg = MockConfigSpace::new_nvidia_gpu();
        
        // Set a 64-bit BAR to address above 4GB
        let addr = 0x1_8000_0000u64; // 6GB
        cfg.set_shadow_bar(1, addr);
        
        // Low 32 bits in BAR1
        let low = cfg.read(0x14, 4);
        // High 32 bits in BAR2
        let high = cfg.read(0x18, 4);
        
        let reconstructed = (high as u64) << 32 | low as u64;
        assert_eq!(reconstructed, addr);
    }
}

#[cfg(test)]
mod proptests {
    use super::mock::*;
    use proptest::prelude::*;

    proptest! {
        /// Property: BAR allocations are always naturally aligned
        #[test]
        fn prop_bar_alignment(
            base in 0x8000_0000u64..0xF000_0000u64,
            sizes in proptest::collection::vec(
                proptest::sample::select(vec![
                    0x1000u64,      // 4KB
                    0x10000,        // 64KB
                    0x100000,       // 1MB
                    0x1000000,      // 16MB
                    0x10000000,     // 256MB
                ]),
                1..6
            )
        ) {
            let mut alloc = MockBarAllocator::new(base);
            
            for size in sizes {
                if let Some(addr) = alloc.allocate(size) {
                    // Must be naturally aligned
                    prop_assert_eq!(addr & (size - 1), 0,
                        "BAR at {:#x} not aligned to size {:#x}", addr, size);
                }
            }
        }

        /// Property: BAR allocations never overlap
        #[test]
        fn prop_bar_no_overlap(
            base in 0x8000_0000u64..0xC000_0000u64,
            sizes in proptest::collection::vec(
                0x1000u64..0x1000_0000u64,
                1..10
            )
        ) {
            let mut alloc = MockBarAllocator::new(base);
            let mut regions: Vec<(u64, u64)> = Vec::new();
            
            for size in sizes {
                // Round size to power of 2 for realistic BAR sizes
                let size = size.next_power_of_two();
                
                if let Some(addr) = alloc.allocate(size) {
                    // Check no overlap with previous
                    for &(prev_addr, prev_size) in &regions {
                        let no_overlap = addr + size <= prev_addr || prev_addr + prev_size <= addr;
                        prop_assert!(no_overlap,
                            "Overlap: [{:#x}, {:#x}) vs [{:#x}, {:#x})",
                            addr, addr + size, prev_addr, prev_addr + prev_size);
                    }
                    regions.push((addr, size));
                }
            }
        }

        /// Property: Shadow BAR reads return shadow values, not config
        #[test]
        fn prop_shadow_bar_isolation(
            bar0_addr in 0xE000_0000u64..0xF000_0000u64,
            bar1_addr in 0x1_0000_0000u64..0x10_0000_0000u64,
        ) {
            let mut cfg = MockConfigSpace::new_nvidia_gpu();
            
            cfg.set_shadow_bar(0, bar0_addr);
            cfg.set_shadow_bar(1, bar1_addr);
            
            // Reading BAR0 should return shadow
            prop_assert_eq!(cfg.read(0x10, 4) as u64, bar0_addr);
            
            // Reading BAR1 (64-bit) low should return low 32 bits
            prop_assert_eq!(cfg.read(0x14, 4) as u64, bar1_addr & 0xFFFF_FFFF);
            
            // Reading BAR2 should return high 32 bits of BAR1
            prop_assert_eq!(cfg.read(0x18, 4) as u64, bar1_addr >> 32);
        }

        /// Property: Config space reads outside BAR region are stable
        #[test]
        fn prop_non_bar_config_stable(offset in 0usize..0x10usize) {
            let cfg = MockConfigSpace::new_nvidia_gpu();
            
            let val1 = cfg.read(offset, 4);
            let val2 = cfg.read(offset, 4);
            
            prop_assert_eq!(val1, val2, "Config read not stable at offset {:#x}", offset);
        }
    }
}
