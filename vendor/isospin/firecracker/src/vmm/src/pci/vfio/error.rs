// Copyright 2025 Isospin Authors
// SPDX-License-Identifier: Apache-2.0
//
//! Error types for VFIO device passthrough.

use std::path::PathBuf;

use super::types::{BarIndex, PciBdf, VfioIrqIndex};

/// Errors that can occur during VFIO device setup and operation.
#[derive(Debug, thiserror::Error)]
pub enum VfioError {
    // === Container/Group errors ===
    /// Failed to open VFIO container at /dev/vfio/vfio: {0}
    #[error("Failed to open VFIO container: {0}")]
    ContainerOpen(#[source] std::io::Error),

    /// Failed to check VFIO API version: {0}
    #[error("Failed to check VFIO API version: {0}")]
    GetApiVersion(#[source] std::io::Error),

    /// Unsupported VFIO API version: got {got}, expected {expected}
    #[error("Unsupported VFIO API version: got {got}, expected {expected}")]
    UnsupportedApiVersion { got: i32, expected: i32 },

    /// VFIO TYPE1 IOMMU not supported
    #[error("VFIO TYPE1 IOMMU not supported by kernel")]
    Type1IommuNotSupported,

    /// Failed to set IOMMU type: {0}
    #[error("Failed to set IOMMU type: {0}")]
    SetIommu(#[source] std::io::Error),

    /// Failed to open IOMMU group {group}: {source}
    #[error("Failed to open IOMMU group {group}: {source}")]
    GroupOpen {
        group: u32,
        #[source]
        source: std::io::Error,
    },

    /// IOMMU group {group} is not viable (not all devices bound to vfio-pci)
    #[error("IOMMU group {group} is not viable - ensure all devices are bound to vfio-pci")]
    GroupNotViable { group: u32 },

    /// PCI segment not initialized (VFIO requires PCI to be enabled)
    #[error("PCI segment not initialized - VFIO devices require PCI transport to be enabled")]
    NoPciSegment,

    /// Failed to add group {group} to container: {source}
    #[error("Failed to add group {group} to container: {source}")]
    GroupSetContainer {
        group: u32,
        #[source]
        source: std::io::Error,
    },

    // === Device errors ===
    /// Device {bdf} not found at {path}
    #[error("Device {bdf} not found at {path}")]
    DeviceNotFound { bdf: PciBdf, path: PathBuf },

    /// Device {bdf} not bound to vfio-pci driver
    #[error("Device {bdf} not bound to vfio-pci driver (current: {current_driver:?})")]
    NotVfioBound {
        bdf: PciBdf,
        current_driver: Option<String>,
    },

    /// Failed to get device fd for {bdf}: {source}
    #[error("Failed to get device fd for {bdf}: {source}")]
    GetDeviceFd {
        bdf: PciBdf,
        #[source]
        source: std::io::Error,
    },

    /// Failed to get device info for {bdf}: {source}
    #[error("Failed to get device info for {bdf}: {source}")]
    GetDeviceInfo {
        bdf: PciBdf,
        #[source]
        source: std::io::Error,
    },

    /// Failed to reset device {bdf}: {source}
    #[error("Failed to reset device {bdf}: {source}")]
    DeviceReset {
        bdf: PciBdf,
        #[source]
        source: std::io::Error,
    },

    // === Region/BAR errors ===
    /// Failed to get region info for BAR {bar:?}: {source}
    #[error("Failed to get region info for BAR {bar:?}: {source}")]
    GetRegionInfo {
        bar: BarIndex,
        #[source]
        source: std::io::Error,
    },

    /// Failed to mmap BAR {bar:?}: {source}
    #[error("Failed to mmap BAR {bar:?}: {source}")]
    MmapBar {
        bar: BarIndex,
        #[source]
        source: std::io::Error,
    },

    /// BAR {bar:?} is not mappable (device may require special handling)
    #[error("BAR {bar:?} is not mappable")]
    BarNotMappable { bar: BarIndex },

    /// Failed to allocate guest address for BAR {bar:?} of size {size:#x}
    #[error("Failed to allocate guest address for BAR {bar:?} of size {size:#x}")]
    BarAllocationFailed { bar: BarIndex, size: u64 },

    // === DMA errors ===
    /// Failed to map DMA region at IOVA {iova:#x}, size {size:#x}: {source}
    #[error("Failed to map DMA region at IOVA {iova:#x}, size {size:#x}: {source}")]
    DmaMap {
        iova: u64,
        size: u64,
        #[source]
        source: std::io::Error,
    },

    /// Failed to unmap DMA region at IOVA {iova:#x}, size {size:#x}: {source}
    #[error("Failed to unmap DMA region at IOVA {iova:#x}, size {size:#x}: {source}")]
    DmaUnmap {
        iova: u64,
        size: u64,
        #[source]
        source: std::io::Error,
    },

    /// DMA mapping overlap detected at IOVA {iova:#x}
    #[error("DMA mapping overlap detected at IOVA {iova:#x}")]
    DmaMappingOverlap { iova: u64 },

    // === Interrupt errors ===
    /// Failed to get IRQ info for {irq:?}: {source}
    #[error("Failed to get IRQ info for {irq:?}: {source}")]
    GetIrqInfo {
        irq: VfioIrqIndex,
        #[source]
        source: std::io::Error,
    },

    /// Failed to enable MSI-X: {0}
    #[error("Failed to enable MSI-X: {0}")]
    EnableMsix(#[source] std::io::Error),

    /// Failed to disable MSI-X: {0}
    #[error("Failed to disable MSI-X: {0}")]
    DisableMsix(#[source] std::io::Error),

    /// Failed to setup irqfd for vector {vector}: {source}
    #[error("Failed to setup irqfd for vector {vector}: {source}")]
    IrqfdSetup {
        vector: u32,
        #[source]
        source: std::io::Error,
    },

    /// MSI-X not supported by device
    #[error("MSI-X not supported by device")]
    MsixNotSupported,

    /// Device {bdf} does not have MSI-X capability
    #[error("Device {bdf} does not have MSI-X capability")]
    NoMsixCapability { bdf: PciBdf },

    /// Too many MSI-X vectors requested: {requested} > {max}
    #[error("Too many MSI-X vectors requested: {requested} > {max}")]
    TooManyMsixVectors { requested: usize, max: usize },

    /// Device {bdf} does not have MSI capability
    #[error("Device {bdf} does not have MSI capability")]
    NoMsiCapability { bdf: PciBdf },

    /// Too many MSI vectors requested: {requested} > {max}
    #[error("Too many MSI vectors requested: {requested} > {max}")]
    TooManyMsiVectors { requested: usize, max: usize },

    /// Failed to set IRQs for device {bdf}: {source}
    #[error("Failed to set IRQs for device {bdf}: {source}")]
    SetIrqs {
        bdf: PciBdf,
        #[source]
        source: std::io::Error,
    },

    // === Config space errors ===
    /// Failed to read config space at offset {offset:#x}: {source}
    #[error("Failed to read config space at offset {offset:#x}: {source}")]
    ConfigRead {
        offset: u64,
        #[source]
        source: std::io::Error,
    },

    /// Failed to write config space at offset {offset:#x}: {source}
    #[error("Failed to write config space at offset {offset:#x}: {source}")]
    ConfigWrite {
        offset: u64,
        #[source]
        source: std::io::Error,
    },

    // === KVM integration errors ===
    /// Failed to create KVM memory region: {0}
    #[error("Failed to create KVM memory region: {0}")]
    KvmMemoryRegion(#[source] std::io::Error),

    /// No KVM memory slot available for BAR mapping
    #[error("No KVM memory slot available for BAR {bar:?} mapping (max slots exceeded)")]
    NoKvmSlotAvailable { bar: BarIndex },

    /// Failed to register KVM irqfd: {0}
    #[error("Failed to register KVM irqfd: {0}")]
    KvmIrqfd(#[source] std::io::Error),

    // === IOMMU group errors ===
    /// Failed to read IOMMU group for device: {0}
    #[error("Failed to read IOMMU group: {0}")]
    IommuGroupRead(#[source] std::io::Error),

    /// Device has no IOMMU group (IOMMU not enabled?)
    #[error("Device has no IOMMU group - ensure IOMMU is enabled in BIOS and kernel")]
    NoIommuGroup,

    /// Invalid PCI address format: {0}
    #[error("Invalid PCI address format: {0} (expected format: 0000:01:00.0)")]
    InvalidPciAddress(String),
}

/// Result type for VFIO operations.
pub type VfioResult<T> = Result<T, VfioError>;

/// Errors during IOMMU group verification.
#[derive(Debug, thiserror::Error)]
pub enum IommuGroupError {
    /// Device {bdf} is not bound to vfio-pci
    #[error("Device {bdf} in IOMMU group {group} is not bound to vfio-pci (bound to: {driver:?})")]
    DeviceNotBound {
        bdf: PciBdf,
        group: u32,
        driver: Option<String>,
    },

    /// Failed to read device driver: {0}
    #[error("Failed to read device driver: {0}")]
    ReadDriver(#[source] std::io::Error),

    /// Failed to enumerate group devices: {0}
    #[error("Failed to enumerate group devices: {0}")]
    EnumerateDevices(#[source] std::io::Error),
}
