//! NVIDIA Driver Proxy
//!
//! This module provides the interface to the real NVIDIA kernel driver.
//! It wraps ioctls to /dev/nvidiactl and /dev/nvidia0, translating handles
//! between virtual (client-facing) and real (driver-facing) spaces.
//!
//! The proxy can operate in two modes:
//! 1. Real mode: Actually talks to /dev/nvidiactl (requires hardware)
//! 2. Mock mode: Simulates driver responses (for testing)
//!
//! ```text
//!   BrokerState
//!       │
//!       ▼ (virtual handles)
//!   ┌───────────────┐
//!   │ HandleTable   │  translate virtual → real
//!   └───────────────┘
//!       │
//!       ▼ (real handles)
//!   ┌───────────────┐
//!   │ NvDriver      │  ioctl wrapper
//!   └───────────────┘
//!       │
//!       ▼
//!   /dev/nvidiactl
//! ```

use std::collections::HashMap;
use std::fs::OpenOptions;
use std::io;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};

use crate::rm_api::{
  NV_RM_API_VERSION_CMD_QUERY, NV_RM_API_VERSION_REPLY_RECOGNIZED, NvCardInfo, NvHandle,
  NvOs00Params, NvOs02Params, NvOs21Params, NvOs33Params, NvOs34Params, NvOs54Params, NvOs55Params,
  NvOs57Params, NvPciInfo, NvRmApiVersion, classes,
};

// =============================================================================
// IOCTL DEFINITIONS
// =============================================================================

/// NVIDIA ioctl magic number ('F')
const NV_IOCTL_MAGIC: u8 = b'F';

/// Generate ioctl command number for NVIDIA escape codes
/// Uses _IOWR (read/write) since all RM calls both send and receive data
///
/// Linux ioctl encoding: _IOWR(type, nr, size)
///   = ((_IOC_READ|_IOC_WRITE) << 30) | (size << 16) | (type << 8) | nr
///   = (3 << 30) | (size << 16) | (type << 8) | nr
macro_rules! nv_ioctl {
  ($escape:expr, $ty:ty) => {{
    const DIR: u64 = 3; // _IOC_READ | _IOC_WRITE
    const TYPE: u64 = NV_IOCTL_MAGIC as u64;
    const NR: u64 = $escape as u64;
    const SIZE: u64 = std::mem::size_of::<$ty>() as u64;
    (DIR << 30) | (SIZE << 16) | (TYPE << 8) | NR
  }};
}

// Pre-computed ioctl numbers for each escape code
const IOCTL_ALLOC: libc::c_ulong = nv_ioctl!(0x2B, NvOs21Params);
const IOCTL_FREE: libc::c_ulong = nv_ioctl!(0x29, NvOs00Params);
const IOCTL_CONTROL: libc::c_ulong = nv_ioctl!(0x2A, NvOs54Params);
const IOCTL_ALLOC_MEMORY: libc::c_ulong = nv_ioctl!(0x27, NvOs02Params);
const IOCTL_MAP_MEMORY: libc::c_ulong = nv_ioctl!(0x4E, NvOs33Params);
const IOCTL_UNMAP_MEMORY: libc::c_ulong = nv_ioctl!(0x4F, NvOs34Params);
const IOCTL_DUP_OBJECT: libc::c_ulong = nv_ioctl!(0x34, NvOs55Params);
const IOCTL_SHARE: libc::c_ulong = nv_ioctl!(0x35, NvOs57Params);

// =============================================================================
// DRIVER TRAIT
// =============================================================================

/// Error type for driver operations
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DriverError {
  /// ioctl failed with OS error
  IoError(i32),

  /// Driver returned error status
  NvStatus(u32),

  /// Invalid parameters
  InvalidParams(String),

  /// Device not open
  NotOpen,
}

impl std::fmt::Display for DriverError {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    match self {
      DriverError::IoError(e) => write!(f, "IO error: {}", e),
      DriverError::NvStatus(s) => write!(f, "NV status: 0x{:08x}", s),
      DriverError::InvalidParams(s) => write!(f, "Invalid params: {}", s),
      DriverError::NotOpen => write!(f, "Device not open"),
    }
  }
}

impl std::error::Error for DriverError {}

pub type DriverResult<T> = Result<T, DriverError>;

/// Trait for NVIDIA driver operations
///
/// This allows both real and mock implementations for testing.
pub trait Driver: Send {
  /// Allocate a new GPU object (NV_ESC_RM_ALLOC)
  fn alloc(
    &mut self,
    h_root: NvHandle,
    h_parent: NvHandle,
    h_object: NvHandle,
    h_class: u32,
    params: Option<&[u8]>,
  ) -> DriverResult<u32>; // Returns status

  /// Free a GPU object (NV_ESC_RM_FREE)
  fn free(&mut self, h_root: NvHandle, h_parent: NvHandle, h_object: NvHandle)
  -> DriverResult<u32>;

  /// Control call (NV_ESC_RM_CONTROL)
  fn control(
    &mut self,
    h_client: NvHandle,
    h_object: NvHandle,
    cmd: u32,
    params: &mut [u8],
  ) -> DriverResult<u32>;

  /// Allocate memory (NV_ESC_RM_ALLOC_MEMORY)
  fn alloc_memory(
    &mut self,
    h_root: NvHandle,
    h_parent: NvHandle,
    h_memory: NvHandle,
    h_class: u32,
    flags: u32,
    size: u64,
  ) -> DriverResult<(u64, u32)>; // Returns (address, status)

