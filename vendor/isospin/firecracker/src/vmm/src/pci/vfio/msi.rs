// Copyright 2025 Isospin Authors
// SPDX-License-Identifier: Apache-2.0
//
//! MSI (Message Signaled Interrupts) support for VFIO devices.
//!
//! This implements plain MSI interrupt handling (capability ID 0x05),
//! which is distinct from MSI-X (capability ID 0x11). Some devices like
//! the NVIDIA RTX PRO 6000 Blackwell GPU only support MSI, not MSI-X.
//!
//! MSI stores interrupt configuration in config space registers, unlike
//! MSI-X which uses a BAR-based table.

/// MSI control register bits
pub const MSI_CTL_ENABLE: u16 = 0x0001;
pub const MSI_CTL_MULTI_MSG_CAPABLE: u16 = 0x000E; // bits 3:1
pub const MSI_CTL_MULTI_MSG_ENABLE: u16 = 0x0070;  // bits 6:4
pub const MSI_CTL_64_BIT_CAPABLE: u16 = 0x0080;    // bit 7
pub const MSI_CTL_PER_VECTOR_MASK: u16 = 0x0100;   // bit 8

/// MSI capability register offsets (relative to capability start)
pub const MSI_CAP_ID_OFFSET: u64 = 0x00;       // Capability ID (0x05)
pub const MSI_CAP_NEXT_OFFSET: u64 = 0x01;     // Next capability pointer
pub const MSI_MSG_CTL_OFFSET: u64 = 0x02;      // Message Control (2 bytes)
pub const MSI_MSG_ADDR_LO_OFFSET: u64 = 0x04;  // Message Address Low (4 bytes)
// For 64-bit:
pub const MSI_MSG_ADDR_HI_OFFSET: u64 = 0x08;  // Message Address High (4 bytes)
pub const MSI_MSG_DATA_64_OFFSET: u64 = 0x0C;  // Message Data (2 bytes, 64-bit addr)
pub const MSI_MASK_64_OFFSET: u64 = 0x10;      // Mask Bits (4 bytes, 64-bit addr)
pub const MSI_PENDING_64_OFFSET: u64 = 0x14;   // Pending Bits (4 bytes, 64-bit addr)
// For 32-bit:
pub const MSI_MSG_DATA_32_OFFSET: u64 = 0x08;  // Message Data (2 bytes, 32-bit addr)
pub const MSI_MASK_32_OFFSET: u64 = 0x0C;      // Mask Bits (4 bytes, 32-bit addr)
pub const MSI_PENDING_32_OFFSET: u64 = 0x10;   // Pending Bits (4 bytes, 32-bit addr)

/// Address mask for MSI address register (bits 31:2 are used)
pub const MSI_MSG_ADDR_LO_MASK: u32 = 0xFFFF_FFFC;

/// Calculate the number of enabled MSI vectors from msg_ctl
pub fn msi_num_enabled_vectors(msg_ctl: u16) -> usize {
    let field = (msg_ctl >> 4) & 0x7;
    if field > 5 {
        return 0;
    }
    1 << field
}

/// Calculate the maximum number of MSI vectors the device supports
pub fn msi_max_vectors(msg_ctl: u16) -> usize {
    let field = (msg_ctl >> 1) & 0x7;
    if field > 5 {
        return 0;
    }
    1 << field
}

/// MSI capability structure (mirrors hardware config space layout)
#[derive(Debug, Clone, Copy, Default)]
pub struct MsiCap {
    /// Message Control Register
    /// - Bit 0: MSI enable
    /// - Bits 3:1: Multiple message capable (log2 of max vectors)
    /// - Bits 6:4: Multiple message enable (log2 of enabled vectors)  
    /// - Bit 7: 64-bit address capable
    /// - Bit 8: Per-vector masking capable
    pub msg_ctl: u16,
    /// Message Address (lower 32 bits)
    pub msg_addr_lo: u32,
    /// Message Address (upper 32 bits, only if 64-bit capable)
    pub msg_addr_hi: u32,
    /// Message Data
    pub msg_data: u16,
    /// Mask Bits (only if per-vector masking capable)
    pub mask_bits: u32,
    /// Pending Bits (only if per-vector masking capable)
    pub pending_bits: u32,
}

impl MsiCap {
    /// Check if MSI supports 64-bit addressing
    pub fn is_64bit(&self) -> bool {
        (self.msg_ctl & MSI_CTL_64_BIT_CAPABLE) != 0
    }

    /// Check if MSI supports per-vector masking
    pub fn has_per_vector_mask(&self) -> bool {
        (self.msg_ctl & MSI_CTL_PER_VECTOR_MASK) != 0
    }

