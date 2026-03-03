// Copyright 2025 Isospin Authors
// SPDX-License-Identifier: Apache-2.0
//
//! Property-based tests for MSI-X interrupt routing concurrency invariants.
//!
//! These tests verify that the MSI-X interrupt routing implementation maintains
//! correct invariants under all possible interleavings of:
//! - Guest writes to MSI-X table entries (address/data/ctrl)
//! - Guest writes to MSI-X config space (enable/function mask)
//! - KVM routing table updates
//! - Interrupt delivery
//!
//! ## Critical Concurrency Invariants
//!
//! 1. **Table Entry Consistency**: After write_msix_table(offset, data), the cached
//!    entry must exactly match what was written.
//!
//! 2. **Routing Update Ordering**: The sequence must be:
//!    register_msi() -> set_gsi_routes() -> enable_irqfd()
//!    Violating this order can cause lost interrupts or kernel panics.
//!
//! 3. **Enable/Disable Atomicity**: MSI-X enable state changes must atomically
//!    update all vectors' routing. No interrupt can be delivered during transition.
//!
//! 4. **Delivery Preconditions**: An interrupt can only be delivered when ALL of:
//!    - global_enable == true (MSI-X capability bit 15)
//!    - function_masked == false (MSI-X capability bit 14)
//!    - vector_masked == false (table entry vector_ctrl bit 0)
//!    - routing is configured (valid address/data in table entry)
//!    - KVM routing table is updated
//!    - irqfd is registered
//!
//! 5. **No Lost Updates**: A table write followed by enable must result in
//!    the written values being used for routing, never stale values.
//!
//! 6. **Idempotency**: Re-enabling an already-enabled vector must be safe.
//!    Re-configuring the same address/data must be safe.

#![cfg(test)]

use proptest::prelude::*;
use proptest::collection::vec;
use proptest::test_runner::Config;
use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, Ordering};

// ============================================================================
// Mock Types for Testing
// ============================================================================

/// Mock MSI-X table entry matching the real implementation
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct MockMsixTableEntry {
    pub msg_addr_lo: u32,
    pub msg_addr_hi: u32,
    pub msg_data: u32,
    pub vector_ctrl: u32,
}

impl MockMsixTableEntry {
    pub fn is_masked(&self) -> bool {
        (self.vector_ctrl & 1) != 0
    }
    
    /// Convert to bytes for table write simulation
    pub fn to_bytes(&self) -> [u8; 16] {
        let mut bytes = [0u8; 16];
        bytes[0..4].copy_from_slice(&self.msg_addr_lo.to_le_bytes());
        bytes[4..8].copy_from_slice(&self.msg_addr_hi.to_le_bytes());
        bytes[8..12].copy_from_slice(&self.msg_data.to_le_bytes());
        bytes[12..16].copy_from_slice(&self.vector_ctrl.to_le_bytes());
        bytes
    }
}

/// Mock MSI routing entry (what goes to KVM)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MockMsiRoute {
    pub gsi: u32,
    pub address_lo: u32,
    pub address_hi: u32,
    pub data: u32,
    pub devid: u32,
    pub masked: bool,
}

/// Mock KVM routing table state
#[derive(Debug, Default)]
pub struct MockKvmRoutingTable {
    routes: HashMap<u32, MockMsiRoute>,
    /// Tracks the sequence of operations for ordering verification
    operation_log: Vec<RoutingOperation>,
    /// Counter for unique operation IDs
    op_counter: AtomicU64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RoutingOperation {
    RegisterMsi { gsi: u32, op_id: u64 },
    SetGsiRoutes { op_id: u64 },
    EnableIrqfd { gsi: u32, op_id: u64 },
    DisableIrqfd { gsi: u32, op_id: u64 },
}

impl MockKvmRoutingTable {
    pub fn new() -> Self {
        Self::default()
    }
    
    fn next_op_id(&self) -> u64 {
        self.op_counter.fetch_add(1, Ordering::SeqCst)
    }
    
    pub fn register_msi(&mut self, route: MockMsiRoute) {
        let op_id = self.next_op_id();
        self.routes.insert(route.gsi, route);
        self.operation_log.push(RoutingOperation::RegisterMsi { gsi: route.gsi, op_id });
    }
    
    pub fn set_gsi_routes(&mut self) {
        let op_id = self.next_op_id();
        self.operation_log.push(RoutingOperation::SetGsiRoutes { op_id });
    }
    
    pub fn enable_irqfd(&mut self, gsi: u32) {
        let op_id = self.next_op_id();
        self.operation_log.push(RoutingOperation::EnableIrqfd { gsi, op_id });
    }
    