  /// Map memory (NV_ESC_RM_MAP_MEMORY)
  fn map_memory(
    &mut self,
    h_client: NvHandle,
    h_device: NvHandle,
    h_memory: NvHandle,
    offset: u64,
    length: u64,
    flags: u32,
  ) -> DriverResult<(u64, u32)>; // Returns (mapped_address, status)

  /// Unmap memory (NV_ESC_RM_UNMAP_MEMORY)
  fn unmap_memory(
    &mut self,
    h_client: NvHandle,
    h_device: NvHandle,
    h_memory: NvHandle,
    address: u64,
    flags: u32,
  ) -> DriverResult<u32>;

  /// Get card info (NV_ESC_CARD_INFO)
  /// Returns information about installed GPUs
  fn card_info(&self, cards: &mut [NvCardInfo]) -> DriverResult<usize>;

  /// Check version string (NV_ESC_CHECK_VERSION_STR)
  /// Validates driver version compatibility
  fn check_version(&self, version: &mut NvRmApiVersion) -> DriverResult<()>;

  /// Check if the driver is connected to real hardware
  fn is_real(&self) -> bool;
}

// =============================================================================
// REAL NVIDIA DRIVER
// =============================================================================

/// Real NVIDIA driver interface
///
/// Opens /dev/nvidiactl and performs actual ioctls.
pub struct NvDriver {
  /// File descriptor for /dev/nvidiactl
  ctl_fd: OwnedFd,

  /// File descriptor for /dev/nvidia0 (optional, for specific GPU ops)
  #[allow(dead_code)]
  gpu_fd: Option<OwnedFd>,
}

impl NvDriver {
  /// Open the NVIDIA control device
  pub fn open() -> io::Result<Self> {
    Self::open_path("/dev/nvidiactl", None)
  }

  /// Open with specific device paths
  pub fn open_path(ctl_path: &str, gpu_path: Option<&str>) -> io::Result<Self> {
    let ctl_file = OpenOptions::new().read(true).write(true).open(ctl_path)?;

    let gpu_fd = if let Some(path) = gpu_path {
      let gpu_file = OpenOptions::new().read(true).write(true).open(path)?;
      Some(unsafe { OwnedFd::from_raw_fd(gpu_file.into_raw_fd()) })
    } else {
      None
    };

    Ok(Self {
      ctl_fd: unsafe { OwnedFd::from_raw_fd(ctl_file.into_raw_fd()) },
      gpu_fd,
    })
  }

  /// Get raw fd for direct operations
  pub fn as_raw_fd(&self) -> RawFd {
    self.ctl_fd.as_raw_fd()
  }

  /// Perform raw ioctl
  ///
  /// Safety: caller must ensure params is valid for the ioctl
  unsafe fn raw_ioctl<T>(&self, cmd: libc::c_ulong, params: &mut T) -> io::Result<()> {
    // On musl, ioctl takes i32 for request; on glibc it's c_ulong.
    // Cast to c_int for portability.
    let ret = libc::ioctl(
      self.ctl_fd.as_raw_fd(),
      cmd as libc::c_int,
      params as *mut T,
    );
    if ret < 0 {
      Err(io::Error::last_os_error())
    } else {
      Ok(())
    }
  }
}

impl Driver for NvDriver {
  fn alloc(
    &mut self,
    h_root: NvHandle,
    h_parent: NvHandle,
    h_object: NvHandle,
    h_class: u32,
    params: Option<&[u8]>,
  ) -> DriverResult<u32> {
    let mut nvos21 = NvOs21Params {
      h_root,
      h_object_parent: h_parent,
      h_object_new: h_object,
      h_class,
      p_alloc_parms: params.map(|p| p.as_ptr() as u64).unwrap_or(0),
      status: 0,
      _pad: 0,
    };

    unsafe {
      self
        .raw_ioctl(IOCTL_ALLOC, &mut nvos21)
        .map_err(|e| DriverError::IoError(e.raw_os_error().unwrap_or(-1)))?;
    }

    Ok(nvos21.status)
  }

  fn free(
    &mut self,
    h_root: NvHandle,
    h_parent: NvHandle,
    h_object: NvHandle,
  ) -> DriverResult<u32> {
    let mut nvos00 = NvOs00Params {
      h_root,
      h_object_parent: h_parent,
      h_object_old: h_object,
      status: 0,
    };

    unsafe {
      self
        .raw_ioctl(IOCTL_FREE, &mut nvos00)
        .map_err(|e| DriverError::IoError(e.raw_os_error().unwrap_or(-1)))?;
    }

    Ok(nvos00.status)
  }

  fn control(
    &mut self,
    h_client: NvHandle,
    h_object: NvHandle,
    cmd: u32,
    params: &mut [u8],
  ) -> DriverResult<u32> {
    let mut nvos54 = NvOs54Params {
      h_client,
      h_object,
      cmd,
      flags: 0,
      params: params.as_mut_ptr() as u64,
      params_size: params.len() as u32,
      status: 0,
    };

    unsafe {
      self
        .raw_ioctl(IOCTL_CONTROL, &mut nvos54)
        .map_err(|e| DriverError::IoError(e.raw_os_error().unwrap_or(-1)))?;
    }

    Ok(nvos54.status)
  }

  fn alloc_memory(
    &mut self,
    h_root: NvHandle,
    h_parent: NvHandle,
    h_memory: NvHandle,
    h_class: u32,
    flags: u32,
    size: u64,
  ) -> DriverResult<(u64, u32)> {
    let mut nvos02 = NvOs02Params {
      h_root,
      h_object_parent: h_parent,
      h_object_new: h_memory,
      h_class,
      flags,
      _pad0: 0,
      p_memory: 0,     // Output
      limit: size - 1, // NVIDIA uses limit = size - 1
      status: 0,
    };

    unsafe {
      self
        .raw_ioctl(IOCTL_ALLOC_MEMORY, &mut nvos02)
        .map_err(|e| DriverError::IoError(e.raw_os_error().unwrap_or(-1)))?;
    }

    Ok((nvos02.p_memory, nvos02.status))
  }

