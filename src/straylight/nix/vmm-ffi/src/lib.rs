// straylight::nix::vmm-ffi
//
// C FFI wrapper for Firecracker VMM library.
// Enables embedding the microVM hypervisor directly into the nix binary.
//
// This crate exposes a minimal C-compatible API for:
//   - Creating and configuring a microVM
//   - Adding block devices and vsock
//   - Starting/stopping the VM
//   - Communicating via vsock
//
// The goal is to eliminate fork/exec overhead and enable tighter integration
// with the build service, including direct memory sharing via shared mappings.

use std::ffi::{CStr, c_char, c_int, c_uint, c_void};
use std::io::Write;
use std::os::unix::io::{FromRawFd, RawFd};
use std::path::PathBuf;
use std::sync::{Arc, Mutex};

// VMM imports
use vmm::builder;
use vmm::resources::VmResources;
use vmm::seccomp::{BpfThreadMap, get_empty_filters};
use vmm::vmm_config::boot_source::BootSourceConfig;
use vmm::vmm_config::drive::BlockDeviceConfig;
use vmm::vmm_config::instance_info::InstanceInfo;
use vmm::vmm_config::machine_config::MachineConfig;
use vmm::vmm_config::vsock::VsockDeviceConfig;
use vmm::{EventManager, Vmm};

// Re-export error codes
pub const VMM_OK: c_int = 0;
pub const VMM_ERR_INVALID_ARG: c_int = -1;
pub const VMM_ERR_KVM_INIT: c_int = -2;
pub const VMM_ERR_VM_CREATE: c_int = -3;
pub const VMM_ERR_BOOT: c_int = -4;
pub const VMM_ERR_DEVICE: c_int = -5;
pub const VMM_ERR_VSOCK: c_int = -6;
pub const VMM_ERR_NOT_IMPLEMENTED: c_int = -98;
pub const VMM_ERR_INTERNAL: c_int = -99;

/// Opaque handle to a VM instance
pub struct VmmHandle {
  /// The VMM instance
  vmm: Option<Arc<Mutex<Vmm>>>,
  /// Event manager for the VMM
  event_manager: Option<EventManager>,
  /// VM resources (configuration)
  vm_resources: VmResources,
  /// Instance info
  instance_info: InstanceInfo,
  /// Event loop thread handle (if started asynchronously)
  event_loop_thread: Option<std::thread::JoinHandle<c_int>>,
}

/// VM configuration passed from C++
#[repr(C)]
pub struct VmmConfig {
  /// Path to kernel image (vmlinux)
  pub kernel_path: *const c_char,
  /// Path to initrd image
  pub initrd_path: *const c_char,
  /// Kernel command line
  pub kernel_cmdline: *const c_char,
  /// Number of vCPUs
  pub vcpu_count: c_uint,
  /// Memory size in MiB
  pub mem_size_mib: c_uint,
}

/// Block device configuration
#[repr(C)]
pub struct VmmBlockDevice {
  /// Device ID (e.g., "store", "output")
  pub drive_id: *const c_char,
  /// Path to block device file
  pub path: *const c_char,
  /// Read-only flag
  pub is_read_only: c_int,
}

/// vsock configuration
#[repr(C)]
pub struct VmmVsockConfig {
  /// Guest CID
  pub guest_cid: c_uint,
  /// Path to Unix domain socket
  pub uds_path: *const c_char,
}

/// VM configuration with embedded kernel/initrd data
/// Used for creating VMs from data embedded in the binary
#[repr(C)]
pub struct VmmEmbeddedConfig {
  /// Pointer to kernel image data (vmlinux, possibly compressed)
  pub kernel_data: *const c_void,
  /// Size of kernel data in bytes
  pub kernel_size: usize,
  /// Pointer to initrd image data (CPIO, possibly compressed)
  pub initrd_data: *const c_void,
  /// Size of initrd data in bytes  
  pub initrd_size: usize,
  /// Kernel command line (may be NULL)
  pub kernel_cmdline: *const c_char,
  /// Number of vCPUs
  pub vcpu_count: c_uint,
  /// Memory size in MiB
  pub mem_size_mib: c_uint,
  /// Compression format for kernel: 0=none, 1=gzip, 2=zstd
  pub kernel_compression: c_uint,
  /// Compression format for initrd: 0=none, 1=gzip, 2=zstd
  pub initrd_compression: c_uint,
}

// ============================================================================
// Helper Functions
// ============================================================================

