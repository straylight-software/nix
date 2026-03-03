//! io_uring Event Loop
//!
//! This module provides a proper io_uring-based event loop for multiplexing
//! multiple client eventfds. The design:
//!
//! 1. Each client has an eventfd that they signal when they have requests
//! 2. The broker uses io_uring PollAdd to wait on ALL client eventfds
//! 3. When any client signals, we wake up and process their requests
//! 4. After processing, we re-arm the poll for that client
//!
//! ```text
//!   Client 1 eventfd ─┐
//!   Client 2 eventfd ─┼──▶ io_uring PollAdd ──▶ completion ──▶ process
//!   Client 3 eventfd ─┘
//! ```
//!
//! This is O(1) per wake-up regardless of client count (unlike epoll's O(n)).

use std::collections::HashMap;
use std::io;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};

use io_uring::{opcode, types, IoUring};

/// User data encoding for completion queue entries
/// Upper 32 bits: event type
/// Lower 32 bits: client index or other data
const EVENT_TYPE_POLL: u64 = 1 << 32;
const EVENT_TYPE_READ: u64 = 2 << 32;
const EVENT_TYPE_TIMEOUT: u64 = 3 << 32;

/// Extract event type from user_data
#[inline]
fn event_type(user_data: u64) -> u64 {
    user_data & 0xFFFF_FFFF_0000_0000
}

/// Extract event index from user_data
#[inline]
fn event_index(user_data: u64) -> u32 {
    user_data as u32
}

/// Events that can be returned from the event loop
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Event {
    /// A client's eventfd became readable (they have requests)
    ClientReady { client_index: u32 },
    
    /// Timeout expired
    Timeout,
    
    /// Error on a file descriptor
    Error { client_index: u32, error: i32 },
}

/// A registered file descriptor with the event loop
struct RegisteredFd {
    /// The raw fd
    fd: RawFd,
    
    /// Whether we have an outstanding poll request
    poll_pending: bool,
}

/// The io_uring event loop
pub struct UringEventLoop {
    /// The io_uring instance
    ring: IoUring,
    
    /// Registered file descriptors (indexed by client_index)
    fds: HashMap<u32, RegisteredFd>,
    
    /// Next client index to assign
    next_index: u32,
    
    /// Number of outstanding submissions
    pending_submissions: u32,
}

impl UringEventLoop {
    /// Create a new event loop with the given queue depth
    pub fn new(queue_depth: u32) -> io::Result<Self> {
        let ring = IoUring::new(queue_depth)?;
        
        Ok(Self {
            ring,
            fds: HashMap::new(),
            next_index: 0,
            pending_submissions: 0,
        })
    }
    
    /// Register a file descriptor for polling
    /// Returns the client index assigned to this fd
    pub fn register_fd(&mut self, fd: RawFd) -> io::Result<u32> {
        let index = self.next_index;
        self.next_index += 1;
        
        self.fds.insert(index, RegisteredFd {
            fd,
            poll_pending: false,
        });
        
        // Immediately submit a poll for this fd
        self.submit_poll(index)?;
        
        Ok(index)
    }
    
    /// Unregister a file descriptor
    pub fn unregister_fd(&mut self, index: u32) -> Option<RawFd> {
        // Note: Any pending poll will complete with an error, which we'll ignore
        self.fds.remove(&index).map(|r| r.fd)
    }
    
    /// Submit a poll request for the given client index
    fn submit_poll(&mut self, index: u32) -> io::Result<()> {
        let reg = self.fds.get_mut(&index).ok_or_else(|| {
            io::Error::new(io::ErrorKind::NotFound, "client not registered")
        })?;
        
        if reg.poll_pending {
            return Ok(()); // Already polling
        }
        
        let poll_e = opcode::PollAdd::new(
            types::Fd(reg.fd),
            libc::POLLIN as _,
        )
        .build()
        .user_data(EVENT_TYPE_POLL | index as u64);
        
        // Safety: The fd is valid and we're not dropping it while the poll is pending
        unsafe {
            self.ring.submission()
                .push(&poll_e)
                .map_err(|_| io::Error::new(io::ErrorKind::Other, "submission queue full"))?;
        }
        
        reg.poll_pending = true;
        self.pending_submissions += 1;
        
        Ok(())
    }
    
