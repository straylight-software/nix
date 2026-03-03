// Copyright 2025 Isospin Authors
// SPDX-License-Identifier: Apache-2.0
//
//! MSI-X interrupt handling for VFIO devices.
//!
//! This implements the MSI-X state machine with the following states:
//! - Masked: Vector is masked, interrupts are held pending
//! - Configured: Vector has valid routing but is masked  
//! - Enabled: Vector is active, interrupts will be delivered
//!
//! State transitions are enforced at the type level where possible.

use std::os::unix::io::RawFd;

/// MSI address (where the interrupt write goes)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MsiAddress {
    pub low: u32,
    pub high: u32,
}

impl MsiAddress {
    pub fn new(low: u32, high: u32) -> Self {
        Self { low, high }
    }

    pub fn as_u64(&self) -> u64 {
        ((self.high as u64) << 32) | (self.low as u64)
    }

    /// Check if this is a valid x86 MSI address (0xFEExxxxx)
    pub fn is_valid_x86(&self) -> bool {
        // x86 MSI addresses are in the range 0xFEE00000-0xFEEFFFFF
        self.high == 0 && (self.low & 0xFFF0_0000) == 0xFEE0_0000
    }
}

/// MSI data (vector number and delivery mode)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MsiData(pub u32);

impl MsiData {
    pub fn new(data: u32) -> Self {
        Self(data)
    }

    /// Get the interrupt vector number
    pub fn vector(&self) -> u8 {
        (self.0 & 0xFF) as u8
    }

    /// Get the delivery mode
    pub fn delivery_mode(&self) -> u8 {
        ((self.0 >> 8) & 0x7) as u8
    }
}

/// State of a single MSI-X vector
#[derive(Debug, Clone)]
pub enum MsixVectorState {
    /// Vector is masked, no routing configured
    Masked,
    /// Vector has routing but is still masked
    Configured {
        address: MsiAddress,
        data: MsiData,
    },
    /// Vector is active and delivering interrupts
    Enabled {
        address: MsiAddress,
        data: MsiData,
        /// The eventfd that VFIO will signal
        eventfd: RawFd,
        /// The GSI this is routed to in KVM
        gsi: u32,
    },
}

impl MsixVectorState {
    /// Check if vector is masked
    pub fn is_masked(&self) -> bool {
        matches!(self, Self::Masked | Self::Configured { .. })
    }

    /// Check if vector is enabled
    pub fn is_enabled(&self) -> bool {
        matches!(self, Self::Enabled { .. })
    }

    /// Check if vector has configuration
    pub fn is_configured(&self) -> bool {
        matches!(self, Self::Configured { .. } | Self::Enabled { .. })
    }

    /// Get the routing info if configured
    pub fn routing(&self) -> Option<(MsiAddress, MsiData)> {
        match self {
            Self::Masked => None,
            Self::Configured { address, data } => Some((*address, *data)),
            Self::Enabled { address, data, .. } => Some((*address, *data)),
        }
    }
}

/// MSI-X table entry as it appears in device memory
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct MsixTableEntry {
    pub msg_addr_lo: u32,
    pub msg_addr_hi: u32,
    pub msg_data: u32,
    pub vector_ctrl: u32,
}

impl MsixTableEntry {
    /// Check if this vector is masked
    pub fn is_masked(&self) -> bool {
        (self.vector_ctrl & 1) != 0
    }

    /// Get the MSI address
    pub fn address(&self) -> MsiAddress {
        MsiAddress::new(self.msg_addr_lo, self.msg_addr_hi)
    }

    /// Get the MSI data
    pub fn data(&self) -> MsiData {
        MsiData::new(self.msg_data)
    }
}

/// MSI-X capability info parsed from config space
#[derive(Debug, Clone)]
pub struct MsixCapability {
    /// Offset of MSI-X capability in config space
    pub cap_offset: u16,
    /// Number of vectors (1-2048)
    pub table_size: u16,
    /// BAR containing MSI-X table
    pub table_bar: u8,
    /// Offset within BAR to table
    pub table_offset: u32,
    /// BAR containing PBA
    pub pba_bar: u8,
    /// Offset within BAR to PBA
    pub pba_offset: u32,
}

