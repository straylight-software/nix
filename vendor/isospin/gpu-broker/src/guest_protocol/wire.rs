//! Wire format definitions for guest-broker communication
//!
//! All types are `repr(C)` for consistent memory layout and use zerocopy
//! for zero-copy parsing. This is critical for kernel module use where
//! we can't afford allocations.
//!
//! Wire format:
//! ```text
//! Request:  [RequestHeader (32 bytes)] [ioctl params (variable)]
//! Response: [ResponseHeader (32 bytes)] [ioctl params (variable)]
//! ```
//!
//! Invariants:
//! - All requests have a unique RequestId within a client session
//! - Every request gets exactly one response with matching RequestId
//! - Payload length never exceeds MAX_PAYLOAD_SIZE

#[cfg(not(feature = "std"))]
use core::{mem, result::Result};

#[cfg(feature = "std")]
use std::{mem, result::Result};

/// Maximum payload size (4KB - header size)
pub const MAX_PAYLOAD_SIZE: usize = 4096 - 32;

/// Request ID - unique within a client session
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[repr(transparent)]
pub struct RequestId(pub u64);

impl RequestId {
    pub const ZERO: Self = Self(0);
    
    #[inline]
    pub fn next(self) -> Self {
        Self(self.0.wrapping_add(1))
    }
}

/// NVIDIA escape codes (ioctl command numbers)
/// These are the ~20 ioctls that comprise the RM API.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum NvEscapeCode {
    // Memory operations
    AllocMemory = 0x27,
    AllocObject = 0x28,
    Free = 0x29,
    Control = 0x2A,
    Alloc = 0x2B,
    
    // Config operations
    ConfigGet = 0x32,
    ConfigSet = 0x33,
    DupObject = 0x34,
    Share = 0x35,
    ConfigGetEx = 0x37,
    ConfigSetEx = 0x38,
    
    // Misc
    I2cAccess = 0x39,
    IdleChannels = 0x41,
    VidHeapControl = 0x4A,
    AccessRegistry = 0x4D,
    
    // Memory mapping
    MapMemory = 0x4E,
    UnmapMemory = 0x4F,
    
    // Events
    GetEventData = 0x52,
    AllocContextDma2 = 0x54,
    AddVblankCallback = 0x56,
    
    // DMA mapping
    MapMemoryDma = 0x57,
    UnmapMemoryDma = 0x58,
    BindContextDma = 0x59,
    
    // FD operations
    ExportObjectToFd = 0x5C,
    ImportObjectFromFd = 0x5D,
    
    // Other
    UpdateDeviceMappingInfo = 0x5E,
    LocklessDiagnostic = 0x5F,
}

impl NvEscapeCode {
    /// Try to convert a u8 to an escape code
    pub fn from_u8(v: u8) -> Option<Self> {
        match v {
            0x27 => Some(Self::AllocMemory),
            0x28 => Some(Self::AllocObject),
            0x29 => Some(Self::Free),
            0x2A => Some(Self::Control),
            0x2B => Some(Self::Alloc),
            0x32 => Some(Self::ConfigGet),
            0x33 => Some(Self::ConfigSet),
            0x34 => Some(Self::DupObject),
            0x35 => Some(Self::Share),
            0x37 => Some(Self::ConfigGetEx),
            0x38 => Some(Self::ConfigSetEx),
            0x39 => Some(Self::I2cAccess),
            0x41 => Some(Self::IdleChannels),
            0x4A => Some(Self::VidHeapControl),
            0x4D => Some(Self::AccessRegistry),
            0x4E => Some(Self::MapMemory),
            0x4F => Some(Self::UnmapMemory),
            0x52 => Some(Self::GetEventData),
            0x54 => Some(Self::AllocContextDma2),
            0x56 => Some(Self::AddVblankCallback),
            0x57 => Some(Self::MapMemoryDma),
            0x58 => Some(Self::UnmapMemoryDma),
            0x59 => Some(Self::BindContextDma),
            0x5C => Some(Self::ExportObjectToFd),
            0x5D => Some(Self::ImportObjectFromFd),
            0x5E => Some(Self::UpdateDeviceMappingInfo),
            0x5F => Some(Self::LocklessDiagnostic),
            _ => None,
        }
    }
    
