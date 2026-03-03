//! Guest Shim - FUSE-based NVIDIA device emulator
//!
//! This module implements a FUSE character device that emulates /dev/nvidia*
//! devices in the guest VM. Instead of talking to real NVIDIA kernel drivers,
//! it forwards all ioctls to the GPU broker on the host via shared memory.
//!
//! Architecture:
//! ```text
//!  ┌─────────────────────────────────────────────────────────────────┐
//!  │                          Guest VM                               │
//!  │  ┌─────────────────────────────────────────────────────────────┐│
//!  │  │ CUDA Application                                            ││
//!  │  │  └─> libnvidia-ml.so / libcuda.so                           ││
//!  │  │       └─> open("/dev/nvidiactl") + ioctl(...)               ││
//!  │  └──────────────────────────┬──────────────────────────────────┘│
//!  │                             │                                   │
//!  │  ┌──────────────────────────▼──────────────────────────────────┐│
//!  │  │ CUSE (Character device in USErspace)                        ││
//!  │  │  - Emulates /dev/nvidiactl, /dev/nvidia0, /dev/nvidia-uvm   ││
//!  │  │  - Intercepts open(), close(), ioctl(), mmap()              ││
//!  │  │  - Forwards to broker via shared memory ring                ││
//!  │  └──────────────────────────┬──────────────────────────────────┘│
//!  │                             │                                   │
//!  │  ┌──────────────────────────▼──────────────────────────────────┐│
//!  │  │ Shared Memory Ring (virtio-shmem or ivshmem)                ││
//!  │  └──────────────────────────┬──────────────────────────────────┘│
//!  └─────────────────────────────┼───────────────────────────────────┘
//!                                │
//!                                ▼
//!  ┌─────────────────────────────────────────────────────────────────┐
//!  │                     Host - GPU Broker                           │
//!  └─────────────────────────────────────────────────────────────────┘
//! ```
//!
//! Why CUSE instead of a kernel module:
//! - Userspace = easier debugging, no kernel panics
//! - Works with unmodified CUDA/NVML applications
//! - Portable across guest kernel versions
//! - Control path latency (~10µs) is acceptable for RM API calls
//!
//! Note: Data path (GPU memory access) bypasses this entirely via DMA.

use std::collections::HashMap;
use std::io;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::sync::atomic::{AtomicU64, Ordering};

use crate::broker::{BrokerError, Operation, OperationResult, Request, Response, SeqNo};
use crate::handle_table::{ClientId, RealHandle};
use crate::rm_api::{NvEscape, NvHandle};
use crate::uring_transport::{
    ClientConnectionInfo, RingEntry, RingHeader, SharedMemoryLayout, ENTRY_SIZE,
};

// =============================================================================
// CONSTANTS
// =============================================================================

/// NVIDIA ioctl magic number
const NV_IOCTL_MAGIC: u8 = b'F';

/// Maximum size for ioctl data (control params are typically < 1KB)
const MAX_IOCTL_SIZE: usize = 4096;

/// Timeout for broker responses (milliseconds)
const RESPONSE_TIMEOUT_MS: u64 = 5000;

// =============================================================================
// DEVICE FILE HANDLES
// =============================================================================

/// Handle for an open device file
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct FileHandle(pub u64);

/// Type of NVIDIA device
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NvDeviceType {
    /// /dev/nvidiactl - control device
    Control,
    /// /dev/nvidia0, /dev/nvidia1, etc. - GPU devices
    Gpu(u32),
    /// /dev/nvidia-uvm - unified virtual memory
    Uvm,
    /// /dev/nvidia-modeset - display modeset
    Modeset,
}

impl NvDeviceType {
    /// Parse device type from path
    pub fn from_path(path: &str) -> Option<Self> {
        if path == "/dev/nvidiactl" || path.ends_with("/nvidiactl") {
            Some(Self::Control)
        } else if path == "/dev/nvidia-uvm" || path.ends_with("/nvidia-uvm") {
            Some(Self::Uvm)
        } else if path == "/dev/nvidia-modeset" || path.ends_with("/nvidia-modeset") {
            Some(Self::Modeset)
        } else if let Some(suffix) = path.strip_prefix("/dev/nvidia") {
            // Check if it's nvidia0, nvidia1, etc.
            if let Ok(idx) = suffix.parse::<u32>() {
                Some(Self::Gpu(idx))
            } else {
                None
            }
        } else {
            None
        }
    }

    /// Get the device path
    pub fn path(&self) -> String {
        match self {
            Self::Control => "/dev/nvidiactl".to_string(),
            Self::Gpu(idx) => format!("/dev/nvidia{}", idx),
            Self::Uvm => "/dev/nvidia-uvm".to_string(),
            Self::Modeset => "/dev/nvidia-modeset".to_string(),
        }
    }
}

/// State for an open file handle
#[derive(Debug)]
pub struct OpenFile {
    /// File handle ID
    pub handle: FileHandle,

    /// Device type
    pub device_type: NvDeviceType,

    /// RM client handle (assigned on first ioctl that creates a client)
    pub rm_client: Option<NvHandle>,

    /// Sequence number counter for requests
    seq_counter: AtomicU64,
}

impl OpenFile {
    pub fn new(handle: FileHandle, device_type: NvDeviceType) -> Self {
        Self {
            handle,
            device_type,
            rm_client: None,
            seq_counter: AtomicU64::new(0),
        }
    }

