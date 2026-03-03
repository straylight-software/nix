//! Transport Layer - Minimal stateful membrane
//!
//! The transport converts bytes ↔ Request/Response. It's the only stateful
//! part of the system (file descriptors, ring buffers, eventfds).
//!
//! For tests, we provide a simulated transport that's purely in-memory.
//! The real transport and simulated transport implement the same trait,
//! so the broker doesn't know which one it's talking to.
//!
//! ```text
//!                        ┌─────────────────────────┐
//!    VM                  │    Transport Trait      │           Broker
//!   bytes ──────────────▶│  fn recv() -> Request   │─────────▶ pure fn
//!   bytes ◀──────────────│  fn send(Response)      │◀───────── pure fn
//!                        └─────────────────────────┘
//!                                   │
//!                    ┌──────────────┴──────────────┐
//!                    │                             │
//!              ┌─────▼─────┐                 ┌─────▼─────┐
//!              │   Real    │                 │ Simulated │
//!              │ io_uring  │                 │  in-mem   │
//!              │  + shmem  │                 │  queues   │
//!              └───────────┘                 └───────────┘
//! ```

use crate::broker::{Request, Response};
use crate::handle_table::ClientId;
use std::collections::VecDeque;

/// Transport trait - the membrane between bytes and pure functions
pub trait Transport {
    /// Receive the next request (blocking or non-blocking depending on impl)
    fn recv(&mut self) -> Option<Request>;

    /// Send a response back to a client
    fn send(&mut self, response: Response);

    /// Check if there are pending requests
    fn has_pending(&self) -> bool;

    /// Flush any buffered responses
    fn flush(&mut self);
}

// =============================================================================
// SIMULATED TRANSPORT (for testing)
// =============================================================================

/// Simulated transport - purely in-memory, no syscalls
///
/// This is what we use for property tests. It behaves identically to the
/// real transport from the broker's perspective.
#[derive(Debug, Default)]
pub struct SimulatedTransport {
    /// Incoming requests (VM → Broker)
    inbox: VecDeque<Request>,

    /// Outgoing responses (Broker → VM)
    outbox: VecDeque<Response>,

    /// Requests that have been processed (for verification)
    processed: Vec<Request>,

    /// Responses that have been sent (for verification)
    sent: Vec<Response>,
}

impl SimulatedTransport {
    pub fn new() -> Self {
        Self::default()
    }

    /// Inject a request (simulates VM sending a request)
    pub fn inject(&mut self, request: Request) {
        self.inbox.push_back(request);
    }

    /// Inject multiple requests
    pub fn inject_all(&mut self, requests: impl IntoIterator<Item = Request>) {
        for req in requests {
            self.inject(req);
        }
    }

    /// Drain all responses (simulates VM receiving responses)
    pub fn drain_responses(&mut self) -> Vec<Response> {
        self.outbox.drain(..).collect()
    }

    /// Get all sent responses (for test verification)
    pub fn all_sent(&self) -> &[Response] {
        &self.sent
    }

    /// Get all processed requests (for test verification)
    pub fn all_processed(&self) -> &[Request] {
        &self.processed
    }

    /// Clear history (for reuse)
    pub fn clear_history(&mut self) {
        self.processed.clear();
        self.sent.clear();
    }
}

impl Transport for SimulatedTransport {
    fn recv(&mut self) -> Option<Request> {
        let req = self.inbox.pop_front()?;
        self.processed.push(req.clone());
        Some(req)
    }

    fn send(&mut self, response: Response) {
        self.sent.push(response.clone());
        self.outbox.push_back(response);
    }

    fn has_pending(&self) -> bool {
        !self.inbox.is_empty()
    }

    fn flush(&mut self) {
        // No-op for simulated transport
    }
}

// =============================================================================
// MULTI-CLIENT SIMULATED TRANSPORT
// =============================================================================

/// Multi-client simulated transport - simulates multiple VMs
#[derive(Debug, Default)]
pub struct MultiClientTransport {
    /// Per-client inboxes
    client_inbox: std::collections::HashMap<ClientId, VecDeque<Request>>,

    /// Per-client outboxes
    client_outbox: std::collections::HashMap<ClientId, VecDeque<Response>>,

    /// Global inbox (merged from all clients, for broker consumption)
    global_inbox: VecDeque<Request>,

    /// All requests processed
    processed: Vec<Request>,

    /// All responses sent
    sent: Vec<Response>,
}

