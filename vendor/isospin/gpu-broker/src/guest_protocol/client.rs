//! Client state machine for guest-side protocol handling
//!
//! Tracks pending requests and matches them with responses.
//! Enforces invariants about request/response pairing.

#[cfg(not(feature = "std"))]
use core::result::Result;

#[cfg(feature = "std")]
use std::result::Result;

use super::wire::{RequestId, RequestHeader, ResponseHeader, NvEscapeCode, WireError};
use super::invariants::{Invariant, InvariantError};

/// Maximum number of pending requests per client
pub const MAX_PENDING_REQUESTS: usize = 256;

/// A pending request awaiting response
#[derive(Debug, Clone, Copy)]
pub struct PendingRequest {
    /// The request ID
    pub id: RequestId,
    
    /// The escape code (for validation)
    pub escape_code: NvEscapeCode,
    
    /// Timestamp or sequence when sent (for timeout detection)
    pub sent_seq: u64,
}

/// Client state machine
///
/// Invariants:
/// 1. pending.len() <= MAX_PENDING_REQUESTS
/// 2. All pending request IDs are unique
/// 3. next_request_id > all pending request IDs
/// 4. Every response must match a pending request
pub struct ClientState {
    /// Next request ID to allocate
    next_request_id: RequestId,
    
    /// Pending requests awaiting responses
    #[cfg(feature = "std")]
    pending: Vec<PendingRequest>,
    
    #[cfg(not(feature = "std"))]
    pending: [Option<PendingRequest>; MAX_PENDING_REQUESTS],
    
    #[cfg(not(feature = "std"))]
    pending_count: usize,
    
    /// Sequence counter for ordering
    current_seq: u64,
    
    /// Statistics
    pub stats: ClientStats,
}

/// Client statistics
#[derive(Debug, Clone, Copy, Default)]
pub struct ClientStats {
    pub requests_sent: u64,
    pub responses_received: u64,
    pub errors: u64,
    pub timeouts: u64,
}

/// Error when processing client operations
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ClientError {
    /// Too many pending requests
    TooManyPending,
    
    /// Response for unknown request
    UnknownRequest(RequestId),
    
    /// Duplicate request ID (invariant violation)
    DuplicateRequestId(RequestId),
    
    /// Wire format error
    Wire(WireError),
    
    /// Invariant violation
    Invariant(InvariantError),
}

impl From<WireError> for ClientError {
    fn from(e: WireError) -> Self {
        ClientError::Wire(e)
    }
}

impl From<InvariantError> for ClientError {
    fn from(e: InvariantError) -> Self {
        ClientError::Invariant(e)
    }
}

impl ClientState {
    /// Create a new client state
    #[cfg(feature = "std")]
    pub fn new() -> Self {
        Self {
            next_request_id: RequestId(1), // Start at 1, 0 is reserved
            pending: Vec::new(),
            current_seq: 0,
            stats: ClientStats::default(),
        }
    }
    
    #[cfg(not(feature = "std"))]
    pub fn new() -> Self {
        Self {
            next_request_id: RequestId(1),
            pending: [None; MAX_PENDING_REQUESTS],
            pending_count: 0,
            current_seq: 0,
            stats: ClientStats::default(),
        }
    }
    
    /// Get number of pending requests
    #[cfg(feature = "std")]
    pub fn pending_count(&self) -> usize {
        self.pending.len()
    }
    
    #[cfg(not(feature = "std"))]
    pub fn pending_count(&self) -> usize {
        self.pending_count
    }
    
    /// Check if there are pending requests
    pub fn has_pending(&self) -> bool {
        self.pending_count() > 0
    }
    
    /// Allocate a new request ID and track the pending request
    ///
    /// Returns the request ID and header to use
    pub fn begin_request(
        &mut self,
        escape_code: NvEscapeCode,
        device_idx: u8,
        payload_len: u32,
    ) -> Result<RequestHeader, ClientError> {
        // Check capacity
        if self.pending_count() >= MAX_PENDING_REQUESTS {
            return Err(ClientError::TooManyPending);
        }
        
        // Allocate request ID
        let request_id = self.next_request_id;
        self.next_request_id = self.next_request_id.next();
        
        // Track pending request
        let pending = PendingRequest {
            id: request_id,
            escape_code,
            sent_seq: self.current_seq,
        };
        self.current_seq += 1;
        
        self.add_pending(pending)?;
        self.stats.requests_sent += 1;
        
        // Create header
        let header = RequestHeader::new(request_id, escape_code, device_idx, payload_len);
        
        Ok(header)
    }
    