/// Create a memfd with the given name and write data to it.
/// Returns the file path as /proc/self/fd/N that can be used with path-based APIs.
fn create_memfd_with_data(name: &str, data: &[u8]) -> Result<PathBuf, std::io::Error> {
  use std::ffi::CString;

  let name_cstr = CString::new(name)
    .map_err(|_| std::io::Error::new(std::io::ErrorKind::InvalidInput, "invalid memfd name"))?;

  // Create memfd
  let fd = unsafe { libc::memfd_create(name_cstr.as_ptr(), libc::MFD_CLOEXEC) };
  if fd < 0 {
    return Err(std::io::Error::last_os_error());
  }

  // Write data to memfd
  let mut file = unsafe { std::fs::File::from_raw_fd(fd) };
  file.write_all(data)?;

  // Seek back to start
  use std::io::Seek;
  file.seek(std::io::SeekFrom::Start(0))?;

  // Don't close the fd - we need it to stay open
  // Convert File back to raw fd and forget it
  let fd = std::os::unix::io::IntoRawFd::into_raw_fd(file);

  // Return path to /proc/self/fd/N
  Ok(PathBuf::from(format!("/proc/self/fd/{}", fd)))
}

/// Decompress data if compressed
/// Compression formats: 0=none, 2=zstd
/// Note: gzip (1) is not currently supported - use zstd for better compression
fn decompress_data(data: &[u8], compression: c_uint) -> Result<Vec<u8>, std::io::Error> {
  match compression {
    0 => Ok(data.to_vec()), // No compression
    2 => {
      // zstd - use the zstd crate
      zstd::stream::decode_all(data).map_err(|e| std::io::Error::new(std::io::ErrorKind::Other, e))
    }
    _ => Err(std::io::Error::new(
      std::io::ErrorKind::InvalidInput,
      format!(
        "unsupported compression format: {} (use 0=none or 2=zstd)",
        compression
      ),
    )),
  }
}

// ============================================================================
// C FFI Functions
// ============================================================================

/// Create a new VM instance with the given configuration.
///
/// Returns a handle on success, NULL on failure.
/// The error code is written to `error_out` if non-NULL.
///
/// NOTE: This is currently a stub. Full VMM integration pending.
#[unsafe(no_mangle)]
pub extern "C" fn vmm_create(config: *const VmmConfig, error_out: *mut c_int) -> *mut VmmHandle {
  let set_error = |code: c_int| {
    if !error_out.is_null() {
      unsafe {
        *error_out = code;
      }
    }
  };

  if config.is_null() {
    set_error(VMM_ERR_INVALID_ARG);
    return std::ptr::null_mut();
  }

  let config = unsafe { &*config };

  // Validate required fields
  if config.kernel_path.is_null() {
    set_error(VMM_ERR_INVALID_ARG);
    return std::ptr::null_mut();
  }

  // Convert C strings to Rust
  let kernel_path = unsafe { CStr::from_ptr(config.kernel_path) }
    .to_str()
    .ok()
    .map(PathBuf::from);

  let kernel_path = match kernel_path {
    Some(p) => p,
    None => {
      set_error(VMM_ERR_INVALID_ARG);
      return std::ptr::null_mut();
    }
  };

  let initrd_path = if config.initrd_path.is_null() {
    None
  } else {
    unsafe { CStr::from_ptr(config.initrd_path) }
      .to_str()
      .ok()
      .map(PathBuf::from)
  };

  let kernel_cmdline = if config.kernel_cmdline.is_null() {
    String::new()
  } else {
    unsafe { CStr::from_ptr(config.kernel_cmdline) }
      .to_str()
      .unwrap_or("")
      .to_string()
  };

  // Create VmResources with configuration
  let mut vm_resources = VmResources::default();

  // Configure boot source
  let boot_source_config = BootSourceConfig {
    kernel_image_path: kernel_path.to_string_lossy().to_string(),
    initrd_path: initrd_path.map(|p| p.to_string_lossy().to_string()),
    boot_args: Some(kernel_cmdline),
  };

  if let Err(_) = vm_resources.build_boot_source(boot_source_config) {
    set_error(VMM_ERR_BOOT);
    return std::ptr::null_mut();
  }

  // Configure machine config
  let machine_config = MachineConfig {
    vcpu_count: config.vcpu_count as u8,
    mem_size_mib: config.mem_size_mib as usize,
    ..MachineConfig::default()
  };

  if let Err(_) = vm_resources.update_machine_config(&machine_config.into()) {
    set_error(VMM_ERR_VM_CREATE);
    return std::ptr::null_mut();
  }

  // Create instance info
  let instance_info = InstanceInfo::default();

  let handle = Box::new(VmmHandle {
    vmm: None,
    event_manager: None,
    vm_resources,
    instance_info,
    event_loop_thread: None,
  });
  set_error(VMM_OK);
  Box::into_raw(handle)
}

