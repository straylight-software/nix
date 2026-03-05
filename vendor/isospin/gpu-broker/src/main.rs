//! GPU Broker - Multiplexes NVIDIA RM API calls from multiple VMs
//!
//! Architecture:
//! - Each VM connects via vsock (for VMs) or Unix socket (for containers)
//! - VM sends RM API calls (ioctls) over the connection
//! - Broker translates handles, validates, and forwards to real /dev/nvidiactl
//! - Results returned to client
//!
//! The GPU driver stays hot - no cold boot penalty per VM.
//!
//! Core design principle (Carmack/aerospace):
//!   Pure function: (State, Input) → (State', Output)
//!   - Deterministic replay
//!   - Time-travel debugging
//!   - The simulation IS the test

use anyhow::{Context, Result};
use clap::Parser;
use gpu_broker::nvml_server::{GpuInfo, NvmlServer};
use gpu_broker::proxy::{BrokerProxy, ProxyConfig};
use gpu_broker::server::{BrokerServer, ServerConfig};
use gpu_broker::vsock::{VsockServer, VsockServerConfig};
use std::thread;
use tracing::info;

/// GPU Broker - multiplexes GPU access for multiple VMs
#[derive(Parser, Debug)]
#[command(name = "gpu-broker")]
#[command(about = "Multiplexes NVIDIA GPU access for multiple VMs")]
struct Args {
  /// Path to NVIDIA control device
  #[arg(long, default_value = "/dev/nvidiactl")]
  nvidiactl: String,

  /// Path to NVIDIA device (GPU 0)
  #[arg(long, default_value = "/dev/nvidia0")]
  nvidia0: String,

  /// Maximum handles per client
  #[arg(long, default_value = "10000")]
  handle_quota: usize,

  /// Maximum concurrent clients
  #[arg(long, default_value = "1000")]
  max_clients: usize,

  /// Unix socket path for client connections
  #[arg(long, default_value = "/run/gpu-broker.sock")]
  socket: String,

  /// Unix socket path for NVML queries (nvidia-smi support)
  #[arg(long, default_value = "/run/gpu-broker-nvml.sock")]
  nvml_socket: String,

  /// vsock port for VM connections (0 to disable)
  #[arg(long, default_value = "9999")]
  vsock_port: u32,

  /// Use mock driver (for testing without GPU)
  #[arg(long)]
  mock: bool,

  /// Enable verbose logging
  #[arg(short, long)]
  verbose: bool,
}

fn main() -> Result<()> {
  let args = Args::parse();

  // Initialize logging
  tracing_subscriber::fmt()
    .with_env_filter(if args.verbose { "debug" } else { "info" })
    .init();

  info!("GPU Broker starting");
  info!("  nvidiactl: {}", args.nvidiactl);
  info!("  nvidia0: {}", args.nvidia0);
  info!("  handle_quota: {}", args.handle_quota);
  info!("  max_clients: {}", args.max_clients);
  info!("  socket: {}", args.socket);
  info!("  nvml_socket: {}", args.nvml_socket);
  info!("  vsock_port: {}", args.vsock_port);
  info!("  mock: {}", args.mock);

  // Build server config
  let config = ServerConfig {
    socket_path: args.socket,
    nvidiactl_path: args.nvidiactl.clone(),
    nvidia0_path: args.nvidia0.clone(),
    handle_quota: args.handle_quota,
    max_clients: args.max_clients,
    use_mock_driver: args.mock,
    uring_queue_depth: 256,
  };

  // Start NVML server in a separate thread
  let nvml_socket_path = args.nvml_socket.clone();
  let nvml_thread = thread::spawn(move || {
    let gpu_info = GpuInfo::default();
    match NvmlServer::with_gpu_info(&nvml_socket_path, gpu_info) {
      Ok(server) => {
        if let Err(e) = server.run() {
          tracing::error!("NVML server error: {}", e);
        }
      }
      Err(e) => {
        tracing::error!("Failed to start NVML server: {}", e);
      }
    }
  });

  // Start vsock server for VM connections (in a separate thread)
  let vsock_port = args.vsock_port;
  let vsock_mock = args.mock;
  let vsock_handle_quota = args.handle_quota;
  let vsock_max_clients = args.max_clients;

  let vsock_thread = if vsock_port > 0 {
    Some(thread::spawn(move || {
      let proxy_config = ProxyConfig {
        handle_quota: vsock_handle_quota,
        max_clients: vsock_max_clients,
        strict_validation: true,
      };

      // Create proxy with mock or real driver
      let vsock_config = VsockServerConfig {
        port: vsock_port,
        max_connections: 1000,
      };

      if vsock_mock {
        let proxy = BrokerProxy::with_mock_driver(proxy_config);
        let server = VsockServer::new(vsock_config, proxy);
        info!("Starting vsock server on port {} (mock driver)", vsock_port);
        if let Err(e) = server.run() {
          tracing::error!("vsock server error: {}", e);
        }
      } else {
        // For real driver, we'd need to pass driver paths
        // For now, just use mock in vsock mode
        tracing::warn!("vsock with real driver not yet implemented, using mock");
        let proxy = BrokerProxy::with_mock_driver(proxy_config);
        let server = VsockServer::new(vsock_config, proxy);
        info!("Starting vsock server on port {}", vsock_port);
        if let Err(e) = server.run() {
          tracing::error!("vsock server error: {}", e);
        }
      }
    }))
  } else {
    info!("vsock server disabled (port=0)");
    None
  };

  // Run main broker server (Unix socket + shared memory)
  let result = if args.mock {
    info!("Using mock driver (no GPU required)");
    let mut server =
      BrokerServer::with_mock_driver(config).context("Failed to create server with mock driver")?;
    server.run().context("Server error")
  } else {
    info!("Using real NVIDIA driver");
    let mut server = BrokerServer::new(config)
      .context("Failed to create server - is the NVIDIA driver loaded?")?;
    server.run().context("Server error")
  };

  // Clean up threads
  drop(nvml_thread);
  drop(vsock_thread);

  result
}

#[cfg(test)]
mod tests {
  use gpu_broker::handle_table::{ClientId, HandleTable, RealHandle, VirtualHandle};

  #[test]
  fn test_broker_handle_isolation() {
    // Test the handle logic without requiring GPU
    let mut handles = HandleTable::new(1000);

    let client1 = ClientId(1);
    let client2 = ClientId(2);

    handles.register_client(client1);
    handles.register_client(client2);

    // Both clients create a handle "0x1"
    handles
      .insert(client1, VirtualHandle(0x1), RealHandle(0x1000))
      .unwrap();
    handles
      .insert(client2, VirtualHandle(0x1), RealHandle(0x2000))
      .unwrap();

    // They map to different real handles
    assert_eq!(
      handles.translate(client1, VirtualHandle(0x1)).unwrap().0,
      0x1000
    );
    assert_eq!(
      handles.translate(client2, VirtualHandle(0x1)).unwrap().0,
      0x2000
    );

    // Client 1 can't accidentally access client 2's handle 0x2000
    // because they'd have to reference it by virtual handle,
    // and virtual 0x1 maps to their own 0x1000
  }
}