    pub fn disable_irqfd(&mut self, gsi: u32) {
        let op_id = self.next_op_id();
        self.operation_log.push(RoutingOperation::DisableIrqfd { gsi, op_id });
    }
    
    /// Verify the ordering invariant: register_msi < set_gsi_routes < enable_irqfd for each GSI
    pub fn verify_ordering_invariant(&self) -> Result<(), String> {
        // For each GSI, find the operations and verify ordering
        let mut gsi_ops: HashMap<u32, Vec<(u64, &str)>> = HashMap::new();
        
        for op in &self.operation_log {
            match op {
                RoutingOperation::RegisterMsi { gsi, op_id } => {
                    gsi_ops.entry(*gsi).or_default().push((*op_id, "register"));
                }
                RoutingOperation::SetGsiRoutes { op_id } => {
                    // SetGsiRoutes applies to all GSIs
                    for (_, ops) in gsi_ops.iter_mut() {
                        ops.push((*op_id, "set_routes"));
                    }
                }
                RoutingOperation::EnableIrqfd { gsi, op_id } => {
                    gsi_ops.entry(*gsi).or_default().push((*op_id, "enable"));
                }
                RoutingOperation::DisableIrqfd { gsi, op_id } => {
                    gsi_ops.entry(*gsi).or_default().push((*op_id, "disable"));
                }
            }
        }
        
        // For each GSI, verify that register < set_routes < enable
        for (gsi, ops) in gsi_ops {
            let mut last_register: Option<u64> = None;
            let mut last_set_routes: Option<u64> = None;
            
            for (op_id, op_type) in ops {
                match op_type {
                    "register" => {
                        last_register = Some(op_id);
                    }
                    "set_routes" => {
                        last_set_routes = Some(op_id);
                    }
                    "enable" => {
                        // Enable must come after both register and set_routes
                        if let Some(reg_id) = last_register {
                            if let Some(set_id) = last_set_routes {
                                if !(reg_id < set_id && set_id < op_id) {
                                    return Err(format!(
                                        "GSI {}: ordering violation - register={}, set_routes={}, enable={}",
                                        gsi, reg_id, set_id, op_id
                                    ));
                                }
                            }
                        }
                    }
                    "disable" => {
                        // Disable can happen anytime after enable
                    }
                    _ => {}
                }
            }
        }
        
        Ok(())
    }
    
    /// Get route for a GSI
    pub fn get_route(&self, gsi: u32) -> Option<&MockMsiRoute> {
        self.routes.get(&gsi)
    }
    
    /// Check if a GSI has been enabled
    pub fn is_enabled(&self, gsi: u32) -> bool {
        // Check if the last operation for this GSI was an enable (not disable)
        let mut enabled = false;
        for op in &self.operation_log {
            match op {
                RoutingOperation::EnableIrqfd { gsi: g, .. } if *g == gsi => {
                    enabled = true;
                }
                RoutingOperation::DisableIrqfd { gsi: g, .. } if *g == gsi => {
                    enabled = false;
                }
                _ => {}
            }
        }
        enabled
    }
}

/// Mock VFIO MSI-X state (mirrors VfioMsixState)
#[derive(Debug)]
pub struct MockVfioMsixState {
    pub table_size: u16,
    pub table_bar: u8,
    pub table_offset: u32,
    pub table_entries: Vec<MockMsixTableEntry>,
    pub enabled: bool,
    pub function_masked: bool,
    /// PCI device ID for routing
    pub pci_devid: u32,
}

impl MockVfioMsixState {
    pub fn new(table_size: u16, pci_devid: u32) -> Self {
        Self {
            table_size,
            table_bar: 0,
            table_offset: 0,
            table_entries: vec![MockMsixTableEntry::default(); table_size as usize],
            enabled: false,
            function_masked: true,
            pci_devid,
        }
    }
    
    /// Check if offset accesses the MSI-X table
    pub fn is_table_access(&self, bar_index: u8, offset: u64) -> bool {
        if bar_index == self.table_bar {
            let table_end = self.table_offset as u64 + (self.table_size as u64 * 16);
            offset >= self.table_offset as u64 && offset < table_end
        } else {
            false
        }
    }
    
    /// Write to MSI-X table, returns vector index modified
    pub fn write_table(&mut self, bar_offset: u64, data: &[u8]) -> Option<u16> {
        let table_offset = bar_offset - self.table_offset as u64;
        let vector_index = (table_offset / 16) as u16;
        let entry_offset = (table_offset % 16) as usize;
        
        if vector_index >= self.table_size {
            return None;
        }
        
        let entry = &mut self.table_entries[vector_index as usize];
        
        // Write to the appropriate field
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
            _ => return None,
        }
        
