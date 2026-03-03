// Copyright 2025 Isospin Authors
// SPDX-License-Identifier: Apache-2.0
//
//! VFIO container management and DMA mapping.
//!
//! The container represents an IOMMU domain - all devices attached to the same
//! container share DMA mappings. For GPU passthrough without vIOMMU, we use a
//! single container with identity mapping (IOVA == GPA).

use std::collections::BTreeMap;
use std::os::unix::io::{AsRawFd, OwnedFd, RawFd};

use super::error::{VfioError, VfioResult};
use super::types::{GuestPhysAddr, HostVirtAddr, IovaAddr};

// VFIO ioctl definitions for container operations
// These match the actual kernel values from <linux/vfio.h>
mod vfio_ioctls {
    use std::os::raw::c_ulong;
    
    // Container ioctls
    pub const VFIO_GET_API_VERSION: c_ulong = 0x3b64;
    pub const VFIO_CHECK_EXTENSION: c_ulong = 0x3b65;
    pub const VFIO_SET_IOMMU: c_ulong = 0x3b66;
    
    // IOMMU ioctls
    pub const VFIO_IOMMU_MAP_DMA: c_ulong = 0x3b71;
    pub const VFIO_IOMMU_UNMAP_DMA: c_ulong = 0x3b72;
}

use vfio_ioctls::*;

/// A DMA mapping tracked by the container.
#[derive(Debug, Clone)]
pub struct DmaMapping {
    /// I/O virtual address (what device sees)
    pub iova: IovaAddr,
    /// Size of the mapping
    pub size: u64,
    /// Host virtual address (for reference/debugging)
    pub hva: HostVirtAddr,
}

/// VFIO container managing an IOMMU domain.
///
/// All DMA mappings are tracked to ensure proper cleanup and prevent leaks.
/// The container enforces that mappings don't overlap.
pub struct VfioContainer {
    /// File descriptor for /dev/vfio/vfio
    fd: OwnedFd,
    /// Active DMA mappings, keyed by IOVA for efficient lookup
    mappings: BTreeMap<u64, DmaMapping>,
    /// Whether the IOMMU has been configured
    iommu_configured: bool,
}

impl VfioContainer {
    /// Create a new VFIO container.
    ///
    /// Opens /dev/vfio/vfio and verifies API compatibility.
    pub fn new() -> VfioResult<Self> {
        // Open container
        let fd = std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open("/dev/vfio/vfio")
            .map_err(VfioError::ContainerOpen)?;

        let fd = OwnedFd::from(fd);

        // Check API version
        let version = unsafe { libc::ioctl(fd.as_raw_fd(), VFIO_GET_API_VERSION) };
        if version < 0 {
            return Err(VfioError::GetApiVersion(std::io::Error::last_os_error()));
        }

        // VFIO_API_VERSION = 0
        if version != 0 {
            return Err(VfioError::UnsupportedApiVersion {
                got: version,
                expected: 0,
            });
        }

        // Check for TYPE1 IOMMU support (VFIO_TYPE1_IOMMU = 1)
        let has_type1 = unsafe { libc::ioctl(fd.as_raw_fd(), VFIO_CHECK_EXTENSION, 1) };
        if has_type1 <= 0 {
            return Err(VfioError::Type1IommuNotSupported);
        }

        Ok(Self {
            fd,
            mappings: BTreeMap::new(),
            iommu_configured: false,
        })
    }

    /// Get the raw file descriptor (for passing to VFIO group).
    pub fn as_raw_fd(&self) -> RawFd {
        self.fd.as_raw_fd()
    }

    /// Configure the IOMMU type (called after first group is added).
    pub fn set_iommu_type(&mut self) -> VfioResult<()> {
        if self.iommu_configured {
            return Ok(());
        }

        // VFIO_TYPE1v2_IOMMU = 3
        let ret = unsafe { libc::ioctl(self.fd.as_raw_fd(), VFIO_SET_IOMMU, 3) };
        if ret < 0 {
            return Err(VfioError::SetIommu(std::io::Error::last_os_error()));
        }

        self.iommu_configured = true;
        Ok(())
    }