  fn map_memory(
    &mut self,
    h_client: NvHandle,
    h_device: NvHandle,
    h_memory: NvHandle,
    offset: u64,
    length: u64,
    flags: u32,
  ) -> DriverResult<(u64, u32)> {
    let mut nvos33 = NvOs33Params {
      h_client,
      h_device,
      h_memory,
      _pad0: 0,
      offset,
      length,
      p_linear_address: 0, // Output
      status: 0,
      flags,
    };

    unsafe {
      self
        .raw_ioctl(IOCTL_MAP_MEMORY, &mut nvos33)
        .map_err(|e| DriverError::IoError(e.raw_os_error().unwrap_or(-1)))?;
    }

    Ok((nvos33.p_linear_address, nvos33.status))
  }

  fn unmap_memory(
    &mut self,
    h_client: NvHandle,
    h_device: NvHandle,
    h_memory: NvHandle,
    address: u64,
    flags: u32,
  ) -> DriverResult<u32> {
    let mut nvos34 = NvOs34Params {
      h_client,
      h_device,
      h_memory,
      _pad0: 0,
      p_linear_address: address,
      status: 0,
      flags,
    };

    unsafe {
      self
        .raw_ioctl(IOCTL_UNMAP_MEMORY, &mut nvos34)
        .map_err(|e| DriverError::IoError(e.raw_os_error().unwrap_or(-1)))?;
    }

    Ok(nvos34.status)
  }

  fn card_info(&self, cards: &mut [NvCardInfo]) -> DriverResult<usize> {
    // For real driver, we'd do the actual ioctl
    // NV_ESC_CARD_INFO returns array of card info structs
    // For now, return error since we'd need to implement the actual ioctl
    if cards.is_empty() {
      return Ok(0);
    }

    // TODO: Implement actual CARD_INFO ioctl
    // The ioctl uses escape code 200 and expects an array of nv_ioctl_card_info_t
    Err(DriverError::InvalidParams(
      "Real card_info not implemented".to_string(),
    ))
  }

  fn check_version(&self, version: &mut NvRmApiVersion) -> DriverResult<()> {
    // For real driver, we'd do the actual ioctl
    // NV_ESC_CHECK_VERSION_STR validates version compatibility
    // TODO: Implement actual CHECK_VERSION_STR ioctl
    Err(DriverError::InvalidParams(
      "Real check_version not implemented".to_string(),
    ))
  }

  fn is_real(&self) -> bool {
    true
  }
}

// Need to use into_raw_fd to avoid double-close
use std::os::fd::IntoRawFd;

// =============================================================================
// MOCK DRIVER (for testing)
// =============================================================================

/// Mock GPU configuration for realistic simulation
#[derive(Debug, Clone)]
pub struct MockGpuConfig {
  /// GPU name (e.g., "NVIDIA RTX PRO 6000")
  pub name: String,

  /// PCI device ID
  pub device_id: u16,

  /// PCI vendor ID (0x10DE for NVIDIA)
  pub vendor_id: u16,

  /// Total framebuffer memory in bytes
  pub fb_size: u64,

  /// BAR0 register size
  pub reg_size: u64,

  /// Compute capability major version
  pub compute_major: u32,

  /// Compute capability minor version
  pub compute_minor: u32,

  /// Number of SMs
  pub sm_count: u32,

  /// Driver version string
  pub driver_version: String,
}

impl Default for MockGpuConfig {
  fn default() -> Self {
    Self {
      name: "NVIDIA RTX PRO 6000".to_string(),
      device_id: 0x2900, // Blackwell
      vendor_id: 0x10DE,
      fb_size: 128 * 1024 * 1024 * 1024, // 128GB
      reg_size: 32 * 1024 * 1024,        // 32MB BAR0
      compute_major: 10,
      compute_minor: 0,
      sm_count: 170,
      driver_version: "570.00.00".to_string(),
    }
  }
}

/// Mock NVIDIA driver for testing without hardware
///
/// Simulates driver behavior with configurable responses.
/// Can be configured to simulate different GPUs with realistic properties.
pub struct MockDriver {
  /// GPU configuration
  pub config: MockGpuConfig,

  /// Allocated objects: handle -> (parent, class)
  objects: HashMap<NvHandle, (NvHandle, u32)>,

  /// Allocated memory: handle -> (address, size)
  memory: HashMap<NvHandle, (u64, u64)>,

  /// Memory mappings: (client, device, memory) -> address
  mappings: HashMap<(NvHandle, NvHandle, NvHandle), u64>,

  /// Next virtual address to return for mappings
  next_address: u64,

  /// Configurable error injection
  inject_error: Option<u32>,

  /// Call log for verification
  call_log: Vec<MockCall>,

  /// Control command handlers for realistic responses
  control_handlers: HashMap<u32, ControlHandler>,
}

/// Handler for control commands
type ControlHandler = Box<dyn Fn(&MockGpuConfig, &mut [u8]) -> u32 + Send>;