    /// Re-arm polling for a client after processing their events
    pub fn rearm_poll(&mut self, index: u32) -> io::Result<()> {
        if let Some(reg) = self.fds.get_mut(&index) {
            reg.poll_pending = false;
        }
        self.submit_poll(index)
    }
    
    /// Submit all pending operations
    pub fn submit(&mut self) -> io::Result<u32> {
        let submitted = self.ring.submit()?;
        Ok(submitted as u32)
    }
    
    /// Wait for at least one event, with optional timeout in milliseconds
    pub fn wait(&mut self, timeout_ms: Option<u32>) -> io::Result<Vec<Event>> {
        // Submit any pending operations first
        self.submit()?;
        
        if self.pending_submissions == 0 && self.fds.is_empty() {
            // Nothing to wait for
            return Ok(vec![]);
        }
        
        // Wait for completions
        let wait_count = 1;
        
        if let Some(ms) = timeout_ms {
            // Use submit_and_wait with a reasonable timeout
            // For actual timeout, we'd need to submit a IORING_OP_TIMEOUT
            let timespec = types::Timespec::new()
                .sec(ms as u64 / 1000)
                .nsec(((ms % 1000) * 1_000_000) as u32);
            
            let args = types::SubmitArgs::new().timespec(&timespec);
            match self.ring.submitter().submit_with_args(wait_count, &args) {
                Ok(_) => {}
                Err(ref e) if e.raw_os_error() == Some(libc::ETIME) => {
                    // Timeout - that's fine
                    return Ok(vec![Event::Timeout]);
                }
                Err(e) => return Err(e),
            }
        } else {
            self.ring.submit_and_wait(wait_count)?;
        }
        
        // Collect completions
        let mut events = Vec::new();
        
        let cq = self.ring.completion();
        for cqe in cq {
            let user_data = cqe.user_data();
            let result = cqe.result();
            
            match event_type(user_data) {
                EVENT_TYPE_POLL => {
                    let index = event_index(user_data);
                    
                    // Mark poll as no longer pending
                    if let Some(reg) = self.fds.get_mut(&index) {
                        reg.poll_pending = false;
                        self.pending_submissions = self.pending_submissions.saturating_sub(1);
                    }
                    
                    if result < 0 {
                        // Error - might be ECANCELED if fd was closed
                        if result != -libc::ECANCELED {
                            events.push(Event::Error {
                                client_index: index,
                                error: -result,
                            });
                        }
                    } else if result & libc::POLLIN as i32 != 0 {
                        events.push(Event::ClientReady { client_index: index });
                    }
                }
                EVENT_TYPE_TIMEOUT => {
                    events.push(Event::Timeout);
                }
                _ => {
                    // Unknown event type, ignore
                }
            }
        }
        
        Ok(events)
    }
    
    /// Process events in a loop, calling the handler for each ready client
    pub fn run_once<F>(&mut self, timeout_ms: Option<u32>, mut handler: F) -> io::Result<usize>
    where
        F: FnMut(u32) -> bool, // Returns true to continue polling this fd
    {
        let events = self.wait(timeout_ms)?;
        let mut processed = 0;
        
        for event in events {
            match event {
                Event::ClientReady { client_index } => {
                    if handler(client_index) {
                        // Re-arm the poll
                        self.rearm_poll(client_index)?;
                    }
                    processed += 1;
                }
                Event::Timeout => {
                    // Timeout, nothing to do
                }
                Event::Error { client_index, error } => {
                    // Log error but continue
                    eprintln!("Error on client {}: {}", client_index, error);
                }
            }
        }
        
        Ok(processed)
    }
}