    /// Map a region for device DMA access.
    ///
    /// # Arguments
    /// * `iova` - I/O virtual address (typically == GPA for identity mapping)
    /// * `size` - Size of the region in bytes
    /// * `hva` - Host virtual address of the backing memory
    ///
    /// # Safety
    /// The caller must ensure:
    /// - The HVA points to valid memory that will outlive the mapping
    /// - The memory is page-aligned
    pub fn map_dma(
        &mut self,
        iova: IovaAddr,
        size: u64,
        hva: HostVirtAddr,
    ) -> VfioResult<()> {
        // Check for overlaps
        if let Some((&existing_iova, existing)) = self.mappings.range(..=iova.raw()).next_back() {
            if existing_iova + existing.size > iova.raw() {
                return Err(VfioError::DmaMappingOverlap { iova: iova.raw() });
            }
        }
        if let Some((&next_iova, _)) = self.mappings.range(iova.raw()..).next() {
            if iova.raw() + size > next_iova {
                return Err(VfioError::DmaMappingOverlap { iova: next_iova });
            }
        }

        #[repr(C)]
        struct VfioDmaMap {
            argsz: u32,
            flags: u32,
            vaddr: u64,
            iova: u64,
            size: u64,
        }

        // VFIO_DMA_MAP_FLAG_READ = 1, VFIO_DMA_MAP_FLAG_WRITE = 2
        let map = VfioDmaMap {
            argsz: std::mem::size_of::<VfioDmaMap>() as u32,
            flags: 1 | 2, // READ | WRITE
            vaddr: hva.raw(),
            iova: iova.raw(),
            size,
        };

        let ret = unsafe { libc::ioctl(self.fd.as_raw_fd(), VFIO_IOMMU_MAP_DMA, &map) };
        if ret < 0 {
            return Err(VfioError::DmaMap {
                iova: iova.raw(),
                size,
                source: std::io::Error::last_os_error(),
            });
        }

        self.mappings.insert(
            iova.raw(),
            DmaMapping {
                iova,
                size,
                hva,
            },
        );

        Ok(())
    }

    /// Unmap a DMA region.
    pub fn unmap_dma(&mut self, iova: IovaAddr, size: u64) -> VfioResult<()> {
        #[repr(C)]
        struct VfioDmaUnmap {
            argsz: u32,
            flags: u32,
            iova: u64,
            size: u64,
        }

        let unmap = VfioDmaUnmap {
            argsz: std::mem::size_of::<VfioDmaUnmap>() as u32,
            flags: 0,
            iova: iova.raw(),
            size,
        };

        let ret = unsafe { libc::ioctl(self.fd.as_raw_fd(), VFIO_IOMMU_UNMAP_DMA, &unmap) };
        if ret < 0 {
            return Err(VfioError::DmaUnmap {
                iova: iova.raw(),
                size,
                source: std::io::Error::last_os_error(),
            });
        }

        self.mappings.remove(&iova.raw());
        Ok(())
    }

    /// Map all guest memory regions for DMA.
    ///
    /// This sets up identity mapping where IOVA == GPA, which is the standard
    /// setup for device passthrough.
    pub fn map_guest_memory<I>(&mut self, regions: I) -> VfioResult<()>
    where
        I: Iterator<Item = (GuestPhysAddr, u64, HostVirtAddr)>,
    {
        for (gpa, size, hva) in regions {
            let iova = IovaAddr::from_gpa(gpa);
            self.map_dma(iova, size, hva)?;
        }
        Ok(())
    }

    /// Get the number of active DMA mappings.
    pub fn mapping_count(&self) -> usize {
        self.mappings.len()
    }

    /// Check if a DMA mapping exists for the given IOVA.
    pub fn has_mapping(&self, iova: IovaAddr) -> bool {
        self.mappings.contains_key(&iova.raw())
    }
}

impl Drop for VfioContainer {
    fn drop(&mut self) {
        // Unmap all DMA regions on drop
        let iovas: Vec<_> = self.mappings.keys().copied().collect();
        for iova in iovas {
            if let Some(mapping) = self.mappings.get(&iova) {
                let _ = self.unmap_dma(IovaAddr::new(iova), mapping.size);
            }
        }
    }
}

/// RAII guard for a DMA mapping that unmaps on drop.
pub struct DmaMappingGuard<'a> {
    container: &'a mut VfioContainer,
    iova: IovaAddr,
    size: u64,
}

impl<'a> DmaMappingGuard<'a> {
    pub fn new(
        container: &'a mut VfioContainer,
        iova: IovaAddr,
        size: u64,
        hva: HostVirtAddr,
    ) -> VfioResult<Self> {
        container.map_dma(iova, size, hva)?;
        Ok(Self {
            container,
            iova,
            size,
        })
    }

    pub fn iova(&self) -> IovaAddr {
        self.iova
    }

    pub fn size(&self) -> u64 {
        self.size
    }
}