/// Create a new VM instance from embedded kernel/initrd data.
///
/// This function creates memfds from the provided data and configures the VM.
/// The data can be compressed (gzip or zstd) and will be decompressed automatically.
///
/// Returns a handle on success, NULL on failure.
/// The error code is written to `error_out` if non-NULL.
#[unsafe(no_mangle)]
pub extern "C" fn vmm_create_from_embedded(
  config: *const VmmEmbeddedConfig,
  error_out: *mut c_int,
) -> *mut VmmHandle {
  let set_error = |code: c_int| {
    if !error_out.is_null() {
      unsafe {
        *error_out = code;
      }
    }
  };

  if config.is_null() {
    set_error(VMM_ERR_INVALID_ARG);
    return std::ptr::null_mut();
  }

  let config = unsafe { &*config };

  // Validate required fields
  if config.kernel_data.is_null() || config.kernel_size == 0 {
    set_error(VMM_ERR_INVALID_ARG);
    return std::ptr::null_mut();
  }

  // Get kernel data slice
  let kernel_data =
    unsafe { std::slice::from_raw_parts(config.kernel_data as *const u8, config.kernel_size) };

  eprintln!(
    "vmm-ffi: kernel data size: {} bytes, compression: {}",
    config.kernel_size, config.kernel_compression
  );

  // Decompress kernel if needed
  let kernel_bytes = match decompress_data(kernel_data, config.kernel_compression) {
    Ok(data) => {
      eprintln!("vmm-ffi: decompressed kernel size: {} bytes", data.len());
      data
    }
    Err(e) => {
      eprintln!("vmm-ffi: kernel decompression failed: {:?}", e);
      set_error(VMM_ERR_BOOT);
      return std::ptr::null_mut();
    }
  };

  // Create memfd for kernel
  let kernel_path = match create_memfd_with_data("vmlinux", &kernel_bytes) {
    Ok(path) => {
      eprintln!("vmm-ffi: kernel memfd path: {}", path.display());
      path
    }
    Err(e) => {
      eprintln!("vmm-ffi: kernel memfd creation failed: {:?}", e);
      set_error(VMM_ERR_BOOT);
      return std::ptr::null_mut();
    }
  };

  // Handle initrd (optional)
  let initrd_path = if !config.initrd_data.is_null() && config.initrd_size > 0 {
    let initrd_data =
      unsafe { std::slice::from_raw_parts(config.initrd_data as *const u8, config.initrd_size) };

    // Decompress initrd if needed (note: initrd itself may be gzip compressed for kernel)
    let initrd_bytes = match decompress_data(initrd_data, config.initrd_compression) {
      Ok(data) => data,
      Err(_) => {
        set_error(VMM_ERR_BOOT);
        return std::ptr::null_mut();
      }
    };

    match create_memfd_with_data("initrd", &initrd_bytes) {
      Ok(path) => Some(path),
      Err(_) => {
        set_error(VMM_ERR_BOOT);
        return std::ptr::null_mut();
      }
    }
  } else {
    None
  };

  // Get kernel cmdline
  let kernel_cmdline = if config.kernel_cmdline.is_null() {
    String::new()
  } else {
    unsafe { CStr::from_ptr(config.kernel_cmdline) }
      .to_str()
      .unwrap_or("")
      .to_string()
  };

  // Create VmResources with configuration
  let mut vm_resources = VmResources::default();

  // Configure boot source
  let boot_source_config = BootSourceConfig {
    kernel_image_path: kernel_path.to_string_lossy().to_string(),
    initrd_path: initrd_path.map(|p| p.to_string_lossy().to_string()),
    boot_args: Some(kernel_cmdline),
  };

  if let Err(_) = vm_resources.build_boot_source(boot_source_config) {
    set_error(VMM_ERR_BOOT);
    return std::ptr::null_mut();
  }

  // Configure machine config
  let machine_config = MachineConfig {
    vcpu_count: config.vcpu_count as u8,
    mem_size_mib: config.mem_size_mib as usize,
    ..MachineConfig::default()
  };

  if let Err(_) = vm_resources.update_machine_config(&machine_config.into()) {
    set_error(VMM_ERR_VM_CREATE);
    return std::ptr::null_mut();
  }

  // Create instance info
  let instance_info = InstanceInfo::default();

  let handle = Box::new(VmmHandle {
    vmm: None,
    event_manager: None,
    vm_resources,
    instance_info,
    event_loop_thread: None,
  });
  set_error(VMM_OK);
  Box::into_raw(handle)
}