/// MSI-X state machine errors
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum MsixError {
    /// Vector index out of range
    InvalidVector { index: u16, max: u16 },
    /// Cannot enable vector that isn't configured
    NotConfigured { index: u16 },
    /// Cannot configure while enabled (must disable first)
    AlreadyEnabled { index: u16 },
    /// Invalid MSI address
    InvalidAddress { address: MsiAddress },
    /// Failed to create eventfd
    EventfdCreate,
    /// Failed to register with KVM
    KvmRegister,
    /// Failed to register with VFIO
    VfioRegister,
}

/// MSI-X table manager
#[derive(Debug)]
pub struct MsixTable {
    /// Vector states
    vectors: Vec<MsixVectorState>,
    /// Global MSI-X enable bit
    enabled: bool,
    /// Global function mask
    function_masked: bool,
}

impl MsixTable {
    /// Create a new MSI-X table with the given number of vectors
    pub fn new(num_vectors: u16) -> Self {
        let vectors = (0..num_vectors)
            .map(|_| MsixVectorState::Masked)
            .collect();

        Self {
            vectors,
            enabled: false,
            function_masked: true,
        }
    }

    /// Get number of vectors
    pub fn len(&self) -> u16 {
        self.vectors.len() as u16
    }

    /// Check if table is empty
    pub fn is_empty(&self) -> bool {
        self.vectors.is_empty()
    }

    /// Get vector state
    pub fn get(&self, index: u16) -> Result<&MsixVectorState, MsixError> {
        self.vectors
            .get(index as usize)
            .ok_or(MsixError::InvalidVector {
                index,
                max: self.len(),
            })
    }

    /// Configure a vector's routing (address + data)
    /// This moves the vector from Masked to Configured
    pub fn configure(
        &mut self,
        index: u16,
        address: MsiAddress,
        data: MsiData,
    ) -> Result<(), MsixError> {
        let max = self.len();
        let state = self
            .vectors
            .get_mut(index as usize)
            .ok_or(MsixError::InvalidVector { index, max })?;

        // Validate address for x86
        if !address.is_valid_x86() {
            return Err(MsixError::InvalidAddress { address });
        }

        match state {
            MsixVectorState::Masked => {
                *state = MsixVectorState::Configured { address, data };
                Ok(())
            }
            MsixVectorState::Configured { .. } => {
                // Allow reconfiguring
                *state = MsixVectorState::Configured { address, data };
                Ok(())
            }
            MsixVectorState::Enabled { .. } => {
                // Must disable first
                Err(MsixError::AlreadyEnabled { index })
            }
        }
    }

    /// Enable a vector (must be configured first)
    /// Returns the old state for cleanup
    pub fn enable(
        &mut self,
        index: u16,
        eventfd: RawFd,
        gsi: u32,
    ) -> Result<(), MsixError> {
        let max = self.len();
        let state = self
            .vectors
            .get_mut(index as usize)
            .ok_or(MsixError::InvalidVector { index, max })?;

        match state {
            MsixVectorState::Masked => Err(MsixError::NotConfigured { index }),
            MsixVectorState::Configured { address, data } => {
                *state = MsixVectorState::Enabled {
                    address: *address,
                    data: *data,
                    eventfd,
                    gsi,
                };
                Ok(())
            }
            MsixVectorState::Enabled { .. } => {
                // Already enabled, could update or error
                // For now, treat as success (idempotent)
                Ok(())
            }
        }
    }

    /// Disable a vector (move back to Configured or Masked)
    pub fn disable(&mut self, index: u16) -> Result<Option<(RawFd, u32)>, MsixError> {
        let max = self.len();
        let state = self
            .vectors
            .get_mut(index as usize)
            .ok_or(MsixError::InvalidVector { index, max })?;

        match state {
            MsixVectorState::Masked => Ok(None),
            MsixVectorState::Configured { .. } => Ok(None),
            MsixVectorState::Enabled {
                address,
                data,
                eventfd,
                gsi,
            } => {
                let old_eventfd = *eventfd;
                let old_gsi = *gsi;
                *state = MsixVectorState::Configured {
                    address: *address,
                    data: *data,
                };
                Ok(Some((old_eventfd, old_gsi)))
            }
        }
    }

