//! GPU Broker - Pure functional core
//!
//! The broker is structured as a pure state machine:
//!
//!   (State, Input) → (State', Output)
//!
//! This gives us:
//! - Deterministic replay from any input sequence
//! - Time-travel debugging (step forward/backward through states)
//! - Exhaustive property testing (run millions of frames in seconds)
//! - The simulation IS the test
//!
//! No callbacks. No threads mutating state. No hidden control flow.
//! Just data in, data out.

use crate::handle_table::{ClientId, HandleError, HandleTable, RealHandle, VirtualHandle};
use crate::rm_api::{NvEscape, NvHandle};

/// Monotonic sequence number for ordering
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
pub struct SeqNo(pub u64);

impl SeqNo {
    pub fn next(self) -> Self {
        SeqNo(self.0 + 1)
    }
}

/// A request from a client
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Request {
    /// Which client sent this
    pub client: ClientId,
    /// Sequence number for ordering/dedup
    pub seq: SeqNo,
    /// The operation requested
    pub op: Operation,
}

/// Operations the broker can perform
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Operation {
    /// Register a new client
    RegisterClient,

    /// Unregister a client (cleanup all handles)
    UnregisterClient,

    /// Allocate a GPU object
    Alloc {
        h_root: NvHandle,
        h_parent: NvHandle,
        h_new: NvHandle,
        class: u32,
    },

    /// Free a GPU object
    Free {
        h_root: NvHandle,
        h_object: NvHandle,
    },

    /// Control command
    Control {
        h_client: NvHandle,
        h_object: NvHandle,
        cmd: u32,
        params: Vec<u8>,
    },

    /// Map memory
    MapMemory {
        h_client: NvHandle,
        h_device: NvHandle,
        h_memory: NvHandle,
        offset: u64,
        length: u64,
    },

    /// Unmap memory
    UnmapMemory {
        h_client: NvHandle,
        h_device: NvHandle,
        h_memory: NvHandle,
    },

    // =========================================================================
    // TOP-LEVEL ESCAPES (200+)
    // These don't go through the RM object model
    // =========================================================================

    /// Get card info (NV_ESC_CARD_INFO = 200)
    /// Returns information about installed GPUs
    CardInfo,

    /// Check version string (NV_ESC_CHECK_VERSION_STR = 210)
    /// Validates driver version compatibility
    CheckVersion {
        cmd: u32,
        version_string: Vec<u8>,
    },

    /// Register fd (NV_ESC_REGISTER_FD = 201)
    /// Registers a control fd with the driver
    RegisterFd {
        ctl_fd: i32,
    },

    /// Allocate OS event (NV_ESC_ALLOC_OS_EVENT = 206)
    AllocOsEvent {
        h_client: NvHandle,
        h_device: NvHandle,
        fd: u32,
    },

    /// Free OS event (NV_ESC_FREE_OS_EVENT = 207)
    FreeOsEvent {
        h_client: NvHandle,
        h_device: NvHandle,
        fd: u32,
    },

    /// Attach GPUs to fd (NV_ESC_ATTACH_GPUS_TO_FD = 212)
    AttachGpusToFd {
        gpu_ids: Vec<u32>,
    },

    /// Get status code (NV_ESC_STATUS_CODE = 209)
    StatusCode {
        domain: u32,
        bus: u8,
        slot: u8,
        function: u8,
    },
}

/// Response to a client
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Response {
    /// Which client this is for
    pub client: ClientId,
    /// Sequence number (matches request)
    pub seq: SeqNo,
    /// The result
    pub result: Result<OperationResult, BrokerError>,
}

/// Successful operation results
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum OperationResult {
    /// Client registered successfully
    Registered,

    /// Client unregistered, returns freed real handles
    Unregistered { freed_handles: Vec<RealHandle> },

    /// Object allocated
    Allocated { real_handle: RealHandle },

    /// Object freed
    Freed { real_handle: RealHandle },

    /// Control command completed
    ControlComplete { status: u32, out_params: Vec<u8> },

    /// Memory mapped
    Mapped { address: u64 },

    /// Memory unmapped
    Unmapped,

    // =========================================================================
    // TOP-LEVEL ESCAPE RESULTS
    // =========================================================================

    /// Card info retrieved
    /// Contains serialized NvCardInfo array
    CardInfoResult {
        num_gpus: u32,
        card_info: Vec<u8>,
    },

    /// Version check completed
    VersionCheckResult {
        reply: u32,
        version_string: Vec<u8>,
    },

    /// Fd registered successfully
    FdRegistered,

    /// OS event allocated
    OsEventAllocated { status: u32 },

    /// OS event freed
    OsEventFreed { status: u32 },

    /// GPUs attached to fd
    GpusAttached,

    /// Status code returned
    StatusCodeResult { status: u32 },
}