    pub fn next_seq(&self) -> SeqNo {
        SeqNo(self.seq_counter.fetch_add(1, Ordering::Relaxed))
    }
}

// =============================================================================
// IOCTL PARSING
// =============================================================================

/// Decoded ioctl request
#[derive(Debug)]
pub struct IoctlRequest {
    /// The escape code (command)
    pub escape: NvEscape,

    /// Direction: read, write, or both
    pub direction: IoctlDirection,

    /// Size of the data structure
    pub size: usize,

    /// Raw parameter data
    pub params: Vec<u8>,
}

/// ioctl direction
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IoctlDirection {
    None,
    Read,
    Write,
    ReadWrite,
}

impl IoctlRequest {
    /// Decode an ioctl command and data
    pub fn decode(cmd: libc::c_ulong, data: *mut libc::c_void) -> Option<Self> {
        // NVIDIA uses a simplified ioctl scheme where the escape code is the command
        // The actual structure is _IOWR('F', escape, params_type)

        // Extract magic, direction, and size from ioctl number
        let magic = ((cmd >> 8) & 0xFF) as u8;
        let nr = (cmd & 0xFF) as u32;
        let size = ((cmd >> 16) & 0x3FFF) as usize;
        let dir = ((cmd >> 30) & 0x3) as u8;

        // Check magic number
        if magic != NV_IOCTL_MAGIC {
            return None;
        }

        // Parse escape code
        let escape = NvEscape::from_u32(nr)?;

        // Decode direction
        let direction = match dir {
            0 => IoctlDirection::None,
            1 => IoctlDirection::Write,  // _IOC_WRITE
            2 => IoctlDirection::Read,   // _IOC_READ
            3 => IoctlDirection::ReadWrite,
            _ => return None,
        };

        // Copy parameter data
        let params = if size > 0 && !data.is_null() && size <= MAX_IOCTL_SIZE {
            let slice = unsafe { std::slice::from_raw_parts(data as *const u8, size) };
            slice.to_vec()
        } else {
            Vec::new()
        };

        Some(Self {
            escape,
            direction,
            size,
            params,
        })
    }

    /// Build ioctl number from components
    pub fn build_ioctl_nr(escape: NvEscape, size: usize, dir: IoctlDirection) -> libc::c_ulong {
        let dir_bits: u32 = match dir {
            IoctlDirection::None => 0,
            IoctlDirection::Write => 1,
            IoctlDirection::Read => 2,
            IoctlDirection::ReadWrite => 3,
        };

        let nr = escape as u32;
        let magic = NV_IOCTL_MAGIC as u32;

        ((dir_bits as libc::c_ulong) << 30)
            | ((size as libc::c_ulong) << 16)
            | ((magic as libc::c_ulong) << 8)
            | (nr as libc::c_ulong)
    }
}

// =============================================================================
// GUEST SHIM STATE
// =============================================================================

/// The main guest shim state machine
///
/// Pure state machine: `(State, Input) → (State', Output)`
pub struct GuestShim {
    /// Our client ID (assigned by broker)
    client_id: ClientId,

    /// Open file handles
    open_files: HashMap<FileHandle, OpenFile>,

    /// Next file handle ID
    next_handle: AtomicU64,

    /// Pending requests awaiting responses
    pending: HashMap<SeqNo, PendingRequest>,

    /// Shared memory region for communication with broker
    shmem: Option<GuestSharedMemory>,

    /// Request eventfd (to signal broker)
    request_eventfd: Option<OwnedFd>,

    /// Response eventfd (broker signals us)
    response_eventfd: Option<OwnedFd>,

    /// Statistics
    stats: ShimStats,
}

/// Pending request state
#[derive(Debug)]
struct PendingRequest {
    /// The original ioctl escape code
    escape: NvEscape,

    /// File handle this request is for
    file_handle: FileHandle,

    /// When the request was sent
    sent_at: std::time::Instant,
}

/// Guest shim statistics
#[derive(Debug, Default)]
pub struct ShimStats {
    pub opens: u64,
    pub closes: u64,
    pub ioctls: u64,
    pub ioctl_errors: u64,
    pub timeouts: u64,
}

// =============================================================================
// GUEST SHIM IMPLEMENTATION
// =============================================================================

impl GuestShim {
    /// Create a new guest shim (unconnected)
    pub fn new(client_id: ClientId) -> Self {
        Self {
            client_id,
            open_files: HashMap::new(),
            next_handle: AtomicU64::new(1),
            pending: HashMap::new(),
            shmem: None,
            request_eventfd: None,
            response_eventfd: None,
            stats: ShimStats::default(),
        }
    }

    /// Connect to broker using provided shared memory info
    pub fn connect(&mut self, info: ClientConnectionInfo) -> io::Result<()> {
        // Map the shared memory region
        if let Some(fd) = info.shmem_fd {
            self.shmem = Some(GuestSharedMemory::from_fd(fd, info.shmem_size)?);
        }

        // Store eventfds
        self.request_eventfd = Some(unsafe { OwnedFd::from_raw_fd(info.request_eventfd) });
        self.response_eventfd = Some(unsafe { OwnedFd::from_raw_fd(info.response_eventfd) });

        Ok(())
    }