/// Add a block device to the VM.
///
/// Must be called before vmm_start().
#[unsafe(no_mangle)]
pub extern "C" fn vmm_add_block_device(
  handle: *mut VmmHandle,
  device: *const VmmBlockDevice,
) -> c_int {
  if handle.is_null() || device.is_null() {
    return VMM_ERR_INVALID_ARG;
  }

  let handle = unsafe { &mut *handle };
  let device = unsafe { &*device };

  if device.drive_id.is_null() || device.path.is_null() {
    return VMM_ERR_INVALID_ARG;
  }

  // Convert C strings
  let drive_id = match unsafe { CStr::from_ptr(device.drive_id) }.to_str() {
    Ok(s) => s.to_string(),
    Err(_) => return VMM_ERR_INVALID_ARG,
  };

  let path = match unsafe { CStr::from_ptr(device.path) }.to_str() {
    Ok(s) => s.to_string(),
    Err(_) => return VMM_ERR_INVALID_ARG,
  };

  eprintln!(
    "vmm-ffi: adding block device '{}' at '{}', read_only={}",
    drive_id,
    path,
    device.is_read_only != 0
  );

  let block_config = BlockDeviceConfig {
    drive_id: drive_id.clone(),
    path_on_host: Some(path.clone()),
    is_root_device: false,
    partuuid: None,
    is_read_only: Some(device.is_read_only != 0),
    cache_type: vmm::devices::virtio::block::CacheType::Unsafe,
    rate_limiter: None,
    file_engine_type: Some(vmm::vmm_config::drive::FileEngineType::default()),
    socket: None,
  };

  if let Err(e) = handle.vm_resources.set_block_device(block_config) {
    eprintln!(
      "vmm-ffi: set_block_device failed for '{}': {:?}",
      drive_id, e
    );
    return VMM_ERR_DEVICE;
  }

  eprintln!(
    "vmm-ffi: block device '{}' added successfully, total devices: {}",
    drive_id,
    handle.vm_resources.block.devices.len()
  );

  VMM_OK
}

/// Configure vsock for the VM.
///
/// Must be called before vmm_start().
#[unsafe(no_mangle)]
pub extern "C" fn vmm_configure_vsock(
  handle: *mut VmmHandle,
  config: *const VmmVsockConfig,
) -> c_int {
  if handle.is_null() || config.is_null() {
    return VMM_ERR_INVALID_ARG;
  }

  let handle = unsafe { &mut *handle };
  let config = unsafe { &*config };

  if config.uds_path.is_null() {
    return VMM_ERR_INVALID_ARG;
  }

  let uds_path = match unsafe { CStr::from_ptr(config.uds_path) }.to_str() {
    Ok(s) => s.to_string(),
    Err(_) => return VMM_ERR_INVALID_ARG,
  };

  eprintln!(
    "vmm-ffi: configuring vsock at '{}', guest_cid={}",
    uds_path, config.guest_cid
  );

  let vsock_config = VsockDeviceConfig {
    vsock_id: Some("vsock0".to_string()),
    guest_cid: config.guest_cid,
    uds_path: uds_path.clone(),
  };

  if let Err(e) = handle.vm_resources.set_vsock_device(vsock_config) {
    eprintln!("vmm-ffi: set_vsock_device failed: {:?}", e);
    return VMM_ERR_VSOCK;
  }

  eprintln!("vmm-ffi: vsock configured successfully");

  VMM_OK
}