/// Errors that can occur
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum BrokerError {
    /// Client not found
    ClientNotFound(ClientId),

    /// Handle error
    Handle(String), // Flattened from HandleError for Clone + Eq

    /// Invalid operation
    InvalidOperation(String),

    /// Driver error (from real NVIDIA driver)
    DriverError { status: u32 },

    /// Quota exceeded
    QuotaExceeded,
}

impl From<HandleError> for BrokerError {
    fn from(e: HandleError) -> Self {
        BrokerError::Handle(e.to_string())
    }
}

/// The complete broker state - explicit, serializable, diffable
#[derive(Debug, Clone)]
pub struct BrokerState {
    /// Handle table (per-client isolation)
    pub handles: HandleTable,

    /// Next real handle to allocate (in real impl, comes from driver)
    pub next_real_handle: u32,

    /// Sequence number for responses
    pub seq: SeqNo,

    /// Total requests processed (for stats)
    pub requests_processed: u64,

    /// Configuration
    pub config: BrokerConfig,
}

/// Broker configuration
#[derive(Debug, Clone)]
pub struct BrokerConfig {
    /// Maximum handles per client
    pub handle_quota: usize,

    /// Maximum clients
    pub max_clients: usize,
}

impl Default for BrokerConfig {
    fn default() -> Self {
        Self {
            handle_quota: 10_000,
            max_clients: 1_000,
        }
    }
}

impl BrokerState {
    /// Create a new broker with default config
    pub fn new() -> Self {
        Self::with_config(BrokerConfig::default())
    }

    /// Create a new broker with custom config
    pub fn with_config(config: BrokerConfig) -> Self {
        Self {
            handles: HandleTable::new(config.handle_quota),
            next_real_handle: 0x1000, // Start above zero for clarity
            seq: SeqNo(0),
            requests_processed: 0,
            config,
        }
    }

    /// The core state transition function.
    ///
    /// Pure function: (State, Input) → (State', Output)
    ///
    /// This is the ONLY way state changes. No side effects.
    /// The returned state is entirely new (CoW in practice).
    pub fn tick(mut self, request: Request) -> (Self, Response) {
        let result = self.process_operation(&request);

        let response = Response {
            client: request.client,
            seq: request.seq,
            result,
        };

        self.requests_processed += 1;
        self.seq = self.seq.next();

        (self, response)
    }

    /// Process a batch of requests (multiple ticks)
    ///
    /// Pure function: (State, [Input]) → (State', [Output])
    pub fn tick_batch(self, requests: Vec<Request>) -> (Self, Vec<Response>) {
        let mut state = self;
        let mut responses = Vec::with_capacity(requests.len());

        for request in requests {
            let (new_state, response) = state.tick(request);
            state = new_state;
            responses.push(response);
        }

        (state, responses)
    }

    /// Process a single operation (internal, modifies self)
    fn process_operation(&mut self, request: &Request) -> Result<OperationResult, BrokerError> {
        match &request.op {
            Operation::RegisterClient => self.op_register_client(request.client),

            Operation::UnregisterClient => self.op_unregister_client(request.client),

            Operation::Alloc {
                h_root,
                h_parent,
                h_new,
                class,
            } => self.op_alloc(request.client, *h_root, *h_parent, *h_new, *class),

            Operation::Free { h_root, h_object } => {
                self.op_free(request.client, *h_root, *h_object)
            }

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

            // Top-level escapes
            Operation::CardInfo => self.op_card_info(),

            Operation::CheckVersion { cmd, version_string } => {
                self.op_check_version(*cmd, version_string)
            }

            Operation::RegisterFd { ctl_fd } => self.op_register_fd(*ctl_fd),

            Operation::AllocOsEvent { h_client, h_device, fd } => {
                self.op_alloc_os_event(request.client, *h_client, *h_device, *fd)
            }

            Operation::FreeOsEvent { h_client, h_device, fd } => {
                self.op_free_os_event(request.client, *h_client, *h_device, *fd)
            }

            Operation::AttachGpusToFd { gpu_ids } => self.op_attach_gpus_to_fd(gpu_ids),

            Operation::StatusCode { domain, bus, slot, function } => {
                self.op_status_code(*domain, *bus, *slot, *function)
            }
        }
    }

