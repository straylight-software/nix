//! GPU Broker Server
//!
//! This module ties together all the components into a running server:
//! - Unix socket for accepting client connections
//! - io_uring for efficient async I/O
//! - Shared memory rings for zero-copy message passing
//! - BrokerProxy for handle translation and driver calls
//!
//! Architecture:
//! ```text
//!  ┌─────────────────────────────────────────────────────────────────┐
//!  │                        BrokerServer                             │
//!  │                                                                 │
//!  │  ┌─────────────────┐    ┌─────────────────┐                    │
//!  │  │  Unix Socket    │    │  io_uring       │                    │
//!  │  │  Listener       │───▶│  Event Loop     │                    │
//!  │  └─────────────────┘    └────────┬────────┘                    │
//!  │                                  │                              │
//!  │  ┌───────────────────────────────▼────────────────────────────┐│
//!  │  │                    Client Manager                          ││
//!  │  │  ┌──────────┐ ┌──────────┐ ┌──────────┐                   ││
//!  │  │  │ Client 1 │ │ Client 2 │ │ Client N │  (SharedMem+evfd) ││
//!  │  │  └──────────┘ └──────────┘ └──────────┘                   ││
//!  │  └────────────────────────────┬───────────────────────────────┘│
//!  │                               │                                 │
//!  │  ┌────────────────────────────▼───────────────────────────────┐│
//!  │  │                     BrokerProxy                            ││
//!  │  │  HandleTable + Driver → translate + forward ioctls         ││
//!  │  └────────────────────────────┬───────────────────────────────┘│
//!  │                               │                                 │
//!  │  ┌────────────────────────────▼───────────────────────────────┐│
//!  │  │                      NvDriver                              ││
//!  │  │  /dev/nvidiactl + /dev/nvidia0                             ││
//!  │  └────────────────────────────────────────────────────────────┘│
//!  └─────────────────────────────────────────────────────────────────┘
//! ```

use std::collections::HashMap;
use std::io;
use std::os::fd::AsRawFd;
use std::os::unix::net::{UnixListener, UnixStream};

use crate::broker::{Operation, Request, SeqNo};
use crate::driver::{Driver, MockDriver, NvDriver};
use crate::handle_table::ClientId;
use crate::proxy::{BrokerProxy, ProxyConfig};
use crate::uring::UringEventLoop;
use crate::uring_transport::{ClientConnection, ClientConnectionInfo, SharedMemoryLayout};

// =============================================================================
// SERVER CONFIGURATION
// =============================================================================

/// Server configuration
#[derive(Debug, Clone)]
pub struct ServerConfig {
    /// Path to Unix socket for client connections
    pub socket_path: String,

    /// Path to NVIDIA control device
    pub nvidiactl_path: String,

    /// Path to NVIDIA GPU device
    pub nvidia0_path: String,

    /// Maximum handles per client
    pub handle_quota: usize,

    /// Maximum concurrent clients
    pub max_clients: usize,

    /// Use mock driver instead of real driver
    pub use_mock_driver: bool,

    /// io_uring queue depth
    pub uring_queue_depth: u32,
}

impl Default for ServerConfig {
    fn default() -> Self {
        Self {
            socket_path: "/run/gpu-broker.sock".to_string(),
            nvidiactl_path: "/dev/nvidiactl".to_string(),
            nvidia0_path: "/dev/nvidia0".to_string(),
            handle_quota: 10_000,
            max_clients: 1_000,
            use_mock_driver: false,
            uring_queue_depth: 256,
        }
    }
}

// =============================================================================
// CLIENT STATE
// =============================================================================

/// State for a connected client
struct ConnectedClient {
    /// Client ID
    id: ClientId,

    /// Unix stream for control messages
    stream: UnixStream,

    /// Shared memory connection for data path
    connection: ClientConnection,

    /// Sequence counter
    next_seq: u64,
}

impl ConnectedClient {
    fn new(id: ClientId, stream: UnixStream) -> io::Result<Self> {
        let connection = ClientConnection::new(id)?;
        Ok(Self {
            id,
            stream,
            connection,
            next_seq: 0,
        })
    }

    fn next_seq(&mut self) -> SeqNo {
        let seq = SeqNo(self.next_seq);
        self.next_seq += 1;
        seq
    }

    /// Get connection info to send to client
    fn connection_info(&self) -> ClientConnectionInfo {
        ClientConnectionInfo {
            client_id: self.id,
            shmem_fd: self.connection.shmem.fd(),
            shmem_size: SharedMemoryLayout::default_layout().total_size,
            request_eventfd: self.connection.request_eventfd.as_raw_fd(),
            response_eventfd: self.connection.response_eventfd.as_raw_fd(),
        }
    }
}