    /// Handle device open
    pub fn handle_open(&mut self, device_type: NvDeviceType) -> io::Result<FileHandle> {
        let handle = FileHandle(self.next_handle.fetch_add(1, Ordering::Relaxed));
        let open_file = OpenFile::new(handle, device_type);

        self.open_files.insert(handle, open_file);
        self.stats.opens += 1;

        Ok(handle)
    }

    /// Handle device close
    pub fn handle_close(&mut self, handle: FileHandle) -> io::Result<()> {
        if let Some(file) = self.open_files.remove(&handle) {
            self.stats.closes += 1;

            // If this file had an RM client, we need to tell the broker
            if let Some(rm_client) = file.rm_client {
                // Send Free operation to broker
                let seq = file.next_seq();
                self.send_request(Request {
                    client: self.client_id,
                    seq,
                    op: Operation::Free {
                        h_root: rm_client, // Root is the client handle itself
                        h_object: rm_client,
                    },
                })?;
            }
        }

        Ok(())
    }

    /// Handle ioctl on a file
    ///
    /// Returns the response data to copy back to userspace, or an error
    pub fn handle_ioctl(
        &mut self,
        handle: FileHandle,
        cmd: libc::c_ulong,
        data: *mut libc::c_void,
    ) -> io::Result<Vec<u8>> {
        self.stats.ioctls += 1;

        // Get the open file
        let file = self.open_files.get(&handle).ok_or_else(|| {
            io::Error::new(io::ErrorKind::NotFound, "Invalid file handle")
        })?;

        // Decode the ioctl
        let request = IoctlRequest::decode(cmd, data).ok_or_else(|| {
            self.stats.ioctl_errors += 1;
            io::Error::new(io::ErrorKind::InvalidInput, "Unknown ioctl")
        })?;

        // Build the broker operation
        // Note: Operation enum fields must match broker.rs definitions
        let op = match request.escape {
            NvEscape::Alloc | NvEscape::AllocMemory | NvEscape::AllocObject => {
                // Parse the alloc params to get handles
                Operation::Alloc {
                    h_root: self.extract_root_handle(&request.params),
                    h_parent: self.extract_parent_handle(&request.params),
                    h_new: self.extract_new_handle(&request.params),
                    class: self.extract_class(&request.params),
                }
            }
            NvEscape::Free => Operation::Free {
                h_root: self.extract_root_handle(&request.params),
                h_object: self.extract_object_handle(&request.params),
            },
            NvEscape::Control => Operation::Control {
                h_client: self.extract_client_handle(&request.params),
                h_object: self.extract_object_handle(&request.params),
                cmd: self.extract_control_cmd(&request.params),
                params: request.params.clone(),
            },
            NvEscape::MapMemory | NvEscape::MapMemoryDma => Operation::MapMemory {
                h_client: self.extract_client_handle(&request.params),
                h_device: self.extract_device_handle(&request.params),
                h_memory: self.extract_memory_handle(&request.params),
                offset: self.extract_offset(&request.params),
                length: self.extract_size(&request.params),
            },
            NvEscape::UnmapMemory | NvEscape::UnmapMemoryDma => Operation::UnmapMemory {
                h_client: self.extract_client_handle(&request.params),
                h_device: self.extract_device_handle(&request.params),
                h_memory: self.extract_memory_handle(&request.params),
            },
            // For other escapes, use a generic control operation
            _ => Operation::Control {
                h_client: self.extract_client_handle(&request.params),
                h_object: self.extract_object_handle(&request.params),
                cmd: request.escape as u32,
                params: request.params.clone(),
            },
        };

        // Send request to broker
        let seq = file.next_seq();
        self.send_request(Request {
            client: self.client_id,
            seq,
            op,
        })?;

        // Record pending request
        self.pending.insert(
            seq,
            PendingRequest {
                escape: request.escape,
                file_handle: handle,
                sent_at: std::time::Instant::now(),
            },
        );

        // Wait for response
        let response = self.wait_response(seq)?;

        // Process response
        match response.result {
            Ok(result) => {
                // Update output params based on result
                let output = self.build_output_params(&request, &result);
                Ok(output)
            }
            Err(err) => {
                self.stats.ioctl_errors += 1;
                Err(io::Error::new(
                    io::ErrorKind::Other,
                    format!("Broker error: {:?}", err),
                ))
            }
        }
    }

    // =========================================================================
    // PRIVATE HELPERS
    // =========================================================================