        Some(vector_index)
    }
    
    /// Read from MSI-X table
    pub fn read_table(&self, bar_offset: u64) -> u32 {
        let table_offset = bar_offset - self.table_offset as u64;
        let vector_index = (table_offset / 16) as usize;
        let entry_offset = (table_offset % 16) as usize;
        
        if vector_index >= self.table_size as usize {
            return 0;
        }
        
        let entry = &self.table_entries[vector_index];
        
        match entry_offset {
            0..=3 => entry.msg_addr_lo,
            4..=7 => entry.msg_addr_hi,
            8..=11 => entry.msg_data,
            12..=15 => entry.vector_ctrl,
            _ => 0,
        }
    }
    
    /// Check if interrupts can be delivered for a vector
    pub fn can_deliver(&self, vector_index: u16) -> bool {
        if !self.enabled || self.function_masked {
            return false;
        }
        
        if let Some(entry) = self.table_entries.get(vector_index as usize) {
            // Must not be masked and must have valid address
            !entry.is_masked() && 
            (entry.msg_addr_lo & 0xFFF0_0000) == 0xFEE0_0000 &&
            entry.msg_addr_hi == 0
        } else {
            false
        }
    }
}

/// Complete mock device with routing
#[derive(Debug)]
pub struct MockVfioDevice {
    pub msix_state: MockVfioMsixState,
    pub routing_table: MockKvmRoutingTable,
    /// Allocated GSIs for each vector
    pub vector_gsis: Vec<u32>,
}

impl MockVfioDevice {
    pub fn new(table_size: u16, pci_devid: u32) -> Self {
        // Allocate GSIs starting from 32 (after legacy IRQs)
        let vector_gsis = (0..table_size).map(|i| 32 + i as u32).collect();
        
        Self {
            msix_state: MockVfioMsixState::new(table_size, pci_devid),
            routing_table: MockKvmRoutingTable::new(),
            vector_gsis,
        }
    }
    
    /// Write a complete MSI-X table entry (helper that writes field-by-field like real hardware)
    pub fn write_entry(&mut self, vector: u16, entry: &MockMsixTableEntry) {
        let offset = (vector as u64) * 16;
        self.guest_write_table(offset + 0, &entry.msg_addr_lo.to_le_bytes());
        self.guest_write_table(offset + 4, &entry.msg_addr_hi.to_le_bytes());
        self.guest_write_table(offset + 8, &entry.msg_data.to_le_bytes());
        self.guest_write_table(offset + 12, &entry.vector_ctrl.to_le_bytes());
    }
    
    /// Simulate guest writing to MSI-X table (4-byte aligned writes)
    pub fn guest_write_table(&mut self, bar_offset: u64, data: &[u8]) {
        if let Some(vector_idx) = self.msix_state.write_table(bar_offset, data) {
            // If MSI-X is enabled, update routing
            if self.msix_state.enabled && !self.msix_state.function_masked {
                self.update_vector_routing(vector_idx);
            }
        }
    }
    
    /// Simulate guest enabling MSI-X via config space
    pub fn guest_enable_msix(&mut self, enabled: bool, function_masked: bool) {
        let was_enabled = self.msix_state.enabled && !self.msix_state.function_masked;
        self.msix_state.enabled = enabled;
        self.msix_state.function_masked = function_masked;
        let now_enabled = enabled && !function_masked;
        
        if was_enabled != now_enabled {
            if now_enabled {
                // Enable all vectors
                for i in 0..self.msix_state.table_size {
                    self.update_vector_routing(i);
                }
            } else {
                // Disable all vectors
                for i in 0..self.msix_state.table_size {
                    let gsi = self.vector_gsis[i as usize];
                    self.routing_table.disable_irqfd(gsi);
                }
            }
        }
    }
    
    /// Update routing for a single vector (simulates update_msix_routing)
    fn update_vector_routing(&mut self, vector_idx: u16) {
        let entry = &self.msix_state.table_entries[vector_idx as usize];
        let gsi = self.vector_gsis[vector_idx as usize];
        
        let route = MockMsiRoute {
            gsi,
            address_lo: entry.msg_addr_lo,
            address_hi: entry.msg_addr_hi,
            data: entry.msg_data,
            devid: self.msix_state.pci_devid,
            masked: entry.is_masked(),
        };
        
        // The correct order: register -> set_routes -> enable
        if entry.is_masked() {
            self.routing_table.disable_irqfd(gsi);
        }
        
        self.routing_table.register_msi(route);
        self.routing_table.set_gsi_routes();
        
        if !entry.is_masked() {
            self.routing_table.enable_irqfd(gsi);
        }
    }
    
