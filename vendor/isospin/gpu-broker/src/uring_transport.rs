//! io_uring Transport Layer
//!
//! This module implements the real transport layer using:
//! - Shared memory ring buffers for zero-copy message passing
//! - eventfd for signaling between VMs and broker
//! - io_uring for efficient async I/O
//!
//! Architecture:
//! ```text
//!  ┌─────────────────────────────────────────────────────────────────┐
//!  │                    Shared Memory Region                         │
//!  ├─────────────────────────────────────────────────────────────────┤
//!  │                                                                 │
//!  │   Request Ring (VM → Broker)                                    │
//!  │   ┌───────────────────────────────────────────────────────┐     │
//!  │   │ head │ tail │ mask │ entries...                       │     │
//!  │   └───────────────────────────────────────────────────────┘     │
//!  │                                                                 │
//!  │   Response Ring (Broker → VM)                                   │
//!  │   ┌───────────────────────────────────────────────────────┐     │
//!  │   │ head │ tail │ mask │ entries...                       │     │
//!  │   └───────────────────────────────────────────────────────┘     │
//!  │                                                                 │
//!  │   Data Buffer Pool                                              │
//!  │   ┌───────────────────────────────────────────────────────┐     │
//!  │   │ slot 0 │ slot 1 │ slot 2 │ ...                        │     │
//!  │   └───────────────────────────────────────────────────────┘     │
//!  │                                                                 │
//!  └─────────────────────────────────────────────────────────────────┘
//!
//!  VM writes request to ring → kicks eventfd → broker wakes
//!  Broker writes response to ring → kicks eventfd → VM wakes
//! ```
//!
//! The ring buffers use a lock-free SPSC (Single Producer Single Consumer)
//! design with atomic head/tail pointers. This is safe because:
//! - Request ring: VM is sole producer, Broker is sole consumer
//! - Response ring: Broker is sole producer, VM is sole consumer

use std::io;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::sync::atomic::{AtomicU32, Ordering};

use crate::broker::{Request, Response};
use crate::handle_table::ClientId;
use crate::transport::{wire, Transport};

// =============================================================================
// CONSTANTS
// =============================================================================

/// Size of ring buffer (must be power of 2)
pub const RING_SIZE: usize = 256;

/// Size of each entry slot in the data buffer
pub const ENTRY_SIZE: usize = 4096;

/// Total size needed for shared memory region per client
pub const SHMEM_SIZE: usize = RingHeader::SIZE * 2  // Two ring headers
    + RING_SIZE * std::mem::size_of::<RingEntry>() * 2  // Two ring arrays
    + RING_SIZE * ENTRY_SIZE * 2;  // Two data buffer pools

// =============================================================================
// RING BUFFER DATA STRUCTURES
// =============================================================================

/// Ring buffer header - placed at start of shared memory region
///
/// Uses atomic operations for lock-free SPSC synchronization.
#[repr(C, align(64))] // Cache line aligned
pub struct RingHeader {
    /// Head index (consumer reads from here, only consumer updates)
    pub head: AtomicU32,
    
    /// Padding to prevent false sharing
    _pad1: [u8; 60],
    
    /// Tail index (producer writes here, only producer updates)
    pub tail: AtomicU32,
    
    /// Padding to prevent false sharing
    _pad2: [u8; 60],
    
    /// Ring size mask (ring_size - 1, for fast modulo)
    pub mask: u32,
    
    /// Ring size
    pub size: u32,
    
    /// Padding
    _pad3: [u8; 56],
}

impl RingHeader {
    pub const SIZE: usize = 192; // 3 cache lines
    
    /// Initialize a new ring header
    pub fn init(&mut self, size: u32) {
        assert!(size.is_power_of_two(), "Ring size must be power of 2");
        self.head.store(0, Ordering::Relaxed);
        self.tail.store(0, Ordering::Relaxed);
        self.mask = size - 1;
        self.size = size;
    }
    
    /// Check if ring is empty (consumer's view)
    #[inline]
    pub fn is_empty(&self) -> bool {
        let head = self.head.load(Ordering::Acquire);
        let tail = self.tail.load(Ordering::Acquire);
        head == tail
    }
    