    /// Check if MSI is enabled
    pub fn is_enabled(&self) -> bool {
        (self.msg_ctl & MSI_CTL_ENABLE) != 0
    }

    /// Get the number of enabled vectors
    pub fn num_enabled_vectors(&self) -> usize {
        msi_num_enabled_vectors(self.msg_ctl)
    }

    /// Get the maximum number of supported vectors
    pub fn max_vectors(&self) -> usize {
        msi_max_vectors(self.msg_ctl)
    }

    /// Check if a specific vector is masked (per-vector masking)
    pub fn is_vector_masked(&self, vector: usize) -> bool {
        if !self.has_per_vector_mask() {
            return false;
        }
        (self.mask_bits >> vector) & 0x1 == 0x1
    }

    /// Get the size of this MSI capability in bytes
    pub fn size(&self) -> u64 {
        let mut size: u64 = 0x0A; // Base: ID + Next + MsgCtl + AddrLo + Data = 10 bytes

        if self.is_64bit() {
            size += 0x04; // Add AddrHi = 4 bytes
        }
        if self.has_per_vector_mask() {
            size += 0x08; // Add Mask + Pending = 8 bytes
        }

        size
    }

    /// Get the offset for message data based on address width
    pub fn msg_data_offset(&self) -> u64 {
        if self.is_64bit() {
            MSI_MSG_DATA_64_OFFSET
        } else {
            MSI_MSG_DATA_32_OFFSET
        }
    }

    /// Get the offset for mask bits based on address width
    pub fn mask_offset(&self) -> Option<u64> {
        if !self.has_per_vector_mask() {
            return None;
        }
        Some(if self.is_64bit() {
            MSI_MASK_64_OFFSET
        } else {
            MSI_MASK_32_OFFSET
        })
    }

    /// Update capability from a config space write
    /// Returns true if the enable state changed
    pub fn update(&mut self, offset: u64, data: &[u8]) -> bool {
        let old_enabled = self.is_enabled();

        match data.len() {
            2 => {
                let value = u16::from_le_bytes([data[0], data[1]]);
                match offset {
                    MSI_MSG_CTL_OFFSET => {
                        // Only allow writes to Enable and Multi-Message Enable bits
                        self.msg_ctl = (self.msg_ctl & !(MSI_CTL_ENABLE | MSI_CTL_MULTI_MSG_ENABLE))
                            | (value & (MSI_CTL_ENABLE | MSI_CTL_MULTI_MSG_ENABLE));
                    }
                    x if x == self.msg_data_offset() => {
                        self.msg_data = value;
                    }
                    _ => {
                        log::debug!("MSI: unhandled 2-byte write at offset {:#x}", offset);
                    }
                }
            }
            4 => {
                let value = u32::from_le_bytes([data[0], data[1], data[2], data[3]]);
                match offset {
                    0x00 => {
                        // Combined read of ID/Next/MsgCtl - extract MsgCtl from upper 16 bits
                        let msg_ctl_value = (value >> 16) as u16;
                        self.msg_ctl = (self.msg_ctl & !(MSI_CTL_ENABLE | MSI_CTL_MULTI_MSG_ENABLE))
                            | (msg_ctl_value & (MSI_CTL_ENABLE | MSI_CTL_MULTI_MSG_ENABLE));
                    }
                    MSI_MSG_ADDR_LO_OFFSET => {
                        self.msg_addr_lo = value & MSI_MSG_ADDR_LO_MASK;
                    }
                    MSI_MSG_ADDR_HI_OFFSET if self.is_64bit() => {
                        self.msg_addr_hi = value;
                    }
                    x if x == self.msg_data_offset() => {
                        self.msg_data = value as u16;
                    }
                    x if self.mask_offset() == Some(x) => {
                        self.mask_bits = value;
                    }
                    _ => {
                        log::debug!("MSI: unhandled 4-byte write at offset {:#x}", offset);
                    }
                }
            }
            _ => {
                log::debug!("MSI: unhandled {}-byte write at offset {:#x}", data.len(), offset);
            }
        }

        let new_enabled = self.is_enabled();
        old_enabled != new_enabled
    }

    /// Read from the capability (for emulation)
    pub fn read(&self, offset: u64) -> u32 {
        match offset {
            MSI_MSG_CTL_OFFSET => self.msg_ctl as u32,
            MSI_MSG_ADDR_LO_OFFSET => self.msg_addr_lo,
            MSI_MSG_ADDR_HI_OFFSET if self.is_64bit() => self.msg_addr_hi,
            x if x == self.msg_data_offset() => self.msg_data as u32,
            x if self.mask_offset() == Some(x) => self.mask_bits,
            x if self.mask_offset().map(|m| m + 4) == Some(x) => self.pending_bits,
            _ => 0,
        }
    }
}