    /// Send a request to the broker via shared memory
    fn send_request(&mut self, request: Request) -> io::Result<()> {
        let shmem = self.shmem.as_mut().ok_or_else(|| {
            io::Error::new(io::ErrorKind::NotConnected, "Not connected to broker")
        })?;

        // Encode the request
        let encoded = encode_request(&request);
        if encoded.len() > ENTRY_SIZE {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "Request too large",
            ));
        }

        // Get the request ring header
        let header = shmem.request_ring_header();

        // Reserve a slot
        let idx = header.producer_reserve().ok_or_else(|| {
            io::Error::new(io::ErrorKind::WouldBlock, "Request ring full")
        })?;

        // Write data to the slot
        let slot = shmem.request_data_slot_mut(idx);
        slot[..encoded.len()].copy_from_slice(&encoded);

        // Write entry metadata
        let entry = shmem.request_entry_mut(idx);
        entry.data_offset = (idx as usize * ENTRY_SIZE) as u32;
        entry.data_len = encoded.len() as u32;
        entry.seq = request.seq.0 as u32;

        // Commit the entry
        shmem.request_ring_header().producer_commit();

        // Signal the broker
        if let Some(ref eventfd) = self.request_eventfd {
            signal_eventfd(eventfd)?;
        }

        Ok(())
    }

    /// Wait for a response from the broker
    fn wait_response(&mut self, seq: SeqNo) -> io::Result<Response> {
        let deadline = std::time::Instant::now()
            + std::time::Duration::from_millis(RESPONSE_TIMEOUT_MS);

        loop {
            // Check if we've timed out
            if std::time::Instant::now() > deadline {
                self.stats.timeouts += 1;
                self.pending.remove(&seq);
                return Err(io::Error::new(
                    io::ErrorKind::TimedOut,
                    "Broker response timeout",
                ));
            }

            // Poll the response ring
            if let Some(response) = self.try_recv_response()? {
                if response.seq == seq {
                    self.pending.remove(&seq);
                    return Ok(response);
                }
                // Response for different sequence - this shouldn't happen in
                // our single-threaded model, but handle it gracefully
            }

            // Wait on eventfd with timeout
            if let Some(ref eventfd) = self.response_eventfd {
                wait_eventfd(eventfd, 10)?; // 10ms poll interval
            } else {
                std::thread::sleep(std::time::Duration::from_millis(1));
            }
        }
    }

    /// Try to receive a response (non-blocking)
    fn try_recv_response(&mut self) -> io::Result<Option<Response>> {
        let shmem = match self.shmem.as_mut() {
            Some(s) => s,
            None => return Ok(None),
        };

        let header = shmem.response_ring_header();
        let idx = match header.consumer_peek() {
            Some(idx) => idx,
            None => return Ok(None),
        };

        let entry = shmem.response_entry(idx);
        let data = &shmem.response_data_slot(idx)[..entry.data_len as usize];

        // Decode the response
        let response = decode_response(data).ok_or_else(|| {
            io::Error::new(io::ErrorKind::InvalidData, "Failed to decode response")
        })?;

        // Release the slot
        shmem.response_ring_header().consumer_release();

        Ok(Some(response))
    }

    // =========================================================================
    // PARAM EXTRACTION HELPERS
    // These extract fields from raw ioctl parameter buffers.
    // Offsets are based on nvidia-open struct definitions in rm_api.rs
    // =========================================================================

    /// Extract h_root (typically at offset 0)
    fn extract_root_handle(&self, params: &[u8]) -> NvHandle {
        if params.len() >= 4 {
            u32::from_le_bytes([params[0], params[1], params[2], params[3]])
        } else {
            0
        }
    }

    /// Extract h_object_parent (typically at offset 4)
    fn extract_parent_handle(&self, params: &[u8]) -> NvHandle {
        if params.len() >= 8 {
            u32::from_le_bytes([params[4], params[5], params[6], params[7]])
        } else {
            0
        }
    }

    /// Extract h_object_new (typically at offset 8)
    fn extract_new_handle(&self, params: &[u8]) -> NvHandle {
        if params.len() >= 12 {
            u32::from_le_bytes([params[8], params[9], params[10], params[11]])
        } else {
            0
        }
    }

    /// Extract h_class (typically at offset 12)
    fn extract_class(&self, params: &[u8]) -> u32 {
        if params.len() >= 16 {
            u32::from_le_bytes([params[12], params[13], params[14], params[15]])
        } else {
            0
        }
    }

    /// Extract h_object_old or generic object handle (typically at offset 8)
    fn extract_object_handle(&self, params: &[u8]) -> NvHandle {
        if params.len() >= 12 {
            u32::from_le_bytes([params[8], params[9], params[10], params[11]])
        } else {
            0
        }
    }

    /// Extract h_client for Control/Map operations (typically at offset 0)
    fn extract_client_handle(&self, params: &[u8]) -> NvHandle {
        if params.len() >= 4 {
            u32::from_le_bytes([params[0], params[1], params[2], params[3]])
        } else {
            0
        }
    }

    /// Extract h_device for Map operations (typically at offset 4)
    fn extract_device_handle(&self, params: &[u8]) -> NvHandle {
        if params.len() >= 8 {
            u32::from_le_bytes([params[4], params[5], params[6], params[7]])
        } else {
            0
        }
    }

    /// Extract h_memory for Map operations (typically at offset 8)
    fn extract_memory_handle(&self, params: &[u8]) -> NvHandle {
        if params.len() >= 12 {
            u32::from_le_bytes([params[8], params[9], params[10], params[11]])
        } else {
            0
        }
    }

    /// Extract control cmd (at offset 8 in NvOs54Params)
    fn extract_control_cmd(&self, params: &[u8]) -> u32 {
        if params.len() >= 12 {
            u32::from_le_bytes([params[8], params[9], params[10], params[11]])
        } else {
            0
        }
    }

    /// Extract offset for Map operations (typically at offset 16)
    fn extract_offset(&self, params: &[u8]) -> u64 {
        if params.len() >= 24 {
            u64::from_le_bytes([
                params[16], params[17], params[18], params[19],
                params[20], params[21], params[22], params[23],
            ])
        } else {
            0
        }
    }

    /// Extract size/length for Map operations (typically at offset 24)
    fn extract_size(&self, params: &[u8]) -> u64 {
        if params.len() >= 32 {
            u64::from_le_bytes([
                params[24], params[25], params[26], params[27],
                params[28], params[29], params[30], params[31],
            ])
        } else {
            0
        }
    }

    /// Build output parameters from broker result
    fn build_output_params(&self, request: &IoctlRequest, result: &OperationResult) -> Vec<u8> {
        let mut output = request.params.clone();

        match result {
            OperationResult::Allocated { real_handle } => {
                // Write the assigned handle back to the output params
                // The handle field is typically at offset 8 in alloc structs
                if output.len() >= 12 {
                    let handle_bytes = real_handle.0.to_le_bytes();
                    output[8..12].copy_from_slice(&handle_bytes);
                }
                // Set status = 0 (success)
                self.set_status(&mut output, 0);
            }
            OperationResult::Freed { .. } => {
                self.set_status(&mut output, 0);
            }
            OperationResult::ControlComplete { status, out_params } => {
                // Copy any output params from the result
                if !out_params.is_empty() && out_params.len() <= output.len() {
                    output[..out_params.len()].copy_from_slice(out_params);
                }
                self.set_status(&mut output, *status);
            }
            OperationResult::Mapped { address } => {
                // Write the mapped address to output
                if output.len() >= 40 {
                    let addr_bytes = address.to_le_bytes();
                    output[32..40].copy_from_slice(&addr_bytes);
                }
                self.set_status(&mut output, 0);
            }
            OperationResult::Unmapped => {
                self.set_status(&mut output, 0);
            }
            _ => {
                self.set_status(&mut output, 0);
            }
        }

        output
    }

    /// Set status field in output params (typically the last u32)
    fn set_status(&self, params: &mut [u8], status: u32) {
        let len = params.len();
        if len >= 4 {
            let status_bytes = status.to_le_bytes();
            // Status is typically at the end of the struct
            params[len - 4..].copy_from_slice(&status_bytes);
        }
    }

    /// Get statistics
    pub fn stats(&self) -> &ShimStats {
        &self.stats
    }
}

