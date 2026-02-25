// echo_server.cpp - Simple TCP echo server using evring
//
// This example demonstrates:
// - Server-side socket operations (bind, listen, accept)
// - Handling multiple clients (simplified - one at a time)
// - Send/recv patterns
//
// Usage: echo_server [port]
//        # In another terminal: nc localhost 8080

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "straylight/evring/evring.h"

// ============================================================================
// Echo client state machine
// ============================================================================

struct echo_client_state {
  enum class phase { receiving, sending, done, error };

  phase current_phase{phase::receiving};
  evring::handle client_handle;
  std::size_t bytes_received{0};
  std::size_t bytes_sent{0};
};

struct echo_client_machine {
  using state_type = echo_client_state;

  explicit echo_client_machine(evring::handle client, std::size_t buffer_size = 4096)
      : client_(client), buffer_(buffer_size) {}

  [[nodiscard]] auto initial() const -> state_type {
    state_type s;
    s.client_handle = client_;
    return s;
  }

  // Provide stable span for recv buffer (buffer is in machine, not state)
  [[nodiscard]] auto recv_buffer_span() const -> evring::stable_span<std::byte> {
    return evring::make_stable_span(std::span{buffer_.data(), buffer_.size()});
  }

  [[nodiscard]] auto step(state_type s, const evring::event& e) const
      -> evring::step_result<state_type> {
    std::vector<evring::operation> ops;

    switch (s.current_phase) {
      case state_type::phase::receiving:
        if (e.result <= 0) {
          // Client disconnected or error
          s.current_phase = state_type::phase::done;
          ops.push_back(evring::operation::make_close(s.client_handle));
        } else {
          // Got data - echo it back
          s.bytes_received = static_cast<std::size_t>(e.result);
          s.current_phase = state_type::phase::sending;
          ops.push_back(evring::operation::make_send(
              s.client_handle, std::span{buffer_.data(), s.bytes_received}, 0));
        }
        break;

      case state_type::phase::sending:
        if (!e.ok()) {
          s.current_phase = state_type::phase::error;
          ops.push_back(evring::operation::make_close(s.client_handle));
        } else {
          s.bytes_sent += static_cast<std::size_t>(e.result);
          // Wait for more data
          s.current_phase = state_type::phase::receiving;
          ops.push_back(evring::operation::make_recv(s.client_handle, recv_buffer_span(), 0));
        }
        break;

      case state_type::phase::done:
      case state_type::phase::error:
        break;
    }

    return {std::move(s), std::move(ops)};
  }

  [[nodiscard]] auto done(const state_type& s) const -> bool {
    return s.current_phase == state_type::phase::done ||
           s.current_phase == state_type::phase::error;
  }

  evring::handle client_;
  mutable std::vector<std::byte> buffer_;
};

// ============================================================================
// Server loop (accept and handle clients)
// ============================================================================

void run_server(evring::ring& ring, std::uint16_t port, int max_clients) {
  // Create listening socket (using POSIX API for setup)
  int listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd < 0) {
    std::perror("socket");
    return;
  }

  int opt = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

  if (::bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::perror("bind");
    ::close(listen_fd);
    return;
  }

  if (::listen(listen_fd, 16) < 0) {
    std::perror("listen");
    ::close(listen_fd);
    return;
  }

  std::printf("Echo server listening on port %u\n", port);
  std::printf("Connect with: nc localhost %u\n", port);
  std::printf("Press Ctrl+C to stop\n\n");

  // Register with io_uring
  auto listen_handle = ring.register_file_descriptor(listen_fd, evring::resource_type::socket);

  int clients_served = 0;
  while (max_clients == 0 || clients_served < max_clients) {
    // Accept next client
    struct sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);

    ring.enqueue(evring::operation::make_accept(
        listen_handle, &client_addr, reinterpret_cast<std::uint32_t*>(&client_len), SOCK_CLOEXEC));
    auto events = ring.submit_and_wait(1);

    if (!events[0].ok()) {
      std::fprintf(stderr, "Accept failed\n");
      continue;
    }

    evring::handle client_handle = events[0].resource_handle;

    char addr_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));
    std::printf("Client connected from %s:%u\n", addr_str, ntohs(client_addr.sin_port));

    // Start receiving from client - create machine first so it has stable buffer
    echo_client_machine client_machine{client_handle};
    ring.enqueue(evring::operation::make_recv(client_handle, client_machine.recv_buffer_span(), 0));
    events = ring.submit_and_wait(1);

    // Process initial recv with the client machine
    auto state = client_machine.initial();

    // Process initial recv
    auto [new_state, ops] = client_machine.step(state, events[0]);
    state = std::move(new_state);

    // Run rest of client handling
    while (!client_machine.done(state) && !ops.empty()) {
      for (const auto& op : ops) {
        ring.enqueue(op);
      }
      events = ring.submit_and_wait(1);
      auto result = client_machine.step(state, events[0]);
      state = std::move(result.state);
      ops = std::move(result.operations);
    }

    std::printf("Client disconnected (sent %zu bytes total)\n\n", state.bytes_sent);
    ++clients_served;
  }

  ::close(listen_fd);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
  std::uint16_t port = 8080;
  int max_clients = 0; // 0 = unlimited

  if (argc > 1) {
    port = static_cast<std::uint16_t>(std::atoi(argv[1]));
  }
  if (argc > 2) {
    max_clients = std::atoi(argv[2]);
  }

  auto ring = evring::make_io_uring_ring(64);
  if (!ring) {
    std::fprintf(stderr, "Failed to create io_uring\n");
    return 1;
  }

  run_server(*ring, port, max_clients);

  return 0;
}