impl Drop for DmaMappingGuard<'_> {
    fn drop(&mut self) {
        let _ = self.container.unmap_dma(self.iova, self.size);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_dma_mapping_overlap_detection() {
        // This would require a real VFIO setup to test properly,
        // but we can test the overlap detection logic
        let mut mappings: BTreeMap<u64, DmaMapping> = BTreeMap::new();
        
        // Add a mapping at 0x1000, size 0x1000
        mappings.insert(
            0x1000,
            DmaMapping {
                iova: IovaAddr::new(0x1000),
                size: 0x1000,
                hva: HostVirtAddr::new(0x7fff0000),
            },
        );

        // Check overlap detection
        let new_iova = 0x1800; // Overlaps with existing
        let overlaps = if let Some((&existing_iova, existing)) = mappings.range(..=new_iova).next_back() {
            existing_iova + existing.size > new_iova
        } else {
            false
        };
        
        assert!(overlaps, "Should detect overlap");

        // Non-overlapping should be fine
        let new_iova = 0x3000;
        let overlaps = if let Some((&existing_iova, existing)) = mappings.range(..=new_iova).next_back() {
            existing_iova + existing.size > new_iova
        } else {
            false
        };
        
        assert!(!overlaps, "Should not detect overlap");
    }
}

/// Mock DMA mapping tracker for testing without real VFIO.
/// This captures the core invariants we need to verify.
#[cfg(test)]
pub mod mock {
    use super::*;

    /// A mock DMA mapping tracker that doesn't require VFIO.
    /// Used for property testing the mapping logic.
    #[derive(Debug, Default)]
    pub struct MockDmaTracker {
        mappings: BTreeMap<u64, DmaMapping>,
    }

    #[derive(Debug, Clone, PartialEq, Eq)]
    pub enum MockDmaError {
        OverlapWithExisting { new_iova: u64, existing_iova: u64 },
        OverlapWithNext { new_iova: u64, next_iova: u64 },
        NotFound { iova: u64 },
        ZeroSize,
    }

    impl MockDmaTracker {
        pub fn new() -> Self {
            Self::default()
        }

        /// Check if a new mapping would overlap with existing ones.
        fn check_overlap(&self, iova: u64, size: u64) -> Result<(), MockDmaError> {
            if size == 0 {
                return Err(MockDmaError::ZeroSize);
            }

            // Check overlap with previous mapping
            if let Some((&existing_iova, existing)) = self.mappings.range(..=iova).next_back() {
                if existing_iova + existing.size > iova {
                    return Err(MockDmaError::OverlapWithExisting {
                        new_iova: iova,
                        existing_iova,
                    });
                }
            }

            // Check overlap with next mapping
            if let Some((&next_iova, _)) = self.mappings.range(iova..).next() {
                // Skip if it's the same address (shouldn't happen with BTreeMap)
                if next_iova != iova && iova + size > next_iova {
                    return Err(MockDmaError::OverlapWithNext {
                        new_iova: iova,
                        next_iova,
                    });
                }
            }

            Ok(())
        }

        /// Add a DMA mapping.
        pub fn map(&mut self, iova: IovaAddr, size: u64, hva: HostVirtAddr) -> Result<(), MockDmaError> {
            self.check_overlap(iova.raw(), size)?;

            self.mappings.insert(
                iova.raw(),
                DmaMapping { iova, size, hva },
            );

            Ok(())
        }

        /// Remove a DMA mapping.
        pub fn unmap(&mut self, iova: IovaAddr) -> Result<DmaMapping, MockDmaError> {
            self.mappings
                .remove(&iova.raw())
                .ok_or(MockDmaError::NotFound { iova: iova.raw() })
        }

        /// Check if a mapping exists at the given IOVA.
        pub fn has_mapping(&self, iova: IovaAddr) -> bool {
            self.mappings.contains_key(&iova.raw())
        }

        /// Get mapping count.
        pub fn len(&self) -> usize {
            self.mappings.len()
        }

        /// Check if empty.
        pub fn is_empty(&self) -> bool {
            self.mappings.is_empty()
        }

        /// Get all mappings as a slice (for verification).
        pub fn mappings(&self) -> impl Iterator<Item = &DmaMapping> {
            self.mappings.values()
        }

        /// Verify internal consistency: no overlapping mappings.
        pub fn verify_no_overlaps(&self) -> bool {
            let mut prev_end: Option<u64> = None;
            
            for (&iova, mapping) in &self.mappings {
                if let Some(end) = prev_end {
                    if iova < end {
                        return false; // Overlap detected
                    }
                }
                prev_end = Some(iova + mapping.size);
            }
            
            true
        }

        /// Get the total mapped size.
        pub fn total_mapped_size(&self) -> u64 {
            self.mappings.values().map(|m| m.size).sum()
        }