    /// Mask a vector (move to Masked state)
    pub fn mask(&mut self, index: u16) -> Result<Option<(RawFd, u32)>, MsixError> {
        let max = self.len();
        let state = self
            .vectors
            .get_mut(index as usize)
            .ok_or(MsixError::InvalidVector { index, max })?;

        match state {
            MsixVectorState::Masked => Ok(None),
            MsixVectorState::Configured { .. } => {
                *state = MsixVectorState::Masked;
                Ok(None)
            }
            MsixVectorState::Enabled { eventfd, gsi, .. } => {
                let old = Some((*eventfd, *gsi));
                *state = MsixVectorState::Masked;
                Ok(old)
            }
        }
    }

    /// Set global MSI-X enable
    pub fn set_enabled(&mut self, enabled: bool) {
        self.enabled = enabled;
    }

    /// Check if MSI-X is globally enabled
    pub fn is_enabled(&self) -> bool {
        self.enabled
    }

    /// Set function mask
    pub fn set_function_masked(&mut self, masked: bool) {
        self.function_masked = masked;
    }

    /// Check if function is masked
    pub fn is_function_masked(&self) -> bool {
        self.function_masked
    }

    /// Check if interrupts can be delivered for a vector
    pub fn can_deliver(&self, index: u16) -> bool {
        if !self.enabled || self.function_masked {
            return false;
        }
        self.vectors
            .get(index as usize)
            .map(|s| s.is_enabled())
            .unwrap_or(false)
    }

    /// Get count of enabled vectors
    pub fn enabled_count(&self) -> usize {
        self.vectors.iter().filter(|s| s.is_enabled()).count()
    }

    /// Get count of configured vectors
    pub fn configured_count(&self) -> usize {
        self.vectors.iter().filter(|s| s.is_configured()).count()
    }