    /// Check if ring is full (producer's view)
    #[inline]
    pub fn is_full(&self) -> bool {
        let head = self.head.load(Ordering::Acquire);
        let tail = self.tail.load(Ordering::Acquire);
        tail.wrapping_sub(head) >= self.size
    }
    
    /// Get number of items in ring
    #[inline]
    pub fn len(&self) -> u32 {
        let head = self.head.load(Ordering::Acquire);
        let tail = self.tail.load(Ordering::Acquire);
        tail.wrapping_sub(head)
    }
    
    /// Producer: reserve a slot, returns index or None if full
    #[inline]
    pub fn producer_reserve(&self) -> Option<u32> {
        let head = self.head.load(Ordering::Acquire);
        let tail = self.tail.load(Ordering::Relaxed);
        
        if tail.wrapping_sub(head) >= self.size {
            return None; // Full
        }
        
        Some(tail & self.mask)
    }
    
    /// Producer: commit after writing to slot
    #[inline]
    pub fn producer_commit(&self) {
        // Ensure writes to slot are visible before advancing tail
        let tail = self.tail.load(Ordering::Relaxed);
        self.tail.store(tail.wrapping_add(1), Ordering::Release);
    }
    
    /// Consumer: get slot to read, returns index or None if empty
    #[inline]
    pub fn consumer_peek(&self) -> Option<u32> {
        let head = self.head.load(Ordering::Relaxed);
        let tail = self.tail.load(Ordering::Acquire);
        
        if head == tail {
            return None; // Empty
        }
        
        Some(head & self.mask)
    }
    
    /// Consumer: release slot after reading
    #[inline]
    pub fn consumer_release(&self) {
        // Ensure reads from slot are complete before advancing head
        let head = self.head.load(Ordering::Relaxed);
        self.head.store(head.wrapping_add(1), Ordering::Release);
    }
}

/// Ring entry - metadata for each slot
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct RingEntry {
    /// Offset into data buffer pool
    pub data_offset: u32,
    
    /// Length of data
    pub data_len: u32,
    
    /// Flags (reserved)
    pub flags: u32,
    
    /// Sequence number for ordering
    pub seq: u32,
}

impl RingEntry {
    pub const SIZE: usize = 16;
}

// =============================================================================
// SHARED MEMORY LAYOUT
// =============================================================================

/// Layout of shared memory region
///
/// ```text
/// Offset 0:                      Request Ring Header (192 bytes)
/// Offset 192:                    Response Ring Header (192 bytes)
/// Offset 384:                    Request Ring Entries (256 * 16 = 4096 bytes)
/// Offset 4480:                   Response Ring Entries (256 * 16 = 4096 bytes)
/// Offset 8576:                   Request Data Pool (256 * 4096 = 1MB)
/// Offset 8576 + 1MB:             Response Data Pool (256 * 4096 = 1MB)
/// ```
pub struct SharedMemoryLayout {
    pub request_header_offset: usize,
    pub response_header_offset: usize,
    pub request_entries_offset: usize,
    pub response_entries_offset: usize,
    pub request_data_offset: usize,
    pub response_data_offset: usize,
    pub total_size: usize,
}

impl SharedMemoryLayout {
    pub fn new(ring_size: usize, entry_size: usize) -> Self {
        let header_size = RingHeader::SIZE;
        let entries_size = ring_size * RingEntry::SIZE;
        let data_size = ring_size * entry_size;
        
        let request_header_offset = 0;
        let response_header_offset = header_size;
        let request_entries_offset = header_size * 2;
        let response_entries_offset = request_entries_offset + entries_size;
        let request_data_offset = response_entries_offset + entries_size;
        let response_data_offset = request_data_offset + data_size;
        let total_size = response_data_offset + data_size;
        
        Self {
            request_header_offset,
            response_header_offset,
            request_entries_offset,
            response_entries_offset,
            request_data_offset,
            response_data_offset,
            total_size,
        }
    }
    
    pub fn default_layout() -> Self {
        Self::new(RING_SIZE, ENTRY_SIZE)
    }
}

// =============================================================================
// SHARED MEMORY REGION
// =============================================================================

/// A mapped shared memory region
pub struct SharedMemoryRegion {
    /// Base address of mapping
    ptr: *mut u8,
    
    /// Size of mapping
    size: usize,
    
