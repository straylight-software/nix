// Copyright 2025 Isospin Authors
// SPDX-License-Identifier: Apache-2.0
//
//! VFIO PCI device passthrough support for Firecracker.
//!
//! This module implements GPU passthrough using the Linux VFIO framework.
//! Key components:
//! - `VfioPciDevice`: Implements the `PciDevice` trait for passthrough devices
//! - `VfioContainer`: Manages IOMMU domain and DMA mappings
//! - `MsixTable`: MSI-X interrupt state machine
//! - Type-safe address handling to prevent GPA/HVA/IOVA confusion

mod types;
mod error;
mod container;
mod device;
mod msi;
mod msix;
mod mmap;

#[cfg(test)]
mod proptests;

#[cfg(test)]
mod msix_routing_proptests;

pub use types::*;
pub use error::*;
pub use container::*;
pub use device::*;
pub use msix::*;
pub use mmap::*;
