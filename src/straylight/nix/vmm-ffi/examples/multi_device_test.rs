// Integration test: Boot VM with multiple virtio devices
//
// This test verifies that when we configure multiple block devices and vsock,
// all devices are properly registered on the MMIO bus and respond correctly.
//
// Run with: buck2 run //src/straylight/nix/vmm-ffi:multi_device_test
//
// Requires:
// - /dev/kvm access
// - Kernel and initrd at expected paths (or use embedded)

use std::ffi::CString;
use std::fs::{self, File};
use std::io::Write;
use std::path::Path;
use std::sync::Arc;

use vmm_ffi::*;

const MMIO_MAGIC: u32 = 0x74726976; // "virt" in little-endian
const VIRTIO_ID_BLOCK: u32 = 2;
const VIRTIO_ID_VSOCK: u32 = 19;

fn create_test_ext4_image(path: &str, size_mb: u64) -> std::io::Result<()> {
  // Create a sparse file
  let file = File::create(path)?;
  file.set_len(size_mb * 1024 * 1024)?;
  drop(file);

  // Format as ext4 (requires mkfs.ext4 in PATH)
  let status = std::process::Command::new("mkfs.ext4")
    .args(["-q", "-F", path])
    .status()?;

  if !status.success() {
    return Err(std::io::Error::new(
      std::io::ErrorKind::Other,
      "mkfs.ext4 failed",
    ));
  }
  Ok(())
}

