//! NVML Query Server
//!
//! Provides a simple protocol for the NVML stub library to query GPU information
//! from the broker. This allows nvidia-smi to show real (or mock) GPU data.
//!
//! Protocol:
//! - Client connects to Unix socket
//! - Sends 8-byte request header
//! - Receives fixed-size response based on query type
//!
//! This runs on a separate socket from the main RM API server since it's
//! a simpler request/response protocol (not shared memory rings).

use std::io::{self, Read, Write};
use std::os::unix::net::{UnixListener, UnixStream};

use crate::driver::MockGpuConfig;

// =============================================================================
// PROTOCOL CONSTANTS
// =============================================================================

/// Magic number for NVML protocol ("NVML" in little endian)
pub const NVML_MAGIC: u32 = 0x4C4D564E;

/// Protocol version
pub const NVML_VERSION: u16 = 1;

/// Query types
#[repr(u16)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NvmlQueryType {
    GpuInfo = 1,
    Utilization = 2,
    Temperature = 3,
    Power = 4,
    Clocks = 5,
    PciInfo = 6,
    ProcessList = 7,
}

impl TryFrom<u16> for NvmlQueryType {
    type Error = ();
    
    fn try_from(value: u16) -> Result<Self, Self::Error> {
        match value {
            1 => Ok(NvmlQueryType::GpuInfo),
            2 => Ok(NvmlQueryType::Utilization),
            3 => Ok(NvmlQueryType::Temperature),
            4 => Ok(NvmlQueryType::Power),
            5 => Ok(NvmlQueryType::Clocks),
            6 => Ok(NvmlQueryType::PciInfo),
            7 => Ok(NvmlQueryType::ProcessList),
            _ => Err(()),
        }
    }
}

// =============================================================================
// REQUEST/RESPONSE STRUCTURES
// =============================================================================

/// Request header (8 bytes, matches C struct)
#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub struct NvmlQueryRequest {
    pub magic: u32,
    pub version: u16,
    pub query_type: u16,
}

/// GPU info response (256 bytes, matches C struct)
#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub struct NvmlGpuInfoResponse {
    pub magic: u32,
    pub status: u32,
    pub name: [u8; 64],
    pub uuid: [u8; 48],
    pub memory_total: u64,
    pub memory_free: u64,
    pub memory_used: u64,
    pub temperature: u32,
    pub power_usage: u32,
    pub power_limit: u32,
    pub gpu_utilization: u32,
    pub memory_utilization: u32,
    pub fan_speed: u32,
    pub pcie_gen: u32,
    pub pcie_width: u32,
    pub compute_major: u32,
    pub compute_minor: u32,
    pub sm_count: u32,
    pub driver_version: [u8; 32],
    pub cuda_version: [u8; 16],
    pub _pad: [u8; 20], // Padding to 256 bytes (236 base + 20 = 256)
}

impl Default for NvmlGpuInfoResponse {
    fn default() -> Self {
        Self {
            magic: NVML_MAGIC,
            status: 0,
            name: [0; 64],
            uuid: [0; 48],
            memory_total: 0,
            memory_free: 0,
            memory_used: 0,
            temperature: 0,
            power_usage: 0,
            power_limit: 0,
            gpu_utilization: 0,
            memory_utilization: 0,
            fan_speed: 0,
            pcie_gen: 0,
            pcie_width: 0,
            compute_major: 0,
            compute_minor: 0,
            sm_count: 0,
            driver_version: [0; 32],
            cuda_version: [0; 16],
            _pad: [0; 20],
        }
    }
}

// =============================================================================
// GPU STATE
// =============================================================================

/// Runtime GPU state (things that can change)
#[derive(Debug, Clone)]
pub struct GpuRuntimeState {
    pub memory_used: u64,
    pub temperature: u32,
    pub power_usage: u32,
    pub gpu_utilization: u32,
    pub memory_utilization: u32,
    pub fan_speed: u32,
    pub clock_graphics: u32,
    pub clock_memory: u32,
}

impl Default for GpuRuntimeState {
    fn default() -> Self {
        Self {
            memory_used: 512 * 1024 * 1024, // 512 MB used
            temperature: 45,
            power_usage: 75_000, // 75W in milliwatts
            gpu_utilization: 0,
            memory_utilization: 0,
            fan_speed: 30,
            clock_graphics: 2520,
            clock_memory: 2250,
        }
    }
}

/// Complete GPU info (config + runtime state)
#[derive(Debug, Clone)]
pub struct GpuInfo {
    pub config: MockGpuConfig,
    pub state: GpuRuntimeState,
    pub uuid: String,
    pub cuda_version: String,
}