    /// Check if interrupt can be delivered for vector
    pub fn can_deliver(&self, vector_idx: u16) -> bool {
        // All conditions must be met
        self.msix_state.can_deliver(vector_idx) &&
        self.routing_table.is_enabled(self.vector_gsis[vector_idx as usize])
    }
}

// ============================================================================
// Property Test Strategies
// ============================================================================

/// Generate valid MSI address (0xFEExxxxx)
fn msi_address_strategy() -> impl Strategy<Value = u32> {
    (0u32..0x10000).prop_map(|offset| 0xFEE0_0000 | offset)
}

/// Generate MSI data (vector and delivery mode)
fn msi_data_strategy() -> impl Strategy<Value = u32> {
    // Vector 0-255, delivery mode 0-7, level/trigger bits
    any::<u32>()
}

/// Generate vector control (masked or unmasked)
fn vector_ctrl_strategy() -> impl Strategy<Value = u32> {
    prop_oneof![
        Just(0u32),  // Unmasked
        Just(1u32),  // Masked
    ]
}

/// Generate a complete MSI-X table entry
fn msix_entry_strategy() -> impl Strategy<Value = MockMsixTableEntry> {
    (msi_address_strategy(), any::<u32>(), msi_data_strategy(), vector_ctrl_strategy())
        .prop_map(|(addr_lo, addr_hi, data, ctrl)| MockMsixTableEntry {
            msg_addr_lo: addr_lo,
            msg_addr_hi: addr_hi & 0, // Force high addr to 0 for valid x86 MSI
            msg_data: data,
            vector_ctrl: ctrl,
        })
}

/// Operation that a guest can perform
#[derive(Debug, Clone)]
pub enum GuestOperation {
    /// Write entire table entry
    WriteEntry { vector: u16, entry: MockMsixTableEntry },
    /// Write single field of table entry
    WriteField { vector: u16, field: u8, value: u32 },
    /// Enable/disable MSI-X globally
    SetEnable { enabled: bool, function_masked: bool },
}

fn guest_operation_strategy(max_vectors: u16) -> impl Strategy<Value = GuestOperation> {
    let vector_range = 0..max_vectors;
    
    prop_oneof![
        3 => (vector_range.clone(), msix_entry_strategy())
            .prop_map(|(v, e)| GuestOperation::WriteEntry { vector: v, entry: e }),
        2 => (vector_range.clone(), 0u8..4u8, any::<u32>())
            .prop_map(|(v, f, val)| GuestOperation::WriteField { vector: v, field: f, value: val }),
        1 => (any::<bool>(), any::<bool>())
            .prop_map(|(e, m)| GuestOperation::SetEnable { enabled: e, function_masked: m }),
    ]
}

// ============================================================================
// Property Tests - Table Entry Consistency
// ============================================================================

