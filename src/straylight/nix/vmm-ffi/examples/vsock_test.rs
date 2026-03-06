// Integration test: vsock communication with guest
//
// This test verifies the full vsock path:
// 1. VM boots with vsock device
// 2. Guest init listens on vsock port 5000
// 3. Host connects via Firecracker's UDS proxy
// 4. Protocol messages are exchanged
//
// Run with: buck2 run //src/straylight/nix/vmm-ffi:vsock_test
//
// Requires:
// - /dev/kvm access
// - Kernel and initrd at /tmp/fc-guest/ (or embedded)

use std::ffi::CString;
use std::fs::{self, File};
use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::path::Path;
use std::time::{Duration, Instant};

use vmm_ffi::*;

// Wire protocol constants (must match vm_protocol.h and nix-builder-init.c)
const VM_PROTOCOL_MAGIC: u32 = 0x4E495842; // "NIXB"
const VM_PROTOCOL_VERSION: u16 = 1;
const VM_VSOCK_BUILD_PORT: u32 = 5000;

const MSG_PING: u16 = 0x0003;
const MSG_PONG: u16 = 0x0104;

#[repr(C, packed)]
struct WireHeader {
  magic: u32,
  version: u16,
  msg_type: u16,
  payload_len: u32,
}

fn create_test_ext4_image(path: &str, size_mb: u64) -> std::io::Result<()> {
  let file = File::create(path)?;
  file.set_len(size_mb * 1024 * 1024)?;
  drop(file);

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

/// Try to connect to the guest via Firecracker's vsock UDS proxy
fn connect_to_guest(uds_path: &str, port: u32, timeout: Duration) -> std::io::Result<UnixStream> {
  let deadline = Instant::now() + timeout;
  let mut last_error = None;

  while Instant::now() < deadline {
    // Wait for socket file to exist
    if !Path::new(uds_path).exists() {
      std::thread::sleep(Duration::from_millis(100));
      continue;
    }

    // Try to connect
    match UnixStream::connect(uds_path) {
      Ok(mut stream) => {
        // Set read/write timeout
        stream.set_read_timeout(Some(Duration::from_secs(5)))?;
        stream.set_write_timeout(Some(Duration::from_secs(5)))?;

        // Send CONNECT command to Firecracker's vsock proxy
        // Format: "CONNECT <port>\n"
        let connect_cmd = format!("CONNECT {}\n", port);
        stream.write_all(connect_cmd.as_bytes())?;

        // Read response: "OK <guest_port>\n" or error
        let mut response = [0u8; 64];
        let n = stream.read(&mut response)?;
        let response_str = String::from_utf8_lossy(&response[..n]);

        if response_str.starts_with("OK ") {
          println!("  Connected to guest port {}", port);
          return Ok(stream);
        } else {
          last_error = Some(std::io::Error::new(
            std::io::ErrorKind::ConnectionRefused,
            format!("vsock CONNECT failed: {}", response_str.trim()),
          ));
        }
      }
      Err(e) => {
        last_error = Some(e);
      }
    }

    std::thread::sleep(Duration::from_millis(100));
  }

  Err(last_error.unwrap_or_else(|| {
    std::io::Error::new(std::io::ErrorKind::TimedOut, "vsock connection timeout")
  }))
}

/// Send PING and wait for PONG
fn ping_guest(stream: &mut UnixStream) -> std::io::Result<Duration> {
  let start = Instant::now();

  // Build PING message
  let header = WireHeader {
    magic: VM_PROTOCOL_MAGIC,
    version: VM_PROTOCOL_VERSION,
    msg_type: MSG_PING,
    payload_len: 0,
  };

  // Write header
  let header_bytes = unsafe {
    std::slice::from_raw_parts(&header as *const _ as *const u8, size_of::<WireHeader>())
  };
  stream.write_all(header_bytes)?;

  // Read response header
  let mut resp_header = [0u8; 12];
  stream.read_exact(&mut resp_header)?;

  // Parse header
  let magic = u32::from_le_bytes([
    resp_header[0],
    resp_header[1],
    resp_header[2],
    resp_header[3],
  ]);
  let version = u16::from_le_bytes([resp_header[4], resp_header[5]]);
  let msg_type = u16::from_le_bytes([resp_header[6], resp_header[7]]);
  let payload_len = u32::from_le_bytes([
    resp_header[8],
    resp_header[9],
    resp_header[10],
    resp_header[11],
  ]);

  // Validate
  if magic != VM_PROTOCOL_MAGIC {
    return Err(std::io::Error::new(
      std::io::ErrorKind::InvalidData,
      format!("bad magic: 0x{:08x}", magic),
    ));
  }
  if version != VM_PROTOCOL_VERSION {
    return Err(std::io::Error::new(
      std::io::ErrorKind::InvalidData,
      format!("bad version: {}", version),
    ));
  }
  if msg_type != MSG_PONG {
    return Err(std::io::Error::new(
      std::io::ErrorKind::InvalidData,
      format!("expected PONG (0x{:04x}), got 0x{:04x}", MSG_PONG, msg_type),
    ));
  }

  // Skip payload if any
  if payload_len > 0 {
    let mut payload = vec![0u8; payload_len as usize];
    stream.read_exact(&mut payload)?;
  }

  Ok(start.elapsed())
}

fn main() {
  println!("VMM FFI vsock Integration Test");
  println!("===============================\n");

  // Check /dev/kvm
  if !Path::new("/dev/kvm").exists() {
    eprintln!("ERROR: /dev/kvm not found. This test requires KVM.");
    std::process::exit(1);
  }

  // Create temp directory
  let test_dir = "/tmp/vmm-vsock-test";
  fs::create_dir_all(test_dir).expect("Failed to create test directory");

  let store_img = format!("{}/store.ext4", test_dir);
  let output_img = format!("{}/output.ext4", test_dir);
  let vsock_path = format!("{}/vsock.sock", test_dir);

  // Clean up
  let _ = fs::remove_file(&vsock_path);

  println!("Creating test artifacts...");
  create_test_ext4_image(&store_img, 64).expect("Failed to create store image");
  create_test_ext4_image(&output_img, 64).expect("Failed to create output image");

  // Check for kernel/initrd
  let kernel_exists = Path::new("/tmp/fc-guest/vmlinux").exists();
  let initrd_exists = Path::new("/tmp/fc-guest/initrd.img").exists();

  if !kernel_exists || !initrd_exists {
    eprintln!("\nERROR: Guest kernel/initrd not found.");
    eprintln!("Please run:");
    eprintln!("  nix build .#firecracker-guest");
    eprintln!("  mkdir -p /tmp/fc-guest");
    eprintln!("  cp result/vmlinux result/initrd.img /tmp/fc-guest/");
    std::process::exit(1);
  }

  // Create VM
  println!("\n[1/5] Creating VM...");
  let kernel_path = CString::new("/tmp/fc-guest/vmlinux").unwrap();
  let initrd_path = CString::new("/tmp/fc-guest/initrd.img").unwrap();
  let cmdline = CString::new("console=ttyS0 reboot=k panic=1 pci=off acpi=off init=/init").unwrap();

  let config = VmmConfig {
    kernel_path: kernel_path.as_ptr(),
    initrd_path: initrd_path.as_ptr(),
    kernel_cmdline: cmdline.as_ptr(),
    vcpu_count: 1,
    mem_size_mib: 512,
  };

  let mut error: i32 = 0;
  let handle = vmm_create(&config, &mut error);
  if handle.is_null() {
    eprintln!("FAIL: vmm_create: {}", unsafe {
      std::ffi::CStr::from_ptr(vmm_strerror(error)).to_string_lossy()
    });
    std::process::exit(1);
  }
  println!("  OK");

  // Add block devices
  println!("\n[2/5] Adding block devices...");
  let store_id = CString::new("store").unwrap();
  let store_path_c = CString::new(store_img.as_str()).unwrap();
  let store_dev = VmmBlockDevice {
    drive_id: store_id.as_ptr(),
    path: store_path_c.as_ptr(),
    is_read_only: 1,
  };
  if vmm_add_block_device(handle, &store_dev) != VMM_OK {
    eprintln!("FAIL: add store block device");
    vmm_destroy(handle);
    std::process::exit(1);
  }

  let output_id = CString::new("output").unwrap();
  let output_path_c = CString::new(output_img.as_str()).unwrap();
  let output_dev = VmmBlockDevice {
    drive_id: output_id.as_ptr(),
    path: output_path_c.as_ptr(),
    is_read_only: 0,
  };
  if vmm_add_block_device(handle, &output_dev) != VMM_OK {
    eprintln!("FAIL: add output block device");
    vmm_destroy(handle);
    std::process::exit(1);
  }
  println!("  OK (store + output)");

  // Configure vsock
  println!("\n[3/5] Configuring vsock...");
  let vsock_path_c = CString::new(vsock_path.as_str()).unwrap();
  let vsock_cfg = VmmVsockConfig {
    guest_cid: 3,
    uds_path: vsock_path_c.as_ptr(),
  };
  if vmm_configure_vsock(handle, &vsock_cfg) != VMM_OK {
    eprintln!("FAIL: configure vsock");
    vmm_destroy(handle);
    std::process::exit(1);
  }
  println!("  OK (CID=3, port={})", VM_VSOCK_BUILD_PORT);

  // Start VM
  println!("\n[4/5] Starting VM...");
  if vmm_start(handle) != VMM_OK {
    eprintln!("FAIL: vmm_start");
    vmm_destroy(handle);
    std::process::exit(1);
  }
  println!("  OK");

  // CRITICAL: Run the event loop in a background thread.
  // Without this, virtio devices won't function as they need
  // the event manager to dispatch MMIO accesses and interrupts.
  let handle_ptr = handle as *mut _ as usize;
  let event_thread = std::thread::spawn(move || {
    let handle = handle_ptr as *mut VmmHandle;
    eprintln!("  [event loop] Starting...");
    let result = vmm_wait(handle);
    eprintln!("  [event loop] Exited with code: {}", result);
    result
  });

  // Wait for vsock socket to appear
  println!("\n  Waiting for vsock socket...");
  let deadline = Instant::now() + Duration::from_secs(5);
  while Instant::now() < deadline {
    if Path::new(&vsock_path).exists() {
      println!("  Socket appeared: {}", vsock_path);
      break;
    }
    std::thread::sleep(Duration::from_millis(50));
  }

  if !Path::new(&vsock_path).exists() {
    eprintln!("FAIL: vsock socket never appeared");
    vmm_shutdown(handle);
    vmm_destroy(handle);
    std::process::exit(1);
  }

  // Try to connect to guest
  println!("\n[5/5] Connecting to guest vsock...");
  match connect_to_guest(&vsock_path, VM_VSOCK_BUILD_PORT, Duration::from_secs(30)) {
    Ok(mut stream) => {
      println!("  Connected!");

      // Send PING
      println!("\n  Sending PING...");
      match ping_guest(&mut stream) {
        Ok(rtt) => {
          println!("  PONG received in {:?}", rtt);
          println!("\n========================================");
          println!("SUCCESS: vsock communication working!");
          println!("========================================");
        }
        Err(e) => {
          eprintln!("FAIL: ping failed: {}", e);
        }
      }
    }
    Err(e) => {
      eprintln!("FAIL: connect failed: {}", e);
      eprintln!("\nPossible causes:");
      eprintln!("  1. Guest init crashed before reaching vsock listen");
      eprintln!("  2. vhost_vsock kernel module not loaded (run: modprobe vhost_vsock)");
      eprintln!("  3. Guest kernel missing VIRTIO_VSOCKETS support");
      eprintln!("\nDebug: Check kernel boot messages above for errors.");
    }
  }

  // Shutdown
  println!("\nShutting down...");
  vmm_shutdown(handle);
  let _ = event_thread.join();
  vmm_destroy(handle);

  // Cleanup
  let _ = fs::remove_file(&store_img);
  let _ = fs::remove_file(&output_img);
  let _ = fs::remove_file(&vsock_path);

  println!("Done.");
}
