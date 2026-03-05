//! File Descriptor Passing via SCM_RIGHTS
//!
//! Unix domain sockets support passing file descriptors between processes
//! using the SCM_RIGHTS control message. This module provides safe wrappers
//! for sending and receiving file descriptors.
//!
//! Protocol:
//! 1. Server creates shared memory + eventfds
//! 2. Server sends message with fds attached via SCM_RIGHTS:
//!    - Data: client_id (8 bytes) + shmem_size (8 bytes)
//!    - Fds: [shmem_fd, request_eventfd, response_eventfd]
//! 3. Client receives message and extracts fds
//! 4. Client mmaps shared memory, uses eventfds for signaling
//!
//! Reference: unix(7), cmsg(3)

use std::io;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::os::unix::net::UnixStream;

/// Maximum number of file descriptors we can pass in one message
const MAX_FDS: usize = 3;

/// Size of control message buffer (must be large enough for CMSG header + fds)
/// CMSG_SPACE(sizeof(int) * MAX_FDS)
const CMSG_BUF_SIZE: usize = 64;

/// Connection info sent from broker to client
#[derive(Debug, Clone)]
pub struct ConnectionMessage {
  /// Client ID assigned by broker
  pub client_id: u64,
  /// Size of shared memory region
  pub shmem_size: u64,
}

impl ConnectionMessage {
  /// Encode to bytes
  pub fn encode(&self) -> [u8; 16] {
    let mut buf = [0u8; 16];
    buf[0..8].copy_from_slice(&self.client_id.to_le_bytes());
    buf[8..16].copy_from_slice(&self.shmem_size.to_le_bytes());
    buf
  }

  /// Decode from bytes
  pub fn decode(buf: &[u8]) -> Option<Self> {
    if buf.len() < 16 {
      return None;
    }
    Some(Self {
      client_id: u64::from_le_bytes(buf[0..8].try_into().ok()?),
      shmem_size: u64::from_le_bytes(buf[8..16].try_into().ok()?),
    })
  }
}

/// File descriptors passed from broker to client
#[derive(Debug)]
pub struct ConnectionFds {
  /// Shared memory region fd
  pub shmem_fd: OwnedFd,
  /// Eventfd for signaling broker (client writes, broker reads)
  pub request_eventfd: OwnedFd,
  /// Eventfd for signaling client (broker writes, client reads)
  pub response_eventfd: OwnedFd,
}

/// Send a message with file descriptors attached
///
/// # Safety
/// The fds must be valid file descriptors that will remain valid
/// until sendmsg completes.
pub fn send_with_fds(
  stream: &UnixStream,
  msg: &ConnectionMessage,
  fds: &[RawFd],
) -> io::Result<()> {
  use libc::{
    CMSG_DATA, CMSG_FIRSTHDR, CMSG_LEN, CMSG_SPACE, SCM_RIGHTS, SOL_SOCKET, c_void, cmsghdr, iovec,
    msghdr, sendmsg,
  };

  if fds.len() > MAX_FDS {
    return Err(io::Error::new(
      io::ErrorKind::InvalidInput,
      format!("Too many fds: {} > {}", fds.len(), MAX_FDS),
    ));
  }

  let data = msg.encode();

  // Set up iovec for the data
  let mut iov = iovec {
    iov_base: data.as_ptr() as *mut c_void,
    iov_len: data.len(),
  };

  // Calculate control message size
  // Note: CMSG_SPACE returns u32 on musl, usize on glibc
  let cmsg_size = unsafe { CMSG_SPACE((std::mem::size_of::<RawFd>() * fds.len()) as u32) };
  let mut cmsg_buf = vec![0u8; cmsg_size as usize];

  // Set up msghdr
  // Note: msg_controllen is u32 on musl, usize on glibc
  let mut msg_hdr: msghdr = unsafe { std::mem::zeroed() };
  msg_hdr.msg_iov = &mut iov;
  msg_hdr.msg_iovlen = 1;
  msg_hdr.msg_control = cmsg_buf.as_mut_ptr() as *mut c_void;
  msg_hdr.msg_controllen = cmsg_size as _;

  // Fill in the control message header
  unsafe {
    let cmsg: *mut cmsghdr = CMSG_FIRSTHDR(&msg_hdr);
    if cmsg.is_null() {
      return Err(io::Error::new(
        io::ErrorKind::Other,
        "Failed to get CMSG_FIRSTHDR",
      ));
    }

    (*cmsg).cmsg_level = SOL_SOCKET;
    (*cmsg).cmsg_type = SCM_RIGHTS;
    // Note: cmsg_len is u32 on musl, usize on glibc
    (*cmsg).cmsg_len = CMSG_LEN((std::mem::size_of::<RawFd>() * fds.len()) as u32) as _;

    // Copy file descriptors into control message data
    let fd_ptr = CMSG_DATA(cmsg) as *mut RawFd;
    for (i, &fd) in fds.iter().enumerate() {
      std::ptr::write(fd_ptr.add(i), fd);
    }
  }

  // Send the message
  let ret = unsafe { sendmsg(stream.as_raw_fd(), &msg_hdr, 0) };

  if ret < 0 {
    return Err(io::Error::last_os_error());
  }

  if ret as usize != data.len() {
    return Err(io::Error::new(
      io::ErrorKind::WriteZero,
      "Short write in sendmsg",
    ));
  }

  Ok(())
}