/// Start the VM.
///
/// This boots the VM and starts vCPU threads.
/// The VM runs asynchronously; use vmm_wait() to block until completion.
#[unsafe(no_mangle)]
pub extern "C" fn vmm_start(handle: *mut VmmHandle) -> c_int {
  if handle.is_null() {
    return VMM_ERR_INVALID_ARG;
  }

  let handle = unsafe { &mut *handle };

  // Create event manager
  let mut event_manager = EventManager::new().expect("Failed to create event manager");

  // Build empty seccomp filters (permissive - disable seccomp for FFI use)
  // Note: Must include "vmm" and "vcpu" keys even if the programs are empty
  let seccomp_filters = get_empty_filters();

  // Debug: check what devices are configured
  let block_count = handle.vm_resources.block.devices.len();
  eprintln!("vmm-ffi: block devices count: {}", block_count);
  // Print configs which have the info we need
  for (i, cfg) in handle.vm_resources.block.configs().iter().enumerate() {
    eprintln!(
      "vmm-ffi: block[{}]: id='{}', path={:?}, root={}, read_only={:?}",
      i, cfg.drive_id, cfg.path_on_host, cfg.is_root_device, cfg.is_read_only
    );
  }
  let vsock_configured = handle.vm_resources.vsock.get().is_some();
  eprintln!("vmm-ffi: vsock configured: {}", vsock_configured);

  // Total expected devices: block_count + (1 if vsock)
  let expected_devices = block_count + if vsock_configured { 1 } else { 0 };
  eprintln!(
    "vmm-ffi: expecting {} virtio devices total",
    expected_devices
  );
  // Verify block devices and Arc strong counts before build
  for (i, dev) in handle.vm_resources.block.devices.iter().enumerate() {
    let cfg = dev.lock().unwrap().config();
    eprintln!(
      "vmm-ffi: pre-build block[{}]: id='{}', Arc strong_count={}",
      i,
      cfg.drive_id,
      Arc::strong_count(dev)
    );
  }

  // Print the original boot cmdline before build
  if let Some(boot_cfg) = &handle.vm_resources.boot_source.builder {
    if let Ok(cmdline_cstr) = boot_cfg.cmdline.as_cstring() {
      eprintln!(
        "vmm-ffi: original boot cmdline: {:?}",
        cmdline_cstr.to_string_lossy()
      );
    }
  }

  // Build the microVM
  eprintln!("vmm-ffi: calling build_microvm_for_boot...");
  let vmm = match builder::build_microvm_for_boot(
    &handle.instance_info,
    &handle.vm_resources,
    &mut event_manager,
    &seccomp_filters,
  ) {
    Ok(vmm) => {
      eprintln!("vmm-ffi: build_microvm_for_boot succeeded");
      vmm
    }
    Err(e) => {
      eprintln!("vmm-ffi: build_microvm_for_boot failed: {:?}", e);
      return VMM_ERR_BOOT;
    }
  };

  // Check Arc strong counts after build - should be 2 (one in vm_resources, one in MmioTransport)
  for (i, dev) in handle.vm_resources.block.devices.iter().enumerate() {
    let cfg = dev.lock().unwrap().config();
    eprintln!(
      "vmm-ffi: post-build block[{}]: id='{}', Arc strong_count={}",
      i,
      cfg.drive_id,
      Arc::strong_count(dev)
    );
  }

  // Test MMIO bus reads at expected addresses
  let vmm_guard = vmm.lock().unwrap();
  let mmio_bus = &vmm_guard.vm.common.mmio_bus;
  for (i, addr) in [0xc0001000u64, 0xc0002000u64, 0xc0003000u64]
    .iter()
    .enumerate()
  {
    // Read magic (offset 0x00)
    let mut magic_data = [0u8; 4];
    let magic_result = mmio_bus.read(*addr, &mut magic_data);
    let magic = u32::from_le_bytes(magic_data);

    // Read version (offset 0x04)
    let mut version_data = [0u8; 4];
    let _ = mmio_bus.read(*addr + 0x04, &mut version_data);
    let version = u32::from_le_bytes(version_data);

    // Read device_id (offset 0x08) - this is what determines the driver
    let mut device_id_data = [0u8; 4];
    let _ = mmio_bus.read(*addr + 0x08, &mut device_id_data);
    let device_id = u32::from_le_bytes(device_id_data);

    eprintln!(
      "vmm-ffi: MMIO[{}] @ 0x{:x}: found={}, magic=0x{:08x}, version={}, device_id={}",
      i,
      addr,
      magic_result.is_ok(),
      magic,
      version,
      device_id
    );
  }
  drop(vmm_guard);

  // Resume the VM (starts vCPU threads)
  if let Err(e) = vmm.lock().unwrap().resume_vm() {
    eprintln!("vmm-ffi: resume_vm failed: {:?}", e);
    return VMM_ERR_BOOT;
  }

  handle.vmm = Some(vmm);
  handle.event_manager = Some(event_manager);

  VMM_OK
}

/// Wait for the VM to exit.
///
/// Blocks until the VM shuts down or an error occurs.
/// Returns the VM exit code.
#[unsafe(no_mangle)]
pub extern "C" fn vmm_wait(handle: *mut VmmHandle) -> c_int {
  if handle.is_null() {
    return VMM_ERR_INVALID_ARG;
  }

  let handle = unsafe { &mut *handle };

  let event_manager = match &mut handle.event_manager {
    Some(em) => em,
    None => return VMM_ERR_INTERNAL,
  };

  let vmm = match &handle.vmm {
    Some(vmm) => vmm.clone(),
    None => return VMM_ERR_INTERNAL,
  };

  // Run the event loop until VM exits
  loop {
    // Check if VM is still running
    {
      let vmm_guard = vmm.lock().unwrap();
      if vmm_guard.instance_info().state == vmm::vmm_config::instance_info::VmState::NotStarted {
        break;
      }
    }

    // Process events with a short timeout
    match event_manager.run_with_timeout(100) {
      Ok(_) => {}
      Err(_) => break,
    }
  }

  VMM_OK
}

