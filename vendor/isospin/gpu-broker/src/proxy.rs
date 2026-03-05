//! GPU Broker Proxy
//!
//! This module connects all the pieces together:
//! - Receives requests from clients (via transport)
//! - Translates handles (virtual → real)
//! - Forwards to driver
//! - Translates results (real → virtual)
//! - Sends responses back
//!
//! ```text
//!   Client Request (virtual handles)
//!          │
//!          ▼
//!   ┌──────────────────┐
//!   │ BrokerProxy      │
//!   │                  │
//!   │ 1. Validate      │
//!   │ 2. Translate     │──▶ HandleTable
//!   │ 3. Call driver   │──▶ Driver
//!   │ 4. Record result │
//!   │ 5. Respond       │
//!   └──────────────────┘
//!          │
//!          ▼
//!   Client Response
//! ```

use std::collections::HashMap;

use crate::broker::{
  BrokerError, BrokerState, Operation, OperationResult, Request, Response, SeqNo,
};
use crate::driver::{Driver, DriverError, MockDriver};
use crate::handle_table::{ClientId, HandleError, HandleTable, RealHandle, VirtualHandle};
use crate::rm_api::{NvHandle, classes};

// =============================================================================
// PROXY ERROR
// =============================================================================

/// Errors that can occur during proxying
#[derive(Debug, Clone)]
pub enum ProxyError {
  /// Client not registered
  ClientNotFound(ClientId),

  /// Handle translation failed
  HandleError(String),

  /// Driver returned error
  DriverError(String),

  /// Quota exceeded
  QuotaExceeded,

  /// Invalid operation
  InvalidOperation(String),
}

impl From<HandleError> for ProxyError {
  fn from(e: HandleError) -> Self {
    ProxyError::HandleError(e.to_string())
  }
}

impl From<DriverError> for ProxyError {
  fn from(e: DriverError) -> Self {
    ProxyError::DriverError(e.to_string())
  }
}

impl From<ProxyError> for BrokerError {
  fn from(e: ProxyError) -> Self {
    match e {
      ProxyError::ClientNotFound(c) => BrokerError::ClientNotFound(c),
      ProxyError::HandleError(s) => BrokerError::Handle(s),
      ProxyError::DriverError(s) => BrokerError::DriverError { status: 0 },
      ProxyError::QuotaExceeded => BrokerError::QuotaExceeded,
      ProxyError::InvalidOperation(s) => BrokerError::InvalidOperation(s),
    }
  }
}

// =============================================================================
// BROKER PROXY
// =============================================================================

/// The main broker proxy that connects everything together
pub struct BrokerProxy<D: Driver> {
  /// Handle table for per-client isolation
  handles: HandleTable,

  /// The real (or mock) driver
  driver: D,

  /// Configuration
  config: ProxyConfig,

  /// Statistics
  stats: ProxyStats,

  /// Per-client root handles (the first handle a client creates)
  client_roots: HashMap<ClientId, RealHandle>,
}

/// Configuration for the proxy
#[derive(Debug, Clone)]
pub struct ProxyConfig {
  /// Maximum handles per client
  pub handle_quota: usize,

  /// Maximum clients
  pub max_clients: usize,

  /// Whether to validate all operations strictly
  pub strict_validation: bool,
}

impl Default for ProxyConfig {
  fn default() -> Self {
    Self {
      handle_quota: 10_000,
      max_clients: 1_000,
      strict_validation: true,
    }
  }
}

/// Statistics about proxy operations
#[derive(Debug, Clone, Default)]
pub struct ProxyStats {
  pub requests_processed: u64,
  pub successful_ops: u64,
  pub failed_ops: u64,
  pub clients_registered: u64,
  pub clients_unregistered: u64,
  pub handles_allocated: u64,
  pub handles_freed: u64,
}

impl<D: Driver> BrokerProxy<D> {
  /// Create a new proxy with the given driver
  pub fn new(driver: D, config: ProxyConfig) -> Self {
    Self {
      handles: HandleTable::new(config.handle_quota),
      driver,
      config,
      stats: ProxyStats::default(),
      client_roots: HashMap::new(),
    }
  }

  /// Process a single request
  pub fn process(&mut self, request: Request) -> Response {
    self.stats.requests_processed += 1;

    let result = self.process_inner(&request);

    match &result {
      Ok(_) => self.stats.successful_ops += 1,
      Err(_) => self.stats.failed_ops += 1,
    }

    Response {
      client: request.client,
      seq: request.seq,
      result: result.map_err(|e| e.into()),
    }
  }