/// Logged calls for test verification
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum MockCall {
  Alloc {
    h_root: NvHandle,
    h_parent: NvHandle,
    h_object: NvHandle,
    h_class: u32,
  },
  Free {
    h_root: NvHandle,
    h_parent: NvHandle,
    h_object: NvHandle,
  },
  Control {
    h_client: NvHandle,
    h_object: NvHandle,
    cmd: u32,
  },
  AllocMemory {
    h_root: NvHandle,
    h_parent: NvHandle,
    h_memory: NvHandle,
    size: u64,
  },
  MapMemory {
    h_client: NvHandle,
    h_device: NvHandle,
    h_memory: NvHandle,
  },
  UnmapMemory {
    h_client: NvHandle,
    h_device: NvHandle,
    h_memory: NvHandle,
  },
}

impl MockDriver {
  /// Create a new mock driver with default GPU config
  pub fn new() -> Self {
    Self::with_config(MockGpuConfig::default())
  }

  /// Create a mock driver with custom GPU configuration
  pub fn with_config(config: MockGpuConfig) -> Self {
    let mut driver = Self {
      config,
      objects: HashMap::new(),
      memory: HashMap::new(),
      mappings: HashMap::new(),
      next_address: 0x1000_0000_0000, // Start at high address
      inject_error: None,
      call_log: Vec::new(),
      control_handlers: HashMap::new(),
    };

    // Register default control handlers
    driver.register_default_handlers();

    driver
  }

  /// Register default control command handlers
  fn register_default_handlers(&mut self) {
    // NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION (0x101)
    self.control_handlers.insert(
      0x00000101,
      Box::new(|config, params| {
        // Returns driver version string
        if params.len() >= 64 {
          let version = config.driver_version.as_bytes();
          let len = version.len().min(63);
          params[..len].copy_from_slice(&version[..len]);
          params[len] = 0; // null terminate
        }
        0 // NV_OK
      }),
    );

    // NV0080_CTRL_CMD_GPU_GET_NUM_SUBDEVICES (0x80280101)
    self.control_handlers.insert(
      0x80280101,
      Box::new(|_config, params| {
        // Returns 1 subdevice
        if params.len() >= 4 {
          params[0..4].copy_from_slice(&1u32.to_le_bytes());
        }
        0
      }),
    );

    // NV2080_CTRL_CMD_GPU_GET_INFO_V2 (0x20800102)
    self.control_handlers.insert(
      0x20800102,
      Box::new(|config, params| {
        // Returns GPU info
        if params.len() >= 32 {
          // Simplified: just return SM count and memory size
          params[0..4].copy_from_slice(&config.sm_count.to_le_bytes());
          params[8..16].copy_from_slice(&config.fb_size.to_le_bytes());
        }
        0
      }),
    );

    // NV2080_CTRL_CMD_FB_GET_INFO_V2 (0x20801303)
    self.control_handlers.insert(
      0x20801303,
      Box::new(|config, params| {
        // Returns framebuffer info
        if params.len() >= 16 {
          params[0..8].copy_from_slice(&config.fb_size.to_le_bytes());
          params[8..16].copy_from_slice(&config.fb_size.to_le_bytes()); // usable = total
        }
        0
      }),
    );

    // NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS (0x201)
    self.control_handlers.insert(
      0x00000201,
      Box::new(|_config, params| {
        // Returns one GPU with ID 0
        if params.len() >= 8 {
          params[0..4].copy_from_slice(&1u32.to_le_bytes()); // count
          params[4..8].copy_from_slice(&0u32.to_le_bytes()); // gpu_id[0]
        }
        0
      }),
    );

    // NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2 (0x202)
    self.control_handlers.insert(
      0x00000202,
      Box::new(|config, params| {
        // Returns GPU info for ID
        if params.len() >= 32 {
          params[0..2].copy_from_slice(&config.device_id.to_le_bytes());
          params[2..4].copy_from_slice(&config.vendor_id.to_le_bytes());
          // PCI BDF: domain=0, bus=1, device=0, function=0
          params[4] = 0; // domain
          params[5] = 1; // bus
          params[6] = 0; // device
          params[7] = 0; // function
        }
        0
      }),
    );

    // NV2080_CTRL_CMD_GPU_GET_NAME_STRING (0x20800110)
    // Returns the GPU name like "NVIDIA RTX PRO 6000"
    self.control_handlers.insert(
      0x20800110,
      Box::new(|config, params| {
        // struct has: u32 flags, then name string (64 bytes ASCII or 128 bytes unicode)
        if params.len() >= 68 {
          let flags = u32::from_le_bytes([params[0], params[1], params[2], params[3]]);
          if flags == 0 {
            // ASCII mode
            let name = config.name.as_bytes();
            let len = name.len().min(63);
            params[4..4 + len].copy_from_slice(&name[..len]);
            params[4 + len] = 0; // null terminate
          }
          // Unicode mode not implemented
        }
        0
      }),
    );

    // NV2080_CTRL_CMD_GPU_GET_SHORT_NAME_STRING (0x20800111)
    self.control_handlers.insert(
      0x20800111,
      Box::new(|config, params| {
        if params.len() >= 64 {
          let name = config.name.as_bytes();
          let len = name.len().min(63);
          params[..len].copy_from_slice(&name[..len]);
          params[len] = 0;
        }
        0
      }),
    );

    // NV2080_CTRL_CMD_THERM_GET_TEMPERATURE (0x20800240 approx)
    // Returns GPU temperature
    self.control_handlers.insert(
      0x20800240,
      Box::new(|_config, params| {
        if params.len() >= 8 {
          // Return simulated temperature: 45C
          let temp: i32 = 45;
          params[0..4].copy_from_slice(&temp.to_le_bytes());
        }
        0
      }),
    );

    // NV2080_CTRL_CMD_GPU_QUERY_ECC_STATUS (0x2080012f)
    // Returns ECC status
    self.control_handlers.insert(
      0x2080012f,
      Box::new(|_config, params| {
        if params.len() >= 4 {
          // ECC enabled = 1
          params[0..4].copy_from_slice(&1u32.to_le_bytes());
        }
        0
      }),
    );

    // NV2080_CTRL_CMD_PERF_GET_CURRENT_PSTATE (0x20802068)
    // Returns current performance state
    self.control_handlers.insert(
      0x20802068,
      Box::new(|_config, params| {
        if params.len() >= 4 {
          // P0 (max performance)
          params[0..4].copy_from_slice(&0u32.to_le_bytes());
        }
        0
      }),
    );

    // NV2080_CTRL_CMD_GPU_GET_COMPUTE_POLICY_CONFIG (0x20800195)
    self.control_handlers.insert(
      0x20800195,
      Box::new(|config, params| {
        if params.len() >= 8 {
          // SM count
          params[0..4].copy_from_slice(&config.sm_count.to_le_bytes());
          // Compute capability: major.minor
          params[4..8]
            .copy_from_slice(&((config.compute_major << 16) | config.compute_minor).to_le_bytes());
        }
        0
      }),
    );

    // NV0000_CTRL_CMD_SYSTEM_GET_CPU_INFO (0x102)
    self.control_handlers.insert(
      0x00000102,
      Box::new(|_config, params| {
        if params.len() >= 16 {
          // Return some plausible CPU info
          params[0..4].copy_from_slice(&64u32.to_le_bytes()); // CPU cores
          params[4..8].copy_from_slice(&2u32.to_le_bytes()); // NUMA nodes
        }
        0
      }),
    );

    // NV2080_CTRL_CMD_RC_GET_WATCHDOG_INFO (0x20802209)
    self.control_handlers.insert(
      0x20802209,
      Box::new(|_config, params| {
        if params.len() >= 8 {
          // Watchdog disabled = 0
          params[0..4].copy_from_slice(&0u32.to_le_bytes());
        }
        0
      }),
    );

    // NV2080_CTRL_CMD_BUS_GET_PCI_INFO (0x20801801)
    self.control_handlers.insert(
      0x20801801,
      Box::new(|config, params| {
        if params.len() >= 16 {
          params[0..2].copy_from_slice(&config.vendor_id.to_le_bytes());
          params[2..4].copy_from_slice(&config.device_id.to_le_bytes());
          // Link speed/width
          params[4..8].copy_from_slice(&16u32.to_le_bytes()); // x16
          params[8..12].copy_from_slice(&5u32.to_le_bytes()); // Gen5
        }
        0
      }),
    );

    // NV2080_CTRL_CMD_FB_GET_INFO (0x20801301)
    self.control_handlers.insert(
      0x20801301,
      Box::new(|config, params| {
        if params.len() >= 32 {
          // Total FB size
          params[0..8].copy_from_slice(&config.fb_size.to_le_bytes());
          // Free FB (simulate 90% free)
          let free = config.fb_size * 9 / 10;
          params[8..16].copy_from_slice(&free.to_le_bytes());
          // Used FB
          let used = config.fb_size / 10;
          params[16..24].copy_from_slice(&used.to_le_bytes());
        }
        0
      }),
    );

    // NV2080_CTRL_CMD_GPU_GET_ENGINES_V2 (0x20800123)
    // Returns available GPU engines
    self.control_handlers.insert(
      0x20800123,
      Box::new(|_config, params| {
        if params.len() >= 16 {
          // Engine count
          params[0..4].copy_from_slice(&8u32.to_le_bytes());
          // Engine types: GR, COPY, NVDEC, NVENC, etc
        }
        0
      }),
    );

    // NV2080_CTRL_CMD_GPU_GET_GID_INFO (0x2080014a)
    // Returns GPU UUID
    self.control_handlers.insert(
      0x2080014a,
      Box::new(|_config, params| {
        if params.len() >= 32 {
          // Simulated GPU UUID
          let uuid = b"GPU-12345678-1234-1234-1234-123456789abc";
          let len = uuid.len().min(params.len());
          params[..len].copy_from_slice(&uuid[..len]);
        }
        0
      }),
    );
  }