    /// Layout information
    layout: SharedMemoryLayout,
    
    /// File descriptor (kept open for sharing)
    #[allow(dead_code)]
    fd: Option<OwnedFd>,
}

// Safety: SharedMemoryRegion can be sent between threads as long as
// we ensure proper synchronization through the atomic ring operations
unsafe impl Send for SharedMemoryRegion {}

impl SharedMemoryRegion {
    /// Create a new anonymous shared memory region
    pub fn create_anonymous(size: usize) -> io::Result<Self> {
        let layout = SharedMemoryLayout::new(RING_SIZE, ENTRY_SIZE);
        assert!(size >= layout.total_size, "Size too small for layout");
        
        // Use memfd_create for anonymous shared memory
        let fd = unsafe {
            let fd = libc::memfd_create(
                b"gpu-broker-shmem\0".as_ptr() as *const libc::c_char,
                libc::MFD_CLOEXEC,
            );
            if fd < 0 {
                return Err(io::Error::last_os_error());
            }
            OwnedFd::from_raw_fd(fd)
        };
        
        // Set size
        unsafe {
            if libc::ftruncate(fd.as_raw_fd(), size as libc::off_t) < 0 {
                return Err(io::Error::last_os_error());
            }
        }
        
        // Map it
        let ptr = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                size,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_SHARED,
                fd.as_raw_fd(),
                0,
            )
        };
        
        if ptr == libc::MAP_FAILED {
            return Err(io::Error::last_os_error());
        }
        
        let mut region = Self {
            ptr: ptr as *mut u8,
            size,
            layout,
            fd: Some(fd),
        };
        
        // Initialize ring headers
        region.init_rings();
        
        Ok(region)
    }
    
    /// Map an existing shared memory region from fd
    pub fn from_fd(fd: RawFd, size: usize) -> io::Result<Self> {
        let layout = SharedMemoryLayout::new(RING_SIZE, ENTRY_SIZE);
        
        let ptr = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                size,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_SHARED,
                fd,
                0,
            )
        };
        
        if ptr == libc::MAP_FAILED {
            return Err(io::Error::last_os_error());
        }
        
        Ok(Self {
            ptr: ptr as *mut u8,
            size,
            layout,
            fd: None, // Caller owns the fd
        })
    }
    
    /// Initialize ring buffers
    fn init_rings(&mut self) {
        let req_header = self.request_ring_header_mut();
        req_header.init(RING_SIZE as u32);
        
        let resp_header = self.response_ring_header_mut();
        resp_header.init(RING_SIZE as u32);
    }
    
    /// Get the file descriptor for sharing
    pub fn fd(&self) -> Option<RawFd> {
        self.fd.as_ref().map(|f| f.as_raw_fd())
    }
    
    // Accessors for ring components
    
    fn request_ring_header(&self) -> &RingHeader {
        unsafe {
            &*(self.ptr.add(self.layout.request_header_offset) as *const RingHeader)
        }
    }
    
    fn request_ring_header_mut(&mut self) -> &mut RingHeader {
        unsafe {
            &mut *(self.ptr.add(self.layout.request_header_offset) as *mut RingHeader)
        }
    }
    
    fn response_ring_header(&self) -> &RingHeader {
        unsafe {
            &*(self.ptr.add(self.layout.response_header_offset) as *const RingHeader)
        }
    }
    
    fn response_ring_header_mut(&mut self) -> &mut RingHeader {
        unsafe {
            &mut *(self.ptr.add(self.layout.response_header_offset) as *mut RingHeader)
        }
    }
    
    fn request_entry(&self, idx: u32) -> &RingEntry {
        unsafe {
            let base = self.ptr.add(self.layout.request_entries_offset) as *const RingEntry;
            &*base.add(idx as usize)
        }
    }
    
    fn request_entry_mut(&mut self, idx: u32) -> &mut RingEntry {
        unsafe {
            let base = self.ptr.add(self.layout.request_entries_offset) as *mut RingEntry;
            &mut *base.add(idx as usize)
        }
    }
    
    fn response_entry(&self, idx: u32) -> &RingEntry {
        unsafe {
            let base = self.ptr.add(self.layout.response_entries_offset) as *const RingEntry;
            &*base.add(idx as usize)
        }
    }
    
    fn response_entry_mut(&mut self, idx: u32) -> &mut RingEntry {
        unsafe {
            let base = self.ptr.add(self.layout.response_entries_offset) as *mut RingEntry;
            &mut *base.add(idx as usize)
        }
    }
    
    fn request_data_slot(&self, idx: u32) -> &[u8] {
        unsafe {
            let base = self.ptr.add(self.layout.request_data_offset);
            let slot = base.add(idx as usize * ENTRY_SIZE);
            std::slice::from_raw_parts(slot, ENTRY_SIZE)
        }
    }
    
    fn request_data_slot_mut(&mut self, idx: u32) -> &mut [u8] {
        unsafe {
            let base = self.ptr.add(self.layout.request_data_offset);
            let slot = base.add(idx as usize * ENTRY_SIZE);
            std::slice::from_raw_parts_mut(slot, ENTRY_SIZE)
        }
    }
    
    fn response_data_slot(&self, idx: u32) -> &[u8] {
        unsafe {
            let base = self.ptr.add(self.layout.response_data_offset);
            let slot = base.add(idx as usize * ENTRY_SIZE);
            std::slice::from_raw_parts(slot, ENTRY_SIZE)
        }
    }
    
    fn response_data_slot_mut(&mut self, idx: u32) -> &mut [u8] {
        unsafe {
            let base = self.ptr.add(self.layout.response_data_offset);
            let slot = base.add(idx as usize * ENTRY_SIZE);
            std::slice::from_raw_parts_mut(slot, ENTRY_SIZE)
        }
    }
}