impl MultiClientTransport {
    pub fn new() -> Self {
        Self::default()
    }

    /// Register a client
    pub fn register_client(&mut self, client: ClientId) {
        self.client_inbox.entry(client).or_default();
        self.client_outbox.entry(client).or_default();
    }

    /// Client sends a request
    pub fn client_send(&mut self, request: Request) {
        let client = request.client;
        self.client_inbox.entry(client).or_default();
        self.global_inbox.push_back(request);
    }

    /// Client receives responses
    pub fn client_recv(&mut self, client: ClientId) -> Vec<Response> {
        self.client_outbox
            .get_mut(&client)
            .map(|q| q.drain(..).collect())
            .unwrap_or_default()
    }

    /// Get all responses for a client (without draining)
    pub fn client_responses(&self, client: ClientId) -> Vec<Response> {
        self.client_outbox
            .get(&client)
            .map(|q| q.iter().cloned().collect())
            .unwrap_or_default()
    }
}

impl Transport for MultiClientTransport {
    fn recv(&mut self) -> Option<Request> {
        let req = self.global_inbox.pop_front()?;
        self.processed.push(req.clone());
        Some(req)
    }

    fn send(&mut self, response: Response) {
        let client = response.client;
        self.sent.push(response.clone());
        self.client_outbox
            .entry(client)
            .or_default()
            .push_back(response);
    }

    fn has_pending(&self) -> bool {
        !self.global_inbox.is_empty()
    }

    fn flush(&mut self) {
        // No-op for simulated transport
    }
}

// =============================================================================
// WIRE FORMAT (serialization)
// =============================================================================

/// Wire format for requests/responses
///
/// This is what actually goes over the shared memory ring.
/// Fixed-size header + variable payload.
pub mod wire {
    use super::*;
    use crate::broker::{Operation, OperationResult, BrokerError, SeqNo};
    use crate::rm_api::NvHandle;

    /// Request header (fixed size, 32 bytes)
    #[repr(C)]
    #[derive(Debug, Clone, Copy)]
    pub struct RequestHeader {
        /// Client ID
        pub client_id: u64,
        /// Sequence number
        pub seq: u64,
        /// Operation type
        pub op_type: u32,
        /// Payload length
        pub payload_len: u32,
        /// Reserved for alignment
        pub _reserved: u64,
    }

    /// Operation type codes
    #[repr(u32)]
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub enum OpType {
        RegisterClient = 0,
        UnregisterClient = 1,
        Alloc = 2,
        Free = 3,
        Control = 4,
        MapMemory = 5,
        UnmapMemory = 6,
        // Top-level escapes
        CardInfo = 7,
        CheckVersion = 8,
        RegisterFd = 9,
        AllocOsEvent = 10,
        FreeOsEvent = 11,
        AttachGpusToFd = 12,
        StatusCode = 13,
    }

    /// Response header (fixed size, 32 bytes)
    #[repr(C)]
    #[derive(Debug, Clone, Copy)]
    pub struct ResponseHeader {
        /// Client ID
        pub client_id: u64,
        /// Sequence number
        pub seq: u64,
        /// Success (1) or error (0)
        pub success: u32,
        /// Result type or error code
        pub result_code: u32,
        /// Payload length
        pub payload_len: u32,
        /// Reserved
        pub _reserved: u32,
    }

    impl RequestHeader {
        pub const SIZE: usize = 32;
    }

    impl ResponseHeader {
        pub const SIZE: usize = 32;
    }

    /// Encode a request to bytes
    pub fn encode_request(request: &Request) -> Vec<u8> {
        let (op_type, payload) = encode_operation(&request.op);
        
        let header = RequestHeader {
            client_id: request.client.0,
            seq: request.seq.0,
            op_type: op_type as u32,
            payload_len: payload.len() as u32,
            _reserved: 0,
        };

        let mut buf = Vec::with_capacity(RequestHeader::SIZE + payload.len());
        
        // Safety: RequestHeader is repr(C) and contains only primitive types
        buf.extend_from_slice(unsafe {
            std::slice::from_raw_parts(
                &header as *const _ as *const u8,
                RequestHeader::SIZE,
            )
        });
        buf.extend_from_slice(&payload);
        
        buf
    }