// =============================================================================
// BROKER SERVER
// =============================================================================

/// The main broker server
pub struct BrokerServer<D: Driver> {
    /// Configuration
    config: ServerConfig,

    /// Unix socket listener
    listener: UnixListener,

    /// io_uring event loop
    event_loop: UringEventLoop,

    /// Connected clients
    clients: HashMap<ClientId, ConnectedClient>,

    /// Map from uring index to client ID
    uring_index_to_client: HashMap<u32, ClientId>,

    /// Broker proxy (handle translation + driver)
    proxy: BrokerProxy<D>,

    /// Next client ID
    next_client_id: u64,

    /// Server statistics
    stats: ServerStats,

    /// Shutdown flag
    shutdown: bool,
}

/// Server statistics
#[derive(Debug, Default, Clone)]
pub struct ServerStats {
    pub clients_connected: u64,
    pub clients_disconnected: u64,
    pub requests_processed: u64,
    pub errors: u64,
    pub uptime_secs: u64,
}

impl BrokerServer<NvDriver> {
    /// Create a new server with the real NVIDIA driver
    pub fn new(config: ServerConfig) -> io::Result<Self> {
        // Open the real driver
        let driver = NvDriver::open_path(&config.nvidiactl_path, Some(&config.nvidia0_path))?;
        Self::with_driver(config, driver)
    }
}

impl BrokerServer<MockDriver> {
    /// Create a new server with a mock driver (for testing)
    pub fn with_mock_driver(config: ServerConfig) -> io::Result<Self> {
        let driver = MockDriver::new();
        Self::with_driver(config, driver)
    }
}

impl<D: Driver> BrokerServer<D> {
    /// Create a server with a specific driver
    pub fn with_driver(config: ServerConfig, driver: D) -> io::Result<Self> {
        // Remove old socket if it exists
        let _ = std::fs::remove_file(&config.socket_path);

        // Create Unix socket listener
        let listener = UnixListener::bind(&config.socket_path)?;
        listener.set_nonblocking(true)?;

        // Create io_uring event loop
        let event_loop = UringEventLoop::new(config.uring_queue_depth)?;

        // Create proxy
        let proxy_config = ProxyConfig {
            handle_quota: config.handle_quota,
            max_clients: config.max_clients,
            strict_validation: true,
        };
        let proxy = BrokerProxy::new(driver, proxy_config);

        Ok(Self {
            config,
            listener,
            event_loop,
            clients: HashMap::new(),
            uring_index_to_client: HashMap::new(),
            proxy,
            next_client_id: 1,
            stats: ServerStats::default(),
            shutdown: false,
        })
    }

    /// Run the server main loop
    pub fn run(&mut self) -> io::Result<()> {
        tracing::info!("GPU Broker server starting on {}", self.config.socket_path);

        // Register listener fd with event loop
        let listener_fd = self.listener.as_raw_fd();

        while !self.shutdown {
            // Accept new connections (non-blocking)
            self.accept_connections()?;

            // Poll all client eventfds for requests
            self.poll_clients()?;

            // Small sleep to avoid busy-waiting when idle
            // In production, we'd use io_uring properly here
            std::thread::sleep(std::time::Duration::from_millis(1));
        }

        tracing::info!("GPU Broker server shutting down");
        Ok(())
    }

    /// Run one iteration of the server loop (for testing)
    pub fn run_once(&mut self) -> io::Result<usize> {
        self.accept_connections()?;
        self.poll_clients()
    }

