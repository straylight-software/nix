// Copyright 2025 Isospin Authors
// SPDX-License-Identifier: Apache-2.0
//
//! Type-safe address types and PCI identifiers for VFIO.
//!
//! These newtypes prevent accidental mixing of different address spaces,
//! which is a common source of bugs in device passthrough code.

use std::fmt;

/// Guest Physical Address - address as seen by the guest kernel.
/// This is what the guest driver uses when programming device DMA.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct GuestPhysAddr(pub u64);

impl GuestPhysAddr {
    pub const fn new(addr: u64) -> Self {
        Self(addr)
    }

    pub const fn raw(self) -> u64 {
        self.0
    }
}

impl fmt::Debug for GuestPhysAddr {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "GPA({:#x})", self.0)
    }
}

impl fmt::Display for GuestPhysAddr {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:#x}", self.0)
    }
}

/// Host Virtual Address - address in the VMM's address space.
/// This is what we use when mmap'ing device BARs.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct HostVirtAddr(pub u64);

impl HostVirtAddr {
    pub const fn new(addr: u64) -> Self {
        Self(addr)
    }

    pub const fn raw(self) -> u64 {
        self.0
    }

    pub fn as_ptr<T>(self) -> *mut T {
        self.0 as *mut T
    }
}

impl fmt::Debug for HostVirtAddr {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "HVA({:#x})", self.0)
    }
}

/// I/O Virtual Address - address used by the IOMMU.
/// For our passthrough setup, IOVA == GPA (identity mapping).
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct IovaAddr(pub u64);

impl IovaAddr {
    pub const fn new(addr: u64) -> Self {
        Self(addr)
    }

    pub const fn raw(self) -> u64 {
        self.0
    }

    /// Create IOVA from GPA using identity mapping.
    /// This is the standard setup for device passthrough.
    pub const fn from_gpa(gpa: GuestPhysAddr) -> Self {
        Self(gpa.0)
    }
}

impl fmt::Debug for IovaAddr {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "IOVA({:#x})", self.0)
    }
}

impl From<GuestPhysAddr> for IovaAddr {
    fn from(gpa: GuestPhysAddr) -> Self {
        Self::from_gpa(gpa)
    }
}

/// PCI Bus:Device.Function address
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub struct PciBdf {
    /// Domain (segment), usually 0
    pub domain: u16,
    /// Bus number (0-255)
    pub bus: u8,
    /// Device number (0-31)
    pub device: u8,
    /// Function number (0-7)
    pub function: u8,
}

impl PciBdf {
    pub const fn new(domain: u16, bus: u8, device: u8, function: u8) -> Self {
        Self {
            domain,
            bus,
            device,
            function,
        }
    }

    /// Parse from string like "0000:01:00.0"
    pub fn parse(s: &str) -> Option<Self> {
        let parts: Vec<&str> = s.split(':').collect();
        if parts.len() != 3 {
            return None;
        }

        let domain = u16::from_str_radix(parts[0], 16).ok()?;
        let bus = u8::from_str_radix(parts[1], 16).ok()?;

        let dev_fn: Vec<&str> = parts[2].split('.').collect();
        if dev_fn.len() != 2 {
            return None;
        }

        let device = u8::from_str_radix(dev_fn[0], 16).ok()?;
        let function = u8::from_str_radix(dev_fn[1], 16).ok()?;

        Some(Self {
            domain,
            bus,
            device,
            function,
        })
    }

    /// Get sysfs path for this device
    pub fn sysfs_path(&self) -> String {
        format!(
            "/sys/bus/pci/devices/{:04x}:{:02x}:{:02x}.{:x}",
            self.domain, self.bus, self.device, self.function
        )
    }
}

impl fmt::Debug for PciBdf {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{:04x}:{:02x}:{:02x}.{:x}",
            self.domain, self.bus, self.device, self.function
        )
    }
}

impl fmt::Display for PciBdf {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{:04x}:{:02x}:{:02x}.{:x}",
            self.domain, self.bus, self.device, self.function
        )
    }
}

/// PCI BAR index (0-5 for standard BARs, 6 for expansion ROM)
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BarIndex(pub u8);

impl BarIndex {
    pub const BAR0: Self = Self(0);
    pub const BAR1: Self = Self(1);
    pub const BAR2: Self = Self(2);
    pub const BAR3: Self = Self(3);
    pub const BAR4: Self = Self(4);
    pub const BAR5: Self = Self(5);
    pub const ROM: Self = Self(6);

    pub const fn new(idx: u8) -> Option<Self> {
        if idx <= 6 {
            Some(Self(idx))
        } else {
            None
        }
    }

    pub const fn as_u8(self) -> u8 {
        self.0
    }

    pub const fn as_usize(self) -> usize {
        self.0 as usize
    }

    /// Config space offset for this BAR register
    pub const fn config_offset(self) -> u8 {
        0x10 + (self.0 * 4)
    }
}