  /// Inject an error for the next call
  pub fn inject_error(&mut self, status: u32) {
    self.inject_error = Some(status);
  }

  /// Get the call log
  pub fn call_log(&self) -> &[MockCall] {
    &self.call_log
  }

  /// Clear the call log
  pub fn clear_log(&mut self) {
    self.call_log.clear();
  }

  /// Check for injected error
  fn check_error(&mut self) -> Option<u32> {
    self.inject_error.take()
  }
}

impl Default for MockDriver {
  fn default() -> Self {
    Self::new()
  }
}

impl Driver for MockDriver {
  fn alloc(
    &mut self,
    h_root: NvHandle,
    h_parent: NvHandle,
    h_object: NvHandle,
    h_class: u32,
    _params: Option<&[u8]>,
  ) -> DriverResult<u32> {
    self.call_log.push(MockCall::Alloc {
      h_root,
      h_parent,
      h_object,
      h_class,
    });

    if let Some(status) = self.check_error() {
      return Ok(status);
    }

    // Check if object already exists
    if self.objects.contains_key(&h_object) {
      return Ok(0x00000005); // NV_ERR_INVALID_OBJECT (duplicate)
    }

    self.objects.insert(h_object, (h_parent, h_class));
    Ok(0) // NV_OK
  }

  fn free(
    &mut self,
    h_root: NvHandle,
    h_parent: NvHandle,
    h_object: NvHandle,
  ) -> DriverResult<u32> {
    self.call_log.push(MockCall::Free {
      h_root,
      h_parent,
      h_object,
    });

    if let Some(status) = self.check_error() {
      return Ok(status);
    }

    // Check if object exists
    if self.objects.remove(&h_object).is_none() {
      return Ok(0x00000005); // NV_ERR_INVALID_OBJECT
    }

    // Also clean up any memory associated with this object
    self.memory.remove(&h_object);

    Ok(0) // NV_OK
  }