impl Drop for SharedMemoryRegion {
    fn drop(&mut self) {
        unsafe {
            libc::munmap(self.ptr as *mut libc::c_void, self.size);
        }
    }
}

// =============================================================================
// CLIENT CONNECTION
// =============================================================================

/// A client's connection to the broker (one per VM)
pub struct ClientConnection {
    /// Client ID
    pub client_id: ClientId,
    
    /// Shared memory region with this client
    pub shmem: SharedMemoryRegion,
    
    /// Eventfd for waking broker when requests arrive
    pub request_eventfd: OwnedFd,
    
    /// Eventfd for waking client when responses arrive
    pub response_eventfd: OwnedFd,
    
    /// Sequence counter for this client
    seq: u64,
}

impl ClientConnection {
    /// Create a new client connection with new shared memory
    pub fn new(client_id: ClientId) -> io::Result<Self> {
        let layout = SharedMemoryLayout::default_layout();
        let shmem = SharedMemoryRegion::create_anonymous(layout.total_size)?;
        
        // Create eventfds
        let request_eventfd = create_eventfd()?;
        let response_eventfd = create_eventfd()?;
        
        Ok(Self {
            client_id,
            shmem,
            request_eventfd,
            response_eventfd,
            seq: 0,
        })
    }
    
    /// Check if there are pending requests from this client
    pub fn has_pending_requests(&self) -> bool {
        !self.shmem.request_ring_header().is_empty()
    }
    
    /// Try to receive a request from this client (non-blocking)
    pub fn try_recv_request(&mut self) -> Option<Request> {
        let header = self.shmem.request_ring_header();
        let idx = header.consumer_peek()?;
        
        let entry = self.shmem.request_entry(idx);
        let data = &self.shmem.request_data_slot(idx)[..entry.data_len as usize];
        
        // Decode the request
        let request = wire::decode_request(data)?;
        
        // Release the slot
        self.shmem.request_ring_header().consumer_release();
        
        Some(request)
    }
    
    /// Send a response to this client (non-blocking, returns false if full)
    pub fn try_send_response(&mut self, response: &Response) -> bool {
        let header = self.shmem.response_ring_header();
        let idx = match header.producer_reserve() {
            Some(idx) => idx,
            None => return false, // Ring full
        };
        
        // Encode the response
        let encoded = encode_response(response);
        if encoded.len() > ENTRY_SIZE {
            return false; // Response too large
        }
        
        // Write data
        let slot = self.shmem.response_data_slot_mut(idx);
        slot[..encoded.len()].copy_from_slice(&encoded);
        
        // Write entry metadata
        let entry = self.shmem.response_entry_mut(idx);
        entry.data_offset = (idx as usize * ENTRY_SIZE) as u32;
        entry.data_len = encoded.len() as u32;
        entry.seq = self.seq as u32;
        self.seq += 1;
        
        // Commit
        self.shmem.response_ring_header().producer_commit();
        
        // Signal client
        let _ = signal_eventfd(&self.response_eventfd);
        
        true
    }
}