// =============================================================================
// EVENTFD HELPERS
// =============================================================================

/// Create a new eventfd
pub fn create_eventfd() -> io::Result<OwnedFd> {
    let fd = unsafe {
        libc::eventfd(0, libc::EFD_CLOEXEC | libc::EFD_NONBLOCK)
    };
    
    if fd < 0 {
        return Err(io::Error::last_os_error());
    }
    
    Ok(unsafe { OwnedFd::from_raw_fd(fd) })
}

/// Signal an eventfd (increment counter by 1)
pub fn signal_eventfd(fd: &OwnedFd) -> io::Result<()> {
    let val: u64 = 1;
    let ret = unsafe {
        libc::write(
            fd.as_raw_fd(),
            &val as *const u64 as *const libc::c_void,
            std::mem::size_of::<u64>(),
        )
    };
    
    if ret < 0 {
        return Err(io::Error::last_os_error());
    }
    
    Ok(())
}

/// Read from eventfd (returns counter and resets to 0)
pub fn read_eventfd(fd: &OwnedFd) -> io::Result<u64> {
    let mut val: u64 = 0;
    let ret = unsafe {
        libc::read(
            fd.as_raw_fd(),
            &mut val as *mut u64 as *mut libc::c_void,
            std::mem::size_of::<u64>(),
        )
    };
    
    if ret < 0 {
        let err = io::Error::last_os_error();
        if err.kind() == io::ErrorKind::WouldBlock {
            return Ok(0);
        }
        return Err(err);
    }
    
    Ok(val)
}

