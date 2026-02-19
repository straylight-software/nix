// test_socket.cpp
//
// Tests for socket operations (connect, accept, send, recv)

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "straylight/evring/evring.h"

namespace {

// ============================================================================
// Test: Create socket via io_uring
// ============================================================================

void test_socket_create() {
  std::printf("test_socket_create: creating TCP socket via io_uring...\n");

  auto ring = evring::make_io_uring_ring(32);

  // Create a TCP socket
  auto socket_op =
      evring::operation::make_socket(AF_INET, SOCK_STREAM, 0, SOCK_NONBLOCK | SOCK_CLOEXEC);
  ring->enqueue(socket_op);

  auto events = ring->submit_and_wait(1);
  assert(events.size() == 1);
  assert(events[0].ok());
  assert(events[0].operation == evring::operation_type::socket);

  // The result should be a valid fd
  int socket_fd = ring->get_file_descriptor(events[0].resource_handle);
  assert(socket_fd >= 0);

  std::printf("test_socket_create: created socket fd=%d\n", socket_fd);

  // Close the socket
  ring->enqueue(evring::operation::make_close(events[0].resource_handle));
  events = ring->submit_and_wait(1);
  assert(events.size() == 1);

  std::printf("test_socket_create: PASSED\n\n");
}

// ============================================================================
// Test: Loopback connect/accept/send/recv
// ============================================================================

void test_loopback_echo() {
  std::printf("test_loopback_echo: testing connect/accept/send/recv...\n");

  // Create a server socket using POSIX (blocking for simplicity)
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(server_fd >= 0);

  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  server_addr.sin_port = 0; // Let OS pick port

  int result =
      bind(server_fd, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr));
  assert(result == 0);

  socklen_t addr_len = sizeof(server_addr);
  getsockname(server_fd, reinterpret_cast<struct sockaddr*>(&server_addr), &addr_len);
  int port = ntohs(server_addr.sin_port);
  std::printf("test_loopback_echo: server listening on port %d\n", port);

  result = listen(server_fd, 5);
  assert(result == 0);

  // Register the server fd so we can use accept via io_uring
  auto ring = evring::make_io_uring_ring(32);
  evring::handle server_handle =
      ring->register_file_descriptor(server_fd, evring::resource_type::socket);

  // Create client socket (blocking)
  ring->enqueue(evring::operation::make_socket(AF_INET, SOCK_STREAM, 0, 0));
  auto events = ring->submit_and_wait(1);
  assert(events.size() == 1);
  assert(events[0].ok());

  evring::handle client_handle = events[0].resource_handle;
  std::printf("test_loopback_echo: created client socket\n");

  // Connect to server
  struct sockaddr_in connect_addr{};
  connect_addr.sin_family = AF_INET;
  connect_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  connect_addr.sin_port = htons(static_cast<uint16_t>(port));

  ring->enqueue(
      evring::operation::make_connect(client_handle, &connect_addr, sizeof(connect_addr)));

  // Accept the connection
  struct sockaddr_in client_addr{};
  socklen_t client_addr_len = sizeof(client_addr);
  ring->enqueue(evring::operation::make_accept(server_handle, &client_addr, &client_addr_len, 0));

  // Wait for both connect and accept
  events = ring->submit_and_wait(2);
  assert(events.size() == 2);

  evring::handle accepted_handle{};
  for (auto& e : events) {
    if (e.operation == evring::operation_type::connect) {
      std::printf("test_loopback_echo: connect completed with result %ld\n", e.result);
      assert(e.result == 0);
    } else if (e.operation == evring::operation_type::accept) {
      assert(e.ok());
      accepted_handle = e.resource_handle;
      std::printf("test_loopback_echo: accepted connection\n");
    }
  }

  // Send data from client to server
  const char* message = "Hello from io_uring!";
  std::vector<std::byte> send_buf(strlen(message));
  std::memcpy(send_buf.data(), message, send_buf.size());

  ring->enqueue(
      evring::operation::make_send(client_handle, std::span{send_buf.data(), send_buf.size()}, 0));

  // Receive on the server side
  std::vector<std::byte> recv_buf(128);
  ring->enqueue(evring::operation::make_recv(
      accepted_handle, evring::make_stable_span(std::span{recv_buf.data(), recv_buf.size()}), 0));

  events = ring->submit_and_wait(2);
  assert(events.size() == 2);

  for (auto& e : events) {
    if (e.operation == evring::operation_type::send) {
      assert(e.ok());
      std::printf("test_loopback_echo: sent %ld bytes\n", e.result);
    } else if (e.operation == evring::operation_type::recv) {
      assert(e.ok());
      assert(e.result == static_cast<int64_t>(strlen(message)));
      std::string received(reinterpret_cast<const char*>(e.data.data()), e.data.size());
      std::printf("test_loopback_echo: received '%s'\n", received.c_str());
      assert(received == message);
    }
  }

  // Cleanup
  ring->enqueue(evring::operation::make_close(client_handle));
  ring->enqueue(evring::operation::make_close(accepted_handle));
  events = ring->submit_and_wait(2);

  close(server_fd);

  std::printf("test_loopback_echo: PASSED\n\n");
}

} // namespace

int main() {
  test_socket_create();
  test_loopback_echo();

  std::printf("all socket tests passed!\n");
  return 0;
}
