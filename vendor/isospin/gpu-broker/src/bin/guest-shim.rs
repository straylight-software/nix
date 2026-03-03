//! Guest Shim - NVIDIA device emulator for VMs
//!
//! This binary runs inside a guest VM and emulates /dev/nvidia* devices.
//! Instead of talking to real NVIDIA kernel drivers, it forwards all ioctls
//! to the GPU broker on the host via shared memory.
//!
//! Usage:
//!   guest-shim --broker-socket /path/to/broker.sock
//!
//! The guest-shim:
//! 1. Connects to the broker via Unix socket
//! 2. Receives shared memory fd and eventfds via SCM_RIGHTS
//! 3. Maps shared memory and sets up ring buffers
//! 4. Creates FUSE/CUSE devices at /dev/nvidia*
//! 5. Intercepts all ioctls and forwards them to broker
//! 6. Returns results to userspace applications
//!
//! This allows unmodified CUDA applications to run in the VM while the
//! actual GPU operations are proxied through the broker.

use std::os::fd::AsRawFd;
use std::os::unix::net::UnixStream;
use std::path::PathBuf;
use std::ptr;
use std::sync::atomic::{AtomicU32, Ordering};

use anyhow::{Context, Result};
use clap::Parser;
use tracing::{debug, info, warn, error};

/// Guest Shim - NVIDIA device emulator for VMs
#[derive(Parser, Debug)]
#[command(name = "guest-shim")]
#[command(about = "Emulates /dev/nvidia* devices, forwarding ioctls to GPU broker")]
struct Args {
    /// Path to broker's Unix socket
    #[arg(long, default_value = "/run/gpu-broker.sock")]
    broker_socket: PathBuf,

    /// Mount point for CUSE devices (will create nvidia* devices here)
    #[arg(long, default_value = "/dev")]
    mount_point: PathBuf,

    /// Enable verbose logging
    #[arg(short, long)]
    verbose: bool,

    /// Dry run - connect but don't create CUSE devices
    #[arg(long)]
    dry_run: bool,

    /// Live test - send an ioctl through the ring and verify response
    #[arg(long)]
    live_test: bool,
}

/// Full connection state after receiving fds from broker
struct BrokerConnection {
    /// Our assigned client ID
    client_id: u64,

    /// Size of shared memory region
    shmem_size: u64,

    /// Mapped shared memory pointer
    shmem_ptr: *mut u8,

    /// Eventfd for signaling broker (we write, broker reads)
    request_eventfd: i32,

    /// Eventfd for broker signaling us (broker writes, we read)
    response_eventfd: i32,
}

impl Drop for BrokerConnection {
    fn drop(&mut self) {
        unsafe {
            if !self.shmem_ptr.is_null() {
                libc::munmap(self.shmem_ptr as *mut libc::c_void, self.shmem_size as usize);
            }
            if self.request_eventfd >= 0 {
                libc::close(self.request_eventfd);
            }
            if self.response_eventfd >= 0 {
                libc::close(self.response_eventfd);
            }
        }
    }
}