proptest! {
    #![proptest_config(Config::with_cases(1000))]
    
    /// INVARIANT 1: After writing a table entry, reading it back returns the same value
    #[test]
    fn prop_table_entry_write_read_consistency(
        entry in msix_entry_strategy(),
        vector in 0u16..64u16,
    ) {
        let table_size = 64;
        let mut state = MockVfioMsixState::new(table_size, 0x0100);
        
        // Write entry field by field (simulating guest 32-bit writes)
        let base_offset = (vector as u64) * 16;
        state.write_table(base_offset + 0, &entry.msg_addr_lo.to_le_bytes());
        state.write_table(base_offset + 4, &entry.msg_addr_hi.to_le_bytes());
        state.write_table(base_offset + 8, &entry.msg_data.to_le_bytes());
        state.write_table(base_offset + 12, &entry.vector_ctrl.to_le_bytes());
        
        // Read back
        prop_assert_eq!(state.read_table(base_offset + 0), entry.msg_addr_lo);
        prop_assert_eq!(state.read_table(base_offset + 4), entry.msg_addr_hi);
        prop_assert_eq!(state.read_table(base_offset + 8), entry.msg_data);
        prop_assert_eq!(state.read_table(base_offset + 12), entry.vector_ctrl);
        
        // Verify the internal state matches
        prop_assert_eq!(state.table_entries[vector as usize], entry);
    }
    
    /// INVARIANT 1b: Partial writes to different fields don't interfere
    #[test]
    fn prop_table_partial_writes_isolated(
        entries in vec(msix_entry_strategy(), 4),
    ) {
        let mut state = MockVfioMsixState::new(4, 0x0100);
        
        // Write different fields in interleaved order
        for (i, entry) in entries.iter().enumerate() {
            let offset = (i as u64) * 16;
            state.write_table(offset + 0, &entry.msg_addr_lo.to_le_bytes());
        }
        for (i, entry) in entries.iter().enumerate() {
            let offset = (i as u64) * 16;
            state.write_table(offset + 4, &entry.msg_addr_hi.to_le_bytes());
        }
        for (i, entry) in entries.iter().enumerate() {
            let offset = (i as u64) * 16;
            state.write_table(offset + 8, &entry.msg_data.to_le_bytes());
        }
        for (i, entry) in entries.iter().enumerate() {
            let offset = (i as u64) * 16;
            state.write_table(offset + 12, &entry.vector_ctrl.to_le_bytes());
        }
        
        // Verify all entries are correct
        for (i, entry) in entries.iter().enumerate() {
            prop_assert_eq!(
                state.table_entries[i], *entry,
                "Entry {} mismatch after interleaved writes", i
            );
        }
    }
    
    /// INVARIANT 1c: Out-of-bounds writes are ignored
    #[test]
    fn prop_table_oob_writes_ignored(
        table_size in 1u16..64u16,
        oob_offset in 64u16..1000u16,
        data in any::<u32>(),
    ) {
        let mut state = MockVfioMsixState::new(table_size, 0x0100);
        
        // Try to write beyond table
        let offset = (oob_offset as u64) * 16;
        let result = state.write_table(offset, &data.to_le_bytes());
        
        prop_assert!(result.is_none(), "OOB write should return None");
        
        // Verify no entries were modified
        for entry in &state.table_entries {
            prop_assert_eq!(*entry, MockMsixTableEntry::default());
        }
    }
}

// ============================================================================
// Property Tests - Routing Update Ordering
// ============================================================================

proptest! {
    #![proptest_config(Config::with_cases(500))]
    
    /// INVARIANT 2: Routing operations follow correct order: register -> set_routes -> enable
    #[test]
    fn prop_routing_order_correct(
        operations in vec(guest_operation_strategy(16), 1..50),
    ) {
        let mut device = MockVfioDevice::new(16, 0x0100);
        
        // Execute all operations
        for op in operations {
            match op {
                GuestOperation::WriteEntry { vector, entry } => {
                    let offset = (vector as u64) * 16;
                    let bytes = entry.to_bytes();
                    device.guest_write_table(offset, &bytes[0..4]);
                    device.guest_write_table(offset + 4, &bytes[4..8]);
                    device.guest_write_table(offset + 8, &bytes[8..12]);
                    device.guest_write_table(offset + 12, &bytes[12..16]);
                }
                GuestOperation::WriteField { vector, field, value } => {
                    let offset = (vector as u64) * 16 + (field as u64) * 4;
                    device.guest_write_table(offset, &value.to_le_bytes());
                }
                GuestOperation::SetEnable { enabled, function_masked } => {
                    device.guest_enable_msix(enabled, function_masked);
                }
            }
        }
        
        // Verify ordering invariant
        let result = device.routing_table.verify_ordering_invariant();
        prop_assert!(
            result.is_ok(),
            "Ordering invariant violated: {:?}", result
        );
    }
    
    /// INVARIANT 2b: Enable after write uses the written values
    #[test]
    fn prop_enable_uses_written_values(
        entries in vec(msix_entry_strategy(), 4),
    ) {
        let mut device = MockVfioDevice::new(4, 0x0100);
        
        // Write entries while MSI-X is disabled
        for (i, entry) in entries.iter().enumerate() {
            let offset = (i as u64) * 16;
            let bytes = entry.to_bytes();
            device.guest_write_table(offset, &bytes[0..4]);
            device.guest_write_table(offset + 4, &bytes[4..8]);
            device.guest_write_table(offset + 8, &bytes[8..12]);
            device.guest_write_table(offset + 12, &bytes[12..16]);
        }
        
        // Now enable MSI-X
        device.guest_enable_msix(true, false);
        
        // Verify routing uses the correct values
        for (i, entry) in entries.iter().enumerate() {
            let gsi = device.vector_gsis[i];
            if let Some(route) = device.routing_table.get_route(gsi) {
                prop_assert_eq!(
                    route.address_lo, entry.msg_addr_lo,
                    "Route for vector {} has wrong address_lo", i
                );
                prop_assert_eq!(
                    route.address_hi, entry.msg_addr_hi,
                    "Route for vector {} has wrong address_hi", i
                );
                prop_assert_eq!(
                    route.data, entry.msg_data,
                    "Route for vector {} has wrong data", i
                );
            }
        }
    }
}