  /// Process a batch of requests
  pub fn process_batch(&mut self, requests: Vec<Request>) -> Vec<Response> {
    requests.into_iter().map(|r| self.process(r)).collect()
  }

  /// Internal processing with typed errors
  fn process_inner(&mut self, request: &Request) -> Result<OperationResult, ProxyError> {
    match &request.op {
      Operation::RegisterClient => self.op_register_client(request.client),
      Operation::UnregisterClient => self.op_unregister_client(request.client),
      Operation::Alloc {
        h_root,
        h_parent,
        h_new,
        class,
      } => self.op_alloc(request.client, *h_root, *h_parent, *h_new, *class),
      Operation::Free { h_root, h_object } => self.op_free(request.client, *h_root, *h_object),
      Operation::Control {
        h_client,
        h_object,
        cmd,
        params,
      } => self.op_control(request.client, *h_client, *h_object, *cmd, params),
      Operation::MapMemory {
        h_client,
        h_device,
        h_memory,
        offset,
        length,
      } => self.op_map_memory(
        request.client,
        *h_client,
        *h_device,
        *h_memory,
        *offset,
        *length,
      ),
      Operation::UnmapMemory {
        h_client,
        h_device,
        h_memory,
      } => self.op_unmap_memory(request.client, *h_client, *h_device, *h_memory),

      // Top-level escapes - proxy directly to driver or return cached info
      Operation::CardInfo => self.op_card_info(),
      Operation::CheckVersion {
        cmd,
        version_string,
      } => self.op_check_version(*cmd, version_string),
      Operation::RegisterFd { ctl_fd } => self.op_register_fd(*ctl_fd),
      Operation::AllocOsEvent {
        h_client,
        h_device,
        fd,
      } => self.op_alloc_os_event(request.client, *h_client, *h_device, *fd),
      Operation::FreeOsEvent {
        h_client,
        h_device,
        fd,
      } => self.op_free_os_event(request.client, *h_client, *h_device, *fd),
      Operation::AttachGpusToFd { gpu_ids } => self.op_attach_gpus_to_fd(gpu_ids),
      Operation::StatusCode {
        domain,
        bus,
        slot,
        function,
      } => self.op_status_code(*domain, *bus, *slot, *function),
    }
  }

  // =========================================================================
  // OPERATION HANDLERS
  // =========================================================================

  fn op_register_client(&mut self, client: ClientId) -> Result<OperationResult, ProxyError> {
    if self.handles.client_ids().len() >= self.config.max_clients {
      return Err(ProxyError::QuotaExceeded);
    }

    self.handles.register_client(client);
    self.stats.clients_registered += 1;

    Ok(OperationResult::Registered)
  }

  fn op_unregister_client(&mut self, client: ClientId) -> Result<OperationResult, ProxyError> {
    // Get all real handles for this client
    let real_handles = self.handles.unregister_client(client);

    // Free them in the driver
    for real_handle in &real_handles {
      // Best effort - continue even if some fail
      let _ = self.driver.free(
        self.client_roots.get(&client).map(|h| h.0).unwrap_or(0),
        0, // Parent doesn't matter for cleanup
        real_handle.0,
      );
    }

    // Remove client's root handle tracking
    self.client_roots.remove(&client);

    self.stats.clients_unregistered += 1;

    Ok(OperationResult::Unregistered {
      freed_handles: real_handles,
    })
  }

  fn op_alloc(
    &mut self,
    client: ClientId,
    h_root: NvHandle,
    h_parent: NvHandle,
    h_new: NvHandle,
    class: u32,
  ) -> Result<OperationResult, ProxyError> {
    // Check if client is registered
    if !self.handles.client_exists(client) {
      return Err(ProxyError::ClientNotFound(client));
    }

    // Translate handles (special case: 0 means "no handle" / root)
    let real_root = if h_root == 0 {
      0
    } else {
      self.handles.translate(client, VirtualHandle(h_root))?.0
    };

    let real_parent = if h_parent == 0 {
      0
    } else {
      self.handles.translate(client, VirtualHandle(h_parent))?.0
    };

    // For root client allocation, the new handle is assigned by driver
    // For other objects, we generate a unique real handle
    let is_root_alloc = class == classes::NV01_ROOT_CLIENT || class == classes::NV01_ROOT;

    // Generate a real handle for the new object
    // In a real system, the driver would assign this
    let real_new = self.generate_real_handle();

    // Call the driver
    let status = self
      .driver
      .alloc(real_root, real_parent, real_new, class, None)?;

    if status != 0 {
      return Err(ProxyError::DriverError(format!(
        "alloc failed: 0x{:08x}",
        status
      )));
    }

    // Record the handle mapping
    self
      .handles
      .insert(client, VirtualHandle(h_new), RealHandle(real_new))?;

    // If this is the root client, record it
    if is_root_alloc {
      self.client_roots.insert(client, RealHandle(real_new));
      self.handles.set_root(client, RealHandle(real_new))?;
    }

    self.stats.handles_allocated += 1;

    Ok(OperationResult::Allocated {
      real_handle: RealHandle(real_new),
    })
  }