    fn op_register_client(&mut self, client: ClientId) -> Result<OperationResult, BrokerError> {
        if self.handles.client_ids().len() >= self.config.max_clients {
            return Err(BrokerError::QuotaExceeded);
        }

        self.handles.register_client(client);
        Ok(OperationResult::Registered)
    }

    fn op_unregister_client(&mut self, client: ClientId) -> Result<OperationResult, BrokerError> {
        let mut freed = self.handles.unregister_client(client);
        // Sort for deterministic ordering (HashMap iteration is non-deterministic)
        freed.sort_by_key(|h| h.0);
        Ok(OperationResult::Unregistered {
            freed_handles: freed,
        })
    }

    fn op_alloc(
        &mut self,
        client: ClientId,
        _h_root: NvHandle,
        _h_parent: NvHandle,
        h_new: NvHandle,
        _class: u32,
    ) -> Result<OperationResult, BrokerError> {
        // Allocate a real handle (in real impl, call driver)
        let real_handle = RealHandle(self.next_real_handle);
        self.next_real_handle += 1;

        // Record the mapping
        self.handles
            .insert(client, VirtualHandle(h_new), real_handle)?;

        Ok(OperationResult::Allocated { real_handle })
    }

    fn op_free(
        &mut self,
        client: ClientId,
        _h_root: NvHandle,
        h_object: NvHandle,
    ) -> Result<OperationResult, BrokerError> {
        let real_handle = self.handles.remove(client, VirtualHandle(h_object))?;

        // In real impl: call driver to free

        Ok(OperationResult::Freed { real_handle })
    }

    fn op_control(
        &mut self,
        client: ClientId,
        h_client: NvHandle,
        h_object: NvHandle,
        cmd: u32,
        _params: &[u8],
    ) -> Result<OperationResult, BrokerError> {
        // Validate handles exist
        let _real_client = self.handles.translate(client, VirtualHandle(h_client))?;
        let _real_object = self.handles.translate(client, VirtualHandle(h_object))?;

        // In real impl: translate params, call driver, translate response

        Ok(OperationResult::ControlComplete {
            status: 0,
            out_params: vec![],
        })
    }

    fn op_map_memory(
        &mut self,
        client: ClientId,
        h_client: NvHandle,
        h_device: NvHandle,
        h_memory: NvHandle,
        _offset: u64,
        _length: u64,
    ) -> Result<OperationResult, BrokerError> {
        // Validate handles
        let _real_client = self.handles.translate(client, VirtualHandle(h_client))?;
        let _real_device = self.handles.translate(client, VirtualHandle(h_device))?;
        let _real_memory = self.handles.translate(client, VirtualHandle(h_memory))?;

        // In real impl: call driver, get mapped address

        Ok(OperationResult::Mapped {
            address: 0xDEAD_BEEF_0000,
        })
    }

    fn op_unmap_memory(
        &mut self,
        client: ClientId,
        h_client: NvHandle,
        h_device: NvHandle,
        h_memory: NvHandle,
    ) -> Result<OperationResult, BrokerError> {
        // Validate handles
        let _real_client = self.handles.translate(client, VirtualHandle(h_client))?;
        let _real_device = self.handles.translate(client, VirtualHandle(h_device))?;
        let _real_memory = self.handles.translate(client, VirtualHandle(h_memory))?;

        // In real impl: call driver

        Ok(OperationResult::Unmapped)
    }

    // =========================================================================
    // TOP-LEVEL ESCAPE OPERATIONS
    // =========================================================================

