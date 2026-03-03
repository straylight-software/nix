// Copyright 2025 Isospin Authors
// SPDX-License-Identifier: Apache-2.0
//
//! Memory-mapped region management for VFIO BAR access.
//!
//! This module provides a safe wrapper around `mmap(2)` for mapping VFIO device
//! BAR regions into the VMM's address space. These mappings can then be registered
//! with KVM as user memory regions, allowing the guest to access device registers
//! directly without VM exits.
//!
//! # Safety Model
//!
//! The [`MmapRegion`] type maintains the following invariants:
//!
//! 1. **Address validity**: `addr` is either null (invalid state, only during error)
//!    or points to a valid `mmap`'d region of exactly `len` bytes.
//!
//! 2. **Lifetime guarantee**: The mapped region remains valid for the lifetime of
//!    the `MmapRegion` instance. `munmap` is called exactly once in `Drop`.
//!
//! 3. **Size bounds**: `len` fits in both `isize` and `libc::size_t`, ensuring
//!    pointer arithmetic is well-defined.
//!
//! 4. **Alignment**: The kernel guarantees page-aligned mappings. We verify this
//!    in debug builds.
//!
//! # Formal Verification Notes (Lean4 FFI)
//!
//! The following properties should be verified:
//!
//! - `mmap_region_valid(r) := r.addr != null && r.len > 0 && r.len <= isize::MAX`
//! - `mmap_region_aligned(r) := r.addr % PAGE_SIZE == 0`
//! - `mmap_drop_idempotent`: Drop can only be called once (Rust guarantees this)
//! - `mmap_no_double_free`: munmap is called exactly once per successful mmap
//!
//! # References
//!
//! - Linux mmap(2): <https://man7.org/linux/man-pages/man2/mmap.2.html>
//! - VFIO region mmap: <https://www.kernel.org/doc/html/latest/driver-api/vfio.html>
//! - KVM memory regions: <https://www.kernel.org/doc/html/latest/virt/kvm/api.html>

use std::io::{Error, ErrorKind, Result};
use std::os::unix::io::RawFd;
use std::ptr::null_mut;

/// Page size constant (4 KiB on x86_64).
///
/// # Knuth: "Premature optimization is the root of all evil"
///
/// We hardcode 4096 rather than calling `sysconf(_SC_PAGESIZE)` because:
/// 1. x86_64 Linux always uses 4 KiB base pages for VFIO mappings
/// 2. This is a compile-time constant that enables better optimization
/// 3. If we ever support huge pages, that's a separate code path
///
/// For formal verification, this should be axiomatized as:
/// `axiom page_size_is_4096 : PAGE_SIZE = 4096`
pub const PAGE_SIZE: u64 = 4096;

/// A memory-mapped region that is automatically unmapped on drop.
///
/// This type represents ownership of a region of virtual address space
/// that has been mapped to a file descriptor (in our case, a VFIO device BAR).
///
/// # Memory Layout
///
/// ```text
/// MmapRegion {
///     addr: *mut u8,  // 8 bytes - pointer to mapped region
///     len: usize,     // 8 bytes - size of mapped region
/// }
/// Total: 16 bytes, 8-byte aligned
/// ```
///
/// # Invariants
///
/// For a valid `MmapRegion r`:
/// - `r.addr` is non-null and page-aligned
/// - `r.len > 0`
/// - `r.len <= isize::MAX as usize`
/// - The memory range `[r.addr, r.addr + r.len)` is a valid mmap'd region
/// - The region was mapped with `MAP_SHARED` (changes visible to device)
///
/// # Safety
///
/// This type is `Send` and `Sync` because:
/// - The mapped memory is device MMIO, not shared application memory
/// - Concurrent access is mediated by the device hardware
/// - The VMM uses this for KVM memory regions, not direct access
///
/// However, users must ensure:
/// - No references to the mapped memory outlive the `MmapRegion`
/// - The file descriptor remains valid until drop (VFIO guarantees this)
#[derive(Debug)]
pub struct MmapRegion {
    /// Pointer to the start of the mapped region.
    ///
    /// # Invariant
    /// Non-null and page-aligned after successful construction.
    addr: *mut u8,