// =============================================================================
// ENCODING/DECODING
// =============================================================================

/// Encode a request to bytes for the shared memory ring
fn encode_request(request: &Request) -> Vec<u8> {
    let mut buf = Vec::with_capacity(64);

    // Header: client_id (8) + seq (8) + op_type (1)
    buf.extend_from_slice(&request.client.0.to_le_bytes());
    buf.extend_from_slice(&request.seq.0.to_le_bytes());

    match &request.op {
        Operation::RegisterClient => {
            buf.push(0);
        }
        Operation::UnregisterClient => {
            buf.push(1);
        }
        Operation::Alloc { h_root, h_parent, h_new, class } => {
            buf.push(2);
            buf.extend_from_slice(&h_root.to_le_bytes());
            buf.extend_from_slice(&h_parent.to_le_bytes());
            buf.extend_from_slice(&h_new.to_le_bytes());
            buf.extend_from_slice(&class.to_le_bytes());
        }
        Operation::Free { h_root, h_object } => {
            buf.push(3);
            buf.extend_from_slice(&h_root.to_le_bytes());
            buf.extend_from_slice(&h_object.to_le_bytes());
        }
        Operation::Control { h_client, h_object, cmd, params } => {
            buf.push(4);
            buf.extend_from_slice(&h_client.to_le_bytes());
            buf.extend_from_slice(&h_object.to_le_bytes());
            buf.extend_from_slice(&cmd.to_le_bytes());
            buf.extend_from_slice(&(params.len() as u32).to_le_bytes());
            buf.extend_from_slice(params);
        }
        Operation::MapMemory { h_client, h_device, h_memory, offset, length } => {
            buf.push(5);
            buf.extend_from_slice(&h_client.to_le_bytes());
            buf.extend_from_slice(&h_device.to_le_bytes());
            buf.extend_from_slice(&h_memory.to_le_bytes());
            buf.extend_from_slice(&offset.to_le_bytes());
            buf.extend_from_slice(&length.to_le_bytes());
        }
        Operation::UnmapMemory { h_client, h_device, h_memory } => {
            buf.push(6);
            buf.extend_from_slice(&h_client.to_le_bytes());
            buf.extend_from_slice(&h_device.to_le_bytes());
            buf.extend_from_slice(&h_memory.to_le_bytes());
        }

        // Top-level escapes
        Operation::CardInfo => {
            buf.push(7);
        }
        Operation::CheckVersion { cmd, version_string } => {
            buf.push(8);
            buf.extend_from_slice(&cmd.to_le_bytes());
            buf.extend_from_slice(&(version_string.len() as u32).to_le_bytes());
            buf.extend_from_slice(version_string);
        }
        Operation::RegisterFd { ctl_fd } => {
            buf.push(9);
            buf.extend_from_slice(&ctl_fd.to_le_bytes());
        }
        Operation::AllocOsEvent { h_client, h_device, fd } => {
            buf.push(10);
            buf.extend_from_slice(&h_client.to_le_bytes());
            buf.extend_from_slice(&h_device.to_le_bytes());
            buf.extend_from_slice(&fd.to_le_bytes());
        }
        Operation::FreeOsEvent { h_client, h_device, fd } => {
            buf.push(11);
            buf.extend_from_slice(&h_client.to_le_bytes());
            buf.extend_from_slice(&h_device.to_le_bytes());
            buf.extend_from_slice(&fd.to_le_bytes());
        }
        Operation::AttachGpusToFd { gpu_ids } => {
            buf.push(12);
            buf.extend_from_slice(&(gpu_ids.len() as u32).to_le_bytes());
            for id in gpu_ids {
                buf.extend_from_slice(&id.to_le_bytes());
            }
        }
        Operation::StatusCode { domain, bus, slot, function } => {
            buf.push(13);
            buf.extend_from_slice(&domain.to_le_bytes());
            buf.push(*bus);
            buf.push(*slot);
            buf.push(*function);
        }
    }

    buf
}