    fn op_card_info(&self) -> Result<OperationResult, BrokerError> {
        // Return simulated card info
        // In real impl, this would come from the driver or cached at startup
        //
        // For now, return a single simulated GPU matching our MockDriver config
        use crate::rm_api::{NvCardInfo, NvPciInfo};
        
        let card = NvCardInfo {
            valid: 1,
            pci_info: NvPciInfo {
                domain: 0,
                bus: 1,
                slot: 0,
                function: 0,
                _pad0: 0,
                vendor_id: 0x10DE,
                device_id: 0x2900, // Blackwell
            },
            gpu_id: 0,
            interrupt_line: 16,
            _pad0: [0; 2],
            reg_address: 0xFD00_0000,
            reg_size: 32 * 1024 * 1024,
            fb_address: 0xE000_0000_0000,
            fb_size: 128 * 1024 * 1024 * 1024, // 128GB
            minor_number: 0,
            dev_name: *b"nvidia0\0\0\0",
            _pad1: [0; 2],
        };

        // Serialize the card info
        let card_bytes = unsafe {
            std::slice::from_raw_parts(
                &card as *const NvCardInfo as *const u8,
                std::mem::size_of::<NvCardInfo>(),
            )
        };

        Ok(OperationResult::CardInfoResult {
            num_gpus: 1,
            card_info: card_bytes.to_vec(),
        })
    }

    fn op_check_version(
        &self,
        cmd: u32,
        client_version: &[u8],
    ) -> Result<OperationResult, BrokerError> {
        use crate::rm_api::{
            NV_RM_API_VERSION_CMD_QUERY, NV_RM_API_VERSION_REPLY_RECOGNIZED,
        };
        
        // Our broker's version string
        let broker_version = b"570.00.00";
        
        let (reply, version_string) = match cmd {
            NV_RM_API_VERSION_CMD_QUERY => {
                // Query mode: just return our version
                (NV_RM_API_VERSION_REPLY_RECOGNIZED, broker_version.to_vec())
            }
            _ => {
                // Strict or relaxed mode: check compatibility
                // For now, always say recognized since we're virtualizing
                (NV_RM_API_VERSION_REPLY_RECOGNIZED, client_version.to_vec())
            }
        };

        Ok(OperationResult::VersionCheckResult {
            reply,
            version_string,
        })
    }

    fn op_register_fd(&self, _ctl_fd: i32) -> Result<OperationResult, BrokerError> {
        // In the broker model, we don't actually register fds - we're proxying
        // Just acknowledge success
        Ok(OperationResult::FdRegistered)
    }

    fn op_alloc_os_event(
        &mut self,
        client: ClientId,
        h_client: NvHandle,
        h_device: NvHandle,
        _fd: u32,
    ) -> Result<OperationResult, BrokerError> {
        // Validate handles exist
        let _real_client = if h_client != 0 {
            self.handles.translate(client, VirtualHandle(h_client))?
        } else {
            RealHandle(0)
        };
        let _real_device = if h_device != 0 {
            self.handles.translate(client, VirtualHandle(h_device))?
        } else {
            RealHandle(0)
        };

        // In real impl: allocate eventfd and associate with client
        // For now, just return success
        Ok(OperationResult::OsEventAllocated { status: 0 })
    }

    fn op_free_os_event(
        &mut self,
        client: ClientId,
        h_client: NvHandle,
        h_device: NvHandle,
        _fd: u32,
    ) -> Result<OperationResult, BrokerError> {
        // Validate handles exist
        let _real_client = if h_client != 0 {
            self.handles.translate(client, VirtualHandle(h_client))?
        } else {
            RealHandle(0)
        };
        let _real_device = if h_device != 0 {
            self.handles.translate(client, VirtualHandle(h_device))?
        } else {
            RealHandle(0)
        };

        // In real impl: free the eventfd association
        Ok(OperationResult::OsEventFreed { status: 0 })
    }

    fn op_attach_gpus_to_fd(&self, _gpu_ids: &[u32]) -> Result<OperationResult, BrokerError> {
        // In the broker model, we manage GPU access ourselves
        // Just acknowledge success
        Ok(OperationResult::GpusAttached)
    }