    /// Iterate over all vectors with their indices
    pub fn iter(&self) -> impl Iterator<Item = (u16, &MsixVectorState)> {
        self.vectors
            .iter()
            .enumerate()
            .map(|(i, s)| (i as u16, s))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn valid_address() -> MsiAddress {
        MsiAddress::new(0xFEE0_0000, 0)
    }

    fn valid_data() -> MsiData {
        MsiData::new(0x30) // Vector 0x30, edge triggered
    }

    #[test]
    fn test_new_table() {
        let table = MsixTable::new(256);
        assert_eq!(table.len(), 256);
        assert!(!table.is_enabled());
        assert!(table.is_function_masked());
        assert_eq!(table.enabled_count(), 0);
        assert_eq!(table.configured_count(), 0);
    }

    #[test]
    fn test_vector_lifecycle() {
        let mut table = MsixTable::new(4);

        // Initial state: masked
        assert!(table.get(0).unwrap().is_masked());

        // Configure
        table.configure(0, valid_address(), valid_data()).unwrap();
        assert!(table.get(0).unwrap().is_configured());
        assert!(!table.get(0).unwrap().is_enabled());
        assert_eq!(table.configured_count(), 1);

        // Enable
        table.enable(0, 42, 100).unwrap();
        assert!(table.get(0).unwrap().is_enabled());
        assert_eq!(table.enabled_count(), 1);

        // Disable
        let old = table.disable(0).unwrap();
        assert_eq!(old, Some((42, 100)));
        assert!(table.get(0).unwrap().is_configured());
        assert!(!table.get(0).unwrap().is_enabled());

        // Mask
        table.mask(0).unwrap();
        assert!(table.get(0).unwrap().is_masked());
    }

    #[test]
    fn test_cannot_enable_unconfigured() {
        let mut table = MsixTable::new(4);
        
        let result = table.enable(0, 42, 100);
        assert_eq!(result, Err(MsixError::NotConfigured { index: 0 }));
    }

    #[test]
    fn test_cannot_reconfigure_enabled() {
        let mut table = MsixTable::new(4);
        
        table.configure(0, valid_address(), valid_data()).unwrap();
        table.enable(0, 42, 100).unwrap();
        
        let result = table.configure(0, valid_address(), MsiData::new(0x40));
        assert_eq!(result, Err(MsixError::AlreadyEnabled { index: 0 }));
    }

    #[test]
    fn test_invalid_vector_index() {
        let mut table = MsixTable::new(4);
        
        let result = table.configure(10, valid_address(), valid_data());
        assert_eq!(result, Err(MsixError::InvalidVector { index: 10, max: 4 }));
    }

    #[test]
    fn test_invalid_address() {
        let mut table = MsixTable::new(4);
        
        // Address outside MSI range
        let bad_addr = MsiAddress::new(0x1234_0000, 0);
        let result = table.configure(0, bad_addr, valid_data());
        assert_eq!(result, Err(MsixError::InvalidAddress { address: bad_addr }));
    }

    #[test]
    fn test_can_deliver() {
        let mut table = MsixTable::new(4);
        
        // Not enabled globally
        table.configure(0, valid_address(), valid_data()).unwrap();
        table.enable(0, 42, 100).unwrap();
        assert!(!table.can_deliver(0)); // Global enable is false
        
        // Enable globally
        table.set_enabled(true);
        assert!(!table.can_deliver(0)); // Function is masked
        
        // Unmask function
        table.set_function_masked(false);
        assert!(table.can_deliver(0)); // Now it can deliver
        
        // Unconfigured vector
        assert!(!table.can_deliver(1));
    }

    #[test]
    fn test_msi_address_validation() {
        // Valid x86 MSI address
        let valid = MsiAddress::new(0xFEE0_1000, 0);
        assert!(valid.is_valid_x86());
        
        // Invalid: high bits set
        let invalid = MsiAddress::new(0xFEE0_0000, 1);
        assert!(!invalid.is_valid_x86());
        
        // Invalid: wrong range
        let invalid = MsiAddress::new(0x1234_0000, 0);
        assert!(!invalid.is_valid_x86());
    }

    #[test]
    fn test_msi_data_parsing() {
        let data = MsiData::new(0x0234);
        assert_eq!(data.vector(), 0x34);
        assert_eq!(data.delivery_mode(), 2);
    }

    #[test]
    fn test_multiple_vectors() {
        let mut table = MsixTable::new(256);
        
        // Configure several vectors
        for i in 0..10 {
            let addr = MsiAddress::new(0xFEE0_0000 | (i << 12), 0);
            let data = MsiData::new(0x30 + i);
            table.configure(i as u16, addr, data).unwrap();
        }
        assert_eq!(table.configured_count(), 10);
        
        // Enable half of them
        for i in 0..5 {
            table.enable(i as u16, 100 + i as RawFd, 200 + i as u32).unwrap();
        }
        assert_eq!(table.enabled_count(), 5);
        assert_eq!(table.configured_count(), 10); // Enabled are also configured
    }

    #[test]
    fn test_routing_info() {
        let mut table = MsixTable::new(4);
        let addr = valid_address();
        let data = valid_data();
        
        // Masked: no routing
        assert!(table.get(0).unwrap().routing().is_none());
        
        // Configured: has routing
        table.configure(0, addr, data).unwrap();
        let routing = table.get(0).unwrap().routing();
        assert_eq!(routing, Some((addr, data)));
        
        // Enabled: still has routing
        table.enable(0, 42, 100).unwrap();
        let routing = table.get(0).unwrap().routing();
        assert_eq!(routing, Some((addr, data)));
    }
}

#[cfg(test)]
mod proptests {
    use super::*;
    use proptest::prelude::*;

    fn valid_msi_address() -> impl Strategy<Value = MsiAddress> {
        // Generate valid x86 MSI addresses (0xFEE0xxxx)
        (0u32..0x10000u32).prop_map(|x| MsiAddress::new(0xFEE0_0000 | x, 0))
    }

    fn valid_msi_data() -> impl Strategy<Value = MsiData> {
        any::<u32>().prop_map(MsiData::new)
    }