/// VFIO region index (matches vfio-bindings constants)
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct VfioRegionIndex(pub u32);

impl VfioRegionIndex {
    pub const BAR0: Self = Self(0);
    pub const BAR1: Self = Self(1);
    pub const BAR2: Self = Self(2);
    pub const BAR3: Self = Self(3);
    pub const BAR4: Self = Self(4);
    pub const BAR5: Self = Self(5);
    pub const ROM: Self = Self(6);
    pub const CONFIG: Self = Self(7);
    pub const VGA: Self = Self(8);

    pub const fn from_bar(bar: BarIndex) -> Self {
        Self(bar.0 as u32)
    }
}

/// VFIO IRQ index
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct VfioIrqIndex(pub u32);

impl VfioIrqIndex {
    pub const INTX: Self = Self(0);
    pub const MSI: Self = Self(1);
    pub const MSIX: Self = Self(2);
    pub const ERR: Self = Self(3);
    pub const REQ: Self = Self(4);
}

/// PCI Vendor ID
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct VendorId(pub u16);

impl VendorId {
    pub const NVIDIA: Self = Self(0x10de);
    pub const AMD: Self = Self(0x1002);
    pub const INTEL: Self = Self(0x8086);

    pub const fn new(id: u16) -> Self {
        Self(id)
    }
}

impl fmt::Debug for VendorId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:#06x}", self.0)
    }
}

/// PCI Device ID
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct DeviceId(pub u16);

impl DeviceId {
    pub const fn new(id: u16) -> Self {
        Self(id)
    }
}

impl fmt::Debug for DeviceId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:#06x}", self.0)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_pci_bdf_parse() {
        let bdf = PciBdf::parse("0000:01:00.0").unwrap();
        assert_eq!(bdf.domain, 0);
        assert_eq!(bdf.bus, 1);
        assert_eq!(bdf.device, 0);
        assert_eq!(bdf.function, 0);

        let bdf = PciBdf::parse("0000:3b:00.1").unwrap();
        assert_eq!(bdf.bus, 0x3b);
        assert_eq!(bdf.function, 1);

        assert!(PciBdf::parse("invalid").is_none());
        assert!(PciBdf::parse("0000:01:00").is_none());
    }

    #[test]
    fn test_address_types_dont_mix() {
        let gpa = GuestPhysAddr::new(0x1000);
        let iova = IovaAddr::from_gpa(gpa);
        
        // IOVA == GPA for identity mapping
        assert_eq!(gpa.raw(), iova.raw());
        
        // But they're different types - this wouldn't compile:
        // let _: GuestPhysAddr = iova;
    }

    #[test]
    fn test_bar_index() {
        assert_eq!(BarIndex::BAR0.config_offset(), 0x10);
        assert_eq!(BarIndex::BAR5.config_offset(), 0x24);
        assert!(BarIndex::new(7).is_none());
    }
}

#[cfg(test)]
mod proptests {
    use super::*;
    use proptest::prelude::*;

    // === Address Type Properties ===

    proptest! {
        /// Property: IOVA identity mapping preserves GPA value
        #[test]
        fn prop_iova_identity_mapping(addr in any::<u64>()) {
            let gpa = GuestPhysAddr::new(addr);
            let iova = IovaAddr::from_gpa(gpa);
            prop_assert_eq!(gpa.raw(), iova.raw());
        }

        /// Property: Address types are self-consistent (roundtrip)
        #[test]
        fn prop_address_roundtrip(addr in any::<u64>()) {
            let gpa = GuestPhysAddr::new(addr);
            prop_assert_eq!(gpa.raw(), addr);
            
            let hva = HostVirtAddr::new(addr);
            prop_assert_eq!(hva.raw(), addr);
            
            let iova = IovaAddr::new(addr);
            prop_assert_eq!(iova.raw(), addr);
        }

        /// Property: From<GuestPhysAddr> for IovaAddr matches from_gpa
        #[test]
        fn prop_iova_from_trait(addr in any::<u64>()) {
            let gpa = GuestPhysAddr::new(addr);
            let iova1 = IovaAddr::from_gpa(gpa);
            let iova2: IovaAddr = gpa.into();
            prop_assert_eq!(iova1, iova2);
        }
    }

    // === PciBdf Properties ===

    /// Strategy for valid PCI BDF components
    fn valid_bdf() -> impl Strategy<Value = (u16, u8, u8, u8)> {
        (
            any::<u16>(),           // domain: 0-65535
            any::<u8>(),            // bus: 0-255
            0u8..32u8,              // device: 0-31
            0u8..8u8,               // function: 0-7
        )
    }