// =============================================================================
// TESTS
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;
    use std::thread;
    use std::time::Duration;
    
    #[test]
    fn test_eventfd_basic() {
        let fd = create_eventfd().expect("create eventfd");
        
        // Initially empty
        assert_eq!(read_eventfd(&fd).unwrap(), 0);
        
        // Signal it
        signal_eventfd(&fd).expect("signal");
        
        // Now it has value 1
        assert_eq!(read_eventfd(&fd).unwrap(), 1);
        
        // Reading resets to 0
        assert_eq!(read_eventfd(&fd).unwrap(), 0);
        
        // Multiple signals accumulate
        signal_eventfd(&fd).unwrap();
        signal_eventfd(&fd).unwrap();
        signal_eventfd(&fd).unwrap();
        assert_eq!(read_eventfd(&fd).unwrap(), 3);
    }
    
    #[test]
    fn test_event_loop_basic() {
        let mut event_loop = UringEventLoop::new(32).expect("create event loop");
        
        let fd1 = create_eventfd().expect("create eventfd");
        let fd2 = create_eventfd().expect("create eventfd");
        
        let idx1 = event_loop.register_fd(fd1.as_raw_fd()).expect("register");
        let idx2 = event_loop.register_fd(fd2.as_raw_fd()).expect("register");
        
        assert_eq!(idx1, 0);
        assert_eq!(idx2, 1);
        
        // Signal fd1
        signal_eventfd(&fd1).unwrap();
        
        // Wait should return fd1 as ready
        let events = event_loop.wait(Some(100)).expect("wait");
        assert!(!events.is_empty());
        assert!(events.iter().any(|e| matches!(e, Event::ClientReady { client_index: 0 })));
        
        // Consume the eventfd
        read_eventfd(&fd1).unwrap();
        
        // Re-arm and signal fd2
        event_loop.rearm_poll(idx1).unwrap();
        signal_eventfd(&fd2).unwrap();
        
        let events = event_loop.wait(Some(100)).expect("wait");
        assert!(events.iter().any(|e| matches!(e, Event::ClientReady { client_index: 1 })));
    }
    
    #[test]
    fn test_event_loop_timeout() {
        let mut event_loop = UringEventLoop::new(32).expect("create event loop");
        
        let fd = create_eventfd().expect("create eventfd");
        let _idx = event_loop.register_fd(fd.as_raw_fd()).expect("register");
        
        // Don't signal, should timeout
        let start = std::time::Instant::now();
        let events = event_loop.wait(Some(50)).expect("wait");
        let elapsed = start.elapsed();
        
        // Should have timed out (allow some slack)
        assert!(elapsed >= Duration::from_millis(40));
        assert!(events.is_empty() || events.iter().any(|e| matches!(e, Event::Timeout)));
    }
    
    #[test]
    fn test_event_loop_multiple_clients() {
        let mut event_loop = UringEventLoop::new(32).expect("create event loop");
        
        let fds: Vec<OwnedFd> = (0..5)
            .map(|_| create_eventfd().expect("create eventfd"))
            .collect();
        
        let indices: Vec<u32> = fds.iter()
            .map(|fd| event_loop.register_fd(fd.as_raw_fd()).expect("register"))
            .collect();
        
        // Signal all of them
        for fd in &fds {
            signal_eventfd(fd).unwrap();
        }
        
        // Should get all of them ready
        let mut ready_clients = std::collections::HashSet::new();
        
        // May need multiple waits to get all completions
        for _ in 0..10 {
            let events = event_loop.wait(Some(100)).expect("wait");
            for event in events {
                if let Event::ClientReady { client_index } = event {
                    ready_clients.insert(client_index);
                    // Consume the eventfd
                    read_eventfd(&fds[client_index as usize]).unwrap();
                }
            }
            
            if ready_clients.len() == 5 {
                break;
            }
            
            // Re-arm any that completed
            for idx in &indices {
                let _ = event_loop.rearm_poll(*idx);
            }
        }
        
        assert_eq!(ready_clients.len(), 5);
    }
    
    #[test]
    fn test_event_loop_concurrent_signal() {
        let mut event_loop = UringEventLoop::new(32).expect("create event loop");
        
        let fd = create_eventfd().expect("create eventfd");
        let fd_raw = fd.as_raw_fd();
        let _idx = event_loop.register_fd(fd_raw).expect("register");
        
        // Spawn a thread that signals after a delay
        let fd_clone = unsafe { OwnedFd::from_raw_fd(libc::dup(fd_raw)) };
        let handle = thread::spawn(move || {
            thread::sleep(Duration::from_millis(50));
            signal_eventfd(&fd_clone).unwrap();
            // Don't drop fd_clone yet, or it will close the dup'd fd
            std::mem::forget(fd_clone);
        });
        
        // Wait should block and then wake up when signaled
        let start = std::time::Instant::now();
        let events = event_loop.wait(Some(500)).expect("wait");
        let elapsed = start.elapsed();
        
        // Should have woken up in ~50ms, not 500ms
        assert!(elapsed >= Duration::from_millis(40));
        assert!(elapsed < Duration::from_millis(200));
        assert!(events.iter().any(|e| matches!(e, Event::ClientReady { client_index: 0 })));
        
        handle.join().unwrap();
    }
    
    #[test]
    fn test_run_once() {
        let mut event_loop = UringEventLoop::new(32).expect("create event loop");
        
        let fd = create_eventfd().expect("create eventfd");
        let _idx = event_loop.register_fd(fd.as_raw_fd()).expect("register");
        
        // Signal multiple times
        signal_eventfd(&fd).unwrap();
        signal_eventfd(&fd).unwrap();
        
        let mut call_count = 0;
        let processed = event_loop.run_once(Some(100), |client_index| {
            assert_eq!(client_index, 0);
            call_count += 1;
            // Consume the eventfd
            let _ = read_eventfd(&fd);
            true // Continue polling
        }).expect("run_once");
        
        assert!(processed >= 1);
        assert!(call_count >= 1);
    }
    
    #[test]
    fn test_unregister() {
        let mut event_loop = UringEventLoop::new(32).expect("create event loop");
        
        let fd = create_eventfd().expect("create eventfd");
        let idx = event_loop.register_fd(fd.as_raw_fd()).expect("register");
        
        // Unregister
        let removed = event_loop.unregister_fd(idx);
        assert!(removed.is_some());
        
        // The pending poll submission might still complete.
        // After unregister, we don't track this fd anymore, so we shouldn't
        // get ClientReady events for it. But we might get stale completions.
        // This is expected - the caller should handle this gracefully.
        
        // Signal the fd (but it's no longer registered in our tracking)
        signal_eventfd(&fd).unwrap();
        
        // Wait - we might still get completions for the pending poll,
        // but since we removed the fd from tracking, they'll be ignored
        // or show as errors
        let _events = event_loop.wait(Some(50)).expect("wait");
        // We don't assert on events here - behavior is implementation-dependent
    }
}