    proptest! {
        /// Property: Configure then enable always succeeds
        #[test]
        fn prop_configure_enable_succeeds(
            num_vectors in 1u16..256u16,
            vector in 0u16..255u16,
            addr in valid_msi_address(),
            data in valid_msi_data(),
        ) {
            let vector = vector % num_vectors;
            let mut table = MsixTable::new(num_vectors);
            
            prop_assert!(table.configure(vector, addr, data).is_ok());
            prop_assert!(table.enable(vector, 42, 100).is_ok());
            prop_assert!(table.get(vector).unwrap().is_enabled());
        }

        /// Property: Enable without configure fails
        #[test]
        fn prop_enable_without_configure_fails(
            num_vectors in 1u16..256u16,
            vector in 0u16..255u16,
        ) {
            let vector = vector % num_vectors;
            let table_size = num_vectors;
            let mut table = MsixTable::new(table_size);
            
            let result = table.enable(vector, 42, 100);
            let is_not_configured = matches!(result, Err(MsixError::NotConfigured { .. }));
            prop_assert!(is_not_configured, "Expected NotConfigured error");
        }

        /// Property: Disable returns correct eventfd/gsi
        #[test]
        fn prop_disable_returns_eventfd(
            addr in valid_msi_address(),
            data in valid_msi_data(),
            eventfd in 0i32..1000i32,
            gsi in 0u32..256u32,
        ) {
            let mut table = MsixTable::new(4);
            
            table.configure(0, addr, data).unwrap();
            table.enable(0, eventfd, gsi).unwrap();
            
            let result = table.disable(0).unwrap();
            prop_assert_eq!(result, Some((eventfd, gsi)));
        }

        /// Property: enabled_count <= configured_count <= len
        #[test]
        fn prop_count_invariants(
            num_vectors in 1u16..100u16,
            configure_count in 0usize..50usize,
            enable_count in 0usize..25usize,
        ) {
            let mut table = MsixTable::new(num_vectors);
            
            let to_configure = configure_count.min(num_vectors as usize);
            let to_enable = enable_count.min(to_configure);
            
            for i in 0..to_configure {
                let addr = MsiAddress::new(0xFEE0_0000 | (i as u32 * 0x1000), 0);
                table.configure(i as u16, addr, MsiData::new(i as u32)).unwrap();
            }
            
            for i in 0..to_enable {
                table.enable(i as u16, i as RawFd, i as u32).unwrap();
            }
            
            prop_assert!(table.enabled_count() <= table.configured_count());
            prop_assert!(table.configured_count() <= table.len() as usize);
        }

        /// Property: can_deliver requires global enable, function unmask, and vector enabled
        #[test]
        fn prop_can_deliver_conditions(
            global_enable in any::<bool>(),
            function_masked in any::<bool>(),
            vector_enabled in any::<bool>(),
        ) {
            let mut table = MsixTable::new(4);
            let addr = MsiAddress::new(0xFEE0_0000, 0);
            let data = MsiData::new(0x30);
            
            table.set_enabled(global_enable);
            table.set_function_masked(function_masked);
            
            if vector_enabled {
                table.configure(0, addr, data).unwrap();
                table.enable(0, 42, 100).unwrap();
            }
            
            let can_deliver = table.can_deliver(0);
            let expected = global_enable && !function_masked && vector_enabled;
            
            prop_assert_eq!(can_deliver, expected);
        }

        /// Property: Invalid vector index always fails
        #[test]
        fn prop_invalid_index_fails(
            num_vectors in 1u16..100u16,
            vector in 100u16..1000u16,
            addr in valid_msi_address(),
            data in valid_msi_data(),
        ) {
            let mut table = MsixTable::new(num_vectors);
            
            // Configure should fail
            let result = table.configure(vector, addr, data);
            let is_invalid = matches!(result, Err(MsixError::InvalidVector { .. }));
            prop_assert!(is_invalid, "Expected InvalidVector error from configure");
            
            // Enable should fail
            let result = table.enable(vector, 42, 100);
            let is_invalid = matches!(result, Err(MsixError::InvalidVector { .. }));
            prop_assert!(is_invalid, "Expected InvalidVector error from enable");
            
            // Get should fail
            let result = table.get(vector);
            let is_invalid = matches!(result, Err(MsixError::InvalidVector { .. }));
            prop_assert!(is_invalid, "Expected InvalidVector error from get");
        }
    }
}