    /// Size of the mapped region in bytes.
    ///
    /// # Invariant
    /// Positive and fits in `isize` (required for safe pointer arithmetic).
    len: usize,
}

// SAFETY: MmapRegion can be sent between threads.
// The mapped memory is device MMIO, and thread safety is handled by:
// 1. KVM's memory region locking
// 2. The device hardware itself
// We don't provide any methods that would cause data races.
unsafe impl Send for MmapRegion {}

// SAFETY: MmapRegion can be shared between threads.
// Same reasoning as Send - we only use this for KVM registration.
unsafe impl Sync for MmapRegion {}

impl MmapRegion {
    /// Create a new memory-mapped region for a VFIO device BAR.
    ///
    /// # Arguments
    ///
    /// * `fd` - File descriptor of the VFIO device (from `VFIO_GROUP_GET_DEVICE_FD`)
    /// * `offset` - Offset within the device file to map (from `vfio_region_info.offset`)
    /// * `size` - Size of the region to map (from `vfio_region_info.size`)
    ///
    /// # Returns
    ///
    /// A new `MmapRegion` on success, or an error if:
    /// - `size` is zero or exceeds `isize::MAX`
    /// - `offset` exceeds `i64::MAX` (mmap offset limit)
    /// - The mmap syscall fails (e.g., invalid fd, no permission)
    ///
    /// # Algorithm
    ///
    /// ```text
    /// mmap_region_new(fd, offset, size):
    ///   1. Validate size fits in isize (for pointer arithmetic safety)
    ///   2. Validate offset fits in off_t (mmap requirement)
    ///   3. Call mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, offset)
    ///   4. Check result != MAP_FAILED
    ///   5. Assert result is page-aligned (debug only)
    ///   6. Return MmapRegion { addr: result, len: size }
    /// ```
    ///
    /// # Example
    ///
    /// ```ignore
    /// let region_info = device.get_region_info(bar_index)?;
    /// let mmap = MmapRegion::new(
    ///     device.as_raw_fd(),
    ///     region_info.offset,
    ///     region_info.size,
    /// )?;
    /// // mmap.addr() can now be passed to KVM as a user memory region
    /// ```
    ///
    /// # Formal Specification
    ///
    /// ```text
    /// pre:  size > 0 && size <= isize::MAX && offset <= i64::MAX
    /// post: result.is_ok() ==>
    ///         let r = result.unwrap();
    ///         r.addr != null &&
    ///         r.addr % PAGE_SIZE == 0 &&
    ///         r.len == size &&
    ///         valid_mapping(r.addr, r.len, fd, offset)
    /// ```
    pub fn new(fd: RawFd, offset: u64, size: u64) -> Result<Self> {
        // Step 1: Validate size fits in isize
        //
        // Knuth: "We should forget about small efficiencies, say about 97% of the time"
        // This check is in the 3% - it's critical for memory safety.
        //
        // Pointer arithmetic in Rust requires that offsets fit in isize.
        // See: https://doc.rust-lang.org/std/primitive.pointer.html#method.offset
        if size == 0 {
            return Err(Error::new(
                ErrorKind::InvalidInput,
                "mmap size must be non-zero",
            ));
        }

        let len: usize = size.try_into().map_err(|_| {
            Error::new(
                ErrorKind::InvalidInput,
                format!("mmap size {:#x} exceeds usize::MAX", size),
            )
        })?;

        // Verify it also fits in isize (required for pointer arithmetic)
        if len > isize::MAX as usize {
            return Err(Error::new(
                ErrorKind::InvalidInput,
                format!("mmap size {:#x} exceeds isize::MAX", size),
            ));
        }

        // Step 2: Validate offset fits in off_t (i64 on 64-bit Linux)
        //
        // The mmap syscall takes offset as off_t, which is i64 on LP64.
        // VFIO region offsets are u64, but should never exceed i64::MAX
        // in practice (they're file offsets within /dev/vfio/N).
        let offset_i64: i64 = offset.try_into().map_err(|_| {
            Error::new(
                ErrorKind::InvalidInput,
                format!("mmap offset {:#x} exceeds i64::MAX", offset),
            )
        })?;

        // Step 3: Call mmap
        //
        // Protection flags:
        // - PROT_READ: Allow reading (guest reads device registers)
        // - PROT_WRITE: Allow writing (guest writes device registers)
        //
        // Map flags:
        // - MAP_SHARED: Changes are visible to the device (required for MMIO)
        //
        // SAFETY: This is a valid mmap call with:
        // - addr = NULL (let kernel choose address)
        // - len = validated positive size that fits in size_t
        // - prot = valid combination of PROT_READ | PROT_WRITE
        // - flags = MAP_SHARED (valid for file-backed mapping)
        // - fd = caller-provided (assumed valid VFIO device fd)
        // - offset = validated to fit in off_t
        let prot = libc::PROT_READ | libc::PROT_WRITE;
        let flags = libc::MAP_SHARED;

        let addr = unsafe { libc::mmap(null_mut(), len, prot, flags, fd, offset_i64) };

        // Step 4: Check for failure
        if addr == libc::MAP_FAILED {
            return Err(Error::last_os_error());
        }

        let addr = addr as *mut u8;

        // Step 5: Debug assertion for page alignment
        //
        // The kernel always returns page-aligned addresses from mmap.
        // This assertion catches bugs in our offset calculations or
        // unexpected kernel behavior.
        debug_assert!(
            (addr as u64) % PAGE_SIZE == 0,
            "mmap returned non-page-aligned address: {:p}",
            addr
        );

        // Step 6: Return the region
        Ok(Self { addr, len })
    }