    /// Process a response and remove the matching pending request
    ///
    /// Returns the matched pending request for verification
    pub fn complete_request(
        &mut self,
        response: &ResponseHeader,
    ) -> Result<PendingRequest, ClientError> {
        let request_id = RequestId(response.request_id);
        
        // Find and remove the pending request
        let pending = self.remove_pending(request_id)?;
        
        self.stats.responses_received += 1;
        if response.status != 0 {
            self.stats.errors += 1;
        }
        
        Ok(pending)
    }
    
    /// Check all invariants
    pub fn check_invariants(&self) -> Result<(), InvariantError> {
        // Invariant 1: pending count within bounds
        if self.pending_count() > MAX_PENDING_REQUESTS {
            return Err(InvariantError::PendingCountExceeded {
                count: self.pending_count(),
                max: MAX_PENDING_REQUESTS,
            });
        }
        
        // Invariant 2: all pending IDs are unique
        // Invariant 3: all pending IDs < next_request_id
        #[cfg(feature = "std")]
        {
            let mut seen = std::collections::HashSet::new();
            for p in &self.pending {
                if !seen.insert(p.id) {
                    return Err(InvariantError::DuplicatePendingId(p.id.0));
                }
                if p.id >= self.next_request_id {
                    return Err(InvariantError::PendingIdExceedsNext {
                        pending_id: p.id.0,
                        next_id: self.next_request_id.0,
                    });
                }
            }
        }
        
        #[cfg(not(feature = "std"))]
        {
            for i in 0..MAX_PENDING_REQUESTS {
                if let Some(p) = &self.pending[i] {
                    // Check against all other pending
                    for j in (i + 1)..MAX_PENDING_REQUESTS {
                        if let Some(q) = &self.pending[j] {
                            if p.id == q.id {
                                return Err(InvariantError::DuplicatePendingId(p.id.0));
                            }
                        }
                    }
                    if p.id >= self.next_request_id {
                        return Err(InvariantError::PendingIdExceedsNext {
                            pending_id: p.id.0,
                            next_id: self.next_request_id.0,
                        });
                    }
                }
            }
        }
        
        Ok(())
    }
    
    /// Get iterator over pending requests (for timeout checking)
    #[cfg(feature = "std")]
    pub fn pending_iter(&self) -> impl Iterator<Item = &PendingRequest> {
        self.pending.iter()
    }
    
    #[cfg(not(feature = "std"))]
    pub fn pending_iter(&self) -> impl Iterator<Item = &PendingRequest> {
        self.pending.iter().filter_map(|p| p.as_ref())
    }
    
    /// Cancel a pending request (e.g., due to timeout)
    pub fn cancel_request(&mut self, request_id: RequestId) -> Result<PendingRequest, ClientError> {
        let pending = self.remove_pending(request_id)?;
        self.stats.timeouts += 1;
        Ok(pending)
    }
    
    // Internal: add a pending request
    #[cfg(feature = "std")]
    fn add_pending(&mut self, pending: PendingRequest) -> Result<(), ClientError> {
        // Check for duplicates (shouldn't happen with monotonic IDs, but verify)
        if self.pending.iter().any(|p| p.id == pending.id) {
            return Err(ClientError::DuplicateRequestId(pending.id));
        }
        self.pending.push(pending);
        Ok(())
    }
    
    #[cfg(not(feature = "std"))]
    fn add_pending(&mut self, pending: PendingRequest) -> Result<(), ClientError> {
        // Find empty slot
        for i in 0..MAX_PENDING_REQUESTS {
            if self.pending[i].is_none() {
                // Check for duplicates first
                for j in 0..MAX_PENDING_REQUESTS {
                    if let Some(p) = &self.pending[j] {
                        if p.id == pending.id {
                            return Err(ClientError::DuplicateRequestId(pending.id));
                        }
                    }
                }
                self.pending[i] = Some(pending);
                self.pending_count += 1;
                return Ok(());
            }
        }
        Err(ClientError::TooManyPending)
    }
    