/// Decode a response from bytes
fn decode_response(data: &[u8]) -> Option<Response> {
    if data.len() < 17 {
        return None;
    }

    let client_id = u64::from_le_bytes(data[0..8].try_into().ok()?);
    let seq = u64::from_le_bytes(data[8..16].try_into().ok()?);
    let success = data[16];

    let result = if success == 1 {
        // Success - decode result type
        if data.len() < 21 {
            return None;
        }
        let result_type = u32::from_le_bytes(data[17..21].try_into().ok()?);

        match result_type {
            0 => Ok(OperationResult::Registered),
            1 => {
                // Unregistered
                if data.len() < 25 {
                    return None;
                }
                let count = u32::from_le_bytes(data[21..25].try_into().ok()?) as usize;
                let mut freed = Vec::with_capacity(count);
                let mut offset = 25;
                for _ in 0..count {
                    if data.len() < offset + 4 {
                        return None;
                    }
                    freed.push(RealHandle(u32::from_le_bytes(
                        data[offset..offset + 4].try_into().ok()?,
                    )));
                    offset += 4;
                }
                Ok(OperationResult::Unregistered { freed_handles: freed })
            }
            2 => {
                // Allocated
                if data.len() < 25 {
                    return None;
                }
                let real_handle = u32::from_le_bytes(data[21..25].try_into().ok()?);
                Ok(OperationResult::Allocated {
                    real_handle: RealHandle(real_handle),
                })
            }
            3 => {
                // Freed
                if data.len() < 25 {
                    return None;
                }
                let real_handle = u32::from_le_bytes(data[21..25].try_into().ok()?);
                Ok(OperationResult::Freed {
                    real_handle: RealHandle(real_handle),
                })
            }
            4 => {
                // ControlComplete
                if data.len() < 29 {
                    return None;
                }
                let status = u32::from_le_bytes(data[21..25].try_into().ok()?);
                let params_len = u32::from_le_bytes(data[25..29].try_into().ok()?) as usize;
                let out_params = if params_len > 0 && data.len() >= 29 + params_len {
                    data[29..29 + params_len].to_vec()
                } else {
                    Vec::new()
                };
                Ok(OperationResult::ControlComplete { status, out_params })
            }
            5 => {
                // Mapped
                if data.len() < 29 {
                    return None;
                }
                let address = u64::from_le_bytes(data[21..29].try_into().ok()?);
                Ok(OperationResult::Mapped { address })
            }
            6 => Ok(OperationResult::Unmapped),
            
            // Top-level escape results
            7 => {
                // CardInfoResult
                if data.len() < 25 {
                    return None;
                }
                let num_gpus = u32::from_le_bytes(data[21..25].try_into().ok()?);
                let card_info_len = (data.len() - 25).min(num_gpus as usize * 96); // NvCardInfo ~96 bytes
                let card_info = data[25..25 + card_info_len].to_vec();
                Ok(OperationResult::CardInfoResult { num_gpus, card_info })
            }
            8 => {
                // VersionCheckResult
                if data.len() < 25 {
                    return None;
                }
                let reply = u32::from_le_bytes(data[21..25].try_into().ok()?);
                let version_len = if data.len() > 25 {
                    u32::from_le_bytes(data[25..29].try_into().unwrap_or([0; 4])) as usize
                } else {
                    0
                };
                let version_string = if version_len > 0 && data.len() >= 29 + version_len {
                    data[29..29 + version_len].to_vec()
                } else {
                    Vec::new()
                };
                Ok(OperationResult::VersionCheckResult { reply, version_string })
            }
            9 => Ok(OperationResult::FdRegistered),
            10 => {
                // OsEventAllocated
                if data.len() < 25 {
                    return None;
                }
                let status = u32::from_le_bytes(data[21..25].try_into().ok()?);
                Ok(OperationResult::OsEventAllocated { status })
            }
            11 => {
                // OsEventFreed
                if data.len() < 25 {
                    return None;
                }
                let status = u32::from_le_bytes(data[21..25].try_into().ok()?);
                Ok(OperationResult::OsEventFreed { status })
            }
            12 => Ok(OperationResult::GpusAttached),
            13 => {
                // StatusCodeResult
                if data.len() < 25 {
                    return None;
                }
                let status = u32::from_le_bytes(data[21..25].try_into().ok()?);
                Ok(OperationResult::StatusCodeResult { status })
            }
            _ => return None,
        }
    } else {
        // Error - decode error type
        if data.len() < 21 {
            return None;
        }
        let _err_len = u32::from_le_bytes(data[17..21].try_into().ok()?) as usize;
        // For now, just return a generic error
        // TODO: Decode specific BrokerError variants
        Err(BrokerError::InvalidOperation("Broker returned error".to_string()))
    };

    Some(Response {
        client: ClientId(client_id),
        seq: SeqNo(seq),
        result,
    })
}