impl Default for GpuInfo {
    fn default() -> Self {
        Self {
            config: MockGpuConfig::default(),
            state: GpuRuntimeState::default(),
            uuid: "GPU-12345678-1234-1234-1234-123456789abc".to_string(),
            cuda_version: "12.8".to_string(),
        }
    }
}

// =============================================================================
// NVML SERVER
// =============================================================================

/// NVML query server
pub struct NvmlServer {
    listener: UnixListener,
    gpu_info: GpuInfo,
}

impl NvmlServer {
    /// Create a new NVML server on the given socket path
    pub fn new(socket_path: &str) -> io::Result<Self> {
        // Remove old socket if it exists
        let _ = std::fs::remove_file(socket_path);
        
        let listener = UnixListener::bind(socket_path)?;
        listener.set_nonblocking(false)?; // Blocking for simplicity
        
        Ok(Self {
            listener,
            gpu_info: GpuInfo::default(),
        })
    }
    
    /// Create with custom GPU info
    pub fn with_gpu_info(socket_path: &str, gpu_info: GpuInfo) -> io::Result<Self> {
        let _ = std::fs::remove_file(socket_path);
        let listener = UnixListener::bind(socket_path)?;
        listener.set_nonblocking(false)?;
        
        Ok(Self {
            listener,
            gpu_info,
        })
    }
    
    /// Get mutable reference to GPU info for updating state
    pub fn gpu_info_mut(&mut self) -> &mut GpuInfo {
        &mut self.gpu_info
    }
    
    /// Handle one client connection (blocking)
    pub fn handle_one(&self) -> io::Result<()> {
        let (mut stream, _addr) = self.listener.accept()?;
        self.handle_client(&mut stream)
    }
    
    /// Run the server loop
    pub fn run(&self) -> io::Result<()> {
        tracing::info!("NVML server listening");
        
        loop {
            match self.listener.accept() {
                Ok((mut stream, _addr)) => {
                    if let Err(e) = self.handle_client(&mut stream) {
                        tracing::warn!("Error handling NVML client: {}", e);
                    }
                }
                Err(e) => {
                    tracing::error!("Accept error: {}", e);
                }
            }
        }
    }
    
    /// Handle a single client request
    fn handle_client(&self, stream: &mut UnixStream) -> io::Result<()> {
        // Read request header
        let mut req_buf = [0u8; 8];
        stream.read_exact(&mut req_buf)?;
        
        let request = unsafe {
            std::ptr::read_unaligned(req_buf.as_ptr() as *const NvmlQueryRequest)
        };
        
        // Validate magic (copy to local to avoid unaligned access)
        let magic = request.magic;
        if magic != NVML_MAGIC {
            tracing::warn!("Invalid NVML magic: 0x{:08x}", magic);
            return Err(io::Error::new(io::ErrorKind::InvalidData, "Invalid magic"));
        }
        
        // Handle query
        match NvmlQueryType::try_from(request.query_type) {
            Ok(NvmlQueryType::GpuInfo) => {
                let response = self.build_gpu_info_response();
                let response_bytes = unsafe {
                    std::slice::from_raw_parts(
                        &response as *const _ as *const u8,
                        std::mem::size_of::<NvmlGpuInfoResponse>()
                    )
                };
                stream.write_all(response_bytes)?;
            }
            Ok(query_type) => {
                tracing::debug!("Unhandled query type: {:?}", query_type);
                // For now, just return GPU info for all queries
                let response = self.build_gpu_info_response();
                let response_bytes = unsafe {
                    std::slice::from_raw_parts(
                        &response as *const _ as *const u8,
                        std::mem::size_of::<NvmlGpuInfoResponse>()
                    )
                };
                stream.write_all(response_bytes)?;
            }
            Err(_) => {
                let query_type = request.query_type;
                tracing::warn!("Unknown query type: {}", query_type);
                // Send error response
                let mut response = NvmlGpuInfoResponse::default();
                response.status = 1; // Error
                let response_bytes = unsafe {
                    std::slice::from_raw_parts(
                        &response as *const _ as *const u8,
                        std::mem::size_of::<NvmlGpuInfoResponse>()
                    )
                };
                stream.write_all(response_bytes)?;
            }
        }
        
        Ok(())
    }
    
