//! NVIDIA Resource Manager (RM) API definitions
//!
//! These are the ioctl commands and parameter structures used to communicate
//! with the NVIDIA kernel driver via /dev/nvidia0 and /dev/nvidiactl.
//!
//! Reference: nvidia-open/src/nvidia/arch/nvalloc/unix/include/nv_escape.h
//!            nvidia-open/src/common/sdk/nvidia/inc/nvos.h
//!
//! There are two categories of escapes:
//! 1. Top-level escapes (200+): System-level queries, version check, fd registration
//! 2. RM escapes (0x27-0x5F): Object management, control commands, memory ops

use zerocopy::{FromBytes, Immutable, IntoBytes, KnownLayout};

// =============================================================================
// TOP-LEVEL ESCAPE CODES (NV_ESC_*)
// =============================================================================

/// Base for top-level escape ioctls (not RM calls)
pub const NV_IOCTL_BASE: u32 = 200;

/// Get info about installed GPUs
pub const NV_ESC_CARD_INFO: u32 = NV_IOCTL_BASE + 0;        // 200

/// Register an fd with the driver
pub const NV_ESC_REGISTER_FD: u32 = NV_IOCTL_BASE + 1;      // 201

/// Allocate an OS event
pub const NV_ESC_ALLOC_OS_EVENT: u32 = NV_IOCTL_BASE + 6;   // 206

/// Free an OS event
pub const NV_ESC_FREE_OS_EVENT: u32 = NV_IOCTL_BASE + 7;    // 207

/// Get status code
pub const NV_ESC_STATUS_CODE: u32 = NV_IOCTL_BASE + 9;      // 209

/// Check driver version string
pub const NV_ESC_CHECK_VERSION_STR: u32 = NV_IOCTL_BASE + 10; // 210

/// Transfer command (wraps RM calls for large params)
pub const NV_ESC_IOCTL_XFER_CMD: u32 = NV_IOCTL_BASE + 11;  // 211

/// Attach GPUs to fd
pub const NV_ESC_ATTACH_GPUS_TO_FD: u32 = NV_IOCTL_BASE + 12; // 212

/// SYS_PARAMS query (system info)
pub const NV_ESC_SYS_PARAMS: u32 = NV_IOCTL_BASE + 14;      // 214

/// NUMA info
pub const NV_ESC_NUMA_INFO: u32 = NV_IOCTL_BASE + 15;       // 215

/// Set NUMA status
pub const NV_ESC_SET_NUMA_STATUS: u32 = NV_IOCTL_BASE + 16; // 216

/// Export object to dmabuf fd
pub const NV_ESC_EXPORT_TO_DMABUF_FD: u32 = NV_IOCTL_BASE + 17; // 217

// =============================================================================
// RM ESCAPE CODES (lower numbers, used with _IOWR)
// =============================================================================

/// Top-level escape codes (200+)
/// These are system-level queries that don't go through the RM object model.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NvTopLevelEscape {
    /// Get info about installed GPUs (nvidia-smi uses this first)
    CardInfo = 200,
    /// Register an fd with the driver
    RegisterFd = 201,
    /// Allocate an OS event
    AllocOsEvent = 206,
    /// Free an OS event
    FreeOsEvent = 207,
    /// Get status code
    StatusCode = 209,
    /// Check driver version string
    CheckVersionStr = 210,
    /// Transfer command (wraps RM calls for large params)
    IoctlXferCmd = 211,
    /// Attach GPUs to fd
    AttachGpusToFd = 212,
    /// SYS_PARAMS query
    SysParams = 214,
    /// NUMA info
    NumaInfo = 215,
    /// Set NUMA status
    SetNumaStatus = 216,
    /// Export to dmabuf
    ExportToDmabufFd = 217,
}

impl NvTopLevelEscape {
    pub fn from_u32(v: u32) -> Option<Self> {
        match v {
            200 => Some(Self::CardInfo),
            201 => Some(Self::RegisterFd),
            206 => Some(Self::AllocOsEvent),
            207 => Some(Self::FreeOsEvent),
            209 => Some(Self::StatusCode),
            210 => Some(Self::CheckVersionStr),
            211 => Some(Self::IoctlXferCmd),
            212 => Some(Self::AttachGpusToFd),
            214 => Some(Self::SysParams),
            215 => Some(Self::NumaInfo),
            216 => Some(Self::SetNumaStatus),
            217 => Some(Self::ExportToDmabufFd),
            _ => None,
        }
    }
    