  fn control(
    &mut self,
    h_client: NvHandle,
    h_object: NvHandle,
    cmd: u32,
    params: &mut [u8],
  ) -> DriverResult<u32> {
    self.call_log.push(MockCall::Control {
      h_client,
      h_object,
      cmd,
    });

    if let Some(status) = self.check_error() {
      return Ok(status);
    }

    // Verify the object exists (handle 0 is allowed for root-level commands)
    if !self.objects.contains_key(&h_object) && h_object != 0 {
      return Ok(0x00000005); // NV_ERR_INVALID_OBJECT
    }

    // Look up handler for this command
    // We need to temporarily remove the handler to avoid borrow issues
    if let Some(handler) = self.control_handlers.get(&cmd) {
      // Call the handler with config reference and params
      let status = handler(&self.config, params);
      return Ok(status);
    }

    // No handler registered - return success with no data modification
    // This allows unknown commands to pass through (useful for testing)
    Ok(0) // NV_OK
  }

  fn alloc_memory(
    &mut self,
    h_root: NvHandle,
    h_parent: NvHandle,
    h_memory: NvHandle,
    _h_class: u32,
    _flags: u32,
    size: u64,
  ) -> DriverResult<(u64, u32)> {
    self.call_log.push(MockCall::AllocMemory {
      h_root,
      h_parent,
      h_memory,
      size,
    });

    if let Some(status) = self.check_error() {
      return Ok((0, status));
    }

    // Allocate virtual address
    let address = self.next_address;
    self.next_address += (size + 0xFFF) & !0xFFF; // Page-align

    self.memory.insert(h_memory, (address, size));
    self.objects.insert(h_memory, (h_parent, 0x3E)); // NV01_MEMORY_SYSTEM

    Ok((address, 0))
  }

  fn map_memory(
    &mut self,
    h_client: NvHandle,
    h_device: NvHandle,
    h_memory: NvHandle,
    _offset: u64,
    _length: u64,
    _flags: u32,
  ) -> DriverResult<(u64, u32)> {
    self.call_log.push(MockCall::MapMemory {
      h_client,
      h_device,
      h_memory,
    });

    if let Some(status) = self.check_error() {
      return Ok((0, status));
    }

    // Check memory exists
    let (address, _size) = self
      .memory
      .get(&h_memory)
      .ok_or_else(|| DriverError::NvStatus(0x00000005))?;

    // Record mapping
    let key = (h_client, h_device, h_memory);
    self.mappings.insert(key, *address);

    Ok((*address, 0))
  }

  fn unmap_memory(
    &mut self,
    h_client: NvHandle,
    h_device: NvHandle,
    h_memory: NvHandle,
    _address: u64,
    _flags: u32,
  ) -> DriverResult<u32> {
    self.call_log.push(MockCall::UnmapMemory {
      h_client,
      h_device,
      h_memory,
    });

    if let Some(status) = self.check_error() {
      return Ok(status);
    }

    // Remove mapping
    let key = (h_client, h_device, h_memory);
    if self.mappings.remove(&key).is_none() {
      return Ok(0x00000005); // NV_ERR_INVALID_OBJECT
    }

    Ok(0)
  }

  fn card_info(&self, cards: &mut [NvCardInfo]) -> DriverResult<usize> {
    if cards.is_empty() {
      return Ok(0);
    }

    // Return info for our simulated GPU
    let card = &mut cards[0];
    card.valid = 1; // NV_TRUE
    card.pci_info = NvPciInfo {
      domain: 0,
      bus: 1,
      slot: 0,
      function: 0,
      _pad0: 0,
      vendor_id: self.config.vendor_id,
      device_id: self.config.device_id,
    };
    card.gpu_id = 0;
    card.interrupt_line = 16;
    card._pad0 = [0; 2];
    card.reg_address = 0xFD00_0000; // Typical BAR0 address
    card.reg_size = self.config.reg_size;
    card.fb_address = 0xE000_0000_0000; // Typical BAR1 address
    card.fb_size = self.config.fb_size;
    card.minor_number = 0;
    // Copy device name
    let name = b"nvidia0\0\0\0";
    card.dev_name.copy_from_slice(name);
    card._pad1 = [0; 2];

    Ok(1) // We have 1 GPU
  }

  fn check_version(&self, version: &mut NvRmApiVersion) -> DriverResult<()> {
    // Handle version check based on command
    match version.cmd {
      NV_RM_API_VERSION_CMD_QUERY => {
        // Query mode: just return our version
        let ver_bytes = self.config.driver_version.as_bytes();
        let len = ver_bytes.len().min(63);
        version.version_string[..len].copy_from_slice(&ver_bytes[..len]);
        version.version_string[len] = 0;
        version.reply = NV_RM_API_VERSION_REPLY_RECOGNIZED;
      }
      _ => {
        // Strict or relaxed mode: check if versions match
        // For mock, always say recognized
        version.reply = NV_RM_API_VERSION_REPLY_RECOGNIZED;
      }
    }
    Ok(())
  }