    /// Decode a request from bytes
    pub fn decode_request(buf: &[u8]) -> Option<Request> {
        if buf.len() < RequestHeader::SIZE {
            return None;
        }

        // Safety: We checked the length
        let header: RequestHeader = unsafe {
            std::ptr::read(buf.as_ptr() as *const RequestHeader)
        };

        let payload = &buf[RequestHeader::SIZE..];
        if payload.len() < header.payload_len as usize {
            return None;
        }

        let payload = &payload[..header.payload_len as usize];
        let op = decode_operation(header.op_type, payload)?;

        Some(Request {
            client: ClientId(header.client_id),
            seq: SeqNo(header.seq),
            op,
        })
    }

    fn encode_operation(op: &Operation) -> (OpType, Vec<u8>) {
        match op {
            Operation::RegisterClient => (OpType::RegisterClient, vec![]),
            Operation::UnregisterClient => (OpType::UnregisterClient, vec![]),
            Operation::Alloc { h_root, h_parent, h_new, class } => {
                let mut payload = Vec::with_capacity(16);
                payload.extend_from_slice(&h_root.to_le_bytes());
                payload.extend_from_slice(&h_parent.to_le_bytes());
                payload.extend_from_slice(&h_new.to_le_bytes());
                payload.extend_from_slice(&class.to_le_bytes());
                (OpType::Alloc, payload)
            }
            Operation::Free { h_root, h_object } => {
                let mut payload = Vec::with_capacity(8);
                payload.extend_from_slice(&h_root.to_le_bytes());
                payload.extend_from_slice(&h_object.to_le_bytes());
                (OpType::Free, payload)
            }
            Operation::Control { h_client, h_object, cmd, params } => {
                let mut payload = Vec::with_capacity(12 + params.len());
                payload.extend_from_slice(&h_client.to_le_bytes());
                payload.extend_from_slice(&h_object.to_le_bytes());
                payload.extend_from_slice(&cmd.to_le_bytes());
                payload.extend_from_slice(params);
                (OpType::Control, payload)
            }
            Operation::MapMemory { h_client, h_device, h_memory, offset, length } => {
                let mut payload = Vec::with_capacity(28);
                payload.extend_from_slice(&h_client.to_le_bytes());
                payload.extend_from_slice(&h_device.to_le_bytes());
                payload.extend_from_slice(&h_memory.to_le_bytes());
                payload.extend_from_slice(&offset.to_le_bytes());
                payload.extend_from_slice(&length.to_le_bytes());
                (OpType::MapMemory, payload)
            }
            Operation::UnmapMemory { h_client, h_device, h_memory } => {
                let mut payload = Vec::with_capacity(12);
                payload.extend_from_slice(&h_client.to_le_bytes());
                payload.extend_from_slice(&h_device.to_le_bytes());
                payload.extend_from_slice(&h_memory.to_le_bytes());
                (OpType::UnmapMemory, payload)
            }
            
            // Top-level escapes
            Operation::CardInfo => (OpType::CardInfo, vec![]),
            Operation::CheckVersion { cmd, version_string } => {
                let mut payload = Vec::with_capacity(4 + version_string.len());
                payload.extend_from_slice(&cmd.to_le_bytes());
                payload.extend_from_slice(version_string);
                (OpType::CheckVersion, payload)
            }
            Operation::RegisterFd { ctl_fd } => {
                let mut payload = Vec::with_capacity(4);
                payload.extend_from_slice(&ctl_fd.to_le_bytes());
                (OpType::RegisterFd, payload)
            }
            Operation::AllocOsEvent { h_client, h_device, fd } => {
                let mut payload = Vec::with_capacity(12);
                payload.extend_from_slice(&h_client.to_le_bytes());
                payload.extend_from_slice(&h_device.to_le_bytes());
                payload.extend_from_slice(&fd.to_le_bytes());
                (OpType::AllocOsEvent, payload)
            }
            Operation::FreeOsEvent { h_client, h_device, fd } => {
                let mut payload = Vec::with_capacity(12);
                payload.extend_from_slice(&h_client.to_le_bytes());
                payload.extend_from_slice(&h_device.to_le_bytes());
                payload.extend_from_slice(&fd.to_le_bytes());
                (OpType::FreeOsEvent, payload)
            }
            Operation::AttachGpusToFd { gpu_ids } => {
                let mut payload = Vec::with_capacity(4 + gpu_ids.len() * 4);
                payload.extend_from_slice(&(gpu_ids.len() as u32).to_le_bytes());
                for id in gpu_ids {
                    payload.extend_from_slice(&id.to_le_bytes());
                }
                (OpType::AttachGpusToFd, payload)
            }
            Operation::StatusCode { domain, bus, slot, function } => {
                let mut payload = Vec::with_capacity(7);
                payload.extend_from_slice(&domain.to_le_bytes());
                payload.push(*bus);
                payload.push(*slot);
                payload.push(*function);
                (OpType::StatusCode, payload)
            }
        }
    }

