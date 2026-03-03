//! Property-based tests for guest protocol
//!
//! These tests verify that invariants hold under all possible inputs.

use proptest::prelude::*;

use super::wire::*;
use super::client::*;
use super::invariants::*;

// =============================================================================
// GENERATORS
// =============================================================================

fn escape_code() -> impl Strategy<Value = NvEscapeCode> {
    prop_oneof![
        Just(NvEscapeCode::AllocMemory),
        Just(NvEscapeCode::AllocObject),
        Just(NvEscapeCode::Free),
        Just(NvEscapeCode::Control),
        Just(NvEscapeCode::Alloc),
        Just(NvEscapeCode::ConfigGet),
        Just(NvEscapeCode::ConfigSet),
        Just(NvEscapeCode::DupObject),
        Just(NvEscapeCode::Share),
        Just(NvEscapeCode::MapMemory),
        Just(NvEscapeCode::UnmapMemory),
    ]
}

fn request_header() -> impl Strategy<Value = RequestHeader> {
    (
        1u64..10000,           // request_id
        escape_code(),         // escape_code
        0u8..4,               // device_idx
        0u32..1000,           // payload_len
    ).prop_map(|(id, esc, dev, len)| {
        RequestHeader::new(RequestId(id), esc, dev, len)
    })
}

fn response_header() -> impl Strategy<Value = ResponseHeader> {
    (
        1u64..10000,           // request_id
        0u32..100,            // status (0 = success)
        0u32..1000,           // payload_len
    ).prop_map(|(id, status, len)| {
        ResponseHeader::new(RequestId(id), status, len)
    })
}

fn payload_bytes() -> impl Strategy<Value = Vec<u8>> {
    proptest::collection::vec(any::<u8>(), 0..1000)
}

// =============================================================================
// WIRE FORMAT TESTS
// =============================================================================

proptest! {
    /// Property: RequestHeader roundtrips through bytes
    #[test]
    fn request_header_roundtrip(header in request_header()) {
        let mut buf = [0u8; RequestHeader::SIZE];
        header.to_bytes(&mut buf).unwrap();
        
        let parsed = RequestHeader::from_bytes(&buf).unwrap();
        
        prop_assert_eq!(parsed.request_id, header.request_id);
        prop_assert_eq!(parsed.escape_code, header.escape_code);
        prop_assert_eq!(parsed.device_idx, header.device_idx);
        prop_assert_eq!(parsed.payload_len, header.payload_len);
    }
    
    /// Property: ResponseHeader roundtrips through bytes
    #[test]
    fn response_header_roundtrip(header in response_header()) {
        let mut buf = [0u8; ResponseHeader::SIZE];
        header.to_bytes(&mut buf).unwrap();
        
        let parsed = ResponseHeader::from_bytes(&buf).unwrap();
        
        prop_assert_eq!(parsed.request_id, header.request_id);
        prop_assert_eq!(parsed.status, header.status);
        prop_assert_eq!(parsed.payload_len, header.payload_len);
    }
    
    /// Property: IoctlRequest roundtrips through encode/decode
    #[test]
    fn ioctl_request_roundtrip(
        header in request_header(),
        payload in payload_bytes()
    ) {
        let payload = IoctlPayload::from_bytes(&payload).unwrap();
        let request = IoctlRequest { 
            header: RequestHeader::new(
                RequestId(header.request_id),
                header.escape().unwrap(),
                header.device_idx,
                payload.len() as u32,
            ),
            payload: payload.clone(),
        };
        
        let mut buf = vec![0u8; RequestHeader::SIZE + request.payload.len()];
        let len = encode_request(&request, &mut buf).unwrap();
        
        let decoded = decode_request(&buf[..len]).unwrap();
        
        prop_assert_eq!(decoded.header.request_id, request.header.request_id);
        prop_assert_eq!(decoded.header.escape_code, request.header.escape_code);
        prop_assert_eq!(decoded.payload.as_bytes(), request.payload.as_bytes());
    }
    
    /// Property: IoctlResponse roundtrips through encode/decode
    #[test]
    fn ioctl_response_roundtrip(
        header in response_header(),
        payload in payload_bytes()
    ) {
        let payload = IoctlPayload::from_bytes(&payload).unwrap();
        let response = IoctlResponse { 
            header: ResponseHeader::new(
                RequestId(header.request_id),
                header.status,
                payload.len() as u32,
            ),
            payload: payload.clone(),
        };
        
        let mut buf = vec![0u8; ResponseHeader::SIZE + response.payload.len()];
        let len = encode_response(&response, &mut buf).unwrap();
        
        let decoded = decode_response(&buf[..len]).unwrap();
        
        prop_assert_eq!(decoded.header.request_id, response.header.request_id);
        prop_assert_eq!(decoded.header.status, response.header.status);
        prop_assert_eq!(decoded.payload.as_bytes(), response.payload.as_bytes());
    }
    
    /// Property: Short buffers fail gracefully
    #[test]
    fn short_buffer_fails(len in 0usize..RequestHeader::SIZE) {
        let buf = vec![0u8; len];
        let result = decode_request(&buf);
        prop_assert!(result.is_err());
    }
    
    /// Property: Invalid escape codes are rejected
    #[test]
    fn invalid_escape_rejected(code in 0u8..255u8) {
        // Skip valid codes
        if NvEscapeCode::from_u8(code).is_some() {
            return Ok(());
        }
        
        let mut buf = [0u8; RequestHeader::SIZE];
        // Write a header with invalid escape code
        buf[8] = code;  // escape_code offset
        
        let header = RequestHeader::from_bytes(&buf).unwrap();
        prop_assert!(header.escape().is_none());
    }
}