/// Receive a message with file descriptors
///
/// Returns the message and extracted file descriptors.
pub fn recv_with_fds(stream: &UnixStream) -> io::Result<(ConnectionMessage, Vec<OwnedFd>)> {
  use libc::{
    CMSG_DATA, CMSG_FIRSTHDR, CMSG_NXTHDR, SCM_RIGHTS, SOL_SOCKET, c_void, cmsghdr, iovec, msghdr,
    recvmsg,
  };

  let mut data = [0u8; 16];

  // Set up iovec for the data
  let mut iov = iovec {
    iov_base: data.as_mut_ptr() as *mut c_void,
    iov_len: data.len(),
  };

  // Control message buffer
  let mut cmsg_buf = vec![0u8; CMSG_BUF_SIZE];

  // Set up msghdr
  // Note: msg_controllen is u32 on musl, usize on glibc
  let mut msg_hdr: msghdr = unsafe { std::mem::zeroed() };
  msg_hdr.msg_iov = &mut iov;
  msg_hdr.msg_iovlen = 1;
  msg_hdr.msg_control = cmsg_buf.as_mut_ptr() as *mut c_void;
  msg_hdr.msg_controllen = cmsg_buf.len() as _;

  // Receive the message
  let ret = unsafe { recvmsg(stream.as_raw_fd(), &mut msg_hdr, 0) };

  if ret < 0 {
    return Err(io::Error::last_os_error());
  }

  if ret == 0 {
    return Err(io::Error::new(
      io::ErrorKind::UnexpectedEof,
      "Connection closed",
    ));
  }

  // Decode the message
  let msg = ConnectionMessage::decode(&data[..ret as usize])
    .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "Invalid message format"))?;

  // Extract file descriptors from control messages
  let mut fds = Vec::new();

  unsafe {
    let mut cmsg: *mut cmsghdr = CMSG_FIRSTHDR(&msg_hdr);

    while !cmsg.is_null() {
      if (*cmsg).cmsg_level == SOL_SOCKET && (*cmsg).cmsg_type == SCM_RIGHTS {
        // Calculate number of fds in this message
        // Note: cmsg_len is u32 on musl, usize on glibc
        let fd_data_len =
          ((*cmsg).cmsg_len as usize).saturating_sub(std::mem::size_of::<cmsghdr>());
        let num_fds = fd_data_len / std::mem::size_of::<RawFd>();

        let fd_ptr = CMSG_DATA(cmsg) as *const RawFd;
        for i in 0..num_fds {
          let fd = std::ptr::read(fd_ptr.add(i));
          // Wrap in OwnedFd to ensure cleanup
          fds.push(OwnedFd::from_raw_fd(fd));
        }
      }

      cmsg = CMSG_NXTHDR(&msg_hdr, cmsg);
    }
  }

  Ok((msg, fds))
}