    proptest! {
        /// Property: PciBdf parse/display roundtrip
        #[test]
        fn prop_bdf_roundtrip((domain, bus, device, function) in valid_bdf()) {
            let bdf = PciBdf::new(domain, bus, device, function);
            let s = bdf.to_string();
            let parsed = PciBdf::parse(&s);
            
            prop_assert!(parsed.is_some(), "Failed to parse: {}", s);
            let parsed = parsed.unwrap();
            
            prop_assert_eq!(bdf.domain, parsed.domain);
            prop_assert_eq!(bdf.bus, parsed.bus);
            prop_assert_eq!(bdf.device, parsed.device);
            prop_assert_eq!(bdf.function, parsed.function);
        }

        /// Property: PciBdf sysfs path is well-formed
        #[test]
        fn prop_bdf_sysfs_path((domain, bus, device, function) in valid_bdf()) {
            let bdf = PciBdf::new(domain, bus, device, function);
            let path = bdf.sysfs_path();
            
            prop_assert!(path.starts_with("/sys/bus/pci/devices/"));
            prop_assert!(path.contains(":"));
            prop_assert!(path.contains("."));
            
            // Should be parseable back from the path
            let filename = path.rsplit('/').next().unwrap();
            let parsed = PciBdf::parse(filename);
            prop_assert!(parsed.is_some());
            prop_assert_eq!(bdf, parsed.unwrap());
        }

        /// Property: PciBdf Display and Debug are consistent
        #[test]
        fn prop_bdf_display_debug((domain, bus, device, function) in valid_bdf()) {
            let bdf = PciBdf::new(domain, bus, device, function);
            let display = format!("{}", bdf);
            let debug = format!("{:?}", bdf);
            prop_assert_eq!(display, debug);
        }
    }

    // === BarIndex Properties ===

    proptest! {
        /// Property: Valid BAR indices are 0-6
        #[test]
        fn prop_bar_index_valid(idx in 0u8..=6u8) {
            let bar = BarIndex::new(idx);
            prop_assert!(bar.is_some());
            prop_assert_eq!(bar.unwrap().as_u8(), idx);
        }

        /// Property: Invalid BAR indices are rejected
        #[test]
        fn prop_bar_index_invalid(idx in 7u8..=255u8) {
            let bar = BarIndex::new(idx);
            prop_assert!(bar.is_none());
        }

        /// Property: BAR config offsets are in correct range (0x10-0x28)
        #[test]
        fn prop_bar_config_offset(idx in 0u8..=6u8) {
            let bar = BarIndex::new(idx).unwrap();
            let offset = bar.config_offset();
            
            // BAR0 at 0x10, BAR5 at 0x24, ROM at 0x28
            let expected = 0x10 + (idx * 4);
            prop_assert_eq!(offset, expected);
            prop_assert!(offset >= 0x10);
            prop_assert!(offset <= 0x28);
        }

        /// Property: BarIndex as_usize matches as_u8
        #[test]
        fn prop_bar_index_conversion(idx in 0u8..=6u8) {
            let bar = BarIndex::new(idx).unwrap();
            prop_assert_eq!(bar.as_usize(), bar.as_u8() as usize);
        }
    }

    // === VfioRegionIndex Properties ===

    proptest! {
        /// Property: VfioRegionIndex from_bar matches BAR index
        #[test]
        fn prop_vfio_region_from_bar(idx in 0u8..=6u8) {
            let bar = BarIndex::new(idx).unwrap();
            let region = VfioRegionIndex::from_bar(bar);
            prop_assert_eq!(region.0, idx as u32);
        }
    }

    // === Vendor/Device ID Properties ===

    proptest! {
        /// Property: VendorId preserves value
        #[test]
        fn prop_vendor_id(id in any::<u16>()) {
            let vid = VendorId::new(id);
            prop_assert_eq!(vid.0, id);
        }

        /// Property: DeviceId preserves value
        #[test]
        fn prop_device_id(id in any::<u16>()) {
            let did = DeviceId::new(id);
            prop_assert_eq!(did.0, id);
        }
    }

    // === Address Ordering Properties ===

    proptest! {
        /// Property: GuestPhysAddr ordering is consistent with u64
        #[test]
        fn prop_gpa_ordering(a in any::<u64>(), b in any::<u64>()) {
            let gpa_a = GuestPhysAddr::new(a);
            let gpa_b = GuestPhysAddr::new(b);
            
            prop_assert_eq!(gpa_a.cmp(&gpa_b), a.cmp(&b));
            prop_assert_eq!(gpa_a < gpa_b, a < b);
            prop_assert_eq!(gpa_a == gpa_b, a == b);
        }

        /// Property: IovaAddr ordering is consistent with u64
        #[test]
        fn prop_iova_ordering(a in any::<u64>(), b in any::<u64>()) {
            let iova_a = IovaAddr::new(a);
            let iova_b = IovaAddr::new(b);
            
            prop_assert_eq!(iova_a.cmp(&iova_b), a.cmp(&b));
        }
    }
}