    /// Build GPU info response from current state
    fn build_gpu_info_response(&self) -> NvmlGpuInfoResponse {
        let mut response = NvmlGpuInfoResponse::default();
        
        // Copy name
        let name_bytes = self.gpu_info.config.name.as_bytes();
        let name_len = name_bytes.len().min(63);
        response.name[..name_len].copy_from_slice(&name_bytes[..name_len]);
        
        // Copy UUID
        let uuid_bytes = self.gpu_info.uuid.as_bytes();
        let uuid_len = uuid_bytes.len().min(47);
        response.uuid[..uuid_len].copy_from_slice(&uuid_bytes[..uuid_len]);
        
        // Memory
        response.memory_total = self.gpu_info.config.fb_size;
        response.memory_used = self.gpu_info.state.memory_used;
        response.memory_free = self.gpu_info.config.fb_size.saturating_sub(self.gpu_info.state.memory_used);
        
        // Thermals/power
        response.temperature = self.gpu_info.state.temperature;
        response.power_usage = self.gpu_info.state.power_usage;
        response.power_limit = 350_000; // 350W
        
        // Utilization
        response.gpu_utilization = self.gpu_info.state.gpu_utilization;
        response.memory_utilization = self.gpu_info.state.memory_utilization;
        response.fan_speed = self.gpu_info.state.fan_speed;
        
        // PCIe
        response.pcie_gen = 5;
        response.pcie_width = 16;
        
        // Compute
        response.compute_major = self.gpu_info.config.compute_major;
        response.compute_minor = self.gpu_info.config.compute_minor;
        response.sm_count = self.gpu_info.config.sm_count;
        
        // Versions
        let driver_bytes = self.gpu_info.config.driver_version.as_bytes();
        let driver_len = driver_bytes.len().min(31);
        response.driver_version[..driver_len].copy_from_slice(&driver_bytes[..driver_len]);
        
        let cuda_bytes = self.gpu_info.cuda_version.as_bytes();
        let cuda_len = cuda_bytes.len().min(15);
        response.cuda_version[..cuda_len].copy_from_slice(&cuda_bytes[..cuda_len]);
        
        response
    }
}

impl Drop for NvmlServer {
    fn drop(&mut self) {
        // Try to get socket path and clean up
        // Note: UnixListener doesn't expose the path, so we can't clean up here
        // The caller should clean up the socket file
    }
}

// =============================================================================
// TESTS
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::tempdir;
    use std::thread;
    
    #[test]
    fn test_nvml_server_response() {
        let dir = tempdir().unwrap();
        let socket_path = dir.path().join("nvml.sock");
        let socket_str = socket_path.to_str().unwrap();
        
        // Start server in background thread
        let socket_path_clone = socket_str.to_string();
        let server_thread = thread::spawn(move || {
            let server = NvmlServer::new(&socket_path_clone).unwrap();
            server.handle_one().unwrap();
        });
        
        // Give server time to start
        thread::sleep(std::time::Duration::from_millis(50));
        
        // Connect client
        let mut client = UnixStream::connect(socket_str).unwrap();
        
        // Send request
        let request = NvmlQueryRequest {
            magic: NVML_MAGIC,
            version: NVML_VERSION,
            query_type: NvmlQueryType::GpuInfo as u16,
        };
        let request_bytes = unsafe {
            std::slice::from_raw_parts(
                &request as *const _ as *const u8,
                std::mem::size_of::<NvmlQueryRequest>()
            )
        };
        client.write_all(request_bytes).unwrap();
        
        // Read response
        let mut response_buf = [0u8; std::mem::size_of::<NvmlGpuInfoResponse>()];
        client.read_exact(&mut response_buf).unwrap();
        
        let response = unsafe {
            std::ptr::read_unaligned(response_buf.as_ptr() as *const NvmlGpuInfoResponse)
        };
        
        // Verify (copy from packed struct to avoid unaligned access)
        let magic = response.magic;
        let status = response.status;
        let memory_total = response.memory_total;
        let pcie_width = response.pcie_width;
        
        assert_eq!(magic, NVML_MAGIC);
        assert_eq!(status, 0);
        assert!(memory_total > 0);
        assert_eq!(pcie_width, 16);
        
        // Name should be "NVIDIA RTX PRO 6000"
        let name = std::str::from_utf8(&response.name).unwrap();
        assert!(name.starts_with("NVIDIA RTX PRO 6000"));
        
        server_thread.join().unwrap();
    }
    
    #[test]
    fn test_nvml_response_size() {
        // Ensure response is exactly 256 bytes for C compatibility
        assert_eq!(std::mem::size_of::<NvmlGpuInfoResponse>(), 256);
    }
    
    #[test]
    fn test_nvml_request_size() {
        // Ensure request is exactly 8 bytes
        assert_eq!(std::mem::size_of::<NvmlQueryRequest>(), 8);
    }
}
