//! vsock Transport for GPU Broker
//!
//! This module provides vsock-based communication with guest VMs running
//! the nvidia-shim kernel module. vsock provides a direct host↔guest
//! communication channel without requiring network configuration.
//!
//! Wire Protocol:
//! ```text
//! Request:  [magic: u32] [version: u32] [client_id: u64] [seq: u64]
//!           [op_type: u32] [payload_len: u32] [payload...]
//!
//! Response: [magic: u32] [version: u32] [client_id: u64] [seq: u64]
//!           [success: u8] [pad: u8*3] [result_len: u32] [result...]
//! ```
//!
//! Architecture:
//! ```text
//!  Guest VM                           Host
//!  ─────────                          ────
//!  CUDA app
//!     │
//!     ▼
//!  /dev/nvidiactl (nvidia-shim.ko)
//!     │
//!     ▼
//!  vsock connect(cid=2, port=9999)
//!     │                               vsock listen(port=9999)
//!     │                                     │
//!     └──────────────────────────────────▶ │
//!                                           ▼
//!                                     VsockServer
//!                                           │
//!                                           ▼
//!                                     BrokerProxy
//!                                           │
//!                                           ▼
//!                                     /dev/nvidiactl (real GPU)
//! ```

use std::io;
use std::os::fd::{AsRawFd, RawFd};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::thread;

use crate::broker::{Operation, OperationResult, Request, Response, SeqNo};
use crate::driver::Driver;
use crate::handle_table::ClientId;
use crate::proxy::BrokerProxy;

// =============================================================================
// CONSTANTS
// =============================================================================

/// Wire protocol magic number ("NVBK")
const WIRE_MAGIC: u32 = 0x4E56424B;

/// Wire protocol version
const WIRE_VERSION: u32 = 1;

/// Default vsock port
pub const DEFAULT_VSOCK_PORT: u32 = 9999;

/// Maximum payload size
const MAX_PAYLOAD_SIZE: usize = 4096;

// =============================================================================
// WIRE PROTOCOL
// =============================================================================

/// Request header from guest
#[derive(Debug, Clone, Copy)]
#[repr(C, packed)]
struct WireRequest {
    magic: u32,
    version: u32,
    client_id: u64,
    seq: u64,
    op_type: u32,
    payload_len: u32,
}

impl WireRequest {
    fn from_bytes(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < std::mem::size_of::<Self>() {
            return None;
        }
        
        let magic = u32::from_le_bytes(bytes[0..4].try_into().ok()?);
        let version = u32::from_le_bytes(bytes[4..8].try_into().ok()?);
        let client_id = u64::from_le_bytes(bytes[8..16].try_into().ok()?);
        let seq = u64::from_le_bytes(bytes[16..24].try_into().ok()?);
        let op_type = u32::from_le_bytes(bytes[24..28].try_into().ok()?);
        let payload_len = u32::from_le_bytes(bytes[28..32].try_into().ok()?);
        
        Some(Self {
            magic,
            version,
            client_id,
            seq,
            op_type,
            payload_len,
        })
    }
}

/// Response header to guest
#[derive(Debug, Clone, Copy)]
#[repr(C, packed)]
struct WireResponse {
    magic: u32,
    version: u32,
    client_id: u64,
    seq: u64,
    success: u8,
    _pad: [u8; 3],
    result_len: u32,
}

impl WireResponse {
    fn to_bytes_full(&self) -> [u8; 28] {
        let mut buf = [0u8; 28];
        buf[0..4].copy_from_slice(&self.magic.to_le_bytes());
        buf[4..8].copy_from_slice(&self.version.to_le_bytes());
        buf[8..16].copy_from_slice(&self.client_id.to_le_bytes());
        buf[16..24].copy_from_slice(&self.seq.to_le_bytes());
        buf[24] = self.success;
        buf[25..28].copy_from_slice(&self._pad);
        buf
    }
}

// =============================================================================
// OPERATION DECODING
// =============================================================================