  fn op_free(
    &mut self,
    client: ClientId,
    h_root: NvHandle,
    h_object: NvHandle,
  ) -> Result<OperationResult, ProxyError> {
    // Translate handles
    let real_root = if h_root == 0 {
      self.client_roots.get(&client).map(|h| h.0).unwrap_or(0)
    } else {
      self.handles.translate(client, VirtualHandle(h_root))?.0
    };

    let real_object = self.handles.translate(client, VirtualHandle(h_object))?.0;

    // Remove from handle table first
    let real_handle = self.handles.remove(client, VirtualHandle(h_object))?;

    // Call driver to free
    let status = self.driver.free(real_root, 0, real_object)?;

    if status != 0 {
      // Put the handle back if driver failed
      // (in practice we might want different behavior)
      let _ = self
        .handles
        .insert(client, VirtualHandle(h_object), real_handle);
      return Err(ProxyError::DriverError(format!(
        "free failed: 0x{:08x}",
        status
      )));
    }

    self.stats.handles_freed += 1;

    Ok(OperationResult::Freed { real_handle })
  }

  fn op_control(
    &mut self,
    client: ClientId,
    h_client: NvHandle,
    h_object: NvHandle,
    cmd: u32,
    params: &[u8],
  ) -> Result<OperationResult, ProxyError> {
    // Translate handles
    let real_client = self.handles.translate(client, VirtualHandle(h_client))?.0;
    let real_object = self.handles.translate(client, VirtualHandle(h_object))?.0;

    // Copy params for potential modification (handle translation in params)
    let mut params_buf = params.to_vec();

    // TODO: Parse params and translate any embedded handles
    // This is command-specific and complex

    // Call driver
    let status = self
      .driver
      .control(real_client, real_object, cmd, &mut params_buf)?;

    if status != 0 {
      return Err(ProxyError::DriverError(format!(
        "control failed: 0x{:08x}",
        status
      )));
    }

    Ok(OperationResult::ControlComplete {
      status,
      out_params: params_buf,
    })
  }

  fn op_map_memory(
    &mut self,
    client: ClientId,
    h_client: NvHandle,
    h_device: NvHandle,
    h_memory: NvHandle,
    offset: u64,
    length: u64,
  ) -> Result<OperationResult, ProxyError> {
    // Translate all handles
    let real_client = self.handles.translate(client, VirtualHandle(h_client))?.0;
    let real_device = self.handles.translate(client, VirtualHandle(h_device))?.0;
    let real_memory = self.handles.translate(client, VirtualHandle(h_memory))?.0;

    // Call driver
    let (address, status) = self.driver.map_memory(
      real_client,
      real_device,
      real_memory,
      offset,
      length,
      0, // flags
    )?;

    if status != 0 {
      return Err(ProxyError::DriverError(format!(
        "map_memory failed: 0x{:08x}",
        status
      )));
    }

    Ok(OperationResult::Mapped { address })
  }

  fn op_unmap_memory(
    &mut self,
    client: ClientId,
    h_client: NvHandle,
    h_device: NvHandle,
    h_memory: NvHandle,
  ) -> Result<OperationResult, ProxyError> {
    // Translate all handles
    let real_client = self.handles.translate(client, VirtualHandle(h_client))?.0;
    let real_device = self.handles.translate(client, VirtualHandle(h_device))?.0;
    let real_memory = self.handles.translate(client, VirtualHandle(h_memory))?.0;

    // Call driver
    let status = self.driver.unmap_memory(
      real_client,
      real_device,
      real_memory,
      0, // address - would need to track this
      0, // flags
    )?;

    if status != 0 {
      return Err(ProxyError::DriverError(format!(
        "unmap_memory failed: 0x{:08x}",
        status
      )));
    }

    Ok(OperationResult::Unmapped)
  }

