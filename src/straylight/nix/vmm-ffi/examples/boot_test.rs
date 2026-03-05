// Simple test to boot a microVM using vmm_ffi
//
// Run with: buck2 run //src/straylight/nix/vmm-ffi:boot_test

use std::ffi::CString;

// Import the FFI functions
use vmm_ffi::*;

fn main() {
  println!("VMM FFI Boot Test");
  println!("=================");

  // Check that we can at least start
  println!("FFI constants:");
  println!("  VMM_OK = {}", VMM_OK);
  println!("  VMM_ERR_INVALID_ARG = {}", VMM_ERR_INVALID_ARG);

  // Test error string function
  let err_msg = unsafe { std::ffi::CStr::from_ptr(vmm_strerror(VMM_OK)) };
  println!("  vmm_strerror(VMM_OK) = {:?}", err_msg);

  println!("Basic FFI test passed!");

  // Now try creating a VM
  println!("\nCreating VM configuration...");

  // Use the firecracker guest from nix build
  let kernel_path = CString::new("/tmp/fc-guest/vmlinux").unwrap();
  let initrd_path = CString::new("/tmp/fc-guest/initrd.img").unwrap();
  let cmdline = CString::new("console=ttyS0 reboot=k panic=1 pci=off").unwrap();

  let config = VmmConfig {
    kernel_path: kernel_path.as_ptr(),
    initrd_path: initrd_path.as_ptr(),
    kernel_cmdline: cmdline.as_ptr(),
    vcpu_count: 1,
    mem_size_mib: 128,
  };

  println!("Creating VM...");
  let mut error: i32 = 0;
  let handle = vmm_create(&config, &mut error);

  if handle.is_null() {
    eprintln!(
      "Failed to create VM: {} ({})",
      unsafe { std::ffi::CStr::from_ptr(vmm_strerror(error)).to_string_lossy() },
      error
    );
    std::process::exit(1);
  }
  println!("VM created successfully");

  println!("Starting VM...");
  let result = vmm_start(handle);
  if result != VMM_OK {
    eprintln!(
      "Failed to start VM: {} ({})",
      unsafe { std::ffi::CStr::from_ptr(vmm_strerror(result)).to_string_lossy() },
      result
    );
    vmm_destroy(handle);
    std::process::exit(1);
  }
  println!("VM started successfully");

  println!("Waiting for VM to exit (timeout 5s)...");
  // Don't actually wait forever - just test that we can start
  std::thread::sleep(std::time::Duration::from_secs(5));

  println!("Shutting down VM...");
  vmm_shutdown(handle);

  println!("Destroying VM handle...");
  vmm_destroy(handle);

  println!("Test completed successfully!");
}