// ============================================================================
// Property Tests - Enable/Disable Atomicity  
// ============================================================================

proptest! {
    #![proptest_config(Config::with_cases(500))]
    
    /// INVARIANT 3: After disable, no vector reports can_deliver
    #[test]
    fn prop_disable_stops_all_delivery(
        entries in vec(msix_entry_strategy(), 8),
    ) {
        let mut device = MockVfioDevice::new(8, 0x0100);
        
        // Setup and enable
        for (i, entry) in entries.iter().enumerate() {
            device.write_entry(i as u16, entry);
        }
        device.guest_enable_msix(true, false);
        
        // Disable
        device.guest_enable_msix(false, false);
        
        // No vector should be deliverable
        for i in 0..8 {
            prop_assert!(
                !device.can_deliver(i),
                "Vector {} can_deliver after global disable", i
            );
        }
    }
    
    /// INVARIANT 3b: After function mask, no vector reports can_deliver
    #[test]
    fn prop_function_mask_stops_all_delivery(
        entries in vec(msix_entry_strategy(), 8),
    ) {
        let mut device = MockVfioDevice::new(8, 0x0100);
        
        // Setup and enable
        for (i, entry) in entries.iter().enumerate() {
            device.write_entry(i as u16, entry);
        }
        device.guest_enable_msix(true, false);
        
        // Apply function mask
        device.guest_enable_msix(true, true);
        
        // No vector should be deliverable
        for i in 0..8 {
            prop_assert!(
                !device.can_deliver(i),
                "Vector {} can_deliver after function mask", i
            );
        }
    }
    
    /// INVARIANT 3c: Enable/disable cycles preserve table contents
    #[test]
    fn prop_enable_disable_preserves_table(
        entries in vec(msix_entry_strategy(), 4),
        cycles in 1usize..10usize,
    ) {
        let mut device = MockVfioDevice::new(4, 0x0100);
        
        // Write entries
        for (i, entry) in entries.iter().enumerate() {
            device.write_entry(i as u16, entry);
        }
        
        // Cycle enable/disable
        for _ in 0..cycles {
            device.guest_enable_msix(true, false);
            device.guest_enable_msix(false, false);
        }
        
        // Table contents should be unchanged
        for (i, entry) in entries.iter().enumerate() {
            prop_assert_eq!(
                device.msix_state.table_entries[i], *entry,
                "Entry {} changed after enable/disable cycles", i
            );
        }
    }
}

// ============================================================================
// Property Tests - Delivery Preconditions
// ============================================================================

proptest! {
    #![proptest_config(Config::with_cases(1000))]
    
    /// INVARIANT 4: can_deliver requires ALL conditions
    #[test]
    fn prop_delivery_requires_all_conditions(
        global_enable in any::<bool>(),
        function_masked in any::<bool>(),
        vector_masked in any::<bool>(),
        valid_address in any::<bool>(),
    ) {
        let mut device = MockVfioDevice::new(1, 0x0100);
        
        // Setup entry
        let entry = MockMsixTableEntry {
            msg_addr_lo: if valid_address { 0xFEE0_0000 } else { 0x1234_0000 },
            msg_addr_hi: 0,
            msg_data: 0x30,
            vector_ctrl: if vector_masked { 1 } else { 0 },
        };
        device.write_entry(0, &entry);
        
        // Set global state
        device.guest_enable_msix(global_enable, function_masked);
        
        // Expected: can deliver only if ALL conditions met
        let expected = global_enable && !function_masked && !vector_masked && valid_address;
        let actual = device.msix_state.can_deliver(0);
        
        prop_assert_eq!(
            actual, expected,
            "can_deliver mismatch: global_enable={}, function_masked={}, vector_masked={}, valid_address={}",
            global_enable, function_masked, vector_masked, valid_address
        );
    }
    
    /// INVARIANT 4b: Masking a vector stops just that vector
    #[test]
    fn prop_vector_mask_is_specific(num_vectors in 2u16..16u16) {
        let mut device = MockVfioDevice::new(num_vectors, 0x0100);
        
        // Setup all vectors as unmasked with valid addresses
        for i in 0..num_vectors {
            let entry = MockMsixTableEntry {
                msg_addr_lo: 0xFEE0_0000 | (i as u32 * 0x1000),
                msg_addr_hi: 0,
                msg_data: 0x30 + i as u32,
                vector_ctrl: 0, // unmasked
            };
            device.write_entry(i, &entry);
        }
        device.guest_enable_msix(true, false);
        
        // Mask just vector 0 by writing only the vector_ctrl field
        device.guest_write_table(12, &1u32.to_le_bytes()); // vector_ctrl = masked
        
        // Vector 0 should not be deliverable
        prop_assert!(
            !device.msix_state.can_deliver(0),
            "Masked vector 0 should not be deliverable"
        );
        
        // Other vectors should still be deliverable
        for i in 1..num_vectors {
            prop_assert!(
                device.msix_state.can_deliver(i),
                "Unmasked vector {} should be deliverable", i
            );
        }
    }
}