  // =========================================================================
  // TOP-LEVEL ESCAPE HANDLERS
  // =========================================================================

  fn op_card_info(&self) -> Result<OperationResult, ProxyError> {
    use crate::rm_api::NvCardInfo;

    let mut cards = [NvCardInfo::default()];
    let count = self.driver.card_info(&mut cards)?;

    // Serialize the card info
    let card_bytes = if count > 0 {
      unsafe {
        std::slice::from_raw_parts(
          cards.as_ptr() as *const u8,
          std::mem::size_of::<NvCardInfo>() * count,
        )
      }
      .to_vec()
    } else {
      Vec::new()
    };

    Ok(OperationResult::CardInfoResult {
      num_gpus: count as u32,
      card_info: card_bytes,
    })
  }

  fn op_check_version(
    &self,
    cmd: u32,
    client_version: &[u8],
  ) -> Result<OperationResult, ProxyError> {
    use crate::rm_api::{NV_RM_API_VERSION_STRING_LENGTH, NvRmApiVersion};

    let mut version = NvRmApiVersion::default();
    version.cmd = cmd;

    // Copy client version string
    let copy_len = client_version.len().min(NV_RM_API_VERSION_STRING_LENGTH);
    version.version_string[..copy_len].copy_from_slice(&client_version[..copy_len]);

    self.driver.check_version(&mut version)?;

    Ok(OperationResult::VersionCheckResult {
      reply: version.reply,
      version_string: version.version_string.to_vec(),
    })
  }

  fn op_register_fd(&self, _ctl_fd: i32) -> Result<OperationResult, ProxyError> {
    // In proxy model, we don't need to register fds - just acknowledge
    Ok(OperationResult::FdRegistered)
  }

  fn op_alloc_os_event(
    &mut self,
    client: ClientId,
    h_client: NvHandle,
    h_device: NvHandle,
    _fd: u32,
  ) -> Result<OperationResult, ProxyError> {
    // Validate handles if present
    if h_client != 0 {
      let _ = self.handles.translate(client, VirtualHandle(h_client))?;
    }
    if h_device != 0 {
      let _ = self.handles.translate(client, VirtualHandle(h_device))?;
    }

    // In real impl: actually allocate eventfd and wire it up
    Ok(OperationResult::OsEventAllocated { status: 0 })
  }

  fn op_free_os_event(
    &mut self,
    client: ClientId,
    h_client: NvHandle,
    h_device: NvHandle,
    _fd: u32,
  ) -> Result<OperationResult, ProxyError> {
    // Validate handles if present
    if h_client != 0 {
      let _ = self.handles.translate(client, VirtualHandle(h_client))?;
    }
    if h_device != 0 {
      let _ = self.handles.translate(client, VirtualHandle(h_device))?;
    }

    Ok(OperationResult::OsEventFreed { status: 0 })
  }

  fn op_attach_gpus_to_fd(&self, _gpu_ids: &[u32]) -> Result<OperationResult, ProxyError> {
    // In proxy model, GPU access is managed by the proxy itself
    Ok(OperationResult::GpusAttached)
  }

  fn op_status_code(
    &self,
    _domain: u32,
    _bus: u8,
    _slot: u8,
    _function: u8,
  ) -> Result<OperationResult, ProxyError> {
    // Return success status
    Ok(OperationResult::StatusCodeResult { status: 0 })
  }

  // =========================================================================
  // HELPERS
  // =========================================================================

  /// Generate a unique real handle
  /// In production, the driver assigns handles, but for mock testing
  /// we need to generate them ourselves
  fn generate_real_handle(&self) -> NvHandle {
    // Use a simple counter based on stats
    // Real implementation would track this properly
    (0x8000_0000 + self.stats.handles_allocated as u32 + 1)
  }

  /// Get current statistics
  pub fn stats(&self) -> &ProxyStats {
    &self.stats
  }

  /// Check if a client is registered
  pub fn is_client_registered(&self, client: ClientId) -> bool {
    self.handles.client_exists(client)
  }

  /// Get handle count for a client
  pub fn client_handle_count(&self, client: ClientId) -> Result<usize, ProxyError> {
    self
      .handles
      .client_handle_count(client)
      .map_err(|e| ProxyError::HandleError(e.to_string()))
  }
}