    fn decode_operation(op_type: u32, payload: &[u8]) -> Option<Operation> {
        match op_type {
            0 => Some(Operation::RegisterClient),
            1 => Some(Operation::UnregisterClient),
            2 if payload.len() >= 16 => {
                Some(Operation::Alloc {
                    h_root: u32::from_le_bytes(payload[0..4].try_into().ok()?),
                    h_parent: u32::from_le_bytes(payload[4..8].try_into().ok()?),
                    h_new: u32::from_le_bytes(payload[8..12].try_into().ok()?),
                    class: u32::from_le_bytes(payload[12..16].try_into().ok()?),
                })
            }
            3 if payload.len() >= 8 => {
                Some(Operation::Free {
                    h_root: u32::from_le_bytes(payload[0..4].try_into().ok()?),
                    h_object: u32::from_le_bytes(payload[4..8].try_into().ok()?),
                })
            }
            4 if payload.len() >= 12 => {
                Some(Operation::Control {
                    h_client: u32::from_le_bytes(payload[0..4].try_into().ok()?),
                    h_object: u32::from_le_bytes(payload[4..8].try_into().ok()?),
                    cmd: u32::from_le_bytes(payload[8..12].try_into().ok()?),
                    params: payload[12..].to_vec(),
                })
            }
            5 if payload.len() >= 28 => {
                Some(Operation::MapMemory {
                    h_client: u32::from_le_bytes(payload[0..4].try_into().ok()?),
                    h_device: u32::from_le_bytes(payload[4..8].try_into().ok()?),
                    h_memory: u32::from_le_bytes(payload[8..12].try_into().ok()?),
                    offset: u64::from_le_bytes(payload[12..20].try_into().ok()?),
                    length: u64::from_le_bytes(payload[20..28].try_into().ok()?),
                })
            }
            6 if payload.len() >= 12 => {
                Some(Operation::UnmapMemory {
                    h_client: u32::from_le_bytes(payload[0..4].try_into().ok()?),
                    h_device: u32::from_le_bytes(payload[4..8].try_into().ok()?),
                    h_memory: u32::from_le_bytes(payload[8..12].try_into().ok()?),
                })
            }
            
            // Top-level escapes
            7 => Some(Operation::CardInfo),
            8 if payload.len() >= 4 => {
                let cmd = u32::from_le_bytes(payload[0..4].try_into().ok()?);
                let version_string = payload[4..].to_vec();
                Some(Operation::CheckVersion { cmd, version_string })
            }
            9 if payload.len() >= 4 => {
                let ctl_fd = i32::from_le_bytes(payload[0..4].try_into().ok()?);
                Some(Operation::RegisterFd { ctl_fd })
            }
            10 if payload.len() >= 12 => {
                Some(Operation::AllocOsEvent {
                    h_client: u32::from_le_bytes(payload[0..4].try_into().ok()?),
                    h_device: u32::from_le_bytes(payload[4..8].try_into().ok()?),
                    fd: u32::from_le_bytes(payload[8..12].try_into().ok()?),
                })
            }
            11 if payload.len() >= 12 => {
                Some(Operation::FreeOsEvent {
                    h_client: u32::from_le_bytes(payload[0..4].try_into().ok()?),
                    h_device: u32::from_le_bytes(payload[4..8].try_into().ok()?),
                    fd: u32::from_le_bytes(payload[8..12].try_into().ok()?),
                })
            }
            12 if payload.len() >= 4 => {
                let count = u32::from_le_bytes(payload[0..4].try_into().ok()?) as usize;
                let mut gpu_ids = Vec::with_capacity(count);
                for i in 0..count {
                    let offset = 4 + i * 4;
                    if payload.len() >= offset + 4 {
                        gpu_ids.push(u32::from_le_bytes(payload[offset..offset+4].try_into().ok()?));
                    }
                }
                Some(Operation::AttachGpusToFd { gpu_ids })
            }
            13 if payload.len() >= 7 => {
                Some(Operation::StatusCode {
                    domain: u32::from_le_bytes(payload[0..4].try_into().ok()?),
                    bus: payload[4],
                    slot: payload[5],
                    function: payload[6],
                })
            }
            _ => None,
        }
    }
}