    /// Check if this is a top-level escape code
    pub fn is_top_level(escape: u32) -> bool {
        escape >= 200 && escape <= 220
    }
}

/// NVIDIA RM escape codes (ioctl command numbers)
/// These are the ~20 ioctls that comprise the RM API.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NvEscape {
    AllocMemory = 0x27,
    AllocObject = 0x28,
    Free = 0x29,
    Control = 0x2A,
    Alloc = 0x2B,
    ConfigGet = 0x32,
    ConfigSet = 0x33,
    DupObject = 0x34,
    Share = 0x35,
    ConfigGetEx = 0x37,
    ConfigSetEx = 0x38,
    I2cAccess = 0x39,
    IdleChannels = 0x41,
    VidHeapControl = 0x4A,
    AccessRegistry = 0x4D,
    MapMemory = 0x4E,
    UnmapMemory = 0x4F,
    GetEventData = 0x52,
    AllocContextDma2 = 0x54,
    AddVblankCallback = 0x56,
    MapMemoryDma = 0x57,
    UnmapMemoryDma = 0x58,
    BindContextDma = 0x59,
    ExportObjectToFd = 0x5C,
    ImportObjectFromFd = 0x5D,
    UpdateDeviceMappingInfo = 0x5E,
    LocklessDiagnostic = 0x5F,
}