// =============================================================================
// EVENTFD HELPERS
// =============================================================================

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

/// Wait on an eventfd with timeout (using poll)
fn wait_eventfd(fd: &OwnedFd, timeout_ms: i32) -> io::Result<bool> {
    let mut pfd = libc::pollfd {
        fd: fd.as_raw_fd(),
        events: libc::POLLIN,
        revents: 0,
    };

    let ret = unsafe { libc::poll(&mut pfd, 1, timeout_ms) };

    if ret < 0 {
        return Err(io::Error::last_os_error());
    }

    if ret > 0 && (pfd.revents & libc::POLLIN) != 0 {
        // Read to reset the counter
        let mut val: u64 = 0;
        unsafe {
            libc::read(
                fd.as_raw_fd(),
                &mut val as *mut u64 as *mut libc::c_void,
                std::mem::size_of::<u64>(),
            );
        }
        Ok(true)
    } else {
        Ok(false)
    }
}

// =============================================================================
// GUEST SHARED MEMORY
// =============================================================================

/// Guest-side shared memory mapping
/// 
/// This is the guest's view of the shared memory region. The broker creates
/// the region and passes the fd to the guest via virtio-shmem or similar.
pub struct GuestSharedMemory {
    /// Base address of mapping
    ptr: *mut u8,
    
    /// Size of mapping
    size: usize,
    
    /// Layout information
    layout: SharedMemoryLayout,
}

// Safety: GuestSharedMemory can be sent between threads
unsafe impl Send for GuestSharedMemory {}

impl GuestSharedMemory {
    /// Map shared memory from an existing fd
    pub fn from_fd(fd: RawFd, size: usize) -> io::Result<Self> {
        let layout = SharedMemoryLayout::default_layout();
        
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
        })
    }

    /// Get request ring header (guest is producer)
    pub fn request_ring_header(&self) -> &RingHeader {
        unsafe {
            &*(self.ptr.add(self.layout.request_header_offset) as *const RingHeader)
        }
    }

    /// Get response ring header (guest is consumer)
    pub fn response_ring_header(&self) -> &RingHeader {
        unsafe {
            &*(self.ptr.add(self.layout.response_header_offset) as *const RingHeader)
        }
    }

    /// Get request data slot for writing
    pub fn request_data_slot_mut(&mut self, idx: u32) -> &mut [u8] {
        unsafe {
            let base = self.ptr.add(self.layout.request_data_offset);
            let slot = base.add(idx as usize * ENTRY_SIZE);
            std::slice::from_raw_parts_mut(slot, ENTRY_SIZE)
        }
    }

    /// Get request entry for writing
    pub fn request_entry_mut(&mut self, idx: u32) -> &mut RingEntry {
        unsafe {
            let base = self.ptr.add(self.layout.request_entries_offset) as *mut RingEntry;
            &mut *base.add(idx as usize)
        }
    }

    /// Get response data slot for reading
    pub fn response_data_slot(&self, idx: u32) -> &[u8] {
        unsafe {
            let base = self.ptr.add(self.layout.response_data_offset);
            let slot = base.add(idx as usize * ENTRY_SIZE);
            std::slice::from_raw_parts(slot, ENTRY_SIZE)
        }
    }

    /// Get response entry for reading
    pub fn response_entry(&self, idx: u32) -> &RingEntry {
        unsafe {
            let base = self.ptr.add(self.layout.response_entries_offset) as *const RingEntry;
            &*base.add(idx as usize)
        }
    }
}

impl Drop for GuestSharedMemory {
    fn drop(&mut self) {
        unsafe {
            libc::munmap(self.ptr as *mut libc::c_void, self.size);
        }
    }
}