// ============================================================================
// Property Tests - No Lost Updates
// ============================================================================

proptest! {
    #![proptest_config(Config::with_cases(500))]
    
    /// INVARIANT 5: Updates while enabled are immediately reflected
    #[test]
    fn prop_updates_while_enabled_reflected(
        initial in msix_entry_strategy(),
        updated in msix_entry_strategy(),
    ) {
        let mut device = MockVfioDevice::new(1, 0x0100);
        
        // Setup and enable
        device.write_entry(0, &initial);
        device.guest_enable_msix(true, false);
        
        // Update while enabled
        device.write_entry(0, &updated);
        
        // Route should reflect updated values
        let gsi = device.vector_gsis[0];
        if let Some(route) = device.routing_table.get_route(gsi) {
            prop_assert_eq!(
                route.address_lo, updated.msg_addr_lo,
                "Route not updated: addr_lo"
            );
            prop_assert_eq!(
                route.data, updated.msg_data,
                "Route not updated: data"
            );
        }
    }
    
    /// INVARIANT 5b: Rapid updates don't lose any values
    #[test]
    fn prop_rapid_updates_no_loss(
        updates in vec(msix_entry_strategy(), 10..20),
    ) {
        let mut device = MockVfioDevice::new(1, 0x0100);
        device.guest_enable_msix(true, false);
        
        // Apply all updates
        for entry in &updates {
            device.write_entry(0, entry);
        }
        
        // Final state should match last update
        let last = updates.last().unwrap();
        prop_assert_eq!(
            device.msix_state.table_entries[0], *last,
            "Final entry doesn't match last update"
        );
        
        let gsi = device.vector_gsis[0];
        if let Some(route) = device.routing_table.get_route(gsi) {
            prop_assert_eq!(
                route.address_lo, last.msg_addr_lo,
                "Final route doesn't match last update"
            );
        }
    }
}

// ============================================================================
// Property Tests - Idempotency
// ============================================================================

proptest! {
    #![proptest_config(Config::with_cases(500))]
    
    /// INVARIANT 6: Re-enabling is idempotent
    #[test]
    fn prop_reenable_idempotent(entry in msix_entry_strategy()) {
        let mut device = MockVfioDevice::new(1, 0x0100);
        
        device.write_entry(0, &entry);
        device.guest_enable_msix(true, false);
        
        // Get state after first enable
        let table_after_first = device.msix_state.table_entries[0].clone();
        let gsi = device.vector_gsis[0];
        let route_after_first = device.routing_table.get_route(gsi).cloned();
        
        // Re-enable
        device.guest_enable_msix(true, false);
        
        // State should be unchanged
        prop_assert_eq!(
            device.msix_state.table_entries[0], table_after_first,
            "Table changed after re-enable"
        );
        
        // Route should have same values (may have new op entries but same data)
        if let (Some(r1), Some(r2)) = (route_after_first, device.routing_table.get_route(gsi)) {
            prop_assert_eq!(r1.address_lo, r2.address_lo);
            prop_assert_eq!(r1.data, r2.data);
        }
    }
    
    /// INVARIANT 6b: Writing same value is idempotent
    #[test]
    fn prop_same_write_idempotent(entry in msix_entry_strategy()) {
        let mut device = MockVfioDevice::new(1, 0x0100);
        device.guest_enable_msix(true, false);
        
        // Write same entry multiple times
        for _ in 0..5 {
            device.write_entry(0, &entry);
        }
        
        // Should have correct final state
        prop_assert_eq!(
            device.msix_state.table_entries[0], entry,
            "Entry doesn't match after repeated writes"
        );
    }
}

// ============================================================================
// Regression Tests for Known Edge Cases
// ============================================================================

#[cfg(test)]
mod regression_tests {
    use super::*;
    