    /// Accept new client connections
    fn accept_connections(&mut self) -> io::Result<()> {
        loop {
            match self.listener.accept() {
                Ok((stream, _addr)) => {
                    if let Err(e) = self.handle_new_client(stream) {
                        tracing::warn!("Failed to accept client: {}", e);
                        self.stats.errors += 1;
                    }
                }
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                    break; // No more pending connections
                }
                Err(e) => {
                    tracing::error!("Accept error: {}", e);
                    self.stats.errors += 1;
                }
            }
        }
        Ok(())
    }

    /// Handle a new client connection
    fn handle_new_client(&mut self, stream: UnixStream) -> io::Result<()> {
        // Assign client ID
        let client_id = ClientId(self.next_client_id);
        self.next_client_id += 1;

        tracing::info!("New client connected: {:?}", client_id);

        // Create client state
        let client = ConnectedClient::new(client_id, stream)?;

        // Register with proxy
        let register_req = Request {
            client: client_id,
            seq: SeqNo(0),
            op: Operation::RegisterClient,
        };
        let response = self.proxy.process(register_req);

        if response.result.is_err() {
            tracing::error!("Failed to register client {:?}: {:?}", client_id, response.result);
            return Err(io::Error::new(io::ErrorKind::Other, "Failed to register client"));
        }

        // Send connection info to client
        let info = client.connection_info();
        self.send_connection_info(&client.stream, &info)?;

        // Register client's request eventfd with event loop
        let eventfd = client.connection.request_eventfd.as_raw_fd();
        let uring_index = self.event_loop.register_fd(eventfd)?;
        self.uring_index_to_client.insert(uring_index, client_id);

        // Store client
        self.clients.insert(client_id, client);
        self.stats.clients_connected += 1;

        Ok(())
    }

    /// Send connection info to client over Unix socket with fds via SCM_RIGHTS
    fn send_connection_info(
        &self,
        stream: &UnixStream,
        info: &ClientConnectionInfo,
    ) -> io::Result<()> {
        use crate::fd_passing::send_connection;

        // Get the fds to pass
        let shmem_fd = info.shmem_fd.ok_or_else(|| {
            io::Error::new(io::ErrorKind::InvalidData, "No shmem_fd")
        })?;

        send_connection(
            stream,
            info.client_id.0,
            info.shmem_size as u64,
            shmem_fd,
            info.request_eventfd,
            info.response_eventfd,
        )
    }

    /// Poll all clients for pending requests
    fn poll_clients(&mut self) -> io::Result<usize> {
        let mut processed = 0;

        // Collect client IDs to avoid borrow issues
        let client_ids: Vec<ClientId> = self.clients.keys().copied().collect();

        for client_id in client_ids {
            // Process all pending requests from this client
            loop {
                let request = {
                    let client = match self.clients.get_mut(&client_id) {
                        Some(c) => c,
                        None => break,
                    };

                    match client.connection.try_recv_request() {
                        Some(mut req) => {
                            // Fill in client ID (client might not know their ID)
                            req.client = client_id;
                            req
                        }
                        None => break,
                    }
                };

                // Process through proxy
                let response = self.proxy.process(request);
                processed += 1;
                self.stats.requests_processed += 1;

                // Send response back
                let client = self.clients.get_mut(&client_id).unwrap();
                if !client.connection.try_send_response(&response) {
                    tracing::warn!("Response ring full for client {:?}", client_id);
                    self.stats.errors += 1;
                }
            }
        }

        Ok(processed)
    }

    /// Disconnect a client
    pub fn disconnect_client(&mut self, client_id: ClientId) -> io::Result<()> {
        if let Some(_client) = self.clients.remove(&client_id) {
            tracing::info!("Disconnecting client {:?}", client_id);

            // Find and remove the uring index mapping
            let uring_index = self.uring_index_to_client
                .iter()
                .find(|(_, &cid)| cid == client_id)
                .map(|(&idx, _)| idx);
            
            if let Some(idx) = uring_index {
                self.event_loop.unregister_fd(idx);
                self.uring_index_to_client.remove(&idx);
            }

            // Unregister from proxy (frees all handles)
            let unreg_req = Request {
                client: client_id,
                seq: SeqNo(0),
                op: Operation::UnregisterClient,
            };
            let _ = self.proxy.process(unreg_req);

            self.stats.clients_disconnected += 1;
        }
        Ok(())
    }

    /// Request server shutdown
    pub fn shutdown(&mut self) {
        self.shutdown = true;
    }

    /// Get server statistics
    pub fn stats(&self) -> &ServerStats {
        &self.stats
    }

    /// Get number of connected clients
    pub fn client_count(&self) -> usize {
        self.clients.len()
    }

    /// Check if a client is connected
    pub fn is_client_connected(&self, client_id: ClientId) -> bool {
        self.clients.contains_key(&client_id)
    }
}

impl<D: Driver> Drop for BrokerServer<D> {
    fn drop(&mut self) {
        // Clean up socket file
        let _ = std::fs::remove_file(&self.config.socket_path);
    }
}