fn decode_operation(op_type: u32, payload: &[u8]) -> Option<Operation> {
    match op_type {
        0 => Some(Operation::RegisterClient),
        1 => Some(Operation::UnregisterClient),
        2 => {
            // Alloc: [h_root: u32] [h_parent: u32] [h_new: u32] [class: u32]
            if payload.len() >= 16 {
                let h_root = u32::from_le_bytes(payload[0..4].try_into().ok()?);
                let h_parent = u32::from_le_bytes(payload[4..8].try_into().ok()?);
                let h_new = u32::from_le_bytes(payload[8..12].try_into().ok()?);
                let class = u32::from_le_bytes(payload[12..16].try_into().ok()?);
                Some(Operation::Alloc {
                    h_root: h_root,
                    h_parent: h_parent,
                    h_new: h_new,
                    class,
                })
            } else {
                None
            }
        }
        3 => {
            // Free: [h_root: u32] [h_object: u32]
            if payload.len() >= 8 {
                let h_root = u32::from_le_bytes(payload[0..4].try_into().ok()?);
                let h_object = u32::from_le_bytes(payload[4..8].try_into().ok()?);
                Some(Operation::Free {
                    h_root: h_root,
                    h_object: h_object,
                })
            } else {
                None
            }
        }
        4 => {
            // Control: [h_client: u32] [h_object: u32] [cmd: u32] [params_size: u32] [params...]
            if payload.len() >= 16 {
                let h_client = u32::from_le_bytes(payload[0..4].try_into().ok()?);
                let h_object = u32::from_le_bytes(payload[4..8].try_into().ok()?);
                let cmd = u32::from_le_bytes(payload[8..12].try_into().ok()?);
                let params_size = u32::from_le_bytes(payload[12..16].try_into().ok()?) as usize;
                let params = if params_size > 0 && payload.len() >= 16 + params_size {
                    payload[16..16 + params_size].to_vec()
                } else {
                    Vec::new()
                };
                Some(Operation::Control {
                    h_client: h_client,
                    h_object: h_object,
                    cmd,
                    params,
                })
            } else {
                None
            }
        }
        5 => {
            // MapMemory: [h_client: u32] [h_device: u32] [h_memory: u32] [offset: u64] [length: u64]
            if payload.len() >= 28 {
                let h_client = u32::from_le_bytes(payload[0..4].try_into().ok()?);
                let h_device = u32::from_le_bytes(payload[4..8].try_into().ok()?);
                let h_memory = u32::from_le_bytes(payload[8..12].try_into().ok()?);
                let offset = u64::from_le_bytes(payload[12..20].try_into().ok()?);
                let length = u64::from_le_bytes(payload[20..28].try_into().ok()?);
                Some(Operation::MapMemory {
                    h_client: h_client,
                    h_device: h_device,
                    h_memory: h_memory,
                    offset,
                    length,
                })
            } else {
                None
            }
        }
        6 => {
            // UnmapMemory: [h_client: u32] [h_device: u32] [h_memory: u32]
            if payload.len() >= 12 {
                let h_client = u32::from_le_bytes(payload[0..4].try_into().ok()?);
                let h_device = u32::from_le_bytes(payload[4..8].try_into().ok()?);
                let h_memory = u32::from_le_bytes(payload[8..12].try_into().ok()?);
                Some(Operation::UnmapMemory {
                    h_client: h_client,
                    h_device: h_device,
                    h_memory: h_memory,
                })
            } else {
                None
            }
        }
        7 => Some(Operation::CardInfo),
        8 => {
            // CheckVersion: [cmd: u32] [version_string...]
            let cmd = if payload.len() >= 4 {
                u32::from_le_bytes(payload[0..4].try_into().ok()?)
            } else {
                0
            };
            let version_string = if payload.len() > 4 {
                payload[4..].to_vec()
            } else {
                Vec::new()
            };
            Some(Operation::CheckVersion { cmd, version_string })
        }
        9 => {
            // RegisterFd: [ctl_fd: i32]
            let ctl_fd = if payload.len() >= 4 {
                i32::from_le_bytes(payload[0..4].try_into().ok()?)
            } else {
                -1
            };
            Some(Operation::RegisterFd { ctl_fd })
        }
        10 => {
            // AllocOsEvent: [h_client: u32] [h_device: u32] [fd: u32]
            if payload.len() >= 12 {
                let h_client = u32::from_le_bytes(payload[0..4].try_into().ok()?);
                let h_device = u32::from_le_bytes(payload[4..8].try_into().ok()?);
                let fd = u32::from_le_bytes(payload[8..12].try_into().ok()?);
                Some(Operation::AllocOsEvent {
                    h_client: h_client,
                    h_device: h_device,
                    fd,
                })
            } else {
                None
            }
        }
        11 => {
            // FreeOsEvent: [h_client: u32] [h_device: u32] [fd: u32]
            if payload.len() >= 12 {
                let h_client = u32::from_le_bytes(payload[0..4].try_into().ok()?);
                let h_device = u32::from_le_bytes(payload[4..8].try_into().ok()?);
                let fd = u32::from_le_bytes(payload[8..12].try_into().ok()?);
                Some(Operation::FreeOsEvent {
                    h_client: h_client,
                    h_device: h_device,
                    fd,
                })
            } else {
                None
            }
        }
        12 => {
            // AttachGpusToFd: [num_gpus: u32] [gpu_ids...]
            let num_gpus = if payload.len() >= 4 {
                u32::from_le_bytes(payload[0..4].try_into().ok()?) as usize
            } else {
                0
            };
            let gpu_ids = if num_gpus > 0 && payload.len() >= 4 + num_gpus * 4 {
                (0..num_gpus)
                    .map(|i| u32::from_le_bytes(payload[4 + i*4..8 + i*4].try_into().unwrap()))
                    .collect()
            } else {
                Vec::new()
            };
            Some(Operation::AttachGpusToFd { gpu_ids })
        }
        13 => {
            // StatusCode: [domain: u32] [bus: u8] [slot: u8] [function: u8]
            if payload.len() >= 7 {
                let domain = u32::from_le_bytes(payload[0..4].try_into().ok()?);
                let bus = payload[4];
                let slot = payload[5];
                let function = payload[6];
                Some(Operation::StatusCode { domain, bus, slot, function })
            } else {
                Some(Operation::StatusCode { domain: 0, bus: 0, slot: 0, function: 0 })
            }
        }
        _ => {
            tracing::warn!("Unknown operation type: {}", op_type);
            None
        }
    }
}