    /// Get the expected parameter size for this escape code
    /// Returns None for variable-size or unknown
    pub fn param_size(&self) -> Option<usize> {
        match self {
            Self::Free => Some(16),          // NvOs00Params
            Self::AllocObject => Some(20),   // NvOs05Params
            Self::Alloc => Some(32),         // NvOs21Params
            Self::Control => Some(32),       // NvOs54Params (+ variable params)
            Self::MapMemory => Some(48),     // NvOs33Params
            Self::UnmapMemory => Some(32),   // NvOs34Params
            Self::DupObject => Some(32),     // NvOs55Params
            Self::Share => Some(24),         // NvOs57Params
            _ => None,  // Variable or not commonly used
        }
    }
}

/// Request header - sent from guest to broker
///
/// Layout (32 bytes, cache-line friendly):
/// ```text
/// [0..8)   request_id: u64
/// [8..9)   escape_code: u8
/// [9..10)  device_idx: u8 (0 = nvidiactl, 1+ = nvidia0, nvidia1, ...)
/// [10..12) flags: u16
/// [12..16) payload_len: u32
/// [16..32) reserved: [u8; 16]
/// ```
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct RequestHeader {
    /// Unique request ID for matching responses
    pub request_id: u64,
    
    /// NVIDIA escape code (ioctl command)
    pub escape_code: u8,
    
    /// Device index (0 = nvidiactl, 1 = nvidia0, etc.)
    pub device_idx: u8,
    
    /// Flags (reserved, must be 0)
    pub flags: u16,
    
    /// Length of payload following this header
    pub payload_len: u32,
    
    /// Reserved for future use
    pub _reserved: [u8; 16],
}

impl RequestHeader {
    pub const SIZE: usize = 32;
    
    /// Create a new request header
    pub fn new(request_id: RequestId, escape_code: NvEscapeCode, device_idx: u8, payload_len: u32) -> Self {
        Self {
            request_id: request_id.0,
            escape_code: escape_code as u8,
            device_idx,
            flags: 0,
            payload_len,
            _reserved: [0; 16],
        }
    }
    
    /// Parse from bytes (copies to handle alignment)
    pub fn from_bytes(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < Self::SIZE {
            return None;
        }
        // Copy to ensure alignment - critical for portability
        let mut header = Self {
            request_id: 0,
            escape_code: 0,
            device_idx: 0,
            flags: 0,
            payload_len: 0,
            _reserved: [0; 16],
        };
        unsafe {
            core::ptr::copy_nonoverlapping(
                bytes.as_ptr(),
                &mut header as *mut Self as *mut u8,
                Self::SIZE,
            );
        }
        Some(header)
    }
    
    /// Write to bytes
    pub fn to_bytes(&self, buf: &mut [u8]) -> Option<()> {
        if buf.len() < Self::SIZE {
            return None;
        }
        // Safety: same as from_bytes
        unsafe {
            core::ptr::copy_nonoverlapping(
                self as *const Self as *const u8,
                buf.as_mut_ptr(),
                Self::SIZE,
            );
        }
        Some(())
    }
    
    /// Get the escape code as enum
    pub fn escape(&self) -> Option<NvEscapeCode> {
        NvEscapeCode::from_u8(self.escape_code)
    }
    
    /// Validate header fields
    pub fn validate(&self) -> Result<(), WireError> {
        if self.escape().is_none() {
            return Err(WireError::InvalidEscapeCode(self.escape_code));
        }
        if self.payload_len as usize > MAX_PAYLOAD_SIZE {
            return Err(WireError::PayloadTooLarge(self.payload_len as usize));
        }
        if self.flags != 0 {
            return Err(WireError::InvalidFlags(self.flags));
        }
        Ok(())
    }
}

/// Response header - sent from broker to guest
///
/// Layout (32 bytes):
/// ```text
/// [0..8)   request_id: u64 (matches request)
/// [8..12)  status: u32 (NVIDIA status code, 0 = success)
/// [12..16) payload_len: u32
/// [16..32) reserved: [u8; 16]
/// ```
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct ResponseHeader {
    /// Request ID this response is for
    pub request_id: u64,
    
    /// NVIDIA status code (0 = NV_OK)
    pub status: u32,
    
    /// Length of payload following this header
    pub payload_len: u32,
    
    /// Reserved for future use
    pub _reserved: [u8; 16],
}