impl NvEscape {
    pub fn from_u32(v: u32) -> Option<Self> {
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
}

/// Handle type - opaque 32-bit identifier for GPU objects
pub type NvHandle = u32;

/// NVOS00_PARAMETERS - NV01_FREE
#[repr(C)]
#[derive(Debug, Clone, Copy, FromBytes, KnownLayout, Immutable)]
pub struct NvOs00Params {
    pub h_root: NvHandle,
    pub h_object_parent: NvHandle,
    pub h_object_old: NvHandle,
    pub status: u32,
}

/// NVOS02_PARAMETERS - NV01_ALLOC_MEMORY
/// Layout: 5x u32 (20 bytes) + 4 bytes padding + 2x u64 (16 bytes) + u32 (4 bytes) = 44 bytes
#[repr(C)]
#[derive(Debug, Clone, Copy, FromBytes, KnownLayout, Immutable)]
pub struct NvOs02Params {
    pub h_root: NvHandle,
    pub h_object_parent: NvHandle,
    pub h_object_new: NvHandle,
    pub h_class: u32,
    pub flags: u32,
    pub _pad0: u32, // padding for 8-byte alignment of p_memory
    pub p_memory: u64, // pointer, 8-byte aligned
    pub limit: u64,
    pub status: u32,
}

/// NVOS05_PARAMETERS - NV01_ALLOC_OBJECT
#[repr(C)]
#[derive(Debug, Clone, Copy, FromBytes, KnownLayout, Immutable)]
pub struct NvOs05Params {
    pub h_root: NvHandle,
    pub h_object_parent: NvHandle,
    pub h_object_new: NvHandle,
    pub h_class: u32,
    pub status: u32,
}

/// NVOS21_PARAMETERS - NV04_ALLOC
/// Layout: 4x u32 (16 bytes) + u64 (8 bytes) + u32 (4 bytes) = 28 bytes, padded to 32
#[repr(C)]
#[derive(Debug, Clone, Copy, FromBytes, KnownLayout, Immutable)]
pub struct NvOs21Params {
    pub h_root: NvHandle,
    pub h_object_parent: NvHandle,
    pub h_object_new: NvHandle,
    pub h_class: u32,
    pub p_alloc_parms: u64, // pointer to class-specific params
    pub status: u32,
    pub _pad: u32,
}

/// NVOS33_PARAMETERS - NV04_MAP_MEMORY
#[repr(C)]
#[derive(Debug, Clone, Copy, FromBytes, KnownLayout, Immutable)]
pub struct NvOs33Params {
    pub h_client: NvHandle,
    pub h_device: NvHandle,
    pub h_memory: NvHandle,
    pub _pad0: u32,
    pub offset: u64,
    pub length: u64,
    pub p_linear_address: u64, // output: mapped address
    pub status: u32,
    pub flags: u32,
}

/// NVOS34_PARAMETERS - NV04_UNMAP_MEMORY
#[repr(C)]
#[derive(Debug, Clone, Copy, FromBytes, KnownLayout, Immutable)]
pub struct NvOs34Params {
    pub h_client: NvHandle,
    pub h_device: NvHandle,
    pub h_memory: NvHandle,
    pub _pad0: u32,
    pub p_linear_address: u64,
    pub status: u32,
    pub flags: u32,
}

/// NVOS54_PARAMETERS - NV04_CONTROL (the big one)
#[repr(C)]
#[derive(Debug, Clone, Copy, FromBytes, KnownLayout, Immutable)]
pub struct NvOs54Params {
    pub h_client: NvHandle,
    pub h_object: NvHandle,
    pub cmd: u32,
    pub flags: u32,
    pub params: u64, // pointer to command-specific params
    pub params_size: u32,
    pub status: u32,
}

/// NVOS55_PARAMETERS - NV04_DUP_OBJECT
#[repr(C)]
#[derive(Debug, Clone, Copy, FromBytes, KnownLayout, Immutable)]
pub struct NvOs55Params {
    pub h_client: NvHandle,
    pub h_parent: NvHandle,
    pub h_object: NvHandle,
    pub h_client_src: NvHandle,
    pub h_object_src: NvHandle,
    pub flags: u32,
    pub status: u32,
    pub _pad: u32,
}

/// NVOS57_PARAMETERS - NV04_SHARE
#[repr(C)]
#[derive(Debug, Clone, Copy, FromBytes, KnownLayout, Immutable)]
pub struct NvOs57Params {
    pub h_client: NvHandle,
    pub h_object: NvHandle,
    pub share_policy: u64, // RS_SHARE_POLICY
    pub status: u32,
    pub _pad: u32,
}

// =============================================================================
// TOP-LEVEL ESCAPE IOCTL STRUCTURES
// =============================================================================

/// PCI information structure
#[repr(C)]
#[derive(Debug, Clone, Copy, Default, FromBytes, KnownLayout, Immutable)]
pub struct NvPciInfo {
    pub domain: u32,       // PCI domain number
    pub bus: u8,           // PCI bus number
    pub slot: u8,          // PCI slot number
    pub function: u8,      // PCI function number
    pub _pad0: u8,         // padding
    pub vendor_id: u16,    // PCI vendor ID (0x10DE for NVIDIA)
    pub device_id: u16,    // PCI device ID
}

/// NV_ESC_CARD_INFO response structure
/// Used by nvidia-smi to enumerate GPUs
#[repr(C)]
#[derive(Debug, Clone, Copy, Default, FromBytes, KnownLayout, Immutable)]
pub struct NvCardInfo {
    pub valid: u32,             // NV_TRUE if this entry is valid
    pub pci_info: NvPciInfo,    // PCI configuration info
    pub gpu_id: u32,            // GPU ID (internal identifier)
    pub interrupt_line: u16,    // IRQ line
    pub _pad0: [u8; 2],         // padding for alignment
    pub reg_address: u64,       // BAR0 physical address
    pub reg_size: u64,          // BAR0 size
    pub fb_address: u64,        // Framebuffer physical address  
    pub fb_size: u64,           // Framebuffer size
    pub minor_number: u32,      // Device minor number (/dev/nvidia0 = 0)
    pub dev_name: [u8; 10],     // Device name
    pub _pad1: [u8; 2],         // padding
}

/// NV_ESC_CHECK_VERSION_STR request/response
/// Used to verify driver version compatibility
pub const NV_RM_API_VERSION_STRING_LENGTH: usize = 64;

#[repr(C)]
#[derive(Debug, Clone, Copy, FromBytes, KnownLayout, Immutable)]
pub struct NvRmApiVersion {
    pub cmd: u32,      // NV_RM_API_VERSION_CMD_*
    pub reply: u32,    // NV_RM_API_VERSION_REPLY_*
    pub version_string: [u8; NV_RM_API_VERSION_STRING_LENGTH],
}

impl Default for NvRmApiVersion {
    fn default() -> Self {
        Self {
            cmd: 0,
            reply: 0,
            version_string: [0u8; NV_RM_API_VERSION_STRING_LENGTH],
        }
    }
}

/// Version check command values
pub const NV_RM_API_VERSION_CMD_STRICT: u32 = 0;
pub const NV_RM_API_VERSION_CMD_RELAXED: u32 = b'1' as u32;
pub const NV_RM_API_VERSION_CMD_QUERY: u32 = b'2' as u32;

/// Version check reply values
pub const NV_RM_API_VERSION_REPLY_UNRECOGNIZED: u32 = 0;
pub const NV_RM_API_VERSION_REPLY_RECOGNIZED: u32 = 1;

/// NV_ESC_REGISTER_FD parameters
/// Used to register an fd with the driver for later use in RM calls
#[repr(C)]
#[derive(Debug, Clone, Copy, Default, FromBytes, KnownLayout, Immutable)]
pub struct NvRegisterFdParams {
    pub ctl_fd: i32,    // The control fd to register
    pub _pad: i32,      // Padding for alignment
}

/// NV_ESC_ALLOC_OS_EVENT parameters
#[repr(C)]
#[derive(Debug, Clone, Copy, Default, FromBytes, KnownLayout, Immutable)]
pub struct NvAllocOsEventParams {
    pub h_client: NvHandle,     // Client handle
    pub h_device: NvHandle,     // Device handle
    pub fd: u32,                // Event fd
    pub status: u32,            // Output: status
}

/// NV_ESC_FREE_OS_EVENT parameters
#[repr(C)]
#[derive(Debug, Clone, Copy, Default, FromBytes, KnownLayout, Immutable)]
pub struct NvFreeOsEventParams {
    pub h_client: NvHandle,     // Client handle
    pub h_device: NvHandle,     // Device handle
    pub fd: u32,                // Event fd to free
    pub status: u32,            // Output: status
}

/// NV_ESC_STATUS_CODE parameters
#[repr(C)]
#[derive(Debug, Clone, Copy, Default, FromBytes, KnownLayout, Immutable)]
pub struct NvStatusCodeParams {
    pub domain: u32,            // PCI domain
    pub bus: u8,                // PCI bus
    pub slot: u8,               // PCI slot
    pub function: u8,           // PCI function
    pub _pad: u8,               // Padding
    pub status: u32,            // Output: status code
}

/// NV_ESC_ATTACH_GPUS_TO_FD parameters
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct NvAttachGpusToFdParams {
    pub gpu_ids: [u32; 32],     // Array of GPU IDs to attach
    pub fd: i32,                // The fd to attach to
    pub num_gpus: u32,          // Number of GPUs in the array
}

/// NV_ESC_SYS_PARAMS parameters
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct NvSysParams {
    pub memblock_size: u64,     // Memory block size
    pub _reserved: [u64; 7],    // Reserved for future use
}

/// Maximum number of card info entries
pub const NV_MAX_CARD_INFO: usize = 32;

// =============================================================================
// STATUS CODES
// =============================================================================

/// Status codes returned by RM
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NvStatus {
    Ok = 0x00000000,
    ErrGeneric = 0x0000FFFF,
    ErrInvalidArgument = 0x00000003,
    ErrInvalidObject = 0x00000005,
    ErrInvalidClient = 0x00000006,
    ErrInvalidState = 0x00000007,
    ErrInsufficientPermissions = 0x00000008,
    ErrNoMemory = 0x0000000C,
    ErrNotSupported = 0x00000056,
}

impl NvStatus {
    pub fn is_ok(self) -> bool {
        matches!(self, NvStatus::Ok)
    }
}

/// Known object classes
pub mod classes {
    /// Root client - first object created, represents a client connection
    pub const NV01_ROOT: u32 = 0x00000000;
    pub const NV01_ROOT_CLIENT: u32 = 0x00000041;
    pub const NV01_ROOT_NON_PRIV: u32 = 0x00000001;