// =============================================================================
// IO_URING TRANSPORT
// =============================================================================

/// The main io_uring transport implementation
pub struct IoUringTransport {
    /// io_uring instance
    ring: io_uring::IoUring,
    
    /// Connected clients
    clients: std::collections::HashMap<ClientId, ClientConnection>,
    
    /// Pending requests (pulled from all clients)
    pending_requests: std::collections::VecDeque<Request>,
    
    /// Responses waiting to be sent
    pending_responses: Vec<Response>,
}

impl IoUringTransport {
    /// Create a new io_uring transport
    pub fn new(queue_depth: u32) -> io::Result<Self> {
        let ring = io_uring::IoUring::new(queue_depth)?;
        
        Ok(Self {
            ring,
            clients: std::collections::HashMap::new(),
            pending_requests: std::collections::VecDeque::new(),
            pending_responses: Vec::new(),
        })
    }
    
    /// Register a new client
    pub fn register_client(&mut self, client_id: ClientId) -> io::Result<&ClientConnection> {
        let conn = ClientConnection::new(client_id)?;
        self.clients.insert(client_id, conn);
        Ok(self.clients.get(&client_id).unwrap())
    }
    
    /// Unregister a client
    pub fn unregister_client(&mut self, client_id: ClientId) -> Option<ClientConnection> {
        self.clients.remove(&client_id)
    }
    
    /// Get client connection info (for passing to VM)
    pub fn get_client_info(&self, client_id: ClientId) -> Option<ClientConnectionInfo> {
        self.clients.get(&client_id).map(|conn| ClientConnectionInfo {
            client_id: conn.client_id,
            shmem_fd: conn.shmem.fd(),
            shmem_size: conn.shmem.size,
            request_eventfd: conn.request_eventfd.as_raw_fd(),
            response_eventfd: conn.response_eventfd.as_raw_fd(),
        })
    }
    
    /// Poll all clients for new requests (non-blocking)
    fn poll_clients(&mut self) {
        for conn in self.clients.values_mut() {
            while let Some(request) = conn.try_recv_request() {
                self.pending_requests.push_back(request);
            }
        }
    }
    
    /// Wait for events using io_uring (blocking with timeout)
    pub fn wait_for_events(&mut self, timeout_ms: Option<u32>) -> io::Result<usize> {
        // Submit any pending operations and wait
        let wait_count = if timeout_ms.is_some() { 1 } else { 0 };
        
        // For now, just poll clients - real impl would use eventfd with io_uring
        self.poll_clients();
        
        if self.pending_requests.is_empty() && timeout_ms.is_some() {
            // Sleep briefly to avoid busy-waiting
            std::thread::sleep(std::time::Duration::from_millis(
                timeout_ms.unwrap_or(10) as u64
            ));
            self.poll_clients();
        }
        
        Ok(self.pending_requests.len())
    }
    
    /// Flush all pending responses to their clients
    pub fn flush_responses(&mut self) {
        let responses = std::mem::take(&mut self.pending_responses);
        
        for response in responses {
            if let Some(conn) = self.clients.get_mut(&response.client) {
                if !conn.try_send_response(&response) {
                    // Ring full, put back for retry
                    self.pending_responses.push(response);
                }
            }
            // If client not found, drop the response
        }
    }
}

impl Transport for IoUringTransport {
    fn recv(&mut self) -> Option<Request> {
        // First check if we have any buffered requests
        if let Some(req) = self.pending_requests.pop_front() {
            return Some(req);
        }
        
        // Poll clients for new requests
        self.poll_clients();
        
        self.pending_requests.pop_front()
    }
    
    fn send(&mut self, response: Response) {
        self.pending_responses.push(response);
    }
    
    fn has_pending(&self) -> bool {
        !self.pending_requests.is_empty()
    }
    
    fn flush(&mut self) {
        self.flush_responses();
    }
}

// =============================================================================
// CLIENT CONNECTION INFO (for passing to VM)
// =============================================================================