// =============================================================================
// THE MAIN LOOP (polled, forward-only)
// =============================================================================

use crate::broker::BrokerState;

/// Run the broker main loop with a given transport
///
/// This is the Carmack-style polled loop:
/// 1. Drain all pending requests
/// 2. Process each through the pure function
/// 3. Send all responses
/// 4. Yield
///
/// No callbacks. No re-entrancy. Forward-only control flow.
pub fn run_loop<T: Transport>(
    mut state: BrokerState,
    transport: &mut T,
    max_iterations: Option<usize>,
) -> BrokerState {
    let mut iterations = 0;

    loop {
        // Check termination
        if let Some(max) = max_iterations {
            if iterations >= max {
                break;
            }
        }

        // 1. Drain all pending requests
        let mut requests = Vec::new();
        while let Some(req) = transport.recv() {
            requests.push(req);
        }

        // If no requests and we have a max, just count and continue
        // In real impl, this would block on eventfd
        if requests.is_empty() {
            if max_iterations.is_some() {
                iterations += 1;
                continue;
            } else {
                // Real impl: block here
                break;
            }
        }

        // 2. Process all requests through the pure function
        let (new_state, responses) = state.tick_batch(requests);
        state = new_state;

        // 3. Send all responses
        for response in responses {
            transport.send(response);
        }

        // 4. Flush
        transport.flush();

        iterations += 1;
    }

    state
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::broker::{Operation, SeqNo};

    #[test]
    fn test_simulated_transport() {
        let mut transport = SimulatedTransport::new();
        let client = ClientId(1);

        // Inject requests
        transport.inject(Request {
            client,
            seq: SeqNo(0),
            op: Operation::RegisterClient,
        });
        transport.inject(Request {
            client,
            seq: SeqNo(1),
            op: Operation::Alloc {
                h_root: 1,
                h_parent: 1,
                h_new: 100,
                class: 0x41,
            },
        });

        assert!(transport.has_pending());
        assert_eq!(transport.recv().unwrap().seq, SeqNo(0));
        assert_eq!(transport.recv().unwrap().seq, SeqNo(1));
        assert!(!transport.has_pending());
    }

    #[test]
    fn test_run_loop_simulated() {
        let mut transport = SimulatedTransport::new();
        let client = ClientId(1);

        transport.inject_all(vec![
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
        ]);

        let state = BrokerState::new();
        let final_state = run_loop(state, &mut transport, Some(10));

        assert_eq!(final_state.requests_processed, 2);
        assert_eq!(transport.all_sent().len(), 2);
        assert!(transport.all_sent().iter().all(|r| r.result.is_ok()));
    }

    #[test]
    fn test_wire_roundtrip() {
        use wire::*;

        let request = Request {
            client: ClientId(42),
            seq: SeqNo(123),
            op: Operation::Alloc {
                h_root: 1,
                h_parent: 2,
                h_new: 3,
                class: 0x41,
            },
        };

        let encoded = encode_request(&request);
        let decoded = decode_request(&encoded).unwrap();

        assert_eq!(decoded.client, request.client);
        assert_eq!(decoded.seq, request.seq);
        assert_eq!(decoded.op, request.op);
    }

    #[test]
    fn test_multi_client_transport() {
        let mut transport = MultiClientTransport::new();
        let client1 = ClientId(1);
        let client2 = ClientId(2);

        transport.register_client(client1);
        transport.register_client(client2);

        // Both clients send requests
        transport.client_send(Request {
            client: client1,
            seq: SeqNo(0),
            op: Operation::RegisterClient,
        });
        transport.client_send(Request {
            client: client2,
            seq: SeqNo(0),
            op: Operation::RegisterClient,
        });

        // Run loop
        let state = BrokerState::new();
        let _final_state = run_loop(state, &mut transport, Some(10));

        // Each client gets their own response
        let r1 = transport.client_recv(client1);
        let r2 = transport.client_recv(client2);

        assert_eq!(r1.len(), 1);
        assert_eq!(r2.len(), 1);
        assert_eq!(r1[0].client, client1);
        assert_eq!(r2[0].client, client2);
    }
}

#[cfg(test)]
mod proptests {
    use super::*;
    use crate::broker::{Operation, SeqNo};
    use proptest::prelude::*;

    fn client_id() -> impl Strategy<Value = ClientId> {
        (0u64..20).prop_map(ClientId)
    }