fn main() -> Result<()> {
    let args = Args::parse();

    // Initialize logging
    tracing_subscriber::fmt()
        .with_env_filter(if args.verbose { "debug" } else { "info" })
        .init();

    info!("Guest Shim starting");
    info!("  broker_socket: {:?}", args.broker_socket);
    info!("  mount_point: {:?}", args.mount_point);
    info!("  dry_run: {}", args.dry_run);

    // Connect to broker
    info!("Connecting to broker...");
    let conn = connect_to_broker(&args.broker_socket)
        .context("Failed to connect to broker")?;

    info!("Connected! Client ID: {}, shmem_size: {}",
          conn.client_id, conn.shmem_size);
    info!("  shmem mapped at: {:p}", conn.shmem_ptr);
    info!("  request_eventfd: {}", conn.request_eventfd);
    info!("  response_eventfd: {}", conn.response_eventfd);

    // Verify shared memory is accessible
    verify_shared_memory(&conn)?;

    if args.dry_run {
        info!("Dry run mode - not creating CUSE devices");
        info!("Connection test successful!");
        return Ok(());
    }

    if args.live_test {
        info!("Live test mode - sending ioctl through ring buffer");
        run_live_test(&conn)?;
        info!("Live test successful!");
        return Ok(());
    }

    // In a full implementation, we would:
    // 1. Create CUSE devices for /dev/nvidiactl, /dev/nvidia0, etc.
    // 2. Enter event loop handling ioctls
    // 3. Forward ioctls to broker via shared memory ring
    // 4. Wait for responses via response_eventfd

    warn!("CUSE device creation not yet implemented");
    warn!("This is a connection test only");

    info!("Guest shim would now create CUSE devices at {:?}", args.mount_point);
    info!("Devices: nvidiactl, nvidia0, nvidia-uvm, nvidia-modeset");

    Ok(())
}

/// Connect to the broker and receive connection info with fds via SCM_RIGHTS
fn connect_to_broker(socket_path: &PathBuf) -> Result<BrokerConnection> {
    let stream = UnixStream::connect(socket_path)
        .context("Failed to connect to Unix socket")?;

    debug!("Connected to socket, waiting for connection info with fds...");

    // Receive message with fds using SCM_RIGHTS
    let (msg, fds) = recv_with_fds(&stream)
        .context("Failed to receive connection info with fds")?;

    if fds.len() != 3 {
        anyhow::bail!("Expected 3 fds, got {}", fds.len());
    }

    let shmem_fd = fds[0];
    let request_eventfd = fds[1];
    let response_eventfd = fds[2];

    debug!("Received fds: shmem={}, req_evfd={}, resp_evfd={}",
           shmem_fd, request_eventfd, response_eventfd);

    // Map the shared memory
    let shmem_ptr = unsafe {
        libc::mmap(
            ptr::null_mut(),
            msg.shmem_size as usize,
            libc::PROT_READ | libc::PROT_WRITE,
            libc::MAP_SHARED,
            shmem_fd,
            0,
        )
    };

    if shmem_ptr == libc::MAP_FAILED {
        // Close the fds we received since we're failing
        unsafe {
            libc::close(shmem_fd);
            libc::close(request_eventfd);
            libc::close(response_eventfd);
        }
        anyhow::bail!("Failed to mmap shared memory: {}", std::io::Error::last_os_error());
    }

    // Close the shmem fd - we have it mapped now
    unsafe { libc::close(shmem_fd); }

    Ok(BrokerConnection {
        client_id: msg.client_id,
        shmem_size: msg.shmem_size,
        shmem_ptr: shmem_ptr as *mut u8,
        request_eventfd,
        response_eventfd,
    })
}

/// Verify shared memory is accessible by reading the ring headers
fn verify_shared_memory(conn: &BrokerConnection) -> Result<()> {
    // The shared memory layout starts with two RingHeader structs
    // Each RingHeader is 192 bytes (3 cache lines)
    // We just verify we can read the first few bytes

    unsafe {
        // Read request ring head (should be 0 initially)
        let head = ptr::read_volatile(conn.shmem_ptr as *const u32);
        debug!("Request ring head: {}", head);

        // Read response ring head (at offset 192)
        let resp_head = ptr::read_volatile(conn.shmem_ptr.add(192) as *const u32);
        debug!("Response ring head: {}", resp_head);
    }

    info!("Shared memory verified accessible");
    Ok(())
}

// =============================================================================
// SCM_RIGHTS fd passing (inline implementation for binary)
// =============================================================================

/// Connection message from broker
struct ConnectionMessage {
    client_id: u64,
    shmem_size: u64,
}