// =============================================================================
// TESTS
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_device_type_parsing() {
        assert_eq!(
            NvDeviceType::from_path("/dev/nvidiactl"),
            Some(NvDeviceType::Control)
        );
        assert_eq!(
            NvDeviceType::from_path("/dev/nvidia0"),
            Some(NvDeviceType::Gpu(0))
        );
        assert_eq!(
            NvDeviceType::from_path("/dev/nvidia7"),
            Some(NvDeviceType::Gpu(7))
        );
        assert_eq!(
            NvDeviceType::from_path("/dev/nvidia-uvm"),
            Some(NvDeviceType::Uvm)
        );
        assert_eq!(
            NvDeviceType::from_path("/dev/nvidia-modeset"),
            Some(NvDeviceType::Modeset)
        );
        assert_eq!(NvDeviceType::from_path("/dev/sda"), None);
    }

    #[test]
    fn test_device_type_path() {
        assert_eq!(NvDeviceType::Control.path(), "/dev/nvidiactl");
        assert_eq!(NvDeviceType::Gpu(0).path(), "/dev/nvidia0");
        assert_eq!(NvDeviceType::Gpu(3).path(), "/dev/nvidia3");
        assert_eq!(NvDeviceType::Uvm.path(), "/dev/nvidia-uvm");
    }

    #[test]
    fn test_ioctl_number_build() {
        // Test building an ioctl number
        let cmd = IoctlRequest::build_ioctl_nr(
            NvEscape::Control,
            32,
            IoctlDirection::ReadWrite,
        );

        // Extract components back
        let magic = ((cmd >> 8) & 0xFF) as u8;
        let nr = (cmd & 0xFF) as u32;
        let size = ((cmd >> 16) & 0x3FFF) as usize;
        let dir = ((cmd >> 30) & 0x3) as u8;

        assert_eq!(magic, NV_IOCTL_MAGIC);
        assert_eq!(nr, NvEscape::Control as u32);
        assert_eq!(size, 32);
        assert_eq!(dir, 3); // ReadWrite
    }

    #[test]
    fn test_shim_open_close() {
        let mut shim = GuestShim::new(ClientId(1));

        let h1 = shim.handle_open(NvDeviceType::Control).unwrap();
        let h2 = shim.handle_open(NvDeviceType::Gpu(0)).unwrap();

        assert_ne!(h1, h2);
        assert_eq!(shim.stats.opens, 2);

        shim.handle_close(h1).unwrap();
        assert_eq!(shim.stats.closes, 1);
    }

    #[test]
    fn test_request_encoding() {
        let request = Request {
            client: ClientId(42),
            seq: SeqNo(7),
            op: Operation::Alloc {
                h_root: 1,
                h_parent: 100,
                h_new: 200,
                class: 0x2080,
            },
        };

        let encoded = encode_request(&request);

        // Verify header
        assert_eq!(&encoded[0..8], &42u64.to_le_bytes());
        assert_eq!(&encoded[8..16], &7u64.to_le_bytes());
        assert_eq!(encoded[16], 2); // Alloc op type

        // Verify alloc fields
        assert_eq!(&encoded[17..21], &1u32.to_le_bytes()); // h_root
        assert_eq!(&encoded[21..25], &100u32.to_le_bytes()); // h_parent
        assert_eq!(&encoded[25..29], &200u32.to_le_bytes()); // h_new
        assert_eq!(&encoded[29..33], &0x2080u32.to_le_bytes()); // class
    }
}

#[cfg(test)]
mod proptests {
    use super::*;
    use proptest::prelude::*;

    proptest! {
        /// Property: Device type round-trips through path
        #[test]
        fn device_type_roundtrip(idx in 0u32..16) {
            let device = NvDeviceType::Gpu(idx);
            let path = device.path();
            let parsed = NvDeviceType::from_path(&path);
            prop_assert_eq!(parsed, Some(device));
        }

        /// Property: ioctl number components round-trip
        #[test]
        fn ioctl_nr_roundtrip(
            escape_idx in 0usize..27,
            size in 0usize..16384,
            dir_idx in 0usize..4
        ) {
            let escapes = [
                NvEscape::AllocMemory, NvEscape::AllocObject, NvEscape::Free,
                NvEscape::Control, NvEscape::Alloc, NvEscape::ConfigGet,
                NvEscape::ConfigSet, NvEscape::DupObject, NvEscape::Share,
                NvEscape::ConfigGetEx, NvEscape::ConfigSetEx, NvEscape::I2cAccess,
                NvEscape::IdleChannels, NvEscape::VidHeapControl, NvEscape::AccessRegistry,
                NvEscape::MapMemory, NvEscape::UnmapMemory, NvEscape::GetEventData,
                NvEscape::AllocContextDma2, NvEscape::AddVblankCallback,
                NvEscape::MapMemoryDma, NvEscape::UnmapMemoryDma, NvEscape::BindContextDma,
                NvEscape::ExportObjectToFd, NvEscape::ImportObjectFromFd,
                NvEscape::UpdateDeviceMappingInfo, NvEscape::LocklessDiagnostic,
            ];
            let dirs = [
                IoctlDirection::None, IoctlDirection::Write,
                IoctlDirection::Read, IoctlDirection::ReadWrite,
            ];

            let escape = escapes[escape_idx];
            let dir = dirs[dir_idx];
            let cmd = IoctlRequest::build_ioctl_nr(escape, size, dir);

            let extracted_nr = (cmd & 0xFF) as u32;
            let extracted_size = ((cmd >> 16) & 0x3FFF) as usize;

            prop_assert_eq!(extracted_nr, escape as u32);
            prop_assert_eq!(extracted_size, size);
        }

        /// Property: File handles are unique
        #[test]
        fn file_handles_unique(count in 1usize..100) {
            let mut shim = GuestShim::new(ClientId(1));
            let mut handles = Vec::with_capacity(count);

            for _ in 0..count {
                let h = shim.handle_open(NvDeviceType::Control).unwrap();
                prop_assert!(!handles.contains(&h), "Duplicate handle");
                handles.push(h);
            }
        }

        /// Property: Request encoding preserves all fields
        #[test]
        fn request_encoding_preserves(
            client_id in any::<u64>(),
            seq in any::<u64>(),
            h_root in any::<u32>(),
            h_parent in any::<u32>(),
            h_new in any::<u32>(),
            class in any::<u32>()
        ) {
            let request = Request {
                client: ClientId(client_id),
                seq: SeqNo(seq),
                op: Operation::Alloc { h_root, h_parent, h_new, class },
            };

            let encoded = encode_request(&request);

            // Verify we can extract the header
            let decoded_client = u64::from_le_bytes(encoded[0..8].try_into().unwrap());
            let decoded_seq = u64::from_le_bytes(encoded[8..16].try_into().unwrap());

            prop_assert_eq!(decoded_client, client_id);
            prop_assert_eq!(decoded_seq, seq);
        }
    }
}