        /// Find which mapping (if any) contains the given address.
        pub fn find_containing(&self, addr: u64) -> Option<&DmaMapping> {
            // Find the mapping with the largest IOVA <= addr
            if let Some((&iova, mapping)) = self.mappings.range(..=addr).next_back() {
                if addr < iova + mapping.size {
                    return Some(mapping);
                }
            }
            None
        }
    }
}

#[cfg(test)]
mod proptests {
    use super::mock::*;
    use super::*;
    use proptest::prelude::*;
    use proptest::collection::vec;

    /// Strategy for generating DMA mapping operations
    #[derive(Debug, Clone)]
    enum DmaOp {
        Map { iova: u64, size: u64, hva: u64 },
        Unmap { iova: u64 },
    }

    fn dma_op_strategy() -> impl Strategy<Value = DmaOp> {
        prop_oneof![
            // Map operation with page-aligned addresses and reasonable sizes
            (
                (0u64..0x1_0000_0000u64).prop_map(|x| x & !0xFFF), // Page-aligned IOVA
                (0x1000u64..0x10_0000u64).prop_map(|x| x & !0xFFF), // Page-aligned size 4KB-1MB
                any::<u64>().prop_map(|x| x & !0xFFF), // Page-aligned HVA
            ).prop_map(|(iova, size, hva)| DmaOp::Map { iova, size, hva }),
            // Unmap operation
            (0u64..0x1_0000_0000u64).prop_map(|x| x & !0xFFF)
                .prop_map(|iova| DmaOp::Unmap { iova }),
        ]
    }

    // Non-proptest test for empty tracker (no inputs needed)
    #[test]
    fn test_empty_tracker() {
        let tracker = MockDmaTracker::new();
        assert!(tracker.is_empty());
        assert_eq!(tracker.len(), 0);
        assert!(tracker.verify_no_overlaps());
    }