// =============================================================================
// TESTS
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::net::UnixStream;
    use tempfile::tempdir;

    fn test_config(socket_path: &str) -> ServerConfig {
        ServerConfig {
            socket_path: socket_path.to_string(),
            use_mock_driver: true,
            ..Default::default()
        }
    }

    #[test]
    fn test_server_creation() {
        let dir = tempdir().unwrap();
        let socket_path = dir.path().join("test.sock");
        let config = test_config(socket_path.to_str().unwrap());

        let server = BrokerServer::with_mock_driver(config);
        assert!(server.is_ok());

        let server = server.unwrap();
        assert_eq!(server.client_count(), 0);
    }

    #[test]
    fn test_client_connection() {
        let dir = tempdir().unwrap();
        let socket_path = dir.path().join("test.sock");
        let config = test_config(socket_path.to_str().unwrap());

        let mut server = BrokerServer::with_mock_driver(config).unwrap();

        // Connect a client
        let client_stream = UnixStream::connect(&socket_path).unwrap();

        // Run one iteration to accept
        server.run_once().unwrap();

        assert_eq!(server.client_count(), 1);
        assert_eq!(server.stats().clients_connected, 1);
    }

    #[test]
    fn test_multiple_clients() {
        let dir = tempdir().unwrap();
        let socket_path = dir.path().join("test.sock");
        let config = test_config(socket_path.to_str().unwrap());

        let mut server = BrokerServer::with_mock_driver(config).unwrap();

        // Connect multiple clients
        let _c1 = UnixStream::connect(&socket_path).unwrap();
        let _c2 = UnixStream::connect(&socket_path).unwrap();
        let _c3 = UnixStream::connect(&socket_path).unwrap();

        // Accept all
        server.run_once().unwrap();

        assert_eq!(server.client_count(), 3);
    }

    #[test]
    fn test_client_disconnect() {
        let dir = tempdir().unwrap();
        let socket_path = dir.path().join("test.sock");
        let config = test_config(socket_path.to_str().unwrap());

        let mut server = BrokerServer::with_mock_driver(config).unwrap();

        // Connect a client
        let _client = UnixStream::connect(&socket_path).unwrap();
        server.run_once().unwrap();

        assert_eq!(server.client_count(), 1);

        // Disconnect
        server.disconnect_client(ClientId(1)).unwrap();

        assert_eq!(server.client_count(), 0);
        assert_eq!(server.stats().clients_disconnected, 1);
    }

    #[test]
    fn test_socket_cleanup() {
        let dir = tempdir().unwrap();
        let socket_path = dir.path().join("test.sock");
        let config = test_config(socket_path.to_str().unwrap());

        {
            let _server = BrokerServer::with_mock_driver(config.clone()).unwrap();
            assert!(socket_path.exists());
        }

        // Socket should be cleaned up after drop
        assert!(!socket_path.exists());
    }
}

#[cfg(test)]
mod proptests {
    use super::*;
    use proptest::prelude::*;
    use tempfile::tempdir;

    proptest! {
        /// Property: Server handles rapid connect/disconnect cycles
        #[test]
        fn rapid_connect_disconnect(iterations in 1usize..20) {
            let dir = tempdir().unwrap();
            let socket_path = dir.path().join("test.sock");
            let config = ServerConfig {
                socket_path: socket_path.to_str().unwrap().to_string(),
                use_mock_driver: true,
                ..Default::default()
            };

            let mut server = BrokerServer::with_mock_driver(config).unwrap();

            for i in 0..iterations {
                // Connect
                let _client = UnixStream::connect(&socket_path).unwrap();
                server.run_once().unwrap();

                prop_assert_eq!(server.client_count(), i + 1);
            }

            // Disconnect all
            for i in 1..=iterations {
                server.disconnect_client(ClientId(i as u64)).unwrap();
            }

            prop_assert_eq!(server.client_count(), 0);
            prop_assert_eq!(server.stats().clients_connected as usize, iterations);
            prop_assert_eq!(server.stats().clients_disconnected as usize, iterations);
        }

        /// Property: Stats remain consistent
        #[test]
        fn stats_consistency(connects in 1usize..10, disconnects in 0usize..10) {
            let dir = tempdir().unwrap();
            let socket_path = dir.path().join("test.sock");
            let config = ServerConfig {
                socket_path: socket_path.to_str().unwrap().to_string(),
                use_mock_driver: true,
                ..Default::default()
            };

            let mut server = BrokerServer::with_mock_driver(config).unwrap();

            // Connect clients
            let mut streams = Vec::new();
            for _ in 0..connects {
                streams.push(UnixStream::connect(&socket_path).unwrap());
            }
            server.run_once().unwrap();

            // Disconnect some
            let actual_disconnects = disconnects.min(connects);
            for i in 1..=actual_disconnects {
                server.disconnect_client(ClientId(i as u64)).unwrap();
            }

            let stats = server.stats();
            prop_assert_eq!(stats.clients_connected as usize, connects);
            prop_assert_eq!(stats.clients_disconnected as usize, actual_disconnects);
            prop_assert_eq!(
                server.client_count(),
                connects - actual_disconnects
            );
        }
    }
}