/// Start the event loop in a background thread.
///
/// This function spawns a thread that runs the event loop, allowing virtio
/// devices to function. Must be called after vmm_start() and before
/// interacting with the VM via vsock.
///
/// The event loop thread will run until the VM is shutdown via vmm_shutdown().
/// Returns VMM_OK on success.
#[unsafe(no_mangle)]
pub extern "C" fn vmm_start_event_loop(handle: *mut VmmHandle) -> c_int {
  if handle.is_null() {
    return VMM_ERR_INVALID_ARG;
  }

  // We need to access the handle from the thread, so we pass the raw pointer
  // This is safe because the handle outlives the thread (we join on destroy)
  let handle_ptr = handle as usize;

  let thread = std::thread::spawn(move || {
    let handle = handle_ptr as *mut VmmHandle;
    if handle.is_null() {
      return VMM_ERR_INVALID_ARG;
    }

    let handle = unsafe { &mut *handle };

    let event_manager = match &mut handle.event_manager {
      Some(em) => em,
      None => return VMM_ERR_INTERNAL,
    };

    let vmm = match &handle.vmm {
      Some(vmm) => vmm.clone(),
      None => return VMM_ERR_INTERNAL,
    };

    // Run the event loop until VM exits
    loop {
      // Check if VM has been told to shutdown
      {
        let vmm_guard = vmm.lock().unwrap();
        if vmm_guard.shutdown_exit_code().is_some() {
          break;
        }
      }

      // Process events with a short timeout
      match event_manager.run_with_timeout(100) {
        Ok(_) => {}
        Err(_) => break,
      }
    }

    VMM_OK
  });

  // Store the thread handle
  let handle = unsafe { &mut *handle };
  handle.event_loop_thread = Some(thread);

  VMM_OK
}

/// Shutdown the VM.
///
/// Forcibly terminates the VM if it's still running.
#[unsafe(no_mangle)]
pub extern "C" fn vmm_shutdown(handle: *mut VmmHandle) -> c_int {
  if handle.is_null() {
    return VMM_ERR_INVALID_ARG;
  }

  let handle = unsafe { &mut *handle };

  if let Some(vmm) = &handle.vmm {
    let mut vmm_guard = vmm.lock().unwrap();
    // Stop the VM
    vmm_guard.stop(vmm::FcExitCode::Ok);
  }

  // Wait for event loop thread to finish
  if let Some(thread) = handle.event_loop_thread.take() {
    let _ = thread.join();
  }

  VMM_OK
}

/// Destroy the VM handle and free resources.
///
/// The handle is invalid after this call.
#[unsafe(no_mangle)]
pub extern "C" fn vmm_destroy(handle: *mut VmmHandle) {
  if !handle.is_null() {
    unsafe {
      let _ = Box::from_raw(handle);
    }
  }
}

/// Get the vsock file descriptor for communication.
///
/// Returns the FD on success, -1 on failure.
/// This FD can be used to connect to guest services.
#[unsafe(no_mangle)]
pub extern "C" fn vmm_get_vsock_fd(handle: *mut VmmHandle) -> RawFd {
  if handle.is_null() {
    return -1;
  }

  let _handle = unsafe { &mut *handle };

  // TODO: Return actual vsock FD
  -1
}

/// Get a human-readable error message for the given error code.
#[unsafe(no_mangle)]
pub extern "C" fn vmm_strerror(error: c_int) -> *const c_char {
  static OK: &[u8] = b"Success\0";
  static INVALID_ARG: &[u8] = b"Invalid argument\0";
  static KVM_INIT: &[u8] = b"KVM initialization failed\0";
  static VM_CREATE: &[u8] = b"VM creation failed\0";
  static BOOT: &[u8] = b"VM boot failed\0";
  static DEVICE: &[u8] = b"Device configuration failed\0";
  static VSOCK: &[u8] = b"vsock error\0";
  static NOT_IMPL: &[u8] = b"Not implemented\0";
  static INTERNAL: &[u8] = b"Internal error\0";
  static UNKNOWN: &[u8] = b"Unknown error\0";

  let msg = match error {
    VMM_OK => OK,
    VMM_ERR_INVALID_ARG => INVALID_ARG,
    VMM_ERR_KVM_INIT => KVM_INIT,
    VMM_ERR_VM_CREATE => VM_CREATE,
    VMM_ERR_BOOT => BOOT,
    VMM_ERR_DEVICE => DEVICE,
    VMM_ERR_VSOCK => VSOCK,
    VMM_ERR_NOT_IMPLEMENTED => NOT_IMPL,
    VMM_ERR_INTERNAL => INTERNAL,
    _ => UNKNOWN,
  };

  msg.as_ptr() as *const c_char
}

// ============================================================================
// Tests
// ============================================================================

#[cfg(test)]
mod tests {
  use super::*;
  use std::sync::Barrier;
  use vmm::vstate::bus::{Bus, BusDeviceSync};