// =============================================================================
// RESULT ENCODING
// =============================================================================

fn encode_response(response: &Response) -> (bool, Vec<u8>) {
    let mut buf = Vec::new();
    
    match &response.result {
        Ok(result) => {
            match result {
                OperationResult::Registered => {
                    buf.extend_from_slice(&0u32.to_le_bytes()); // result_type
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
                    buf.extend_from_slice(&0u32.to_le_bytes()); // status = success
                    buf.extend_from_slice(&real_handle.0.to_le_bytes());
                }
                OperationResult::Freed { real_handle } => {
                    buf.extend_from_slice(&3u32.to_le_bytes());
                    buf.extend_from_slice(&0u32.to_le_bytes()); // status
                    buf.extend_from_slice(&real_handle.0.to_le_bytes());
                }
                OperationResult::ControlComplete { status, out_params } => {
                    buf.extend_from_slice(&4u32.to_le_bytes());
                    buf.extend_from_slice(&status.to_le_bytes());
                    buf.extend_from_slice(out_params);
                }
                OperationResult::Mapped { address } => {
                    buf.extend_from_slice(&5u32.to_le_bytes());
                    buf.extend_from_slice(&0u32.to_le_bytes()); // status
                    buf.extend_from_slice(&address.to_le_bytes());
                }
                OperationResult::Unmapped => {
                    buf.extend_from_slice(&6u32.to_le_bytes());
                    buf.extend_from_slice(&0u32.to_le_bytes());
                }
                OperationResult::CardInfoResult { num_gpus, card_info } => {
                    buf.extend_from_slice(&7u32.to_le_bytes());
                    buf.extend_from_slice(&num_gpus.to_le_bytes());
                    buf.extend_from_slice(card_info);
                }
                OperationResult::VersionCheckResult { reply, version_string } => {
                    buf.extend_from_slice(&8u32.to_le_bytes());
                    buf.extend_from_slice(&reply.to_le_bytes());
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
            (true, buf)
        }
        Err(e) => {
            buf.extend_from_slice(&255u32.to_le_bytes());
            let msg = format!("{:?}", e);
            let msg_bytes = msg.as_bytes();
            buf.extend_from_slice(&(msg_bytes.len() as u32).to_le_bytes());
            buf.extend_from_slice(msg_bytes);
            (false, buf)
        }
    }
}

// =============================================================================
// VSOCK SERVER
// =============================================================================

/// vsock server configuration
#[derive(Debug, Clone)]
pub struct VsockServerConfig {
    /// Port to listen on
    pub port: u32,
    
    /// Maximum concurrent connections
    pub max_connections: usize,
}

impl Default for VsockServerConfig {
    fn default() -> Self {
        Self {
            port: DEFAULT_VSOCK_PORT,
            max_connections: 1000,
        }
    }
}

/// vsock server for guest VM connections
pub struct VsockServer<D: Driver + Send + 'static> {
    config: VsockServerConfig,
    proxy: Arc<std::sync::Mutex<BrokerProxy<D>>>,
    next_client_id: AtomicU64,
    shutdown: AtomicBool,
}

impl<D: Driver + Send + 'static> VsockServer<D> {
    /// Create a new vsock server
    pub fn new(config: VsockServerConfig, proxy: BrokerProxy<D>) -> Self {
        Self {
            config,
            proxy: Arc::new(std::sync::Mutex::new(proxy)),
            next_client_id: AtomicU64::new(1),
            shutdown: AtomicBool::new(false),
        }
    }
    
    /// Run the server (blocking)
    pub fn run(&self) -> io::Result<()> {
        // Create vsock listener
        let listener = self.create_vsock_listener()?;
        
        tracing::info!("vsock server listening on port {}", self.config.port);
        
        while !self.shutdown.load(Ordering::Relaxed) {
            match self.accept_connection(&listener) {
                Ok(stream) => {
                    let client_id = self.next_client_id.fetch_add(1, Ordering::Relaxed);
                    let proxy = Arc::clone(&self.proxy);
                    
                    tracing::info!("New vsock connection, client_id={}", client_id);
                    
                    // Handle connection in new thread
                    thread::spawn(move || {
                        if let Err(e) = Self::handle_connection(stream, client_id, proxy) {
                            tracing::error!("Connection error for client {}: {}", client_id, e);
                        }
                        tracing::info!("Client {} disconnected", client_id);
                    });
                }
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                    // Timeout, check shutdown flag
                    continue;
                }
                Err(e) => {
                    tracing::error!("Accept error: {}", e);
                }
            }
        }
        
        Ok(())
    }
    
    /// Request server shutdown
    pub fn shutdown(&self) {
        self.shutdown.store(true, Ordering::Relaxed);
    }
    
    /// Create vsock listener socket
    fn create_vsock_listener(&self) -> io::Result<RawFd> {
        use libc::{
            socket, bind, listen, setsockopt,
            AF_VSOCK, SOCK_STREAM, SOL_SOCKET, SO_REUSEADDR,
            sockaddr_vm, VMADDR_CID_ANY,
        };
        
        unsafe {
            // Create socket
            let fd = socket(AF_VSOCK, SOCK_STREAM, 0);
            if fd < 0 {
                return Err(io::Error::last_os_error());
            }
            
            // Set SO_REUSEADDR
            let optval: i32 = 1;
            setsockopt(
                fd,
                SOL_SOCKET,
                SO_REUSEADDR,
                &optval as *const _ as *const libc::c_void,
                std::mem::size_of::<i32>() as libc::socklen_t,
            );
            
            // Bind to VMADDR_CID_ANY (accept from any guest)
            let mut addr: sockaddr_vm = std::mem::zeroed();
            addr.svm_family = AF_VSOCK as u16;
            addr.svm_port = self.config.port;
            addr.svm_cid = VMADDR_CID_ANY;
            
            let ret = bind(
                fd,
                &addr as *const _ as *const libc::sockaddr,
                std::mem::size_of::<sockaddr_vm>() as libc::socklen_t,
            );
            if ret < 0 {
                libc::close(fd);
                return Err(io::Error::last_os_error());
            }
            
            // Listen
            let ret = listen(fd, self.config.max_connections as i32);
            if ret < 0 {
                libc::close(fd);
                return Err(io::Error::last_os_error());
            }
            
            Ok(fd)
        }
    }
    
    /// Accept a connection
    fn accept_connection(&self, listener: &RawFd) -> io::Result<VsockStream> {
        use libc::{accept, sockaddr_vm};
        
        unsafe {
            let mut addr: sockaddr_vm = std::mem::zeroed();
            let mut addr_len = std::mem::size_of::<sockaddr_vm>() as libc::socklen_t;
            
            let fd = accept(
                *listener,
                &mut addr as *mut _ as *mut libc::sockaddr,
                &mut addr_len,
            );
            
            if fd < 0 {
                return Err(io::Error::last_os_error());
            }
            
            Ok(VsockStream { fd })
        }
    }
    
    /// Handle a single connection
    fn handle_connection(
        mut stream: VsockStream,
        client_id: u64,
        proxy: Arc<std::sync::Mutex<BrokerProxy<D>>>,
    ) -> io::Result<()> {
        let mut header_buf = [0u8; 32];
        let mut payload_buf = vec![0u8; MAX_PAYLOAD_SIZE];
        
        // Register client with proxy
        {
            let mut proxy = proxy.lock().unwrap();
            let req = Request {
                client: ClientId(client_id),
                seq: SeqNo(0),
                op: Operation::RegisterClient,
            };
            let _ = proxy.process(req);
        }
        
        loop {
            // Read request header
            if let Err(e) = stream.read_exact(&mut header_buf) {
                if e.kind() == io::ErrorKind::UnexpectedEof {
                    break; // Client disconnected
                }
                return Err(e);
            }
            
            // Parse header
            let req_header = match WireRequest::from_bytes(&header_buf) {
                Some(h) => h,
                None => {
                    tracing::warn!("Invalid request header");
                    continue;
                }
            };
            
            // Validate magic
            let magic = req_header.magic;
            if magic != WIRE_MAGIC {
                tracing::warn!("Bad magic: 0x{:x}", magic);
                continue;
            }
            
            // Read payload
            let payload_len = req_header.payload_len as usize;
            if payload_len > MAX_PAYLOAD_SIZE {
                tracing::warn!("Payload too large: {}", payload_len);
                continue;
            }
            
            if payload_len > 0 {
                stream.read_exact(&mut payload_buf[..payload_len])?;
            }
            
            // Decode operation
            let op_type = req_header.op_type;
            let operation = match decode_operation(op_type, &payload_buf[..payload_len]) {
                Some(op) => op,
                None => {
                    tracing::warn!("Failed to decode operation type {}", op_type);
                    // Send error response
                    let resp = WireResponse {
                        magic: WIRE_MAGIC,
                        version: WIRE_VERSION,
                        client_id,
                        seq: req_header.seq,
                        success: 0,
                        _pad: [0; 3],
                        result_len: 0,
                    };
                    stream.write_all(&resp.to_bytes_full())?;
                    continue;
                }
            };
            
            // Process through proxy
            let request = Request {
                client: ClientId(client_id),
                seq: SeqNo(req_header.seq),
                op: operation,
            };
            
            let response = {
                let mut proxy = proxy.lock().unwrap();
                proxy.process(request)
            };
            
            // Encode result
            let (success, result_bytes) = encode_response(&response);
            
            // Send response header
            let resp = WireResponse {
                magic: WIRE_MAGIC,
                version: WIRE_VERSION,
                client_id,
                seq: req_header.seq,
                success: if success { 1 } else { 0 },
                _pad: [0; 3],
                result_len: result_bytes.len() as u32,
            };
            
            stream.write_all(&resp.to_bytes_full())?;
            
            // Send result payload
            if !result_bytes.is_empty() {
                stream.write_all(&result_bytes)?;
            }
        }
        
        // Unregister client
        {
            let mut proxy = proxy.lock().unwrap();
            let req = Request {
                client: ClientId(client_id),
                seq: SeqNo(0),
                op: Operation::UnregisterClient,
            };
            let _ = proxy.process(req);
        }
        
        Ok(())
    }
}