/// Receive message with file descriptors via SCM_RIGHTS
fn recv_with_fds(stream: &UnixStream) -> std::io::Result<(ConnectionMessage, Vec<i32>)> {
    use libc::{
        c_void, cmsghdr, iovec, msghdr, recvmsg, SCM_RIGHTS, SOL_SOCKET,
        CMSG_DATA, CMSG_FIRSTHDR, CMSG_NXTHDR,
    };

    let mut data = [0u8; 16];

    // Set up iovec for the data
    let mut iov = iovec {
        iov_base: data.as_mut_ptr() as *mut c_void,
        iov_len: data.len(),
    };

    // Control message buffer (large enough for 3 fds)
    let mut cmsg_buf = vec![0u8; 64];

    // Set up msghdr
    let mut msg_hdr: msghdr = unsafe { std::mem::zeroed() };
    msg_hdr.msg_iov = &mut iov;
    msg_hdr.msg_iovlen = 1;
    msg_hdr.msg_control = cmsg_buf.as_mut_ptr() as *mut c_void;
    msg_hdr.msg_controllen = cmsg_buf.len();

    // Receive the message
    let ret = unsafe { recvmsg(stream.as_raw_fd(), &mut msg_hdr, 0) };

    if ret < 0 {
        return Err(std::io::Error::last_os_error());
    }

    if ret == 0 {
        return Err(std::io::Error::new(
            std::io::ErrorKind::UnexpectedEof,
            "Connection closed",
        ));
    }

    // Decode the message
    let client_id = u64::from_le_bytes(data[0..8].try_into().unwrap());
    let shmem_size = u64::from_le_bytes(data[8..16].try_into().unwrap());

    let msg = ConnectionMessage {
        client_id,
        shmem_size,
    };

    // Extract file descriptors from control messages
    let mut fds = Vec::new();

    unsafe {
        let mut cmsg: *mut cmsghdr = CMSG_FIRSTHDR(&msg_hdr);

        while !cmsg.is_null() {
            if (*cmsg).cmsg_level == SOL_SOCKET && (*cmsg).cmsg_type == SCM_RIGHTS {
                // Calculate number of fds in this message
                let fd_data_len = (*cmsg).cmsg_len - std::mem::size_of::<cmsghdr>();
                let num_fds = fd_data_len / std::mem::size_of::<i32>();

                let fd_ptr = CMSG_DATA(cmsg) as *const i32;
                for i in 0..num_fds {
                    let fd = ptr::read(fd_ptr.add(i));
                    fds.push(fd);
                }
            }

            cmsg = CMSG_NXTHDR(&msg_hdr, cmsg);
        }
    }

    Ok((msg, fds))
}

// =============================================================================
// CUSE Implementation (placeholder)
// =============================================================================

/// CUSE device types we need to emulate
#[allow(dead_code)]
enum CuseDevice {
    /// /dev/nvidiactl - control device
    Control,
    /// /dev/nvidia0 - GPU 0
    Gpu0,
    /// /dev/nvidia-uvm - unified virtual memory
    Uvm,
    /// /dev/nvidia-modeset - display modeset
    Modeset,
}

#[allow(dead_code)]
impl CuseDevice {
    fn name(&self) -> &'static str {
        match self {
            Self::Control => "nvidiactl",
            Self::Gpu0 => "nvidia0",
            Self::Uvm => "nvidia-uvm",
            Self::Modeset => "nvidia-modeset",
        }
    }

    fn major(&self) -> u32 {
        // NVIDIA uses major number 195
        195
    }

    fn minor(&self) -> u32 {
        match self {
            Self::Control => 255,
            Self::Gpu0 => 0,
            Self::Uvm => 254,
            Self::Modeset => 253,
        }
    }
}

// The actual CUSE implementation would:
// 1. Open /dev/cuse
// 2. Send CUSE_INIT request
// 3. Create character device with specified major/minor
// 4. Handle FUSE_OPEN, FUSE_RELEASE, FUSE_IOCTL requests
// 5. Forward ioctls to broker via shared memory ring
// 6. Return results to userspace

// This is a significant amount of code and would require either:
// - The `fuser` crate (high-level FUSE bindings)
// - Direct CUSE protocol implementation
// - A kernel module approach instead

