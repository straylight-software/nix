// Copyright 2025 Isospin Authors
// SPDX-License-Identifier: Apache-2.0
//
//! Configuration for VFIO PCI device passthrough.

use serde::{Deserialize, Serialize};

/// Configuration for a VFIO PCI device.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct VfioDeviceConfig {
    /// Unique identifier for the device.
    pub id: String,
    /// PCI address in BDF format (e.g., "0000:01:00.0").
    pub pci_address: String,
    /// Optional NVIDIA GPUDirect clique ID for P2P.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub gpudirect_clique: Option<u8>,
}

/// Errors for VFIO device configuration.
#[derive(Debug, thiserror::Error, displaydoc::Display)]
pub enum VfioConfigError {
    /// Invalid PCI address format: {0}
    InvalidPciAddress(String),
    /// Device not found: {0}
    DeviceNotFound(String),
    /// Device not bound to vfio-pci: {0}
    NotVfioBound(String),
    /// Duplicate device ID: {0}
    DuplicateId(String),
}
