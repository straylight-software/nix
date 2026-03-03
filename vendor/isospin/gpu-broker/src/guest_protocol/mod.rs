//! Guest Protocol - Pure, testable core for guest-side NVIDIA device emulation
//!
//! This module contains all the logic for:
//! - Serializing ioctl parameters to wire format
//! - Deserializing responses from the broker
//! - Tracking pending requests and matching responses
//! - Enforcing invariants
//!
//! Design principle: This code is `no_std` compatible so it can be used in:
//! - The kernel module (nvidia-shim.ko)
//! - Userspace tests with full property testing
//!
//! The kernel module becomes a thin wrapper that:
//! 1. Copies data from userspace
//! 2. Calls this library to serialize
//! 3. Writes to shared memory ring
//! 4. Waits for response
//! 5. Calls this library to deserialize
//! 6. Copies data back to userspace

#![cfg_attr(not(feature = "std"), no_std)]

pub mod wire;
pub mod client;
pub mod invariants;

#[cfg(test)]
mod tests;

// Re-export commonly used types
pub use wire::{
    RequestHeader, ResponseHeader, IoctlRequest, IoctlResponse,
    NvEscapeCode, RequestId,
};
pub use client::{ClientState, PendingRequest};
pub use invariants::{Invariant, InvariantError, check_invariants};