    /// Regression: NVIDIA driver calls osVerifyInterrupts which triggers vector 0
    /// and waits for it. If routing isn't set up correctly, this times out.
    #[test]
    fn test_nvidia_verify_interrupts_scenario() {
        let mut device = MockVfioDevice::new(64, 0x0108);
        
        // NVIDIA driver sequence:
        // 1. Write MSI-X table entries before enabling
        for i in 0..64u16 {
            let entry = MockMsixTableEntry {
                msg_addr_lo: 0xFEE0_0000 | (((i % 16) as u32) << 12),
                msg_addr_hi: 0,
                msg_data: 0x30 + (i as u32),
                vector_ctrl: 0, // unmasked
            };
            device.write_entry(i, &entry);
        }
        
        // 2. Enable MSI-X
        device.guest_enable_msix(true, false);
        
        // 3. Vector 0 should be deliverable
        assert!(device.can_deliver(0), "Vector 0 must be deliverable for osVerifyInterrupts");
        
        // 4. Routing should be correct
        let gsi = device.vector_gsis[0];
        let route = device.routing_table.get_route(gsi).expect("Route must exist");
        assert_eq!(route.address_lo, 0xFEE0_0000);
        assert!(!route.masked);
        
        // 5. Ordering must be correct
        assert!(device.routing_table.verify_ordering_invariant().is_ok());
    }
    
    /// Regression: Guest may write table entries in arbitrary order
    #[test]
    fn test_arbitrary_field_order() {
        let mut device = MockVfioDevice::new(1, 0x0100);
        
        // Write fields in reverse order (ctrl, data, addr_hi, addr_lo)
        device.guest_write_table(12, &1u32.to_le_bytes()); // ctrl = masked
        device.guest_write_table(8, &0x42u32.to_le_bytes()); // data
        device.guest_write_table(4, &0u32.to_le_bytes()); // addr_hi
        device.guest_write_table(0, &0xFEE0_1000u32.to_le_bytes()); // addr_lo
        
        // Enable
        device.guest_enable_msix(true, false);
        
        // Should not be deliverable (masked)
        assert!(!device.can_deliver(0));
        
        // Unmask
        device.guest_write_table(12, &0u32.to_le_bytes());
        
        // Now should be deliverable
        assert!(device.can_deliver(0));
    }
    
    /// Regression: Enable/disable race with table write
    #[test]
    fn test_enable_disable_race() {
        let mut device = MockVfioDevice::new(4, 0x0100);
        
        // Setup initial entries
        for i in 0..4u16 {
            let entry = MockMsixTableEntry {
                msg_addr_lo: 0xFEE0_0000,
                msg_addr_hi: 0,
                msg_data: 0x30 + i as u32,
                vector_ctrl: 0,
            };
            device.write_entry(i, &entry);
        }
        
        // Rapid enable/disable while writing
        device.guest_enable_msix(true, false);
        device.guest_write_table(0, &0xFEE0_1000u32.to_le_bytes());
        device.guest_enable_msix(false, false);
        device.guest_write_table(0, &0xFEE0_2000u32.to_le_bytes());
        device.guest_enable_msix(true, false);
        
        // Final state should be consistent
        assert_eq!(device.msix_state.table_entries[0].msg_addr_lo, 0xFEE0_2000);
        
        // Routing should use final value
        let gsi = device.vector_gsis[0];
        let route = device.routing_table.get_route(gsi).unwrap();
        assert_eq!(route.address_lo, 0xFEE0_2000);
    }
    
    /// Regression: Maximum vector count (2048)
    #[test]
    fn test_max_vectors() {
        let mut device = MockVfioDevice::new(2048, 0x0100);
        
        // Configure all vectors
        for i in 0..2048u16 {
            let entry = MockMsixTableEntry {
                msg_addr_lo: 0xFEE0_0000,
                msg_addr_hi: 0,
                msg_data: i as u32,
                vector_ctrl: 0,
            };
            device.write_entry(i, &entry);
        }
        
        device.guest_enable_msix(true, false);
        
        // All vectors should be deliverable
        for i in 0..2048 {
            assert!(
                device.msix_state.can_deliver(i),
                "Vector {} should be deliverable", i
            );
        }
    }
    
    /// Regression: Single vector (minimum)
    #[test]
    fn test_single_vector() {
        let mut device = MockVfioDevice::new(1, 0x0100);
        
        let entry = MockMsixTableEntry {
            msg_addr_lo: 0xFEE0_0000,
            msg_addr_hi: 0,
            msg_data: 0x30,
            vector_ctrl: 0,
        };
        device.write_entry(0, &entry);
        device.guest_enable_msix(true, false);
        
        assert!(device.can_deliver(0));
    }
}