  fn is_real(&self) -> bool {
    false
  }
}

// =============================================================================
// TESTS
// =============================================================================

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn test_mock_driver_alloc_free() {
    let mut driver = MockDriver::new();

    // Allocate root client
    let status = driver
      .alloc(0, 0, 0x1, classes::NV01_ROOT_CLIENT, None)
      .unwrap();
    assert_eq!(status, 0);

    // Allocate device
    let status = driver
      .alloc(0x1, 0x1, 0x2, classes::NV01_DEVICE_0, None)
      .unwrap();
    assert_eq!(status, 0);

    // Free device
    let status = driver.free(0x1, 0x1, 0x2).unwrap();
    assert_eq!(status, 0);

    // Double free should fail
    let status = driver.free(0x1, 0x1, 0x2).unwrap();
    assert_ne!(status, 0);

    // Verify call log
    assert_eq!(driver.call_log().len(), 4);
  }

  #[test]
  fn test_mock_driver_duplicate_alloc() {
    let mut driver = MockDriver::new();

    // Allocate once
    let status = driver
      .alloc(0, 0, 0x1, classes::NV01_ROOT_CLIENT, None)
      .unwrap();
    assert_eq!(status, 0);

    // Duplicate should fail
    let status = driver
      .alloc(0, 0, 0x1, classes::NV01_ROOT_CLIENT, None)
      .unwrap();
    assert_ne!(status, 0);
  }

  #[test]
  fn test_mock_driver_memory() {
    let mut driver = MockDriver::new();

    // Setup client and device
    driver
      .alloc(0, 0, 0x1, classes::NV01_ROOT_CLIENT, None)
      .unwrap();
    driver
      .alloc(0x1, 0x1, 0x2, classes::NV01_DEVICE_0, None)
      .unwrap();

    // Allocate memory
    let (addr, status) = driver
      .alloc_memory(0x1, 0x2, 0x100, classes::NV01_MEMORY_SYSTEM, 0, 4096)
      .unwrap();
    assert_eq!(status, 0);
    assert!(addr > 0);

    // Map memory
    let (mapped, status) = driver.map_memory(0x1, 0x2, 0x100, 0, 4096, 0).unwrap();
    assert_eq!(status, 0);
    assert_eq!(mapped, addr);

    // Unmap
    let status = driver.unmap_memory(0x1, 0x2, 0x100, mapped, 0).unwrap();
    assert_eq!(status, 0);
  }

  #[test]
  fn test_mock_driver_error_injection() {
    let mut driver = MockDriver::new();

    // Inject error
    driver.inject_error(0x00000003); // NV_ERR_INVALID_ARGUMENT

    let status = driver
      .alloc(0, 0, 0x1, classes::NV01_ROOT_CLIENT, None)
      .unwrap();
    assert_eq!(status, 0x00000003);

    // Next call should succeed (injection is one-shot)
    let status = driver
      .alloc(0, 0, 0x1, classes::NV01_ROOT_CLIENT, None)
      .unwrap();
    assert_eq!(status, 0);
  }

  #[test]
  fn test_mock_driver_control() {
    let mut driver = MockDriver::new();

    // Setup
    driver
      .alloc(0, 0, 0x1, classes::NV01_ROOT_CLIENT, None)
      .unwrap();

    // Control call
    let mut params = [0u8; 32];
    let status = driver.control(0x1, 0x1, 0x12345, &mut params).unwrap();
    assert_eq!(status, 0);

    // Verify log
    assert!(
      driver
        .call_log()
        .iter()
        .any(|c| matches!(c, MockCall::Control { cmd: 0x12345, .. }))
    );
  }

  #[test]
  fn test_ioctl_numbers() {
    // Verify our ioctl numbers are computed correctly
    // These should match the kernel driver expectations
    assert!(IOCTL_ALLOC > 0);
    assert!(IOCTL_FREE > 0);
    assert!(IOCTL_CONTROL > 0);

    // The numbers should be different
    assert_ne!(IOCTL_ALLOC, IOCTL_FREE);
    assert_ne!(IOCTL_ALLOC, IOCTL_CONTROL);
    assert_ne!(IOCTL_FREE, IOCTL_CONTROL);
  }

  #[test]
  fn test_mock_driver_card_info() {
    let driver = MockDriver::new();
    let mut cards = [NvCardInfo::default()];

    let count = driver.card_info(&mut cards).unwrap();
    assert_eq!(count, 1);
    assert_eq!(cards[0].valid, 1);
    assert_eq!(cards[0].pci_info.vendor_id, 0x10DE);
    assert_eq!(cards[0].pci_info.device_id, 0x2900); // Blackwell
    assert_eq!(cards[0].fb_size, 128 * 1024 * 1024 * 1024); // 128GB
  }

  #[test]
  fn test_mock_driver_check_version() {
    let driver = MockDriver::new();
    let mut version = NvRmApiVersion::default();
    version.cmd = NV_RM_API_VERSION_CMD_QUERY;

    driver.check_version(&mut version).unwrap();

    assert_eq!(version.reply, NV_RM_API_VERSION_REPLY_RECOGNIZED);
    // Version string should start with "570"
    assert!(version.version_string[0] == b'5');
    assert!(version.version_string[1] == b'7');
  }

  #[test]
  fn test_control_handler_gpu_name() {
    let mut driver = MockDriver::new();
    driver
      .alloc(0, 0, 0x1, classes::NV01_ROOT_CLIENT, None)
      .unwrap();

    // NV2080_CTRL_CMD_GPU_GET_NAME_STRING = 0x20800110
    let mut params = [0u8; 128];
    params[0..4].copy_from_slice(&0u32.to_le_bytes()); // ASCII mode

    let status = driver.control(0x1, 0x1, 0x20800110, &mut params).unwrap();
    assert_eq!(status, 0);

    // Name should be at offset 4
    let name = std::str::from_utf8(&params[4..]).unwrap();
    assert!(name.starts_with("NVIDIA RTX PRO 6000"));
  }

  #[test]
  fn test_control_handler_fb_info() {
    let mut driver = MockDriver::new();
    driver
      .alloc(0, 0, 0x1, classes::NV01_ROOT_CLIENT, None)
      .unwrap();

    // NV2080_CTRL_CMD_FB_GET_INFO = 0x20801301
    let mut params = [0u8; 64];
    let status = driver.control(0x1, 0x1, 0x20801301, &mut params).unwrap();
    assert_eq!(status, 0);

    // Total FB at offset 0
    let total = u64::from_le_bytes(params[0..8].try_into().unwrap());
    assert_eq!(total, 128 * 1024 * 1024 * 1024);

    // Free FB at offset 8 (90% of total)
    let free = u64::from_le_bytes(params[8..16].try_into().unwrap());
    assert_eq!(free, total * 9 / 10);
  }

  #[test]
  fn test_control_handler_pci_info() {
    let mut driver = MockDriver::new();
    driver
      .alloc(0, 0, 0x1, classes::NV01_ROOT_CLIENT, None)
      .unwrap();

    // NV2080_CTRL_CMD_BUS_GET_PCI_INFO = 0x20801801
    let mut params = [0u8; 32];
    let status = driver.control(0x1, 0x1, 0x20801801, &mut params).unwrap();
    assert_eq!(status, 0);

    // Vendor ID at offset 0
    let vendor = u16::from_le_bytes(params[0..2].try_into().unwrap());
    assert_eq!(vendor, 0x10DE);

    // Device ID at offset 2
    let device = u16::from_le_bytes(params[2..4].try_into().unwrap());
    assert_eq!(device, 0x2900);

    // Link width at offset 4
    let width = u32::from_le_bytes(params[4..8].try_into().unwrap());
    assert_eq!(width, 16); // x16
  }

  #[test]
  fn test_control_handler_unknown_cmd() {
    let mut driver = MockDriver::new();
    driver
      .alloc(0, 0, 0x1, classes::NV01_ROOT_CLIENT, None)
      .unwrap();

    // Unknown command should return success with no data modification
    let mut params = [0xAA; 32];
    let status = driver.control(0x1, 0x1, 0xDEADBEEF, &mut params).unwrap();
    assert_eq!(status, 0);

    // Params should be unchanged (unknown command doesn't modify)
    assert!(params.iter().all(|&b| b == 0xAA));
  }
}