    fn op_status_code(
        &self,
        _domain: u32,
        _bus: u8,
        _slot: u8,
        _function: u8,
    ) -> Result<OperationResult, BrokerError> {
        // Return success status for the GPU
        // In real impl: query actual driver status
        Ok(OperationResult::StatusCodeResult { status: 0 })
    }

    /// Check all invariants. Call after every tick in tests.
    pub fn check_invariants(&self) -> Result<(), String> {
        self.handles.check_invariants()?;

        // Additional broker-level invariants
        if self.handles.client_ids().len() > self.config.max_clients {
            return Err(format!(
                "Too many clients: {} > {}",
                self.handles.client_ids().len(),
                self.config.max_clients
            ));
        }

        Ok(())
    }
}

impl Default for BrokerState {
    fn default() -> Self {
        Self::new()
    }
}

// =============================================================================
// REPLAY SUPPORT
// =============================================================================

/// A recorded session that can be replayed
#[derive(Debug, Clone)]
pub struct Recording {
    /// Initial state
    pub initial_state: BrokerState,
    /// All requests in order
    pub requests: Vec<Request>,
}

impl Recording {
    /// Create a new recording starting from a state
    pub fn new(initial_state: BrokerState) -> Self {
        Self {
            initial_state,
            requests: Vec::new(),
        }
    }

    /// Add a request to the recording
    pub fn record(&mut self, request: Request) {
        self.requests.push(request);
    }

    /// Replay the entire recording, returning final state and all responses
    pub fn replay(&self) -> (BrokerState, Vec<Response>) {
        self.initial_state.clone().tick_batch(self.requests.clone())
    }

    /// Replay up to a specific request index
    pub fn replay_to(&self, index: usize) -> (BrokerState, Vec<Response>) {
        let requests = self.requests[..index.min(self.requests.len())].to_vec();
        self.initial_state.clone().tick_batch(requests)
    }

    /// Binary search for the first request that causes an invariant violation
    pub fn find_invariant_violation(&self) -> Option<usize> {
        let mut lo = 0;
        let mut hi = self.requests.len();

        // First check if there's any violation at all
        let (final_state, _) = self.replay();
        if final_state.check_invariants().is_ok() {
            return None;
        }

        // Binary search
        while lo < hi {
            let mid = (lo + hi) / 2;
            let (state, _) = self.replay_to(mid + 1);

            if state.check_invariants().is_err() {
                hi = mid;
            } else {
                lo = mid + 1;
            }
        }

        Some(lo)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_pure_tick() {
        let state = BrokerState::new();
        let client = ClientId(1);

        // Register client
        let req1 = Request {
            client,
            seq: SeqNo(0),
            op: Operation::RegisterClient,
        };
        let (state, resp1) = state.tick(req1);
        assert!(resp1.result.is_ok());

        // Allocate handle
        let req2 = Request {
            client,
            seq: SeqNo(1),
            op: Operation::Alloc {
                h_root: 1,
                h_parent: 1,
                h_new: 100,
                class: 0x41,
            },
        };
        let (state, resp2) = state.tick(req2);
        assert!(resp2.result.is_ok());

        // Verify state
        assert_eq!(state.requests_processed, 2);
        state.check_invariants().unwrap();
    }

    #[test]
    fn test_replay_determinism() {
        let state = BrokerState::new();
        let client = ClientId(1);

        let requests = vec![
            Request {
                client,
                seq: SeqNo(0),
                op: Operation::RegisterClient,
            },
            Request {
                client,
                seq: SeqNo(1),
                op: Operation::Alloc {
                    h_root: 1,
                    h_parent: 1,
                    h_new: 100,
                    class: 0x41,
                },
            },
            Request {
                client,
                seq: SeqNo(2),
                op: Operation::Alloc {
                    h_root: 1,
                    h_parent: 1,
                    h_new: 101,
                    class: 0x41,
                },
            },
        ];

        // Run twice
        let (state1, responses1) = state.clone().tick_batch(requests.clone());
        let (state2, responses2) = state.clone().tick_batch(requests);

        // Must be identical
        assert_eq!(responses1, responses2);
        assert_eq!(state1.requests_processed, state2.requests_processed);
        assert_eq!(state1.next_real_handle, state2.next_real_handle);
    }

    #[test]
    fn test_recording_replay() {
        let state = BrokerState::new();
        let client = ClientId(1);

        let mut recording = Recording::new(state);

        recording.record(Request {
            client,
            seq: SeqNo(0),
            op: Operation::RegisterClient,
        });

        recording.record(Request {
            client,
            seq: SeqNo(1),
            op: Operation::Alloc {
                h_root: 1,
                h_parent: 1,
                h_new: 100,
                class: 0x41,
            },
        });

        // Replay full
        let (state, responses) = recording.replay();
        assert_eq!(responses.len(), 2);
        assert!(responses.iter().all(|r| r.result.is_ok()));
        state.check_invariants().unwrap();

        // Replay partial
        let (state_partial, responses_partial) = recording.replay_to(1);
        assert_eq!(responses_partial.len(), 1);
        state_partial.check_invariants().unwrap();
    }
}

#[cfg(test)]
mod proptests {
    use super::*;
    use proptest::prelude::*;