// =============================================================================
// VSOCK STREAM
// =============================================================================

/// Wrapper for vsock file descriptor
struct VsockStream {
    fd: RawFd,
}

impl VsockStream {
    fn read_exact(&mut self, buf: &mut [u8]) -> io::Result<()> {
        let mut total = 0;
        while total < buf.len() {
            let n = unsafe {
                libc::read(
                    self.fd,
                    buf[total..].as_mut_ptr() as *mut libc::c_void,
                    buf.len() - total,
                )
            };
            if n < 0 {
                return Err(io::Error::last_os_error());
            }
            if n == 0 {
                return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "connection closed"));
            }
            total += n as usize;
        }
        Ok(())
    }
    
    fn write_all(&mut self, buf: &[u8]) -> io::Result<()> {
        let mut total = 0;
        while total < buf.len() {
            let n = unsafe {
                libc::write(
                    self.fd,
                    buf[total..].as_ptr() as *const libc::c_void,
                    buf.len() - total,
                )
            };
            if n < 0 {
                return Err(io::Error::last_os_error());
            }
            total += n as usize;
        }
        Ok(())
    }
}

impl Drop for VsockStream {
    fn drop(&mut self) {
        unsafe { libc::close(self.fd); }
    }
}

impl AsRawFd for VsockStream {
    fn as_raw_fd(&self) -> RawFd {
        self.fd
    }
}