// For now, we have the full connection infrastructure with fd passing.
// The CUSE layer can be added incrementally.

// =============================================================================
// LIVE TEST - End-to-end ioctl through ring buffer
// =============================================================================

/// Shared memory layout constants (must match uring_transport.rs)
const RING_SIZE: usize = 256;
const ENTRY_SIZE: usize = 4096;
const RING_HEADER_SIZE: usize = 192;
const RING_ENTRY_SIZE: usize = 16;

/// Ring header offsets within cache-line aligned structure
const HEAD_OFFSET: usize = 0;
const TAIL_OFFSET: usize = 64;
const MASK_OFFSET: usize = 128;

/// Shared memory layout offsets
fn layout_offsets() -> (usize, usize, usize, usize, usize, usize) {
    let request_header = 0;
    let response_header = RING_HEADER_SIZE;
    let request_entries = RING_HEADER_SIZE * 2;
    let response_entries = request_entries + RING_SIZE * RING_ENTRY_SIZE;
    let request_data = response_entries + RING_SIZE * RING_ENTRY_SIZE;
    let response_data = request_data + RING_SIZE * ENTRY_SIZE;
    (request_header, response_header, request_entries, response_entries, request_data, response_data)
}

/// Run a live end-to-end test through the ring buffer
fn run_live_test(conn: &BrokerConnection) -> Result<()> {
    info!("=== LIVE WIRE TEST ===");
    
    let (req_hdr, resp_hdr, req_entries, resp_entries, req_data, resp_data) = layout_offsets();
    
    // Step 1: Encode a RegisterClient request
    // Wire format: [client_id: u64] [seq: u64] [op_type: u32] [payload_len: u32] [reserved: u64]
    let client_id = conn.client_id;
    let seq: u64 = 1;
    let op_type: u32 = 0;  // RegisterClient
    let payload_len: u32 = 0;
    
    let mut request_bytes = Vec::with_capacity(32);
    request_bytes.extend_from_slice(&client_id.to_le_bytes());
    request_bytes.extend_from_slice(&seq.to_le_bytes());
    request_bytes.extend_from_slice(&op_type.to_le_bytes());
    request_bytes.extend_from_slice(&payload_len.to_le_bytes());
    request_bytes.extend_from_slice(&0u64.to_le_bytes());  // reserved
    
    info!("Step 1: Encoded RegisterClient request ({} bytes)", request_bytes.len());
    debug!("  client_id: {}", client_id);
    debug!("  seq: {}", seq);
    debug!("  op_type: {} (RegisterClient)", op_type);
    
    // Step 2: Write request to ring buffer
    unsafe {
        let base = conn.shmem_ptr;
        
        // Read ring header to get current tail
        let head_ptr = base.add(req_hdr + HEAD_OFFSET) as *const AtomicU32;
        let tail_ptr = base.add(req_hdr + TAIL_OFFSET) as *const AtomicU32;
        let mask_ptr = base.add(req_hdr + MASK_OFFSET) as *const u32;
        
        let head = (*head_ptr).load(Ordering::Acquire);
        let tail = (*tail_ptr).load(Ordering::Relaxed);
        let mask = ptr::read_volatile(mask_ptr);
        
        debug!("  Request ring: head={}, tail={}, mask={}", head, tail, mask);
        
        // Check if ring is full
        if tail.wrapping_sub(head) >= RING_SIZE as u32 {
            anyhow::bail!("Request ring is full!");
        }
        
        let slot_idx = tail & mask;
        debug!("  Writing to slot {}", slot_idx);
        
        // Write data to data pool
        let data_slot = base.add(req_data + slot_idx as usize * ENTRY_SIZE);
        ptr::copy_nonoverlapping(request_bytes.as_ptr(), data_slot, request_bytes.len());
        
        // Write entry metadata
        let entry_ptr = base.add(req_entries + slot_idx as usize * RING_ENTRY_SIZE);
        let entry_data_offset = (slot_idx as usize * ENTRY_SIZE) as u32;
        let entry_data_len = request_bytes.len() as u32;
        ptr::write_volatile(entry_ptr as *mut u32, entry_data_offset);
        ptr::write_volatile(entry_ptr.add(4) as *mut u32, entry_data_len);
        ptr::write_volatile(entry_ptr.add(8) as *mut u32, 0);  // flags
        ptr::write_volatile(entry_ptr.add(12) as *mut u32, seq as u32);  // seq
        
        // Memory barrier then advance tail
        std::sync::atomic::fence(Ordering::Release);
        (*tail_ptr).store(tail.wrapping_add(1), Ordering::Release);
        
        info!("Step 2: Wrote request to ring slot {}", slot_idx);
    }
    
    // Step 3: Signal broker via eventfd
    let signal_val: u64 = 1;
    let ret = unsafe {
        libc::write(
            conn.request_eventfd,
            &signal_val as *const u64 as *const libc::c_void,
            8,
        )
    };
    if ret < 0 {
        anyhow::bail!("Failed to signal request eventfd: {}", std::io::Error::last_os_error());
    }
    info!("Step 3: Signaled broker via request_eventfd");
    
    // Step 4: Wait for response via eventfd (with timeout)
    info!("Step 4: Waiting for response...");
    let mut pollfd = libc::pollfd {
        fd: conn.response_eventfd,
        events: libc::POLLIN,
        revents: 0,
    };
    let timeout_ms = 5000;  // 5 second timeout
    let ret = unsafe { libc::poll(&mut pollfd, 1, timeout_ms) };
    
    if ret < 0 {
        anyhow::bail!("poll() failed: {}", std::io::Error::last_os_error());
    }
    if ret == 0 {
        anyhow::bail!("Timeout waiting for response after {}ms", timeout_ms);
    }
    
    // Read the eventfd to clear it
    let mut event_val: u64 = 0;
    unsafe {
        libc::read(
            conn.response_eventfd,
            &mut event_val as *mut u64 as *mut libc::c_void,
            8,
        );
    }
    debug!("  Response eventfd signaled (value={})", event_val);
    
    // Step 5: Read response from ring buffer
    info!("Step 5: Reading response from ring...");
    unsafe {
        let base = conn.shmem_ptr;
        
        // Read response ring header
        let head_ptr = base.add(resp_hdr + HEAD_OFFSET) as *const AtomicU32;
        let tail_ptr = base.add(resp_hdr + TAIL_OFFSET) as *const AtomicU32;
        let mask_ptr = base.add(resp_hdr + MASK_OFFSET) as *const u32;
        
        let head = (*head_ptr).load(Ordering::Relaxed);
        let tail = (*tail_ptr).load(Ordering::Acquire);
        let mask = ptr::read_volatile(mask_ptr);
        
        debug!("  Response ring: head={}, tail={}, mask={}", head, tail, mask);
        
        if head == tail {
            anyhow::bail!("Response ring is empty after signal!");
        }
        
        let slot_idx = head & mask;
        debug!("  Reading from slot {}", slot_idx);
        
        // Read entry metadata
        let entry_ptr = base.add(resp_entries + slot_idx as usize * RING_ENTRY_SIZE);
        let data_len = ptr::read_volatile(entry_ptr.add(4) as *const u32) as usize;
        
        // Read response data
        let data_slot = base.add(resp_data + slot_idx as usize * ENTRY_SIZE);
        let mut response_bytes = vec![0u8; data_len];
        ptr::copy_nonoverlapping(data_slot, response_bytes.as_mut_ptr(), data_len);
        
        // Advance head to consume
        std::sync::atomic::fence(Ordering::Acquire);
        (*head_ptr).store(head.wrapping_add(1), Ordering::Release);
        
        // Parse response
        // Wire format: [client_id: u64] [seq: u64] [success: u8] [result...]
        if response_bytes.len() < 17 {
            anyhow::bail!("Response too short: {} bytes", response_bytes.len());
        }
        
        let resp_client_id = u64::from_le_bytes(response_bytes[0..8].try_into().unwrap());
        let resp_seq = u64::from_le_bytes(response_bytes[8..16].try_into().unwrap());
        let success = response_bytes[16];
        
        info!("Step 6: Response received!");
        info!("  client_id: {} (expected {})", resp_client_id, client_id);
        info!("  seq: {} (expected {})", resp_seq, seq);
        info!("  success: {} ({})", success, if success == 1 { "OK" } else { "ERROR" });
        
        // Validate response
        if resp_client_id != client_id {
            error!("Client ID mismatch!");
            anyhow::bail!("Response client_id {} != request client_id {}", resp_client_id, client_id);
        }
        if resp_seq != seq {
            error!("Sequence number mismatch!");
            anyhow::bail!("Response seq {} != request seq {}", resp_seq, seq);
        }
        if success != 1 {
            error!("Operation failed!");
            anyhow::bail!("RegisterClient operation failed (success={})", success);
        }
        
        // If we got success=1, check result type
        if response_bytes.len() >= 21 {
            let result_type = u32::from_le_bytes(response_bytes[17..21].try_into().unwrap());
            debug!("  result_type: {}", result_type);
        }
    }
    
    info!("=== RegisterClient PASSED ===");
    
    // Now test an Alloc operation - allocate a root client first
    info!("");
    info!("=== Testing Alloc operation (NV01_ROOT_CLIENT) ===");
    
    let seq: u64 = 2;
    let op_type: u32 = 2;  // Alloc
    let h_root: u32 = 0;   // 0 means no root (this IS the root)
    let h_parent: u32 = 0; // 0 means no parent
    let h_new: u32 = 1;    // virtual handle for our root client
    let class: u32 = 0x41; // NV01_ROOT_CLIENT
    
    let mut request_bytes = Vec::with_capacity(48);
    request_bytes.extend_from_slice(&client_id.to_le_bytes());
    request_bytes.extend_from_slice(&seq.to_le_bytes());
    request_bytes.extend_from_slice(&op_type.to_le_bytes());
    request_bytes.extend_from_slice(&16u32.to_le_bytes()); // payload_len = 4 * u32
    request_bytes.extend_from_slice(&0u64.to_le_bytes());  // reserved
    // Payload
    request_bytes.extend_from_slice(&h_root.to_le_bytes());
    request_bytes.extend_from_slice(&h_parent.to_le_bytes());
    request_bytes.extend_from_slice(&h_new.to_le_bytes());
    request_bytes.extend_from_slice(&class.to_le_bytes());
    
    info!("Step 1: Encoded Alloc request ({} bytes)", request_bytes.len());
    debug!("  h_root: {}, h_parent: {}, h_new: {}, class: 0x{:x}", h_root, h_parent, h_new, class);
    
    // Write to ring
    unsafe {
        let base = conn.shmem_ptr;
        let tail_ptr = base.add(req_hdr + TAIL_OFFSET) as *const AtomicU32;
        let mask_ptr = base.add(req_hdr + MASK_OFFSET) as *const u32;
        
        let tail = (*tail_ptr).load(Ordering::Relaxed);
        let mask = ptr::read_volatile(mask_ptr);
        let slot_idx = tail & mask;
        
        // Write data
        let data_slot = base.add(req_data + slot_idx as usize * ENTRY_SIZE);
        ptr::copy_nonoverlapping(request_bytes.as_ptr(), data_slot, request_bytes.len());
        
        // Write entry
        let entry_ptr = base.add(req_entries + slot_idx as usize * RING_ENTRY_SIZE);
        ptr::write_volatile(entry_ptr as *mut u32, (slot_idx as usize * ENTRY_SIZE) as u32);
        ptr::write_volatile(entry_ptr.add(4) as *mut u32, request_bytes.len() as u32);
        ptr::write_volatile(entry_ptr.add(8) as *mut u32, 0);
        ptr::write_volatile(entry_ptr.add(12) as *mut u32, seq as u32);
        
        std::sync::atomic::fence(Ordering::Release);
        (*tail_ptr).store(tail.wrapping_add(1), Ordering::Release);
        
        info!("Step 2: Wrote request to ring slot {}", slot_idx);
    }
    
    // Signal broker
    let signal_val: u64 = 1;
    unsafe {
        libc::write(conn.request_eventfd, &signal_val as *const u64 as *const libc::c_void, 8);
    }
    info!("Step 3: Signaled broker");
    
    // Wait for response
    info!("Step 4: Waiting for response...");
    let mut pollfd = libc::pollfd {
        fd: conn.response_eventfd,
        events: libc::POLLIN,
        revents: 0,
    };
    unsafe { libc::poll(&mut pollfd, 1, 5000) };
    
    let mut event_val: u64 = 0;
    unsafe { libc::read(conn.response_eventfd, &mut event_val as *mut u64 as *mut libc::c_void, 8); }
    
    // Read response
    unsafe {
        let base = conn.shmem_ptr;
        let head_ptr = base.add(resp_hdr + HEAD_OFFSET) as *const AtomicU32;
        let tail_ptr = base.add(resp_hdr + TAIL_OFFSET) as *const AtomicU32;
        let mask_ptr = base.add(resp_hdr + MASK_OFFSET) as *const u32;
        
        let head = (*head_ptr).load(Ordering::Relaxed);
        let tail = (*tail_ptr).load(Ordering::Acquire);
        let mask = ptr::read_volatile(mask_ptr);
        
        if head == tail {
            anyhow::bail!("Response ring empty!");
        }
        
        let slot_idx = head & mask;
        let entry_ptr = base.add(resp_entries + slot_idx as usize * RING_ENTRY_SIZE);
        let data_len = ptr::read_volatile(entry_ptr.add(4) as *const u32) as usize;
        
        let data_slot = base.add(resp_data + slot_idx as usize * ENTRY_SIZE);
        let mut response_bytes = vec![0u8; data_len];
        ptr::copy_nonoverlapping(data_slot, response_bytes.as_mut_ptr(), data_len);
        
        (*head_ptr).store(head.wrapping_add(1), Ordering::Release);
        
        let resp_client_id = u64::from_le_bytes(response_bytes[0..8].try_into().unwrap());
        let resp_seq = u64::from_le_bytes(response_bytes[8..16].try_into().unwrap());
        let success = response_bytes[16];
        
        info!("Step 5: Response received!");
        info!("  client_id: {} (expected {})", resp_client_id, client_id);
        info!("  seq: {} (expected {})", resp_seq, seq);
        info!("  success: {} ({})", success, if success == 1 { "OK" } else { "ERROR" });
        
        if success == 1 && response_bytes.len() >= 25 {
            let result_type = u32::from_le_bytes(response_bytes[17..21].try_into().unwrap());
            if result_type == 2 {  // Allocated
                let real_handle = u32::from_le_bytes(response_bytes[21..25].try_into().unwrap());
                info!("  result: Allocated(real_handle=0x{:x})", real_handle);
            }
        } else if success == 0 {
            // Print error details
            if response_bytes.len() > 17 {
                let err_len = u32::from_le_bytes(response_bytes[17..21].try_into().unwrap_or([0;4])) as usize;
                if response_bytes.len() >= 21 + err_len {
                    let err_msg = String::from_utf8_lossy(&response_bytes[21..21+err_len]);
                    error!("  error: {}", err_msg);
                }
            }
            debug!("  raw response: {:?}", &response_bytes);
        }
        
        if resp_seq != seq || success != 1 {
            anyhow::bail!("Alloc operation failed!");
        }
    }
    
    info!("=== Alloc PASSED ===");
    info!("");
    info!("=== ALL LIVE WIRE TESTS PASSED ===");
    Ok(())
}