    /// Device - represents a GPU
    pub const NV01_DEVICE_0: u32 = 0x00000080;

    /// Subdevice - represents a subdevice (for multi-GPU)
    pub const NV20_SUBDEVICE_0: u32 = 0x00002080;

    /// Memory classes
    pub const NV01_MEMORY_SYSTEM: u32 = 0x0000003E;
    pub const NV01_MEMORY_SYSTEM_OS_DESCRIPTOR: u32 = 0x00000071;

    /// Channel classes (for command submission)
    pub const KEPLER_CHANNEL_GPFIFO_A: u32 = 0x0000A06F;
    pub const VOLTA_CHANNEL_GPFIFO_A: u32 = 0x0000C36F;
    pub const TURING_CHANNEL_GPFIFO_A: u32 = 0x0000C46F;
    pub const AMPERE_CHANNEL_GPFIFO_A: u32 = 0x0000C56F;
    pub const HOPPER_CHANNEL_GPFIFO_A: u32 = 0x0000C86F;

    /// Compute classes
    pub const KEPLER_COMPUTE_A: u32 = 0x0000A0C0;
    pub const VOLTA_COMPUTE_A: u32 = 0x0000C3C0;
    pub const TURING_COMPUTE_A: u32 = 0x0000C5C0;
    pub const AMPERE_COMPUTE_A: u32 = 0x0000C6C0;
    pub const HOPPER_COMPUTE_A: u32 = 0x0000C9C0;
}

/// ioctl number calculation for NVIDIA driver
/// The NVIDIA driver uses a custom ioctl scheme
pub fn nv_ioctl_cmd(escape: NvEscape) -> libc::c_ulong {
    // NVIDIA uses: _IOWR('F', escape_code, data)
    // But actually they just use the escape code directly in a custom scheme
    // The actual ioctl is on /dev/nvidiactl with command = escape code
    escape as libc::c_ulong
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem;

    #[test]
    fn test_struct_sizes() {
        // Verify our struct layouts match the kernel driver
        // These sizes come from nvidia-open/src/common/sdk/nvidia/inc/nvos.h
        assert_eq!(mem::size_of::<NvOs00Params>(), 16);
        assert_eq!(mem::size_of::<NvOs05Params>(), 20);
        assert_eq!(mem::size_of::<NvOs54Params>(), 32);
        assert_eq!(mem::size_of::<NvOs33Params>(), 48);
        assert_eq!(mem::size_of::<NvOs34Params>(), 32);
    }

    #[test]
    fn test_struct_alignment() {
        // Verify alignment requirements
        assert_eq!(mem::align_of::<NvOs00Params>(), 4);
        assert_eq!(mem::align_of::<NvOs21Params>(), 8); // Has u64 field
        assert_eq!(mem::align_of::<NvOs54Params>(), 8); // Has u64 field
        assert_eq!(mem::align_of::<NvOs33Params>(), 8); // Has u64 field
    }

    #[test]
    fn test_escape_roundtrip() {
        for escape in [
            NvEscape::Alloc,
            NvEscape::Free,
            NvEscape::Control,
            NvEscape::MapMemory,
        ] {
            let v = escape as u32;
            assert_eq!(NvEscape::from_u32(v), Some(escape));
        }
    }

    #[test]
    fn test_escape_all_values() {
        // Test all valid escape codes
        let all_escapes = [
            (0x27, NvEscape::AllocMemory),
            (0x28, NvEscape::AllocObject),
            (0x29, NvEscape::Free),
            (0x2A, NvEscape::Control),
            (0x2B, NvEscape::Alloc),
            (0x32, NvEscape::ConfigGet),
            (0x33, NvEscape::ConfigSet),
            (0x34, NvEscape::DupObject),
            (0x35, NvEscape::Share),
            (0x37, NvEscape::ConfigGetEx),
            (0x38, NvEscape::ConfigSetEx),
            (0x39, NvEscape::I2cAccess),
            (0x41, NvEscape::IdleChannels),
            (0x4A, NvEscape::VidHeapControl),
            (0x4D, NvEscape::AccessRegistry),
            (0x4E, NvEscape::MapMemory),
            (0x4F, NvEscape::UnmapMemory),
            (0x52, NvEscape::GetEventData),
            (0x54, NvEscape::AllocContextDma2),
            (0x56, NvEscape::AddVblankCallback),
            (0x57, NvEscape::MapMemoryDma),
            (0x58, NvEscape::UnmapMemoryDma),
            (0x59, NvEscape::BindContextDma),
            (0x5C, NvEscape::ExportObjectToFd),
            (0x5D, NvEscape::ImportObjectFromFd),
            (0x5E, NvEscape::UpdateDeviceMappingInfo),
            (0x5F, NvEscape::LocklessDiagnostic),
        ];

        for (code, expected) in all_escapes {
            assert_eq!(NvEscape::from_u32(code), Some(expected));
        }
    }

    #[test]
    fn test_escape_invalid_values() {
        // Test some invalid escape codes
        assert_eq!(NvEscape::from_u32(0x00), None);
        assert_eq!(NvEscape::from_u32(0x01), None);
        assert_eq!(NvEscape::from_u32(0x26), None); // Just before AllocMemory
        assert_eq!(NvEscape::from_u32(0x2C), None); // Just after Alloc
        assert_eq!(NvEscape::from_u32(0x60), None); // Just after last valid
        assert_eq!(NvEscape::from_u32(0xFF), None);
        assert_eq!(NvEscape::from_u32(u32::MAX), None);
    }
}

#[cfg(test)]
mod proptests {
    use super::*;
    use proptest::prelude::*;
    use zerocopy::FromBytes;