// =============================================================================
// CLIENT STATE TESTS
// =============================================================================

/// Operation on client state
#[derive(Debug, Clone)]
enum ClientOp {
    BeginRequest(NvEscapeCode),
    CompleteRequest(u64),  // request_id to complete
    CancelRequest(u64),
}

fn client_op() -> impl Strategy<Value = ClientOp> {
    prop_oneof![
        escape_code().prop_map(ClientOp::BeginRequest),
        (1u64..100).prop_map(ClientOp::CompleteRequest),
        (1u64..100).prop_map(ClientOp::CancelRequest),
    ]
}

proptest! {
    /// Property: Client state invariants always hold after any operation sequence
    #[test]
    fn client_invariants_always_hold(ops in proptest::collection::vec(client_op(), 0..500)) {
        let mut client = ClientState::new();
        
        for op in ops {
            match op {
                ClientOp::BeginRequest(escape) => {
                    // Ignore errors (e.g., too many pending)
                    let _ = client.begin_request(escape, 0, 32);
                }
                ClientOp::CompleteRequest(id) => {
                    let response = ResponseHeader::new(RequestId(id), 0, 0);
                    let _ = client.complete_request(&response);
                }
                ClientOp::CancelRequest(id) => {
                    let _ = client.cancel_request(RequestId(id));
                }
            }
            
            // Invariants must hold after every operation
            prop_assert!(client.check_invariants().is_ok());
        }
    }
    
    /// Property: Every request gets exactly one response
    #[test]
    fn every_request_gets_one_response(
        ops in proptest::collection::vec(escape_code(), 1..100)
    ) {
        let mut client = ClientState::new();
        let mut pending_ids = Vec::new();
        
        // Send all requests
        for escape in ops {
            if let Ok(header) = client.begin_request(escape, 0, 32) {
                pending_ids.push(header.request_id);
            }
        }
        
        // Complete all in random order (but we'll do in order for simplicity)
        for id in pending_ids.iter().rev() {
            let response = ResponseHeader::new(RequestId(*id), 0, 0);
            let result = client.complete_request(&response);
            prop_assert!(result.is_ok());
        }
        
        // No pending requests remain
        prop_assert_eq!(client.pending_count(), 0);
    }
    
    /// Property: Request IDs are monotonically increasing
    #[test]
    fn request_ids_monotonic(n in 1usize..100) {
        let mut client = ClientState::new();
        let mut prev_id = 0u64;
        
        for _ in 0..n {
            if let Ok(header) = client.begin_request(NvEscapeCode::Alloc, 0, 32) {
                prop_assert!(header.request_id > prev_id);
                prev_id = header.request_id;
            }
        }
    }
    
    /// Property: Completing unknown request returns error
    #[test]
    fn unknown_request_fails(id in 1u64..1000) {
        let client = ClientState::new();
        let response = ResponseHeader::new(RequestId(id), 0, 0);
        
        // No requests pending, so any completion should fail
        // Need mutable client for complete_request
        let mut client = client;
        let result = client.complete_request(&response);
        prop_assert!(matches!(result, Err(ClientError::UnknownRequest(_))));
    }
    
    /// Property: Pending count never exceeds MAX_PENDING_REQUESTS
    #[test]
    fn pending_count_bounded(n in 0usize..500) {
        let mut client = ClientState::new();
        
        for _ in 0..n {
            let _ = client.begin_request(NvEscapeCode::Alloc, 0, 32);
        }
        
        prop_assert!(client.pending_count() <= MAX_PENDING_REQUESTS);
    }
    
    /// Property: Out-of-order completion works correctly
    #[test]
    fn out_of_order_completion(n in 2usize..50) {
        let mut client = ClientState::new();
        let mut ids = Vec::new();
        
        // Send N requests
        for _ in 0..n {
            if let Ok(header) = client.begin_request(NvEscapeCode::Alloc, 0, 32) {
                ids.push(header.request_id);
            }
        }
        
        // Complete in reverse order
        for id in ids.iter().rev() {
            let response = ResponseHeader::new(RequestId(*id), 0, 0);
            prop_assert!(client.complete_request(&response).is_ok());
            prop_assert!(client.check_invariants().is_ok());
        }
        
        prop_assert_eq!(client.pending_count(), 0);
    }
    
    /// Property: Statistics are accurate
    #[test]
    fn stats_accurate(
        sends in 1usize..50,
        completes in 0usize..50,
        errors in 0usize..10
    ) {
        let mut client = ClientState::new();
        let completes = completes.min(sends);
        let errors = errors.min(completes);
        
        // Send requests
        let mut ids = Vec::new();
        for _ in 0..sends {
            if let Ok(header) = client.begin_request(NvEscapeCode::Alloc, 0, 32) {
                ids.push(header.request_id);
            }
        }
        
        // Complete some with success
        for (i, id) in ids.iter().take(completes).enumerate() {
            let status = if i < errors { 1 } else { 0 };
            let response = ResponseHeader::new(RequestId(*id), status, 0);
            let _ = client.complete_request(&response);
        }
        
        prop_assert_eq!(client.stats.requests_sent, ids.len() as u64);
        prop_assert_eq!(client.stats.responses_received, completes.min(ids.len()) as u64);
        prop_assert_eq!(client.stats.errors, errors.min(completes.min(ids.len())) as u64);
    }
}