// =============================================================================
// CONVENIENCE CONSTRUCTOR
// =============================================================================

impl BrokerProxy<MockDriver> {
  /// Create a proxy with a mock driver (for testing)
  pub fn with_mock_driver(config: ProxyConfig) -> Self {
    Self::new(MockDriver::new(), config)
  }
}

// =============================================================================
// TESTS
// =============================================================================

#[cfg(test)]
mod tests {
  use super::*;

  fn make_proxy() -> BrokerProxy<MockDriver> {
    BrokerProxy::with_mock_driver(ProxyConfig::default())
  }

  #[test]
  fn test_register_client() {
    let mut proxy = make_proxy();
    let client = ClientId(1);

    let req = Request {
      client,
      seq: SeqNo(0),
      op: Operation::RegisterClient,
    };

    let resp = proxy.process(req);
    assert!(resp.result.is_ok());
    assert!(proxy.is_client_registered(client));
  }

  #[test]
  fn test_alloc_root_client() {
    let mut proxy = make_proxy();
    let client = ClientId(1);

    // Register
    proxy.process(Request {
      client,
      seq: SeqNo(0),
      op: Operation::RegisterClient,
    });

    // Alloc root
    let resp = proxy.process(Request {
      client,
      seq: SeqNo(1),
      op: Operation::Alloc {
        h_root: 0,
        h_parent: 0,
        h_new: 1,
        class: classes::NV01_ROOT_CLIENT,
      },
    });

    assert!(resp.result.is_ok());
    assert_eq!(proxy.client_handle_count(client).unwrap(), 1);
  }

  #[test]
  fn test_full_sequence() {
    let mut proxy = make_proxy();
    let client = ClientId(1);

    // 1. Register
    let resp = proxy.process(Request {
      client,
      seq: SeqNo(0),
      op: Operation::RegisterClient,
    });
    assert!(resp.result.is_ok());

    // 2. Alloc root client
    let resp = proxy.process(Request {
      client,
      seq: SeqNo(1),
      op: Operation::Alloc {
        h_root: 0,
        h_parent: 0,
        h_new: 1,
        class: classes::NV01_ROOT_CLIENT,
      },
    });
    assert!(resp.result.is_ok());

    // 3. Alloc device
    let resp = proxy.process(Request {
      client,
      seq: SeqNo(2),
      op: Operation::Alloc {
        h_root: 1,
        h_parent: 1,
        h_new: 2,
        class: classes::NV01_DEVICE_0,
      },
    });
    assert!(resp.result.is_ok());

    // 4. Free device
    let resp = proxy.process(Request {
      client,
      seq: SeqNo(3),
      op: Operation::Free {
        h_root: 1,
        h_object: 2,
      },
    });
    assert!(resp.result.is_ok());

    // 5. Unregister
    let resp = proxy.process(Request {
      client,
      seq: SeqNo(4),
      op: Operation::UnregisterClient,
    });
    assert!(resp.result.is_ok());

    // Client should no longer exist
    assert!(!proxy.is_client_registered(client));
  }

  #[test]
  fn test_unregistered_client_fails() {
    let mut proxy = make_proxy();
    let client = ClientId(999);

    let resp = proxy.process(Request {
      client,
      seq: SeqNo(0),
      op: Operation::Alloc {
        h_root: 0,
        h_parent: 0,
        h_new: 1,
        class: classes::NV01_ROOT_CLIENT,
      },
    });

    assert!(resp.result.is_err());
  }

  #[test]
  fn test_client_isolation() {
    let mut proxy = make_proxy();
    let client1 = ClientId(1);
    let client2 = ClientId(2);

    // Register both
    proxy.process(Request {
      client: client1,
      seq: SeqNo(0),
      op: Operation::RegisterClient,
    });
    proxy.process(Request {
      client: client2,
      seq: SeqNo(0),
      op: Operation::RegisterClient,
    });

    // Both create handle "1"
    proxy.process(Request {
      client: client1,
      seq: SeqNo(1),
      op: Operation::Alloc {
        h_root: 0,
        h_parent: 0,
        h_new: 1,
        class: classes::NV01_ROOT_CLIENT,
      },
    });
    proxy.process(Request {
      client: client2,
      seq: SeqNo(1),
      op: Operation::Alloc {
        h_root: 0,
        h_parent: 0,
        h_new: 1,
        class: classes::NV01_ROOT_CLIENT,
      },
    });

    // Both should have exactly one handle
    assert_eq!(proxy.client_handle_count(client1).unwrap(), 1);
    assert_eq!(proxy.client_handle_count(client2).unwrap(), 1);

    // Client 1 frees their handle
    proxy.process(Request {
      client: client1,
      seq: SeqNo(2),
      op: Operation::Free {
        h_root: 1,
        h_object: 1,
      },
    });

    // Client 1 should have 0, client 2 still has 1
    assert_eq!(proxy.client_handle_count(client1).unwrap(), 0);
    assert_eq!(proxy.client_handle_count(client2).unwrap(), 1);
  }