/// Send connection info with all three fds
pub fn send_connection(
  stream: &UnixStream,
  client_id: u64,
  shmem_size: u64,
  shmem_fd: RawFd,
  request_eventfd: RawFd,
  response_eventfd: RawFd,
) -> io::Result<()> {
  let msg = ConnectionMessage {
    client_id,
    shmem_size,
  };
  let fds = [shmem_fd, request_eventfd, response_eventfd];
  send_with_fds(stream, &msg, &fds)
}

/// Receive connection info with all three fds
pub fn recv_connection(stream: &UnixStream) -> io::Result<(ConnectionMessage, ConnectionFds)> {
  let (msg, fds) = recv_with_fds(stream)?;

  if fds.len() != 3 {
    // Close any fds we received to prevent leak
    // (OwnedFd handles this automatically when dropped)
    return Err(io::Error::new(
      io::ErrorKind::InvalidData,
      format!("Expected 3 fds, got {}", fds.len()),
    ));
  }

  let mut fds = fds.into_iter();
  let connection_fds = ConnectionFds {
    shmem_fd: fds.next().unwrap(),
    request_eventfd: fds.next().unwrap(),
    response_eventfd: fds.next().unwrap(),
  };

  Ok((msg, connection_fds))
}

// =============================================================================
// TESTS
// =============================================================================

#[cfg(test)]
mod tests {
  use super::*;
  use std::os::unix::net::UnixStream;

  #[test]
  fn test_message_encode_decode() {
    let msg = ConnectionMessage {
      client_id: 42,
      shmem_size: 1024 * 1024,
    };

    let encoded = msg.encode();
    let decoded = ConnectionMessage::decode(&encoded).unwrap();

    assert_eq!(decoded.client_id, 42);
    assert_eq!(decoded.shmem_size, 1024 * 1024);
  }

  #[test]
  fn test_fd_passing_roundtrip() {
    // Create a socket pair
    let (server, client) = UnixStream::pair().unwrap();

    // Create some file descriptors to pass
    // Use eventfds since they're easy to create
    let fd1 = unsafe {
      let fd = libc::eventfd(0, libc::EFD_CLOEXEC);
      assert!(fd >= 0, "Failed to create eventfd");
      OwnedFd::from_raw_fd(fd)
    };
    let fd2 = unsafe {
      let fd = libc::eventfd(0, libc::EFD_CLOEXEC);
      assert!(fd >= 0, "Failed to create eventfd");
      OwnedFd::from_raw_fd(fd)
    };
    let fd3 = unsafe {
      let fd = libc::eventfd(0, libc::EFD_CLOEXEC);
      assert!(fd >= 0, "Failed to create eventfd");
      OwnedFd::from_raw_fd(fd)
    };

    // Send from server
    send_connection(
      &server,
      123,
      4096,
      fd1.as_raw_fd(),
      fd2.as_raw_fd(),
      fd3.as_raw_fd(),
    )
    .unwrap();

    // Receive on client
    let (msg, fds) = recv_connection(&client).unwrap();

    assert_eq!(msg.client_id, 123);
    assert_eq!(msg.shmem_size, 4096);

    // Verify we got valid fds by writing to them
    let val: u64 = 1;
    unsafe {
      let ret = libc::write(
        fds.request_eventfd.as_raw_fd(),
        &val as *const u64 as *const libc::c_void,
        8,
      );
      assert_eq!(ret, 8, "Failed to write to passed eventfd");
    }
  }