/// MSI state for a VFIO device
#[derive(Debug)]
pub struct VfioMsiState {
    /// Capability offset in config space
    pub cap_offset: u16,
    /// MSI capability registers
    pub cap: MsiCap,
}

impl VfioMsiState {
    /// Create a new MSI state from parsed config space
    pub fn new(cap_offset: u16, msg_ctl: u16) -> Self {
        Self {
            cap_offset,
            cap: MsiCap {
                msg_ctl,
                ..Default::default()
            },
        }
    }

    /// Check if a config space offset is within this MSI capability
    pub fn contains_offset(&self, offset: u64) -> bool {
        offset >= self.cap_offset as u64 && offset < self.cap_offset as u64 + self.cap.size()
    }

    /// Get the offset within the MSI capability for a config space offset
    pub fn relative_offset(&self, offset: u64) -> u64 {
        offset - self.cap_offset as u64
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_msi_num_vectors() {
        // msg_ctl bits 6:4 encode log2 of enabled vectors
        assert_eq!(msi_num_enabled_vectors(0x0000), 1); // 0 -> 2^0 = 1
        assert_eq!(msi_num_enabled_vectors(0x0010), 2); // 1 -> 2^1 = 2
        assert_eq!(msi_num_enabled_vectors(0x0020), 4); // 2 -> 2^2 = 4
        assert_eq!(msi_num_enabled_vectors(0x0030), 8); // 3 -> 2^3 = 8
        assert_eq!(msi_num_enabled_vectors(0x0040), 16); // 4 -> 2^4 = 16
        assert_eq!(msi_num_enabled_vectors(0x0050), 32); // 5 -> 2^5 = 32
        assert_eq!(msi_num_enabled_vectors(0x0060), 0); // 6 -> invalid
    }

    #[test]
    fn test_msi_cap_size() {
        // 32-bit, no mask
        let cap = MsiCap { msg_ctl: 0x0000, ..Default::default() };
        assert_eq!(cap.size(), 10);

        // 64-bit, no mask
        let cap = MsiCap { msg_ctl: MSI_CTL_64_BIT_CAPABLE, ..Default::default() };
        assert_eq!(cap.size(), 14);

        // 32-bit, with mask
        let cap = MsiCap { msg_ctl: MSI_CTL_PER_VECTOR_MASK, ..Default::default() };
        assert_eq!(cap.size(), 18);

        // 64-bit, with mask
        let cap = MsiCap { msg_ctl: MSI_CTL_64_BIT_CAPABLE | MSI_CTL_PER_VECTOR_MASK, ..Default::default() };
        assert_eq!(cap.size(), 22);
    }

    #[test]
    fn test_msi_cap_enable() {
        let mut cap = MsiCap::default();
        assert!(!cap.is_enabled());

        // Enable via 2-byte write
        let data = (MSI_CTL_ENABLE).to_le_bytes();
        cap.update(MSI_MSG_CTL_OFFSET, &data);
        assert!(cap.is_enabled());

        // Disable
        let data = 0u16.to_le_bytes();
        cap.update(MSI_MSG_CTL_OFFSET, &data);
        assert!(!cap.is_enabled());
    }

    #[test]
    fn test_msi_cap_address() {
        let mut cap = MsiCap { msg_ctl: MSI_CTL_64_BIT_CAPABLE, ..Default::default() };

        // Set low address
        let data = 0xFEE0_1000u32.to_le_bytes();
        cap.update(MSI_MSG_ADDR_LO_OFFSET, &data);
        assert_eq!(cap.msg_addr_lo, 0xFEE0_1000);

        // Set high address
        let data = 0x0000_0001u32.to_le_bytes();
        cap.update(MSI_MSG_ADDR_HI_OFFSET, &data);
        assert_eq!(cap.msg_addr_hi, 0x0000_0001);
    }

    #[test]
    fn test_msi_state_contains() {
        let state = VfioMsiState::new(0x48, MSI_CTL_64_BIT_CAPABLE);
        
        assert!(state.contains_offset(0x48));
        assert!(state.contains_offset(0x4C));
        assert!(state.contains_offset(0x48 + 13)); // Just inside
        assert!(!state.contains_offset(0x48 + 14)); // Just outside
        assert!(!state.contains_offset(0x40));
    }
}