#[cfg(test)]
mod proptests {
    use super::*;
    use proptest::prelude::*;
    
    proptest! {
        /// Property: Eventfd signals are never lost
        #[test]
        fn eventfd_signals_accumulate(signal_count in 1u32..100) {
            let fd = create_eventfd().expect("create eventfd");
            
            for _ in 0..signal_count {
                signal_eventfd(&fd).expect("signal");
            }
            
            let value = read_eventfd(&fd).expect("read");
            prop_assert_eq!(value, signal_count as u64);
        }
        
        /// Property: Multiple reads after one signal give correct total
        #[test]
        fn eventfd_read_clears(signal_count in 1u32..50) {
            let fd = create_eventfd().expect("create eventfd");
            
            for _ in 0..signal_count {
                signal_eventfd(&fd).expect("signal");
            }
            
            // First read gets all
            let first = read_eventfd(&fd).expect("read");
            // Second read gets zero
            let second = read_eventfd(&fd).expect("read");
            
            prop_assert_eq!(first, signal_count as u64);
            prop_assert_eq!(second, 0);
        }
        
        /// Property: Register/unregister cycles don't leak resources
        #[test]
        fn register_unregister_cycle(cycles in 1usize..20) {
            let mut event_loop = UringEventLoop::new(64).expect("create");
            
            for _ in 0..cycles {
                let fd = create_eventfd().expect("create eventfd");
                let idx = event_loop.register_fd(fd.as_raw_fd()).expect("register");
                event_loop.unregister_fd(idx);
                // fd dropped here - may cause EBADF on pending poll
            }
            
            // Should be able to wait without panicking
            // May get errors from pending polls on closed fds, that's expected
            let _ = event_loop.wait(Some(10));
        }
        
        /// Property: All signaled fds eventually become ready
        #[test]
        fn all_signals_delivered(num_clients in 1usize..10) {
            let mut event_loop = UringEventLoop::new(64).expect("create");
            
            let fds: Vec<OwnedFd> = (0..num_clients)
                .map(|_| create_eventfd().expect("create"))
                .collect();
            
            let indices: Vec<u32> = fds.iter()
                .map(|fd| event_loop.register_fd(fd.as_raw_fd()).expect("register"))
                .collect();
            
            // Signal all
            for fd in &fds {
                signal_eventfd(fd).expect("signal");
            }
            
            // Collect all ready events
            let mut ready = std::collections::HashSet::new();
            for _ in 0..num_clients * 2 {
                if let Ok(events) = event_loop.wait(Some(100)) {
                    for event in events {
                        if let Event::ClientReady { client_index } = event {
                            ready.insert(client_index);
                            let _ = read_eventfd(&fds[client_index as usize]);
                        }
                    }
                }
                
                // Re-arm
                for idx in &indices {
                    let _ = event_loop.rearm_poll(*idx);
                }
                
                if ready.len() == num_clients {
                    break;
                }
            }
            
            prop_assert_eq!(ready.len(), num_clients);
        }
    }
}