  #[test]
  fn test_shmem_fd_passing() {
    use std::ptr;

    // Create a socket pair
    let (server, client) = UnixStream::pair().unwrap();

    // Create shared memory on server side
    let shmem_fd = unsafe {
      let fd = libc::memfd_create(
        b"test-shmem\0".as_ptr() as *const libc::c_char,
        libc::MFD_CLOEXEC,
      );
      assert!(fd >= 0, "Failed to create memfd");

      // Set size
      let ret = libc::ftruncate(fd, 4096);
      assert_eq!(ret, 0, "Failed to ftruncate");

      OwnedFd::from_raw_fd(fd)
    };

    // Map and write some data
    unsafe {
      let ptr = libc::mmap(
        ptr::null_mut(),
        4096,
        libc::PROT_READ | libc::PROT_WRITE,
        libc::MAP_SHARED,
        shmem_fd.as_raw_fd(),
        0,
      );
      assert_ne!(ptr, libc::MAP_FAILED, "Failed to mmap");

      // Write magic value
      *(ptr as *mut u64) = 0xDEADBEEF_CAFEBABE;

      libc::munmap(ptr, 4096);
    }

    // Create dummy eventfds
    let evfd1 = unsafe { OwnedFd::from_raw_fd(libc::eventfd(0, libc::EFD_CLOEXEC)) };
    let evfd2 = unsafe { OwnedFd::from_raw_fd(libc::eventfd(0, libc::EFD_CLOEXEC)) };

    // Send
    send_connection(
      &server,
      999,
      4096,
      shmem_fd.as_raw_fd(),
      evfd1.as_raw_fd(),
      evfd2.as_raw_fd(),
    )
    .unwrap();

    // Receive
    let (msg, fds) = recv_connection(&client).unwrap();
    assert_eq!(msg.client_id, 999);

    // Map the received shmem fd and verify data
    unsafe {
      let ptr = libc::mmap(
        ptr::null_mut(),
        4096,
        libc::PROT_READ | libc::PROT_WRITE,
        libc::MAP_SHARED,
        fds.shmem_fd.as_raw_fd(),
        0,
      );
      assert_ne!(ptr, libc::MAP_FAILED, "Failed to mmap received fd");

      let value = *(ptr as *const u64);
      assert_eq!(value, 0xDEADBEEF_CAFEBABE, "Shared memory data mismatch");

      libc::munmap(ptr, 4096);
    }
  }
}

#[cfg(test)]
mod proptests {
  use super::*;
  use proptest::prelude::*;

  proptest! {
      /// Property: Message encoding is reversible
      #[test]
      fn message_roundtrip(client_id: u64, shmem_size: u64) {
          let msg = ConnectionMessage { client_id, shmem_size };
          let encoded = msg.encode();
          let decoded = ConnectionMessage::decode(&encoded).unwrap();

          prop_assert_eq!(decoded.client_id, client_id);
          prop_assert_eq!(decoded.shmem_size, shmem_size);
      }

      /// Property: Can pass varying numbers of fds (1-3)
      #[test]
      fn varying_fd_count(num_fds in 1usize..=3) {
          let (server, client) = UnixStream::pair().unwrap();

          // Create fds
          let fds: Vec<OwnedFd> = (0..num_fds)
              .map(|_| unsafe {
                  OwnedFd::from_raw_fd(libc::eventfd(0, libc::EFD_CLOEXEC))
              })
              .collect();

          let raw_fds: Vec<RawFd> = fds.iter().map(|fd| fd.as_raw_fd()).collect();

          let msg = ConnectionMessage {
              client_id: 42,
              shmem_size: 1024,
          };

          send_with_fds(&server, &msg, &raw_fds).unwrap();
          let (recv_msg, recv_fds) = recv_with_fds(&client).unwrap();

          prop_assert_eq!(recv_msg.client_id, 42);
          prop_assert_eq!(recv_fds.len(), num_fds);
      }
  }
}