  #[test]
  fn test_create_destroy() {
    // Test that creating a VMM with a non-existent kernel path fails gracefully.
    // A full integration test would require real kernel/initrd files.
    let config = VmmConfig {
      kernel_path: b"/nonexistent/kernel\0".as_ptr() as *const c_char,
      initrd_path: std::ptr::null(),
      kernel_cmdline: std::ptr::null(),
      vcpu_count: 1,
      mem_size_mib: 512,
    };

    let mut error: c_int = 0;
    let handle = vmm_create(&config, &mut error);

    // Should fail because the kernel file doesn't exist
    assert!(handle.is_null());
    assert_eq!(error, VMM_ERR_BOOT);
  }

  #[test]
  fn test_null_config() {
    let mut error: c_int = 0;
    let handle = vmm_create(std::ptr::null(), &mut error);

    assert!(handle.is_null());
    assert_eq!(error, VMM_ERR_INVALID_ARG);
  }

  // ========================================================================
  // MMIO Bus routing tests
  // ========================================================================

  /// A mock virtio-mmio device that responds with configurable values
  /// at the standard virtio-mmio register offsets.
  struct MockVirtioMmioDevice {
    magic: u32,     // offset 0x00
    version: u32,   // offset 0x04
    device_id: u32, // offset 0x08
    vendor_id: u32, // offset 0x0c
    #[allow(dead_code)]
    name: String,
  }

  impl MockVirtioMmioDevice {
    fn new(device_id: u32, name: &str) -> Self {
      Self {
        magic: 0x74726976, // "virt" in little-endian
        version: 2,
        device_id,
        vendor_id: 0x554d4551, // "QEMU" - standard for virtio
        name: name.to_string(),
      }
    }
  }

  impl BusDeviceSync for MockVirtioMmioDevice {
    fn read(&self, _base: u64, offset: u64, data: &mut [u8]) {
      let value = match offset {
        0x00 => self.magic,
        0x04 => self.version,
        0x08 => self.device_id,
        0x0c => self.vendor_id,
        _ => 0,
      };

      // Copy value to data buffer (little-endian)
      if data.len() >= 4 {
        data[0] = (value & 0xff) as u8;
        data[1] = ((value >> 8) & 0xff) as u8;
        data[2] = ((value >> 16) & 0xff) as u8;
        data[3] = ((value >> 24) & 0xff) as u8;
      }
    }

    fn write(&self, _base: u64, _offset: u64, _data: &[u8]) -> Option<Arc<Barrier>> {
      None
    }
  }

  fn read_u32_from_bus(bus: &Bus, addr: u64) -> Result<u32, ()> {
    let mut data = [0u8; 4];
    bus.read(addr, &mut data).map_err(|_| ())?;
    Ok(u32::from_le_bytes(data))
  }

  #[test]
  fn test_single_device_on_bus() {
    let bus = Bus::new();
    let device = Arc::new(MockVirtioMmioDevice::new(2, "block"));

    // Insert at address 0x1000 with size 0x1000
    // NOTE: Bus stores Weak references, so we must keep the Arc alive
    bus.insert(device.clone(), 0x1000, 0x1000).unwrap();

    // Read magic
    let magic = read_u32_from_bus(&bus, 0x1000).unwrap();
    assert_eq!(magic, 0x74726976, "Magic should be 'virt'");

    // Read version
    let version = read_u32_from_bus(&bus, 0x1004).unwrap();
    assert_eq!(version, 2, "Version should be 2");

    // Read device_id
    let device_id = read_u32_from_bus(&bus, 0x1008).unwrap();
    assert_eq!(device_id, 2, "Device ID should be 2 (block)");

    // Keep device alive until end of test
    drop(device);
  }