    /// Returns a pointer to the start of the mapped region.
    ///
    /// # Safety
    ///
    /// The returned pointer is valid for the lifetime of this `MmapRegion`.
    /// Callers must not:
    /// - Dereference the pointer after the `MmapRegion` is dropped
    /// - Create mutable references to the memory (use atomics or raw pointers)
    ///
    /// For KVM user memory regions, pass this directly to `kvm_userspace_memory_region.userspace_addr`.
    ///
    /// # Formal Specification
    ///
    /// ```text
    /// pre:  self is valid MmapRegion
    /// post: result != null && result % PAGE_SIZE == 0
    /// ```
    #[inline]
    pub fn addr(&self) -> *mut u8 {
        self.addr
    }

    /// Returns the size of the mapped region in bytes.
    ///
    /// # Guarantees
    ///
    /// The returned value:
    /// - Is greater than zero
    /// - Fits in `isize` (safe for pointer arithmetic)
    /// - Equals the `size` parameter passed to [`MmapRegion::new`]
    ///
    /// # Formal Specification
    ///
    /// ```text
    /// pre:  self is valid MmapRegion
    /// post: result > 0 && result <= isize::MAX
    /// ```
    #[inline]
    pub fn len(&self) -> usize {
        self.len
    }

    /// Returns whether the mapped region is empty.
    ///
    /// This always returns `false` for a valid `MmapRegion`, since we reject
    /// zero-size mappings in the constructor. Provided for API completeness
    /// and clippy compliance.
    #[inline]
    pub fn is_empty(&self) -> bool {
        // Invariant: len > 0 for all valid MmapRegions
        self.len == 0
    }
}