    proptest! {
        /// Property: NvOs00Params can be read from any 16-byte aligned buffer
        #[test]
        fn nvos00_from_bytes(
            h_root in any::<u32>(),
            h_object_parent in any::<u32>(),
            h_object_old in any::<u32>(),
            status in any::<u32>()
        ) {
            let bytes: [u8; 16] = unsafe {
                std::mem::transmute(NvOs00Params {
                    h_root,
                    h_object_parent,
                    h_object_old,
                    status,
                })
            };

            let parsed = NvOs00Params::ref_from_bytes(&bytes).unwrap();
            prop_assert_eq!(parsed.h_root, h_root);
            prop_assert_eq!(parsed.h_object_parent, h_object_parent);
            prop_assert_eq!(parsed.h_object_old, h_object_old);
            prop_assert_eq!(parsed.status, status);
        }

        /// Property: NvOs54Params (Control) can be read from any valid buffer
        #[test]
        fn nvos54_from_bytes(
            h_client in any::<u32>(),
            h_object in any::<u32>(),
            cmd in any::<u32>(),
            flags in any::<u32>(),
            params in any::<u64>(),
            params_size in any::<u32>(),
            status in any::<u32>()
        ) {
            let bytes: [u8; 32] = unsafe {
                std::mem::transmute(NvOs54Params {
                    h_client,
                    h_object,
                    cmd,
                    flags,
                    params,
                    params_size,
                    status,
                })
            };

            let parsed = NvOs54Params::ref_from_bytes(&bytes).unwrap();
            prop_assert_eq!(parsed.h_client, h_client);
            prop_assert_eq!(parsed.h_object, h_object);
            prop_assert_eq!(parsed.cmd, cmd);
            prop_assert_eq!(parsed.flags, flags);
            prop_assert_eq!(parsed.params, params);
            prop_assert_eq!(parsed.params_size, params_size);
            prop_assert_eq!(parsed.status, status);
        }

        /// Property: Short buffers fail to parse
        #[test]
        fn short_buffer_fails(len in 0usize..16) {
            let bytes = vec![0u8; len];
            prop_assert!(NvOs00Params::ref_from_bytes(&bytes).is_err());
        }

        /// Property: Escape codes are in valid ranges
        #[test]
        fn escape_in_range(code in any::<u32>()) {
            if let Some(escape) = NvEscape::from_u32(code) {
                let back = escape as u32;
                prop_assert_eq!(back, code);
            }
        }

        /// Adversarial: Random bytes can be parsed if length is correct
        #[test]
        fn random_bytes_nvos00(bytes in proptest::collection::vec(any::<u8>(), 16..=16)) {
            // Should always succeed for 16 bytes
            let result = NvOs00Params::ref_from_bytes(&bytes);
            prop_assert!(result.is_ok());
        }

        /// Adversarial: Random bytes with exact size for NvOs54
        #[test]
        fn random_bytes_nvos54(bytes in proptest::collection::vec(any::<u8>(), 32..=32)) {
            let result = NvOs54Params::ref_from_bytes(&bytes);
            prop_assert!(result.is_ok());
        }

        /// Adversarial: All handle values are valid
        #[test]
        fn all_handle_values_valid(h in any::<u32>()) {
            // Handles should be opaque - any u32 is valid
            let _: NvHandle = h;
        }
    }
}