/// Information needed to connect to the broker from a VM
#[derive(Debug, Clone)]
pub struct ClientConnectionInfo {
    pub client_id: ClientId,
    pub shmem_fd: Option<RawFd>,
    pub shmem_size: usize,
    pub request_eventfd: RawFd,
    pub response_eventfd: RawFd,
}

// =============================================================================
// HELPER FUNCTIONS
// =============================================================================

/// Create an eventfd
fn create_eventfd() -> io::Result<OwnedFd> {
    let fd = unsafe {
        libc::eventfd(0, libc::EFD_CLOEXEC | libc::EFD_NONBLOCK)
    };
    
    if fd < 0 {
        return Err(io::Error::last_os_error());
    }
    
    Ok(unsafe { OwnedFd::from_raw_fd(fd) })
}

/// Signal an eventfd
fn signal_eventfd(fd: &OwnedFd) -> io::Result<()> {
    let val: u64 = 1;
    let ret = unsafe {
        libc::write(
            fd.as_raw_fd(),
            &val as *const u64 as *const libc::c_void,
            std::mem::size_of::<u64>(),
        )
    };
    
    if ret < 0 {
        return Err(io::Error::last_os_error());
    }
    
    Ok(())
}

/// Read from eventfd (resets counter)
#[allow(dead_code)]
fn read_eventfd(fd: &OwnedFd) -> io::Result<u64> {
    let mut val: u64 = 0;
    let ret = unsafe {
        libc::read(
            fd.as_raw_fd(),
            &mut val as *mut u64 as *mut libc::c_void,
            std::mem::size_of::<u64>(),
        )
    };
    
    if ret < 0 {
        let err = io::Error::last_os_error();
        if err.kind() == io::ErrorKind::WouldBlock {
            return Ok(0);
        }
        return Err(err);
    }
    
    Ok(val)
}

/// Encode a response to bytes (temporary - should move to wire module)
fn encode_response(response: &Response) -> Vec<u8> {
    use crate::broker::OperationResult;
    
    // Simple encoding: client_id (8) + seq (8) + success (1) + result_len (4) + result
    let mut buf = Vec::with_capacity(64);
    
    buf.extend_from_slice(&response.client.0.to_le_bytes());
    buf.extend_from_slice(&response.seq.0.to_le_bytes());
    
    match &response.result {
        Ok(result) => {
            buf.push(1); // success
            match result {
                OperationResult::Registered => {
                    buf.extend_from_slice(&0u32.to_le_bytes()); // result type
                }
                OperationResult::Unregistered { freed_handles } => {
                    buf.extend_from_slice(&1u32.to_le_bytes());
                    buf.extend_from_slice(&(freed_handles.len() as u32).to_le_bytes());
                    for h in freed_handles {
                        buf.extend_from_slice(&h.0.to_le_bytes());
                    }
                }
                OperationResult::Allocated { real_handle } => {
                    buf.extend_from_slice(&2u32.to_le_bytes());
                    buf.extend_from_slice(&real_handle.0.to_le_bytes());
                }
                OperationResult::Freed { real_handle } => {
                    buf.extend_from_slice(&3u32.to_le_bytes());
                    buf.extend_from_slice(&real_handle.0.to_le_bytes());
                }
                OperationResult::ControlComplete { status, out_params } => {
                    buf.extend_from_slice(&4u32.to_le_bytes());
                    buf.extend_from_slice(&status.to_le_bytes());
                    buf.extend_from_slice(&(out_params.len() as u32).to_le_bytes());
                    buf.extend_from_slice(out_params);
                }
                OperationResult::Mapped { address } => {
                    buf.extend_from_slice(&5u32.to_le_bytes());
                    buf.extend_from_slice(&address.to_le_bytes());
                }
                OperationResult::Unmapped => {
                    buf.extend_from_slice(&6u32.to_le_bytes());
                }
                
                // Top-level escape results
                OperationResult::CardInfoResult { num_gpus, card_info } => {
                    buf.extend_from_slice(&7u32.to_le_bytes());
                    buf.extend_from_slice(&num_gpus.to_le_bytes());
                    buf.extend_from_slice(card_info);
                }
                OperationResult::VersionCheckResult { reply, version_string } => {
                    buf.extend_from_slice(&8u32.to_le_bytes());
                    buf.extend_from_slice(&reply.to_le_bytes());
                    buf.extend_from_slice(&(version_string.len() as u32).to_le_bytes());
                    buf.extend_from_slice(version_string);
                }
                OperationResult::FdRegistered => {
                    buf.extend_from_slice(&9u32.to_le_bytes());
                }
                OperationResult::OsEventAllocated { status } => {
                    buf.extend_from_slice(&10u32.to_le_bytes());
                    buf.extend_from_slice(&status.to_le_bytes());
                }
                OperationResult::OsEventFreed { status } => {
                    buf.extend_from_slice(&11u32.to_le_bytes());
                    buf.extend_from_slice(&status.to_le_bytes());
                }
                OperationResult::GpusAttached => {
                    buf.extend_from_slice(&12u32.to_le_bytes());
                }
                OperationResult::StatusCodeResult { status } => {
                    buf.extend_from_slice(&13u32.to_le_bytes());
                    buf.extend_from_slice(&status.to_le_bytes());
                }
            }
        }
        Err(err) => {
            buf.push(0); // error
            let err_str = format!("{:?}", err);
            buf.extend_from_slice(&(err_str.len() as u32).to_le_bytes());
            buf.extend_from_slice(err_str.as_bytes());
        }
    }
    
    buf
}