#[cfg(test)]
mod proptests {
  use super::*;
  use proptest::prelude::*;

  proptest! {
      /// Property: Alloc followed by free always succeeds
      #[test]
      fn alloc_free_cycle(
          h_object in 1u32..10000,
          h_class in prop_oneof![
              Just(classes::NV01_ROOT_CLIENT),
              Just(classes::NV01_DEVICE_0),
              Just(classes::NV20_SUBDEVICE_0),
          ]
      ) {
          let mut driver = MockDriver::new();

          // Alloc should succeed
          let status = driver.alloc(0, 0, h_object, h_class, None).unwrap();
          prop_assert_eq!(status, 0);

          // Free should succeed
          let status = driver.free(0, 0, h_object).unwrap();
          prop_assert_eq!(status, 0);

          // Free again should fail
          let status = driver.free(0, 0, h_object).unwrap();
          prop_assert_ne!(status, 0);
      }

      /// Property: Memory allocation returns unique addresses
      #[test]
      fn memory_addresses_unique(
          sizes in proptest::collection::vec(4096u64..1048576, 1..20)
      ) {
          let mut driver = MockDriver::new();
          driver.alloc(0, 0, 1, classes::NV01_ROOT_CLIENT, None).unwrap();
          driver.alloc(1, 1, 2, classes::NV01_DEVICE_0, None).unwrap();

          let mut addresses = std::collections::HashSet::new();

          for (i, size) in sizes.iter().enumerate() {
              let h = 0x100 + i as u32;
              let (addr, status) = driver.alloc_memory(1, 2, h, classes::NV01_MEMORY_SYSTEM, 0, *size).unwrap();
              prop_assert_eq!(status, 0);
              prop_assert!(!addresses.contains(&addr), "Duplicate address");
              addresses.insert(addr);
          }
      }

      /// Property: Call log captures all operations
      #[test]
      fn call_log_complete(
          ops in proptest::collection::vec(
              prop_oneof![
                  Just("alloc"),
                  Just("free"),
                  Just("control"),
              ],
              1..50
          )
      ) {
          let mut driver = MockDriver::new();
          let mut expected_len = 0;

          // First alloc a root object
          driver.alloc(0, 0, 1, classes::NV01_ROOT_CLIENT, None).unwrap();
          expected_len += 1;

          for op in ops {
              match op {
                  "alloc" => {
                      let _ = driver.alloc(1, 1, 999, classes::NV01_DEVICE_0, None);
                  }
                  "free" => {
                      let _ = driver.free(1, 1, 999);
                  }
                  "control" => {
                      let mut params = [0u8; 8];
                      let _ = driver.control(1, 1, 0x123, &mut params);
                  }
                  _ => unreachable!(),
              }
              expected_len += 1;
          }

          prop_assert_eq!(driver.call_log().len(), expected_len);
      }
  }
}