impl Drop for MmapRegion {
    /// Unmap the region when dropped.
    ///
    /// # Panics
    ///
    /// Panics if `munmap` fails. This should never happen for a valid mapping,
    /// and indicates a serious bug (double-free, memory corruption, etc.).
    ///
    /// # Formal Specification
    ///
    /// ```text
    /// pre:  self is valid MmapRegion
    /// post: munmap(self.addr, self.len) == 0
    ///       memory range [self.addr, self.addr + self.len) is no longer mapped
    /// ```
    fn drop(&mut self) {
        // SAFETY: We only reach here with a valid mapping because:
        // 1. Constructor only succeeds if mmap succeeded
        // 2. We don't provide any way to invalidate the mapping
        // 3. Rust's ownership system ensures drop is called exactly once
        let result = unsafe { libc::munmap(self.addr as *mut libc::c_void, self.len) };

        // munmap should never fail for a valid mapping.
        // Possible causes of failure:
        // - EINVAL: Invalid address or length (would indicate our bug)
        // - Memory corruption
        //
        // We panic rather than silently leaking because:
        // 1. This indicates a serious bug that should be caught in testing
        // 2. Continuing with corrupted memory state is worse than crashing
        assert_eq!(
            result, 0,
            "munmap({:p}, {:#x}) failed: {}",
            self.addr,
            self.len,
            Error::last_os_error()
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Test that zero-size mapping is rejected.
    #[test]
    fn test_zero_size_rejected() {
        // Use /dev/zero as a mappable fd for testing
        let fd = unsafe { libc::open(c"/dev/zero".as_ptr(), libc::O_RDWR) };
        assert!(fd >= 0, "Failed to open /dev/zero");

        let result = MmapRegion::new(fd, 0, 0);
        assert!(result.is_err());
        assert!(result
            .unwrap_err()
            .to_string()
            .contains("must be non-zero"));

        unsafe { libc::close(fd) };
    }

    /// Test that excessively large size is rejected.
    #[test]
    fn test_huge_size_rejected() {
        let fd = unsafe { libc::open(c"/dev/zero".as_ptr(), libc::O_RDWR) };
        assert!(fd >= 0, "Failed to open /dev/zero");

        // Size larger than isize::MAX
        let huge_size = (isize::MAX as u64) + 1;
        let result = MmapRegion::new(fd, 0, huge_size);
        assert!(result.is_err());

        unsafe { libc::close(fd) };
    }

    /// Test that excessively large offset is rejected.
    #[test]
    fn test_huge_offset_rejected() {
        let fd = unsafe { libc::open(c"/dev/zero".as_ptr(), libc::O_RDWR) };
        assert!(fd >= 0, "Failed to open /dev/zero");

        // Offset larger than i64::MAX
        let huge_offset = (i64::MAX as u64) + 1;
        let result = MmapRegion::new(fd, huge_offset, PAGE_SIZE);
        assert!(result.is_err());
        assert!(result.unwrap_err().to_string().contains("i64::MAX"));

        unsafe { libc::close(fd) };
    }

    /// Test successful mapping of /dev/zero.
    #[test]
    fn test_valid_mapping() {
        let fd = unsafe { libc::open(c"/dev/zero".as_ptr(), libc::O_RDWR) };
        assert!(fd >= 0, "Failed to open /dev/zero");

        let size = PAGE_SIZE * 4; // 16 KiB
        let result = MmapRegion::new(fd, 0, size);
        assert!(result.is_ok(), "mmap failed: {:?}", result.err());

        let region = result.unwrap();
        assert!(!region.addr().is_null());
        assert_eq!(region.len(), size as usize);
        assert!(!region.is_empty());

        // Verify page alignment
        assert_eq!((region.addr() as u64) % PAGE_SIZE, 0);

        unsafe { libc::close(fd) };
        // Drop happens here, munmap is called
    }

    /// Test that mapping with invalid fd fails.
    #[test]
    fn test_invalid_fd_fails() {
        let result = MmapRegion::new(-1, 0, PAGE_SIZE);
        assert!(result.is_err());
        // Should be EBADF
        assert_eq!(result.unwrap_err().raw_os_error(), Some(libc::EBADF));
    }

    /// Test that we can read from a mapped region (using /dev/zero).
    #[test]
    fn test_read_mapped_memory() {
        let fd = unsafe { libc::open(c"/dev/zero".as_ptr(), libc::O_RDWR) };
        assert!(fd >= 0);

        let region = MmapRegion::new(fd, 0, PAGE_SIZE).unwrap();

        // /dev/zero should give us all zeros
        // SAFETY: We just mapped this region and it's valid
        let first_byte = unsafe { *region.addr() };
        assert_eq!(first_byte, 0);

        unsafe { libc::close(fd) };
    }

    /// Test that we can write to a mapped region.
    #[test]
    fn test_write_mapped_memory() {
        // Create a temporary file for read-write testing
        let fd = unsafe {
            libc::memfd_create(c"test_mmap".as_ptr(), 0)
        };
        assert!(fd >= 0, "memfd_create failed");

        // Extend file to page size
        let result = unsafe { libc::ftruncate(fd, PAGE_SIZE as i64) };
        assert_eq!(result, 0, "ftruncate failed");

        let region = MmapRegion::new(fd, 0, PAGE_SIZE).unwrap();

        // Write a pattern
        // SAFETY: We just mapped this region and it's valid
        unsafe {
            *region.addr() = 0xAB;
            *region.addr().add(1) = 0xCD;
        }

        // Read it back
        let byte0 = unsafe { *region.addr() };
        let byte1 = unsafe { *region.addr().add(1) };
        assert_eq!(byte0, 0xAB);
        assert_eq!(byte1, 0xCD);

        unsafe { libc::close(fd) };
    }
}

#[cfg(test)]
mod proptests {
    use super::*;
    use proptest::prelude::*;

    proptest! {
        /// Property: Valid sizes within bounds should succeed with /dev/zero.
        ///
        /// This tests that our size validation logic correctly accepts
        /// sizes in the valid range.
        #[test]
        fn prop_valid_size_mapping(
            // Use reasonable sizes for testing (1 page to 1 GiB)
            size in (PAGE_SIZE..=(1u64 << 30))
        ) {
            // Skip sizes that aren't page-aligned (mmap may fail)
            if size % PAGE_SIZE != 0 {
                return Ok(());
            }

            let fd = unsafe { libc::open(c"/dev/zero".as_ptr(), libc::O_RDWR) };
            prop_assert!(fd >= 0, "Failed to open /dev/zero");

            // Note: Large mappings may fail due to system limits,
            // but they should fail gracefully, not panic
            let result = MmapRegion::new(fd, 0, size);

            // Either it succeeds with correct properties, or it fails gracefully
            match result {
                Ok(region) => {
                    prop_assert!(!region.addr().is_null());
                    prop_assert_eq!(region.len(), size as usize);
                    prop_assert_eq!((region.addr() as u64) % PAGE_SIZE, 0);
                }
                Err(e) => {
                    // ENOMEM is acceptable for large mappings
                    prop_assert!(
                        e.raw_os_error() == Some(libc::ENOMEM) ||
                        e.raw_os_error() == Some(libc::EINVAL),
                        "Unexpected error: {:?}", e
                    );
                }
            }

            unsafe { libc::close(fd) };
        }

        /// Property: Zero size should always fail.
        #[test]
        fn prop_zero_size_always_fails(offset in 0u64..u64::MAX) {
            let fd = unsafe { libc::open(c"/dev/zero".as_ptr(), libc::O_RDWR) };
            prop_assert!(fd >= 0);

            let result = MmapRegion::new(fd, offset, 0);
            prop_assert!(result.is_err());
            prop_assert!(result.unwrap_err().to_string().contains("non-zero"));

            unsafe { libc::close(fd) };
        }

        /// Property: Sizes exceeding isize::MAX should always fail.
        #[test]
        fn prop_oversized_always_fails(
            extra in 1u64..1000u64  // How much over isize::MAX
        ) {
            let fd = unsafe { libc::open(c"/dev/zero".as_ptr(), libc::O_RDWR) };
            prop_assert!(fd >= 0);

            let huge_size = (isize::MAX as u64).saturating_add(extra);
            let result = MmapRegion::new(fd, 0, huge_size);
            prop_assert!(result.is_err());

            unsafe { libc::close(fd) };
        }

        /// Property: Offsets exceeding i64::MAX should always fail.
        #[test]
        fn prop_huge_offset_always_fails(
            extra in 1u64..1000u64
        ) {
            let fd = unsafe { libc::open(c"/dev/zero".as_ptr(), libc::O_RDWR) };
            prop_assert!(fd >= 0);

            let huge_offset = (i64::MAX as u64).saturating_add(extra);
            let result = MmapRegion::new(fd, huge_offset, PAGE_SIZE);
            prop_assert!(result.is_err());
            prop_assert!(result.unwrap_err().to_string().contains("i64::MAX"));

            unsafe { libc::close(fd) };
        }

        /// Property: Invalid fd should always fail with EBADF.
        #[test]
        fn prop_invalid_fd_always_ebadf(
            fd in -1000i32..0i32,
            size in (PAGE_SIZE..PAGE_SIZE * 16)
        ) {
            let result = MmapRegion::new(fd, 0, size);
            prop_assert!(result.is_err());
            prop_assert_eq!(
                result.unwrap_err().raw_os_error(),
                Some(libc::EBADF),
                "Expected EBADF for invalid fd"
            );
        }

        /// Property: Page-aligned addresses from successful mappings.
        ///
        /// For any successful mapping, the returned address must be page-aligned.
        /// This is a kernel guarantee that we verify.
        #[test]
        fn prop_successful_mapping_page_aligned(
            pages in 1u64..256u64
        ) {
            let fd = unsafe { libc::open(c"/dev/zero".as_ptr(), libc::O_RDWR) };
            prop_assert!(fd >= 0);

            let size = pages * PAGE_SIZE;
            if let Ok(region) = MmapRegion::new(fd, 0, size) {
                prop_assert_eq!(
                    (region.addr() as u64) % PAGE_SIZE,
                    0,
                    "Address {:p} is not page-aligned",
                    region.addr()
                );
            }

            unsafe { libc::close(fd) };
        }

        /// Property: Length invariant preserved.
        ///
        /// The length returned by `len()` must equal the size passed to `new()`.
        #[test]
        fn prop_length_invariant(pages in 1u64..256u64) {
            let fd = unsafe { libc::open(c"/dev/zero".as_ptr(), libc::O_RDWR) };
            prop_assert!(fd >= 0);

            let size = pages * PAGE_SIZE;
            if let Ok(region) = MmapRegion::new(fd, 0, size) {
                prop_assert_eq!(region.len(), size as usize);
                prop_assert!(!region.is_empty());
            }

            unsafe { libc::close(fd) };
        }

        /// Property: Mapping at various offsets within a memfd.
        ///
        /// We should be able to map at any page-aligned offset within a file.
        #[test]
        fn prop_various_offsets(
            offset_pages in 0u64..64u64,
            size_pages in 1u64..64u64
        ) {
            let fd = unsafe {
                libc::memfd_create(c"test_offsets".as_ptr(), 0)
            };
            prop_assert!(fd >= 0);

            let offset = offset_pages * PAGE_SIZE;
            let size = size_pages * PAGE_SIZE;
            let total_size = offset + size;

            // Extend file to cover the region we want to map
            let result = unsafe { libc::ftruncate(fd, total_size as i64) };
            prop_assert_eq!(result, 0);

            let mapping_result = MmapRegion::new(fd, offset, size);

            // Should succeed since we sized the file correctly
            match mapping_result {
                Ok(region) => {
                    prop_assert!(!region.addr().is_null());
                    prop_assert_eq!(region.len(), size as usize);
                }
                Err(e) => {
                    // ENOMEM can happen under memory pressure
                    prop_assert_eq!(
                        e.raw_os_error(),
                        Some(libc::ENOMEM),
                        "Unexpected error: {:?}", e
                    );
                }
            }

            unsafe { libc::close(fd) };
        }
    }

    /// Property: Multiple mappings can coexist.
    ///
    /// This tests that we can have multiple MmapRegion instances
    /// simultaneously without interference.
    #[test]
    fn test_multiple_mappings_coexist() {
        let fd = unsafe { libc::open(c"/dev/zero".as_ptr(), libc::O_RDWR) };
        assert!(fd >= 0);

        let regions: Vec<_> = (0..10)
            .map(|_| MmapRegion::new(fd, 0, PAGE_SIZE).unwrap())
            .collect();

        // All regions should have distinct addresses
        let addrs: Vec<_> = regions.iter().map(|r| r.addr()).collect();
        for i in 0..addrs.len() {
            for j in (i + 1)..addrs.len() {
                assert_ne!(
                    addrs[i], addrs[j],
                    "Two mappings have same address: {:p}",
                    addrs[i]
                );
            }
        }

        // All regions should have correct length
        for region in &regions {
            assert_eq!(region.len(), PAGE_SIZE as usize);
        }

        unsafe { libc::close(fd) };
        // All regions dropped here, all munmap calls should succeed
    }
}