impl ResponseHeader {
    pub const SIZE: usize = 32;
    
    /// Create a new response header
    pub fn new(request_id: RequestId, status: u32, payload_len: u32) -> Self {
        Self {
            request_id: request_id.0,
            status,
            payload_len,
            _reserved: [0; 16],
        }
    }
    
    /// Parse from bytes (copies to handle alignment)
    pub fn from_bytes(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < Self::SIZE {
            return None;
        }
        let mut header = Self {
            request_id: 0,
            status: 0,
            payload_len: 0,
            _reserved: [0; 16],
        };
        unsafe {
            core::ptr::copy_nonoverlapping(
                bytes.as_ptr(),
                &mut header as *mut Self as *mut u8,
                Self::SIZE,
            );
        }
        Some(header)
    }
    
    /// Write to bytes
    pub fn to_bytes(&self, buf: &mut [u8]) -> Option<()> {
        if buf.len() < Self::SIZE {
            return None;
        }
        unsafe {
            core::ptr::copy_nonoverlapping(
                self as *const Self as *const u8,
                buf.as_mut_ptr(),
                Self::SIZE,
            );
        }
        Some(())
    }
    
    /// Check if this is a success response
    pub fn is_success(&self) -> bool {
        self.status == 0
    }
    
    /// Validate header fields
    pub fn validate(&self) -> Result<(), WireError> {
        if self.payload_len as usize > MAX_PAYLOAD_SIZE {
            return Err(WireError::PayloadTooLarge(self.payload_len as usize));
        }
        Ok(())
    }
}

/// A complete ioctl request (header + payload)
#[derive(Debug, Clone)]
pub struct IoctlRequest {
    pub header: RequestHeader,
    pub payload: IoctlPayload,
}

/// A complete ioctl response (header + payload)
#[derive(Debug, Clone)]
pub struct IoctlResponse {
    pub header: ResponseHeader,
    pub payload: IoctlPayload,
}

/// Payload for ioctl request/response
/// 
/// In no_std context, this would be a fixed-size array.
/// In std context, we use Vec for convenience.
#[derive(Debug, Clone)]
pub struct IoctlPayload {
    #[cfg(feature = "std")]
    data: Vec<u8>,
    
    #[cfg(not(feature = "std"))]
    data: [u8; MAX_PAYLOAD_SIZE],
    
    #[cfg(not(feature = "std"))]
    len: usize,
}

impl IoctlPayload {
    /// Create empty payload
    #[cfg(feature = "std")]
    pub fn empty() -> Self {
        Self { data: Vec::new() }
    }
    
    #[cfg(not(feature = "std"))]
    pub fn empty() -> Self {
        Self { data: [0; MAX_PAYLOAD_SIZE], len: 0 }
    }
    
    /// Create from bytes
    #[cfg(feature = "std")]
    pub fn from_bytes(bytes: &[u8]) -> Result<Self, WireError> {
        if bytes.len() > MAX_PAYLOAD_SIZE {
            return Err(WireError::PayloadTooLarge(bytes.len()));
        }
        Ok(Self { data: bytes.to_vec() })
    }
    
    #[cfg(not(feature = "std"))]
    pub fn from_bytes(bytes: &[u8]) -> Result<Self, WireError> {
        if bytes.len() > MAX_PAYLOAD_SIZE {
            return Err(WireError::PayloadTooLarge(bytes.len()));
        }
        let mut data = [0; MAX_PAYLOAD_SIZE];
        data[..bytes.len()].copy_from_slice(bytes);
        Ok(Self { data, len: bytes.len() })
    }
    
    /// Get payload bytes
    #[cfg(feature = "std")]
    pub fn as_bytes(&self) -> &[u8] {
        &self.data
    }
    
    #[cfg(not(feature = "std"))]
    pub fn as_bytes(&self) -> &[u8] {
        &self.data[..self.len]
    }
    
    /// Get mutable payload bytes
    #[cfg(feature = "std")]
    pub fn as_bytes_mut(&mut self) -> &mut [u8] {
        &mut self.data
    }
    
    #[cfg(not(feature = "std"))]
    pub fn as_bytes_mut(&mut self) -> &mut [u8] {
        &mut self.data[..self.len]
    }
    