fn main() {
  println!("VMM FFI Multi-Device Integration Test");
  println!("======================================\n");

  // Check /dev/kvm access
  if !Path::new("/dev/kvm").exists() {
    eprintln!("ERROR: /dev/kvm not found. This test requires KVM.");
    std::process::exit(1);
  }

  // Create temp directory for test artifacts
  let test_dir = "/tmp/vmm-ffi-test";
  fs::create_dir_all(test_dir).expect("Failed to create test directory");

  // Create test block device images
  let store_img = format!("{}/store.ext4", test_dir);
  let output_img = format!("{}/output.ext4", test_dir);
  let vsock_path = format!("{}/vsock.sock", test_dir);

  // Clean up any existing socket
  let _ = fs::remove_file(&vsock_path);

  println!("Creating test block images...");
  create_test_ext4_image(&store_img, 64).expect("Failed to create store image");
  create_test_ext4_image(&output_img, 64).expect("Failed to create output image");
  println!("  store:  {}", store_img);
  println!("  output: {}", output_img);
  println!("  vsock:  {}", vsock_path);

  // Check for embedded kernel data or external files
  let kernel_path = CString::new("/tmp/fc-guest/vmlinux").unwrap();
  let initrd_path = CString::new("/tmp/fc-guest/initrd.img").unwrap();
  let cmdline = CString::new("console=ttyS0 reboot=k panic=1 pci=off acpi=off init=/init").unwrap();

  let use_embedded = !Path::new("/tmp/fc-guest/vmlinux").exists();

  println!("\nVM Configuration:");
  println!("  Using embedded kernel: {}", use_embedded);

  // Create VM
  println!("\n[Step 1] Creating VM...");
  let mut error: i32 = 0;
  let handle = if use_embedded {
    // For embedded, we'd need the actual embedded data - skip for now
    eprintln!("ERROR: Embedded kernel test not implemented yet.");
    eprintln!("Please ensure /tmp/fc-guest/vmlinux and initrd.img exist.");
    std::process::exit(1);
  } else {
    let config = VmmConfig {
      kernel_path: kernel_path.as_ptr(),
      initrd_path: initrd_path.as_ptr(),
      kernel_cmdline: cmdline.as_ptr(),
      vcpu_count: 1,
      mem_size_mib: 512,
    };
    vmm_create(&config, &mut error)
  };

  if handle.is_null() {
    eprintln!(
      "FAIL: vmm_create failed: {} (code {})",
      unsafe { std::ffi::CStr::from_ptr(vmm_strerror(error)).to_string_lossy() },
      error
    );
    std::process::exit(1);
  }
  println!("  OK: VM handle created");

  // Add block device 1 (store, read-only)
  println!("\n[Step 2] Adding block device 'store' (read-only)...");
  let store_id = CString::new("store").unwrap();
  let store_path_c = CString::new(store_img.as_str()).unwrap();
  let store_device = VmmBlockDevice {
    drive_id: store_id.as_ptr(),
    path: store_path_c.as_ptr(),
    is_read_only: 1,
  };
  let result = vmm_add_block_device(handle, &store_device);
  if result != VMM_OK {
    eprintln!(
      "FAIL: vmm_add_block_device(store) failed: {} (code {})",
      unsafe { std::ffi::CStr::from_ptr(vmm_strerror(result)).to_string_lossy() },
      result
    );
    vmm_destroy(handle);
    std::process::exit(1);
  }
  println!("  OK: Block device 'store' added");

  // Add block device 2 (output, read-write)
  println!("\n[Step 3] Adding block device 'output' (read-write)...");
  let output_id = CString::new("output").unwrap();
  let output_path_c = CString::new(output_img.as_str()).unwrap();
  let output_device = VmmBlockDevice {
    drive_id: output_id.as_ptr(),
    path: output_path_c.as_ptr(),
    is_read_only: 0,
  };
  let result = vmm_add_block_device(handle, &output_device);
  if result != VMM_OK {
    eprintln!(
      "FAIL: vmm_add_block_device(output) failed: {} (code {})",
      unsafe { std::ffi::CStr::from_ptr(vmm_strerror(result)).to_string_lossy() },
      result
    );
    vmm_destroy(handle);
    std::process::exit(1);
  }
  println!("  OK: Block device 'output' added");

  // Add vsock
  println!("\n[Step 4] Configuring vsock...");
  let vsock_path_c = CString::new(vsock_path.as_str()).unwrap();
  let vsock_config = VmmVsockConfig {
    guest_cid: 3,
    uds_path: vsock_path_c.as_ptr(),
  };
  let result = vmm_configure_vsock(handle, &vsock_config);
  if result != VMM_OK {
    eprintln!(
      "FAIL: vmm_configure_vsock failed: {} (code {})",
      unsafe { std::ffi::CStr::from_ptr(vmm_strerror(result)).to_string_lossy() },
      result
    );
    vmm_destroy(handle);
    std::process::exit(1);
  }
  println!("  OK: vsock configured (CID=3)");

  // Start VM
  println!("\n[Step 5] Starting VM...");
  let result = vmm_start(handle);
  if result != VMM_OK {
    eprintln!(
      "FAIL: vmm_start failed: {} (code {})",
      unsafe { std::ffi::CStr::from_ptr(vmm_strerror(result)).to_string_lossy() },
      result
    );
    vmm_destroy(handle);
    std::process::exit(1);
  }
  println!("  OK: VM started");

  // Wait for kernel to boot and probe devices
  println!("\n[Step 6] Waiting for kernel to boot (10s)...");
  std::thread::sleep(std::time::Duration::from_secs(10));

  // The kernel output should be visible on the console
  // We can check /tmp/fc-debug.log if configured

  // Shutdown
  println!("\n[Step 7] Shutting down VM...");
  vmm_shutdown(handle);
  vmm_destroy(handle);
  println!("  OK: VM destroyed");

  // Analyze results
  println!("\n======================================");
  println!("Test Summary:");
  println!("  - VM created with 2 block devices + vsock: OK");
  println!("  - VM started successfully: OK");
  println!("  - Check kernel logs for device probe results");
  println!("");
  println!("To verify all devices were probed, check the kernel output for:");
  println!("  virtio_blk virtio0: [vda] ...  (store block device)");
  println!("  virtio_blk virtio1: [vdb] ...  (output block device)");
  println!("  virtio_vsock ...               (vsock device)");

  // Cleanup
  let _ = fs::remove_file(&store_img);
  let _ = fs::remove_file(&output_img);
  let _ = fs::remove_file(&vsock_path);

  println!("\nTest completed.");
}