    proptest! {
        /// Property: Single mapping is always valid
        #[test]
        fn prop_single_mapping(
            iova in (0u64..0x1_0000_0000u64).prop_map(|x| x & !0xFFF),
            size in (0x1000u64..0x10_0000u64).prop_map(|x| (x & !0xFFF).max(0x1000)),
            hva in any::<u64>().prop_map(|x| x & !0xFFF),
        ) {
            let mut tracker = MockDmaTracker::new();
            
            let result = tracker.map(
                IovaAddr::new(iova),
                size,
                HostVirtAddr::new(hva),
            );
            
            prop_assert!(result.is_ok());
            prop_assert_eq!(tracker.len(), 1);
            prop_assert!(tracker.has_mapping(IovaAddr::new(iova)));
            prop_assert!(tracker.verify_no_overlaps());
        }

        /// Property: Map then unmap returns to empty
        #[test]
        fn prop_map_unmap_roundtrip(
            iova in (0u64..0x1_0000_0000u64).prop_map(|x| x & !0xFFF),
            size in (0x1000u64..0x10_0000u64).prop_map(|x| (x & !0xFFF).max(0x1000)),
            hva in any::<u64>().prop_map(|x| x & !0xFFF),
        ) {
            let mut tracker = MockDmaTracker::new();
            
            tracker.map(IovaAddr::new(iova), size, HostVirtAddr::new(hva)).unwrap();
            prop_assert_eq!(tracker.len(), 1);
            
            let unmapped = tracker.unmap(IovaAddr::new(iova));
            prop_assert!(unmapped.is_ok());
            prop_assert!(tracker.is_empty());
        }

        /// Property: Double unmap fails
        #[test]
        fn prop_double_unmap_fails(
            iova in (0u64..0x1_0000_0000u64).prop_map(|x| x & !0xFFF),
            size in (0x1000u64..0x10_0000u64).prop_map(|x| (x & !0xFFF).max(0x1000)),
            hva in any::<u64>().prop_map(|x| x & !0xFFF),
        ) {
            let mut tracker = MockDmaTracker::new();
            
            tracker.map(IovaAddr::new(iova), size, HostVirtAddr::new(hva)).unwrap();
            tracker.unmap(IovaAddr::new(iova)).unwrap();
            
            let second_unmap = tracker.unmap(IovaAddr::new(iova));
            let is_not_found = matches!(second_unmap, Err(MockDmaError::NotFound { .. }));
            prop_assert!(is_not_found, "Expected NotFound error");
        }

        /// Property: Overlapping mappings are rejected
        #[test]
        fn prop_overlap_rejected(
            base in (0x1000u64..0x1_0000_0000u64).prop_map(|x| x & !0xFFF),
            size in (0x2000u64..0x10_0000u64).prop_map(|x| (x & !0xFFF).max(0x2000)),
            hva in any::<u64>().prop_map(|x| x & !0xFFF),
        ) {
            let mut tracker = MockDmaTracker::new();
            
            // First mapping
            tracker.map(IovaAddr::new(base), size, HostVirtAddr::new(hva)).unwrap();
            
            // Try to map overlapping region (middle of first)
            let overlap_start = base + (size / 2);
            let result = tracker.map(
                IovaAddr::new(overlap_start),
                size,
                HostVirtAddr::new(hva + 0x1000),
            );
            
            prop_assert!(result.is_err());
            prop_assert!(tracker.verify_no_overlaps());
        }

        /// Property: Adjacent (non-overlapping) mappings are allowed
        #[test]
        fn prop_adjacent_allowed(
            base in (0x1000u64..0x8000_0000u64).prop_map(|x| x & !0xFFF),
            size in (0x1000u64..0x100_0000u64).prop_map(|x| (x & !0xFFF).max(0x1000)),
            hva in any::<u64>().prop_map(|x| x & !0xFFF),
        ) {
            let mut tracker = MockDmaTracker::new();
            
            // First mapping
            tracker.map(IovaAddr::new(base), size, HostVirtAddr::new(hva)).unwrap();
            
            // Adjacent mapping (immediately after)
            let adjacent_start = base + size;
            let result = tracker.map(
                IovaAddr::new(adjacent_start),
                size,
                HostVirtAddr::new(hva + size),
            );
            
            prop_assert!(result.is_ok());
            prop_assert_eq!(tracker.len(), 2);
            prop_assert!(tracker.verify_no_overlaps());
        }

        /// Property: Random sequence of operations maintains invariants
        #[test]
        fn prop_random_ops_maintain_invariants(
            ops in vec(dma_op_strategy(), 0..50)
        ) {
            let mut tracker = MockDmaTracker::new();
            
            for op in ops {
                match op {
                    DmaOp::Map { iova, size, hva } => {
                        let _ = tracker.map(
                            IovaAddr::new(iova),
                            size.max(0x1000), // Ensure non-zero size
                            HostVirtAddr::new(hva),
                        );
                    }
                    DmaOp::Unmap { iova } => {
                        let _ = tracker.unmap(IovaAddr::new(iova));
                    }
                }
                
                // Invariant: no overlaps after any operation
                prop_assert!(
                    tracker.verify_no_overlaps(),
                    "Overlap detected after operation"
                );
            }
        }

        /// Property: find_containing returns correct mapping
        #[test]
        fn prop_find_containing(
            base in (0x1000u64..0x1_0000_0000u64).prop_map(|x| x & !0xFFF),
            size in (0x2000u64..0x10_0000u64).prop_map(|x| (x & !0xFFF).max(0x2000)),
            offset in 0u64..0x10_0000u64,
            hva in any::<u64>().prop_map(|x| x & !0xFFF),
        ) {
            let mut tracker = MockDmaTracker::new();
            tracker.map(IovaAddr::new(base), size, HostVirtAddr::new(hva)).unwrap();
            
            // Address within mapping should be found
            let addr_in = base + (offset % size);
            let found = tracker.find_containing(addr_in);
            prop_assert!(found.is_some());
            prop_assert_eq!(found.unwrap().iova.raw(), base);
            
            // Address before mapping should not be found (if base > 0)
            if base > 0 {
                let addr_before = base - 1;
                prop_assert!(tracker.find_containing(addr_before).is_none());
            }
            
            // Address after mapping should not be found
            let addr_after = base + size;
            prop_assert!(tracker.find_containing(addr_after).is_none());
        }

        /// Property: total_mapped_size is sum of all mapping sizes
        #[test]
        fn prop_total_mapped_size(
            mappings in vec(
                (
                    (0u64..0x1_0000_0000u64).prop_map(|x| x & !0xFFF),
                    (0x1000u64..0x10_0000u64).prop_map(|x| (x & !0xFFF).max(0x1000)),
                ),
                0..10
            )
        ) {
            let mut tracker = MockDmaTracker::new();
            let mut expected_total = 0u64;
            
            // Add non-overlapping mappings by spacing them far apart
            for (i, (base_offset, size)) in mappings.into_iter().enumerate() {
                let iova = (i as u64) * 0x1000_0000 + base_offset;
                if tracker.map(
                    IovaAddr::new(iova),
                    size,
                    HostVirtAddr::new(0x7fff_0000_0000 + iova),
                ).is_ok() {
                    expected_total += size;
                }
            }
            
            prop_assert_eq!(tracker.total_mapped_size(), expected_total);
        }
    }
}