  #[test]
  fn test_multiple_devices_on_bus() {
    let bus = Bus::new();

    // Three devices at Firecracker's standard MMIO addresses
    // NOTE: Bus stores Weak references, so we must keep the Arcs alive
    let block0 = Arc::new(MockVirtioMmioDevice::new(2, "store"));
    let block1 = Arc::new(MockVirtioMmioDevice::new(2, "output"));
    let vsock = Arc::new(MockVirtioMmioDevice::new(19, "vsock"));

    // Insert at standard Firecracker addresses
    const MMIO_BASE: u64 = 0xc0001000;
    const MMIO_SIZE: u64 = 0x1000;

    bus.insert(block0.clone(), MMIO_BASE, MMIO_SIZE).unwrap();
    bus
      .insert(block1.clone(), MMIO_BASE + MMIO_SIZE, MMIO_SIZE)
      .unwrap();
    bus
      .insert(vsock.clone(), MMIO_BASE + 2 * MMIO_SIZE, MMIO_SIZE)
      .unwrap();

    // Verify device 0 (block)
    let magic0 = read_u32_from_bus(&bus, MMIO_BASE).unwrap();
    let device_id0 = read_u32_from_bus(&bus, MMIO_BASE + 0x08).unwrap();
    assert_eq!(magic0, 0x74726976, "Device 0 magic");
    assert_eq!(device_id0, 2, "Device 0 should be block");

    // Verify device 1 (block)
    let magic1 = read_u32_from_bus(&bus, MMIO_BASE + MMIO_SIZE).unwrap();
    let device_id1 = read_u32_from_bus(&bus, MMIO_BASE + MMIO_SIZE + 0x08).unwrap();
    assert_eq!(magic1, 0x74726976, "Device 1 magic");
    assert_eq!(device_id1, 2, "Device 1 should be block");

    // Verify device 2 (vsock)
    let magic2 = read_u32_from_bus(&bus, MMIO_BASE + 2 * MMIO_SIZE).unwrap();
    let device_id2 = read_u32_from_bus(&bus, MMIO_BASE + 2 * MMIO_SIZE + 0x08).unwrap();
    assert_eq!(magic2, 0x74726976, "Device 2 magic");
    assert_eq!(device_id2, 19, "Device 2 should be vsock");

    // Keep devices alive until end of test
    drop((block0, block1, vsock));
  }

  #[test]
  fn test_bus_address_boundaries() {
    let bus = Bus::new();

    // NOTE: Bus stores Weak references, so we must keep the Arcs alive
    let device0 = Arc::new(MockVirtioMmioDevice::new(2, "dev0"));
    let device1 = Arc::new(MockVirtioMmioDevice::new(19, "dev1"));

    // Two adjacent devices
    bus.insert(device0.clone(), 0x1000, 0x1000).unwrap();
    bus.insert(device1.clone(), 0x2000, 0x1000).unwrap();

    // Last valid address for device0
    let _ = read_u32_from_bus(&bus, 0x1ffc).unwrap();

    // First address for device1
    let magic1 = read_u32_from_bus(&bus, 0x2000).unwrap();
    assert_eq!(magic1, 0x74726976);

    let id1 = read_u32_from_bus(&bus, 0x2008).unwrap();
    assert_eq!(id1, 19, "Should reach device1 at 0x2008");

    // Address before device0 should fail
    assert!(bus.read(0x0ffc, &mut [0u8; 4]).is_err());

    // Address after device1 should fail
    assert!(bus.read(0x3000, &mut [0u8; 4]).is_err());

    // Keep devices alive until end of test
    drop((device0, device1));
  }

  /// Test that simulates what happens during kernel virtio-mmio probe:
  /// The kernel reads magic, version, and device_id from each registered
  /// MMIO address to identify devices.
  #[test]
  fn test_kernel_probe_sequence() {
    let bus = Bus::new();

    // Register devices at the addresses passed via kernel command line:
    // virtio_mmio.device=4K@0xc0001000:7
    // virtio_mmio.device=4K@0xc0002000:8
    // virtio_mmio.device=4K@0xc0003000:9
    // NOTE: Bus stores Weak references, so we must keep the Arcs alive
    let store = Arc::new(MockVirtioMmioDevice::new(2, "store"));
    let output = Arc::new(MockVirtioMmioDevice::new(2, "output"));
    let vsock = Arc::new(MockVirtioMmioDevice::new(19, "vsock"));

    bus.insert(store.clone(), 0xc0001000, 0x1000).unwrap();
    bus.insert(output.clone(), 0xc0002000, 0x1000).unwrap();
    bus.insert(vsock.clone(), 0xc0003000, 0x1000).unwrap();

    // Simulate kernel probing each device
    for (i, addr) in [0xc0001000u64, 0xc0002000, 0xc0003000].iter().enumerate() {
      // Step 1: Read magic (offset 0)
      let magic = read_u32_from_bus(&bus, *addr).unwrap();
      assert_eq!(
        magic, 0x74726976,
        "Device {} at {:#x} should have correct magic",
        i, addr
      );

      // Step 2: Read version (offset 4)
      let version = read_u32_from_bus(&bus, addr + 4).unwrap();
      assert_eq!(
        version, 2,
        "Device {} at {:#x} should have version 2",
        i, addr
      );

      // Step 3: Read device ID (offset 8)
      let device_id = read_u32_from_bus(&bus, addr + 8).unwrap();
      let expected_id = if i < 2 { 2 } else { 19 };
      assert_eq!(
        device_id, expected_id,
        "Device {} at {:#x} should have device_id {}",
        i, addr, expected_id
      );
    }

    // Keep devices alive until end of test
    drop((store, output, vsock));
  }
}