    fn operation() -> impl Strategy<Value = Operation> {
        prop_oneof![
            Just(Operation::RegisterClient),
            Just(Operation::UnregisterClient),
            (1u32..100, 1u32..100, 1u32..1000, 0x41u32..0x50).prop_map(|(r, p, n, c)| {
                Operation::Alloc {
                    h_root: r,
                    h_parent: p,
                    h_new: n,
                    class: c,
                }
            }),
            (1u32..100, 1u32..1000).prop_map(|(r, o)| Operation::Free {
                h_root: r,
                h_object: o,
            }),
        ]
    }

    fn request() -> impl Strategy<Value = Request> {
        (client_id(), 0u64..10000, operation()).prop_map(|(client, seq, op)| Request {
            client,
            seq: SeqNo(seq),
            op,
        })
    }

    proptest! {
        /// Property: Wire encoding is lossless
        #[test]
        fn wire_roundtrip(req in request()) {
            let encoded = wire::encode_request(&req);
            let decoded = wire::decode_request(&encoded);
            
            prop_assert!(decoded.is_some());
            let decoded = decoded.unwrap();
            prop_assert_eq!(decoded.client, req.client);
            prop_assert_eq!(decoded.seq, req.seq);
            prop_assert_eq!(decoded.op, req.op);
        }

        /// Property: Simulated transport preserves all requests
        #[test]
        fn transport_preserves_requests(requests in proptest::collection::vec(request(), 0..100)) {
            let mut transport = SimulatedTransport::new();
            transport.inject_all(requests.clone());

            let mut received = Vec::new();
            while let Some(req) = transport.recv() {
                received.push(req);
            }

            prop_assert_eq!(received, requests);
        }

        /// Property: Run loop processes all requests
        #[test]
        fn run_loop_processes_all(requests in proptest::collection::vec(request(), 0..100)) {
            let mut transport = SimulatedTransport::new();
            transport.inject_all(requests.clone());

            let state = BrokerState::new();
            let final_state = run_loop(state, &mut transport, Some(10));

            // Should have processed all requests
            prop_assert_eq!(final_state.requests_processed as usize, requests.len());
            // Should have sent a response for each
            prop_assert_eq!(transport.all_sent().len(), requests.len());
        }

        /// Property: Responses match requests (same client, same seq)
        #[test]
        fn responses_match_requests(requests in proptest::collection::vec(request(), 1..50)) {
            let mut transport = SimulatedTransport::new();
            transport.inject_all(requests.clone());

            let state = BrokerState::new();
            let _ = run_loop(state, &mut transport, Some(10));

            let responses = transport.all_sent();
            
            for (req, resp) in requests.iter().zip(responses.iter()) {
                prop_assert_eq!(req.client, resp.client);
                prop_assert_eq!(req.seq, resp.seq);
            }
        }

        /// Property: Multi-client isolation - responses go to correct client
        #[test]
        fn multi_client_response_routing(
            ops_per_client in proptest::collection::vec(
                (0u64..5, proptest::collection::vec(operation(), 0..20)),
                1..10
            )
        ) {
            let mut transport = MultiClientTransport::new();
            
            // Register and send requests for each client
            let mut expected_counts: std::collections::HashMap<ClientId, usize> = 
                std::collections::HashMap::new();
            
            let mut seq = 0u64;
            for (client_id, ops) in &ops_per_client {
                let client = ClientId(*client_id);
                transport.register_client(client);
                
                for op in ops {
                    transport.client_send(Request {
                        client,
                        seq: SeqNo(seq),
                        op: op.clone(),
                    });
                    seq += 1;
                    *expected_counts.entry(client).or_default() += 1;
                }
            }

            // Run the loop
            let state = BrokerState::new();
            let _ = run_loop(state, &mut transport, Some(10));

            // Verify each unique client got exactly their responses
            // Note: same client_id can appear multiple times in ops_per_client,
            // so we verify against the aggregated expected_counts map
            let mut verified_clients = std::collections::HashSet::new();
            for (client_id, _ops) in &ops_per_client {
                let client = ClientId(*client_id);
                
                // Only verify each unique client once
                if verified_clients.contains(&client) {
                    continue;
                }
                verified_clients.insert(client);
                
                let responses = transport.client_recv(client);
                
                // All responses should be for this client
                for resp in &responses {
                    prop_assert_eq!(resp.client, client);
                }
                
                // Count should match the aggregated expected count
                let expected = expected_counts.get(&client).copied().unwrap_or(0);
                prop_assert_eq!(responses.len(), expected);
            }
        }
    }
}