    /// Get length
    #[cfg(feature = "std")]
    pub fn len(&self) -> usize {
        self.data.len()
    }
    
    #[cfg(not(feature = "std"))]
    pub fn len(&self) -> usize {
        self.len
    }
    
    /// Check if empty
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

/// Wire format errors
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WireError {
    /// Buffer too small
    BufferTooSmall { expected: usize, actual: usize },
    
    /// Invalid escape code
    InvalidEscapeCode(u8),
    
    /// Payload exceeds maximum size
    PayloadTooLarge(usize),
    
    /// Invalid flags
    InvalidFlags(u16),
    
    /// Mismatched request/response ID
    RequestIdMismatch { expected: u64, actual: u64 },
}

/// Encode a request to a buffer
/// Returns the number of bytes written
pub fn encode_request(request: &IoctlRequest, buf: &mut [u8]) -> Result<usize, WireError> {
    let total_len = RequestHeader::SIZE + request.payload.len();
    
    if buf.len() < total_len {
        return Err(WireError::BufferTooSmall {
            expected: total_len,
            actual: buf.len(),
        });
    }
    
    request.header.to_bytes(&mut buf[..RequestHeader::SIZE])
        .ok_or(WireError::BufferTooSmall { expected: RequestHeader::SIZE, actual: buf.len() })?;
    
    buf[RequestHeader::SIZE..total_len].copy_from_slice(request.payload.as_bytes());
    
    Ok(total_len)
}

/// Decode a request from a buffer
pub fn decode_request(buf: &[u8]) -> Result<IoctlRequest, WireError> {
    if buf.len() < RequestHeader::SIZE {
        return Err(WireError::BufferTooSmall {
            expected: RequestHeader::SIZE,
            actual: buf.len(),
        });
    }
    
    let header = RequestHeader::from_bytes(buf)
        .ok_or(WireError::BufferTooSmall { expected: RequestHeader::SIZE, actual: buf.len() })?;
    
    header.validate()?;
    
    let payload_start = RequestHeader::SIZE;
    let payload_end = payload_start + header.payload_len as usize;
    
    if buf.len() < payload_end {
        return Err(WireError::BufferTooSmall {
            expected: payload_end,
            actual: buf.len(),
        });
    }
    
    let payload = IoctlPayload::from_bytes(&buf[payload_start..payload_end])?;
    
    Ok(IoctlRequest { header, payload })
}

/// Encode a response to a buffer
pub fn encode_response(response: &IoctlResponse, buf: &mut [u8]) -> Result<usize, WireError> {
    let total_len = ResponseHeader::SIZE + response.payload.len();
    
    if buf.len() < total_len {
        return Err(WireError::BufferTooSmall {
            expected: total_len,
            actual: buf.len(),
        });
    }
    
    response.header.to_bytes(&mut buf[..ResponseHeader::SIZE])
        .ok_or(WireError::BufferTooSmall { expected: ResponseHeader::SIZE, actual: buf.len() })?;
    
    buf[ResponseHeader::SIZE..total_len].copy_from_slice(response.payload.as_bytes());
    
    Ok(total_len)
}

/// Decode a response from a buffer
pub fn decode_response(buf: &[u8]) -> Result<IoctlResponse, WireError> {
    if buf.len() < ResponseHeader::SIZE {
        return Err(WireError::BufferTooSmall {
            expected: ResponseHeader::SIZE,
            actual: buf.len(),
        });
    }
    
    let header = ResponseHeader::from_bytes(buf)
        .ok_or(WireError::BufferTooSmall { expected: ResponseHeader::SIZE, actual: buf.len() })?;
    
    header.validate()?;
    
    let payload_start = ResponseHeader::SIZE;
    let payload_end = payload_start + header.payload_len as usize;
    
    if buf.len() < payload_end {
        return Err(WireError::BufferTooSmall {
            expected: payload_end,
            actual: buf.len(),
        });
    }
    
    let payload = IoctlPayload::from_bytes(&buf[payload_start..payload_end])?;
    
    Ok(IoctlResponse { header, payload })
}

// Compile-time assertions for struct sizes
const _: () = assert!(mem::size_of::<RequestHeader>() == 32);
const _: () = assert!(mem::size_of::<ResponseHeader>() == 32);