// =============================================================================
// WIRE FORMAT EDGE CASES
// =============================================================================

proptest! {
    /// Property: Maximum payload size is respected
    #[test]
    fn max_payload_respected(size in MAX_PAYLOAD_SIZE..MAX_PAYLOAD_SIZE * 2) {
        let bytes = vec![0u8; size];
        let result = IoctlPayload::from_bytes(&bytes);
        prop_assert!(result.is_err());
    }
    
    /// Property: Empty payload is valid
    #[test]
    fn empty_payload_valid(_dummy in Just(())) {
        let payload = IoctlPayload::empty();
        prop_assert_eq!(payload.len(), 0);
        prop_assert!(payload.is_empty());
    }
    
    /// Property: Payload preserves data exactly
    #[test]
    fn payload_preserves_data(data in proptest::collection::vec(any::<u8>(), 0..MAX_PAYLOAD_SIZE)) {
        let payload = IoctlPayload::from_bytes(&data).unwrap();
        prop_assert_eq!(payload.as_bytes(), data.as_slice());
    }
}

// =============================================================================
// STRESS TESTS
// =============================================================================

proptest! {
    #![proptest_config(ProptestConfig::with_cases(50))]
    
    /// Property: Client handles rapid request/response cycles
    #[test]
    fn rapid_request_response(cycles in 1usize..1000) {
        let mut client = ClientState::new();
        
        for _ in 0..cycles {
            // Send
            let header = client.begin_request(NvEscapeCode::Control, 0, 64).unwrap();
            
            // Complete
            let response = ResponseHeader::new(RequestId(header.request_id), 0, 32);
            client.complete_request(&response).unwrap();
        }
        
        prop_assert_eq!(client.pending_count(), 0);
        prop_assert_eq!(client.stats.requests_sent, cycles as u64);
        prop_assert_eq!(client.stats.responses_received, cycles as u64);
    }
    
    /// Property: Request ID wrapping is handled correctly
    #[test]
    fn request_id_near_max(start in (u64::MAX - 1000)..u64::MAX) {
        let mut client = ClientState::new();
        // Manually set next_request_id near max
        // (In real code we'd need an internal method or just trust monotonicity)
        
        // Can't directly set, so just verify wrapping behavior of RequestId
        let id = RequestId(start);
        let next = id.next();
        
        prop_assert_eq!(next.0, start.wrapping_add(1));
    }
}