// =============================================================================
// TESTS
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;
    use crate::broker::{Operation, SeqNo};
    
    #[test]
    fn test_ring_header_basic() {
        let mut header = RingHeader {
            head: AtomicU32::new(0),
            _pad1: [0; 60],
            tail: AtomicU32::new(0),
            _pad2: [0; 60],
            mask: 0,
            size: 0,
            _pad3: [0; 56],
        };
        
        header.init(16);
        
        assert!(header.is_empty());
        assert!(!header.is_full());
        assert_eq!(header.len(), 0);
        
        // Reserve and commit some entries
        for i in 0..16 {
            let idx = header.producer_reserve();
            assert!(idx.is_some(), "Failed to reserve slot {}", i);
            header.producer_commit();
        }
        
        assert!(!header.is_empty());
        assert!(header.is_full());
        assert_eq!(header.len(), 16);
        
        // Can't reserve when full
        assert!(header.producer_reserve().is_none());
        
        // Consume some
        for _ in 0..8 {
            let idx = header.consumer_peek();
            assert!(idx.is_some());
            header.consumer_release();
        }
        
        assert!(!header.is_empty());
        assert!(!header.is_full());
        assert_eq!(header.len(), 8);
    }
    
    #[test]
    fn test_shared_memory_region() {
        let layout = SharedMemoryLayout::default_layout();
        let shmem = SharedMemoryRegion::create_anonymous(layout.total_size);
        
        assert!(shmem.is_ok(), "Failed to create shared memory: {:?}", shmem.err());
        
        let mut shmem = shmem.unwrap();
        
        // Verify ring headers are initialized
        assert!(shmem.request_ring_header().is_empty());
        assert!(shmem.response_ring_header().is_empty());
        
        // Test request ring operations
        let header = shmem.request_ring_header();
        let idx = header.producer_reserve().unwrap();
        
        // Write some data
        let data = b"test request data";
        let slot = shmem.request_data_slot_mut(idx);
        slot[..data.len()].copy_from_slice(data);
        
        let entry = shmem.request_entry_mut(idx);
        entry.data_len = data.len() as u32;
        
        shmem.request_ring_header().producer_commit();
        
        // Verify we can read it back
        assert!(!shmem.request_ring_header().is_empty());
        let read_idx = shmem.request_ring_header().consumer_peek().unwrap();
        let read_entry = shmem.request_entry(read_idx);
        let read_data = &shmem.request_data_slot(read_idx)[..read_entry.data_len as usize];
        assert_eq!(read_data, data);
    }
    
    #[test]
    fn test_client_connection() {
        let client_id = ClientId(42);
        let conn = ClientConnection::new(client_id);
        
        assert!(conn.is_ok(), "Failed to create connection: {:?}", conn.err());
        
        let mut conn = conn.unwrap();
        assert_eq!(conn.client_id, client_id);
        assert!(!conn.has_pending_requests());
        
        // Test sending a response
        let response = Response {
            client: client_id,
            seq: SeqNo(0),
            result: Ok(crate::broker::OperationResult::Registered),
        };
        
        assert!(conn.try_send_response(&response));
    }
    
    #[test]
    fn test_iouring_transport() {
        let transport = IoUringTransport::new(32);
        assert!(transport.is_ok(), "Failed to create transport: {:?}", transport.err());
        
        let mut transport = transport.unwrap();
        
        // Register a client
        let client_id = ClientId(1);
        let result = transport.register_client(client_id);
        assert!(result.is_ok());
        
        // Get client info
        let info = transport.get_client_info(client_id);
        assert!(info.is_some());
        let info = info.unwrap();
        assert_eq!(info.client_id, client_id);
        
        // No pending requests initially
        assert!(!transport.has_pending());
    }
    
    #[test]
    fn test_eventfd() {
        let fd = create_eventfd();
        assert!(fd.is_ok());
        
        let fd = fd.unwrap();
        
        // Signal it
        assert!(signal_eventfd(&fd).is_ok());
        
        // Read it back
        let val = read_eventfd(&fd);
        assert!(val.is_ok());
        assert_eq!(val.unwrap(), 1);
        
        // Should be empty now
        let val = read_eventfd(&fd);
        assert!(val.is_ok());
        assert_eq!(val.unwrap(), 0);
    }
}