    // Internal: remove a pending request by ID
    #[cfg(feature = "std")]
    fn remove_pending(&mut self, request_id: RequestId) -> Result<PendingRequest, ClientError> {
        let idx = self.pending
            .iter()
            .position(|p| p.id == request_id)
            .ok_or(ClientError::UnknownRequest(request_id))?;
        Ok(self.pending.remove(idx))
    }
    
    #[cfg(not(feature = "std"))]
    fn remove_pending(&mut self, request_id: RequestId) -> Result<PendingRequest, ClientError> {
        for i in 0..MAX_PENDING_REQUESTS {
            if let Some(p) = &self.pending[i] {
                if p.id == request_id {
                    let pending = *p;
                    self.pending[i] = None;
                    self.pending_count -= 1;
                    return Ok(pending);
                }
            }
        }
        Err(ClientError::UnknownRequest(request_id))
    }
}

impl Default for ClientState {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(all(test, feature = "std"))]
mod tests {
    use super::*;
    
    #[test]
    fn test_client_state_basic() {
        let mut client = ClientState::new();
        
        assert_eq!(client.pending_count(), 0);
        assert!(!client.has_pending());
        
        // Send a request
        let header = client.begin_request(NvEscapeCode::Alloc, 0, 32).unwrap();
        assert_eq!(header.request_id, 1);
        assert_eq!(client.pending_count(), 1);
        assert!(client.has_pending());
        
        // Check invariants
        assert!(client.check_invariants().is_ok());
        
        // Complete the request
        let response = ResponseHeader::new(RequestId(1), 0, 0);
        let pending = client.complete_request(&response).unwrap();
        assert_eq!(pending.id.0, 1);
        assert_eq!(client.pending_count(), 0);
    }
    
    #[test]
    fn test_client_state_multiple_requests() {
        let mut client = ClientState::new();
        
        // Send multiple requests
        let h1 = client.begin_request(NvEscapeCode::Alloc, 0, 32).unwrap();
        let h2 = client.begin_request(NvEscapeCode::Control, 1, 64).unwrap();
        let h3 = client.begin_request(NvEscapeCode::Free, 0, 16).unwrap();
        
        assert_eq!(h1.request_id, 1);
        assert_eq!(h2.request_id, 2);
        assert_eq!(h3.request_id, 3);
        assert_eq!(client.pending_count(), 3);
        
        // Complete out of order
        let r2 = ResponseHeader::new(RequestId(2), 0, 0);
        client.complete_request(&r2).unwrap();
        assert_eq!(client.pending_count(), 2);
        
        let r1 = ResponseHeader::new(RequestId(1), 0, 0);
        client.complete_request(&r1).unwrap();
        assert_eq!(client.pending_count(), 1);
        
        let r3 = ResponseHeader::new(RequestId(3), 0, 0);
        client.complete_request(&r3).unwrap();
        assert_eq!(client.pending_count(), 0);
    }
    
    #[test]
    fn test_client_state_unknown_response() {
        let mut client = ClientState::new();
        
        let r = ResponseHeader::new(RequestId(999), 0, 0);
        let result = client.complete_request(&r);
        
        assert!(matches!(result, Err(ClientError::UnknownRequest(_))));
    }
    
    #[test]
    fn test_client_state_too_many_pending() {
        let mut client = ClientState::new();
        
        // Fill up pending queue
        for _ in 0..MAX_PENDING_REQUESTS {
            client.begin_request(NvEscapeCode::Alloc, 0, 32).unwrap();
        }
        
        // Next should fail
        let result = client.begin_request(NvEscapeCode::Alloc, 0, 32);
        assert!(matches!(result, Err(ClientError::TooManyPending)));
    }
    
    #[test]
    fn test_client_state_invariants() {
        let mut client = ClientState::new();
        
        // Initially valid
        assert!(client.check_invariants().is_ok());
        
        // After some operations, still valid
        client.begin_request(NvEscapeCode::Alloc, 0, 32).unwrap();
        client.begin_request(NvEscapeCode::Control, 0, 64).unwrap();
        assert!(client.check_invariants().is_ok());
        
        let r = ResponseHeader::new(RequestId(1), 0, 0);
        client.complete_request(&r).unwrap();
        assert!(client.check_invariants().is_ok());
    }
}