// =============================================================================
// TESTS
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;
    
    use crate::handle_table::RealHandle;
    
    #[test]
    fn test_wire_request_parse() {
        let mut buf = [0u8; 32];
        buf[0..4].copy_from_slice(&WIRE_MAGIC.to_le_bytes());
        buf[4..8].copy_from_slice(&WIRE_VERSION.to_le_bytes());
        buf[8..16].copy_from_slice(&42u64.to_le_bytes());
        buf[16..24].copy_from_slice(&100u64.to_le_bytes());
        buf[24..28].copy_from_slice(&2u32.to_le_bytes()); // op_type = Alloc
        buf[28..32].copy_from_slice(&16u32.to_le_bytes()); // payload_len
        
        let req = WireRequest::from_bytes(&buf).unwrap();
        // Copy packed fields to local vars
        let magic = req.magic;
        let version = req.version;
        let client_id = req.client_id;
        let seq = req.seq;
        let op_type = req.op_type;
        let payload_len = req.payload_len;
        
        assert_eq!(magic, WIRE_MAGIC);
        assert_eq!(version, WIRE_VERSION);
        assert_eq!(client_id, 42);
        assert_eq!(seq, 100);
        assert_eq!(op_type, 2);
        assert_eq!(payload_len, 16);
    }
    
    #[test]
    fn test_decode_alloc() {
        let mut payload = [0u8; 16];
        payload[0..4].copy_from_slice(&1u32.to_le_bytes()); // h_root
        payload[4..8].copy_from_slice(&2u32.to_le_bytes()); // h_parent
        payload[8..12].copy_from_slice(&3u32.to_le_bytes()); // h_new
        payload[12..16].copy_from_slice(&0x41u32.to_le_bytes()); // class
        
        let op = decode_operation(2, &payload).unwrap();
        match op {
            Operation::Alloc { h_root, h_parent, h_new, class } => {
                assert_eq!(h_root, 1);
                assert_eq!(h_parent, 2);
                assert_eq!(h_new, 3);
                assert_eq!(class, 0x41);
            }
            _ => panic!("Expected Alloc"),
        }
    }
    
    #[test]
    fn test_encode_allocated() {
        let response = Response {
            client: ClientId(1),
            seq: SeqNo(0),
            result: Ok(OperationResult::Allocated { real_handle: RealHandle(0x1234) }),
        };
        let (success, encoded) = encode_response(&response);
        
        assert!(success);
        // result_type (4) + status (4) + handle (4) = 12 bytes
        assert_eq!(encoded.len(), 12);
        assert_eq!(u32::from_le_bytes(encoded[0..4].try_into().unwrap()), 2); // Allocated
        assert_eq!(u32::from_le_bytes(encoded[4..8].try_into().unwrap()), 0); // status
        assert_eq!(u32::from_le_bytes(encoded[8..12].try_into().unwrap()), 0x1234); // handle
    }
}