    fn client_id() -> impl Strategy<Value = ClientId> {
        (0u64..20).prop_map(ClientId)
    }

    fn handle() -> impl Strategy<Value = NvHandle> {
        1u32..1000
    }

    fn operation(client: ClientId) -> impl Strategy<Value = Operation> {
        prop_oneof![
            Just(Operation::RegisterClient),
            Just(Operation::UnregisterClient),
            (handle(), handle(), handle(), 0x41u32..0x50)
                .prop_map(|(r, p, n, c)| Operation::Alloc {
                    h_root: r,
                    h_parent: p,
                    h_new: n,
                    class: c
                }),
            (handle(), handle()).prop_map(|(r, o)| Operation::Free {
                h_root: r,
                h_object: o
            }),
        ]
    }

    fn request() -> impl Strategy<Value = Request> {
        (client_id(), 0u64..1000).prop_flat_map(|(client, seq)| {
            operation(client).prop_map(move |op| Request {
                client,
                seq: SeqNo(seq),
                op,
            })
        })
    }

    proptest! {
        /// Property: Invariants hold after any sequence of operations
        #[test]
        fn invariants_always_hold(requests in proptest::collection::vec(request(), 0..500)) {
            let mut state = BrokerState::new();

            for request in requests {
                let (new_state, _response) = state.tick(request);
                new_state.check_invariants().unwrap();
                state = new_state;
            }
        }

        /// Property: Replay produces identical results
        #[test]
        fn replay_deterministic(requests in proptest::collection::vec(request(), 0..200)) {
            let state = BrokerState::new();

            let (state1, responses1) = state.clone().tick_batch(requests.clone());
            let (state2, responses2) = state.clone().tick_batch(requests);

            prop_assert_eq!(responses1, responses2);
            prop_assert_eq!(state1.requests_processed, state2.requests_processed);
        }

        /// Property: State is consistent at any replay point
        #[test]
        fn partial_replay_consistent(
            requests in proptest::collection::vec(request(), 1..100),
            stop_at in 0usize..100
        ) {
            let recording = Recording {
                initial_state: BrokerState::new(),
                requests: requests.clone(),
            };

            let (state, responses) = recording.replay_to(stop_at);

            // State should be consistent
            state.check_invariants().unwrap();

            // Should have processed the right number
            let expected = stop_at.min(requests.len());
            prop_assert_eq!(responses.len(), expected);
        }

        /// Property: Unregistered client's operations fail
        #[test]
        fn unregistered_client_fails(
            h_new in handle(),
            seq in 0u64..100
        ) {
            let state = BrokerState::new();
            let client = ClientId(999); // Never registered

            let request = Request {
                client,
                seq: SeqNo(seq),
                op: Operation::Alloc {
                    h_root: 1,
                    h_parent: 1,
                    h_new,
                    class: 0x41,
                },
            };

            let (state, response) = state.tick(request);
            prop_assert!(response.result.is_err());
            state.check_invariants().unwrap();
        }

        /// Property: Successful alloc followed by free works
        #[test]
        fn alloc_then_free(h_new in handle()) {
            let state = BrokerState::new();
            let client = ClientId(1);

            let requests = vec![
                Request {
                    client,
                    seq: SeqNo(0),
                    op: Operation::RegisterClient,
                },
                Request {
                    client,
                    seq: SeqNo(1),
                    op: Operation::Alloc {
                        h_root: 1,
                        h_parent: 1,
                        h_new,
                        class: 0x41,
                    },
                },
                Request {
                    client,
                    seq: SeqNo(2),
                    op: Operation::Free {
                        h_root: 1,
                        h_object: h_new,
                    },
                },
            ];

            let (state, responses) = state.tick_batch(requests);

            // All should succeed
            for resp in &responses {
                prop_assert!(resp.result.is_ok(), "Failed: {:?}", resp);
            }

            state.check_invariants().unwrap();
        }

        /// Property: Double free fails
        #[test]
        fn double_free_fails(h_new in handle()) {
            let state = BrokerState::new();
            let client = ClientId(1);

            let requests = vec![
                Request {
                    client,
                    seq: SeqNo(0),
                    op: Operation::RegisterClient,
                },
                Request {
                    client,
                    seq: SeqNo(1),
                    op: Operation::Alloc {
                        h_root: 1,
                        h_parent: 1,
                        h_new,
                        class: 0x41,
                    },
                },
                Request {
                    client,
                    seq: SeqNo(2),
                    op: Operation::Free {
                        h_root: 1,
                        h_object: h_new,
                    },
                },
                Request {
                    client,
                    seq: SeqNo(3),
                    op: Operation::Free {
                        h_root: 1,
                        h_object: h_new,
                    },
                },
            ];

            let (state, responses) = state.tick_batch(requests);

            // First three succeed, fourth fails
            prop_assert!(responses[0].result.is_ok());
            prop_assert!(responses[1].result.is_ok());
            prop_assert!(responses[2].result.is_ok());
            prop_assert!(responses[3].result.is_err());

            state.check_invariants().unwrap();
        }

        /// Property: Client isolation - one client can't free another's handle
        #[test]
        fn cross_client_free_fails(h_new in handle()) {
            let state = BrokerState::new();
            let client1 = ClientId(1);
            let client2 = ClientId(2);

            let requests = vec![
                Request {
                    client: client1,
                    seq: SeqNo(0),
                    op: Operation::RegisterClient,
                },
                Request {
                    client: client2,
                    seq: SeqNo(1),
                    op: Operation::RegisterClient,
                },
                Request {
                    client: client1,
                    seq: SeqNo(2),
                    op: Operation::Alloc {
                        h_root: 1,
                        h_parent: 1,
                        h_new,
                        class: 0x41,
                    },
                },
                // Client 2 tries to free client 1's handle
                Request {
                    client: client2,
                    seq: SeqNo(3),
                    op: Operation::Free {
                        h_root: 1,
                        h_object: h_new,
                    },
                },
            ];

            let (state, responses) = state.tick_batch(requests);

            // First three succeed, fourth fails (client 2 doesn't own that handle)
            prop_assert!(responses[0].result.is_ok());
            prop_assert!(responses[1].result.is_ok());
            prop_assert!(responses[2].result.is_ok());
            prop_assert!(responses[3].result.is_err());

            state.check_invariants().unwrap();
        }

        /// Stress: Many clients, many operations, invariants hold
        #[test]
        fn stress_many_clients(
            ops in proptest::collection::vec(
                (0u64..10, 0u32..50, any::<bool>()),
                0..1000
            )
        ) {
            let mut state = BrokerState::new();
            let mut seq = 0u64;

            // Register all clients first
            for i in 0..10 {
                let (new_state, _) = state.tick(Request {
                    client: ClientId(i),
                    seq: SeqNo(seq),
                    op: Operation::RegisterClient,
                });
                state = new_state;
                seq += 1;
            }

            // Random alloc/free operations
            for (client_id, handle, is_alloc) in ops {
                let client = ClientId(client_id);
                let op = if is_alloc {
                    Operation::Alloc {
                        h_root: 1,
                        h_parent: 1,
                        h_new: handle,
                        class: 0x41,
                    }
                } else {
                    Operation::Free {
                        h_root: 1,
                        h_object: handle,
                    }
                };

                let (new_state, _response) = state.tick(Request {
                    client,
                    seq: SeqNo(seq),
                    op,
                });

                new_state.check_invariants().unwrap();
                state = new_state;
                seq += 1;
            }
        }
    }
}
