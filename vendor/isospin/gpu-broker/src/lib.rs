//! GPU Broker Library
//!
//! Multiplexes NVIDIA RM API calls from multiple VMs to a single GPU.
//!
//! # Architecture
//!
//! ```text
//!   VM 1 ──┐
//!   VM 2 ──┼──▶ GPU Broker ──▶ /dev/nvidiactl ──▶ Real GPU
//!   VM N ──┘      │
//!                 ├── Handle isolation (per-client namespace)
//!                 ├── Quota enforcement
//!                 └── Pure functional core (deterministic replay)
//! ```
//!
//! # Core Design Principle
//!
//! The broker is structured as a pure state machine:
//!
//! ```text
//!   (State, Input) → (State', Output)
//! ```
//!
//! This gives us:
//! - Deterministic replay from any input sequence
//! - Time-travel debugging (step forward/backward through states)
//! - Exhaustive property testing (run millions of frames in seconds)
//! - The simulation IS the test
//!
//! # Modules
//!
//! - [`broker`]: Pure functional state machine (core logic)
//! - [`handle_table`]: Client handle isolation and translation
//! - [`rm_api`]: NVIDIA Resource Manager API definitions
//! - [`driver`]: NVIDIA driver proxy (real + mock)
//! - [`proxy`]: BrokerProxy coordination layer
//! - [`transport`]: Transport trait and simulated transport
//! - [`uring`]: io_uring event loop wrapper
//! - [`uring_transport`]: Shared memory + io_uring transport
//! - [`fd_passing`]: Unix SCM_RIGHTS utilities
//! - [`guest_shim`]: FUSE/CUSE device emulation for guests
//!
//! # Modules requiring `tracing` feature
//!
//! These modules are only available when the `tracing` feature is enabled:
//! - [`server`]: Unix socket server
//! - [`vsock`]: VM vsock server
//! - [`nvml_server`]: nvidia-smi compatibility shim

pub mod broker;
pub mod driver;
pub mod fd_passing;
pub mod guest_shim;
pub mod handle_table;
pub mod proxy;
pub mod rm_api;
pub mod transport;
pub mod uring;
pub mod uring_transport;

// These modules require the `tracing` crate which is not yet in third-party.
// Enable with `features = ["tracing"]` once tracing is added.
#[cfg(feature = "tracing")]
pub mod nvml_server;
#[cfg(feature = "tracing")]
pub mod server;
#[cfg(feature = "tracing")]
pub mod vsock;

// Re-export commonly used types
pub use broker::{BrokerState, Operation, Request, Response, SeqNo};
pub use handle_table::{ClientId, HandleTable, RealHandle, VirtualHandle};
pub use proxy::{BrokerProxy, ProxyConfig};

#[cfg(feature = "tracing")]
pub use server::{BrokerServer, ServerConfig};
#[cfg(feature = "tracing")]
pub use vsock::{VsockServer, VsockServerConfig};