  #[test]
  fn test_stats() {
    let mut proxy = make_proxy();
    let client = ClientId(1);

    proxy.process(Request {
      client,
      seq: SeqNo(0),
      op: Operation::RegisterClient,
    });

    proxy.process(Request {
      client,
      seq: SeqNo(1),
      op: Operation::Alloc {
        h_root: 0,
        h_parent: 0,
        h_new: 1,
        class: classes::NV01_ROOT_CLIENT,
      },
    });

    let stats = proxy.stats();
    assert_eq!(stats.requests_processed, 2);
    assert_eq!(stats.clients_registered, 1);
    assert_eq!(stats.handles_allocated, 1);
  }
}

#[cfg(test)]
mod proptests {
  use super::*;
  use proptest::prelude::*;

  proptest! {
      /// Property: Register then alloc always succeeds for valid handles
      #[test]
      fn register_alloc_succeeds(
          client_id in 1u64..1000,
          h_new in 1u32..10000
      ) {
          let mut proxy = make_proxy();
          let client = ClientId(client_id);

          let r1 = proxy.process(Request {
              client,
              seq: SeqNo(0),
              op: Operation::RegisterClient,
          });
          prop_assert!(r1.result.is_ok());

          let r2 = proxy.process(Request {
              client,
              seq: SeqNo(1),
              op: Operation::Alloc {
                  h_root: 0,
                  h_parent: 0,
                  h_new,
                  class: classes::NV01_ROOT_CLIENT,
              },
          });
          prop_assert!(r2.result.is_ok());
      }

      /// Property: Stats are always consistent
      #[test]
      fn stats_consistent(
          num_clients in 1usize..10,
          allocs_per_client in 1usize..10
      ) {
          let mut proxy = make_proxy();

          for i in 0..num_clients {
              let client = ClientId(i as u64 + 1);
              proxy.process(Request {
                  client,
                  seq: SeqNo(0),
                  op: Operation::RegisterClient,
              });

              for j in 0..allocs_per_client {
                  proxy.process(Request {
                      client,
                      seq: SeqNo((j + 1) as u64),
                      op: Operation::Alloc {
                          h_root: 0,
                          h_parent: 0,
                          h_new: (j + 1) as u32,
                          class: classes::NV01_ROOT_CLIENT,
                      },
                  });
              }
          }

          let stats = proxy.stats();
          let expected_requests = num_clients + num_clients * allocs_per_client;
          prop_assert_eq!(stats.requests_processed as usize, expected_requests);
          prop_assert_eq!(stats.clients_registered as usize, num_clients);
      }

      /// Property: Unregister frees all handles
      #[test]
      fn unregister_cleans_up(
          num_handles in 1usize..20
      ) {
          let mut proxy = make_proxy();
          let client = ClientId(1);

          proxy.process(Request {
              client,
              seq: SeqNo(0),
              op: Operation::RegisterClient,
          });

          for i in 0..num_handles {
              proxy.process(Request {
                  client,
                  seq: SeqNo((i + 1) as u64),
                  op: Operation::Alloc {
                      h_root: 0,
                      h_parent: 0,
                      h_new: (i + 1) as u32,
                      class: if i == 0 { classes::NV01_ROOT_CLIENT } else { classes::NV01_DEVICE_0 },
                  },
              });
          }

          prop_assert_eq!(proxy.client_handle_count(client).unwrap(), num_handles);

          proxy.process(Request {
              client,
              seq: SeqNo(100),
              op: Operation::UnregisterClient,
          });

          prop_assert!(!proxy.is_client_registered(client));
      }
  }

  fn make_proxy() -> BrokerProxy<MockDriver> {
    BrokerProxy::with_mock_driver(ProxyConfig::default())
  }
}