#[cfg(test)]
mod proptests {
    use super::*;
    use proptest::prelude::*;
    
    proptest! {
        /// Property: Ring buffer never overflows or underflows
        #[test]
        fn ring_buffer_bounds(
            ops in proptest::collection::vec(
                prop_oneof![Just(true), Just(false)], // true = produce, false = consume
                0..1000
            )
        ) {
            let mut header = RingHeader {
                head: AtomicU32::new(0),
                _pad1: [0; 60],
                tail: AtomicU32::new(0),
                _pad2: [0; 60],
                mask: 0,
                size: 0,
                _pad3: [0; 56],
            };
            header.init(64);
            
            let mut count = 0i32;
            
            for is_produce in ops {
                if is_produce {
                    if header.producer_reserve().is_some() {
                        header.producer_commit();
                        count += 1;
                    }
                } else {
                    if header.consumer_peek().is_some() {
                        header.consumer_release();
                        count -= 1;
                    }
                }
                
                // Invariants
                prop_assert!(count >= 0, "Underflow detected");
                prop_assert!(count <= 64, "Overflow detected");
                prop_assert_eq!(header.len() as i32, count);
            }
        }
        
        /// Property: Data written to ring can be read back correctly
        #[test]
        fn ring_data_integrity(
            data_items in proptest::collection::vec(
                proptest::collection::vec(any::<u8>(), 0..100),
                1..50
            )
        ) {
            let layout = SharedMemoryLayout::default_layout();
            let mut shmem = SharedMemoryRegion::create_anonymous(layout.total_size)
                .expect("Failed to create shmem");
            
            // Write all items
            for (i, data) in data_items.iter().enumerate() {
                let header = shmem.request_ring_header();
                if let Some(idx) = header.producer_reserve() {
                    let slot = shmem.request_data_slot_mut(idx);
                    let len = data.len().min(ENTRY_SIZE);
                    slot[..len].copy_from_slice(&data[..len]);
                    
                    let entry = shmem.request_entry_mut(idx);
                    entry.data_len = len as u32;
                    entry.seq = i as u32;
                    
                    shmem.request_ring_header().producer_commit();
                }
            }
            
            // Read all items back and verify
            let mut read_count = 0;
            while let Some(idx) = shmem.request_ring_header().consumer_peek() {
                let entry = shmem.request_entry(idx);
                let read_data = &shmem.request_data_slot(idx)[..entry.data_len as usize];
                
                let original = &data_items[entry.seq as usize];
                let expected_len = original.len().min(ENTRY_SIZE);
                
                prop_assert_eq!(read_data.len(), expected_len);
                prop_assert_eq!(read_data, &original[..expected_len]);
                
                shmem.request_ring_header().consumer_release();
                read_count += 1;
            }
            
            prop_assert_eq!(read_count, data_items.len().min(RING_SIZE));
        }
    }
}
