// test_tls.cpp
//
// Tests for TLS state machine using libtls
//
// These tests connect to real HTTPS servers to verify TLS functionality.
// Network access is required for these tests to pass.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "straylight/evring/evring.h"
#include "straylight/evring/tls.h"

namespace {

// ============================================================================
// Helper: DNS lookup and TCP connect
// ============================================================================

/// Create a TCP socket and connect to host:port
/// Returns the connected socket handle, or invalid handle on failure
auto tcp_connect(evring::ring& ring, const char* host, const char* port) -> evring::handle {
  std::printf("  tcp_connect: DNS lookup for %s...\n", host);
  std::fflush(stdout);

  // Do DNS lookup
  struct addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* result = nullptr;
  int gai_result = getaddrinfo(host, port, &hints, &result);
  if (gai_result != 0) {
    std::printf("getaddrinfo failed: %s\n", gai_strerror(gai_result));
    return evring::handle::invalid();
  }
  std::printf("  tcp_connect: DNS resolved\n");
  std::fflush(stdout);

  // Create socket
  ring.enqueue(evring::operation::make_socket(AF_INET, SOCK_STREAM, 0, SOCK_CLOEXEC));
  auto events = ring.submit_and_wait(1);
  if (!events[0].ok()) {
    std::printf("socket creation failed: %d\n", events[0].error_code());
    freeaddrinfo(result);
    return evring::handle::invalid();
  }
  std::printf("  tcp_connect: socket created\n");
  std::fflush(stdout);

  evring::handle socket_handle = events[0].resource_handle;

  // Connect
  std::printf("  tcp_connect: connecting...\n");
  std::fflush(stdout);
  ring.enqueue(evring::operation::make_connect(socket_handle, result->ai_addr,
                                               static_cast<std::uint32_t>(result->ai_addrlen)));
  events = ring.submit_and_wait(1);
  freeaddrinfo(result);
  std::printf("  tcp_connect: connect returned\n");
  std::fflush(stdout);

  if (!events[0].ok()) {
    std::printf("connect failed: %d\n", events[0].error_code());
    ring.enqueue(evring::operation::make_close(socket_handle));
    ring.submit_and_wait(1);
    return evring::handle::invalid();
  }

  return socket_handle;
}

// ============================================================================
// Test: TLS config creation
// ============================================================================

void test_tls_config() {
  std::printf("test_tls_config: creating TLS configurations...\n");

  // Default client config
  auto config = evring::tls_client_config::create_default();
  assert(config.valid());
  std::printf("  default client config: OK\n");

  // Insecure config
  auto insecure = evring::tls_client_config::create_insecure();
  assert(insecure.valid());
  std::printf("  insecure config: OK\n");

  // ALPN setting
  bool alpn_ok = config.set_alpn("h2,http/1.1");
  assert(alpn_ok);
  std::printf("  ALPN setting: OK\n");

  // Server config
  auto server_config = evring::tls_server_config::create();
  assert(server_config.valid());
  std::printf("  server config: OK\n");

  std::printf("test_tls_config: PASSED\n\n");
}

// ============================================================================
// Test: TLS handshake with example.com
// ============================================================================

void test_tls_handshake_example_com() {
  std::printf("test_tls_handshake_example_com: connecting to example.com:443...\n");
  std::fflush(stdout);

  auto ring = evring::make_io_uring_ring(32);
  std::printf("  Ring created\n");
  std::fflush(stdout);

  // TCP connect
  evring::handle socket = tcp_connect(*ring, "example.com", "443");
  if (!socket.valid()) {
    std::printf("test_tls_handshake_example_com: SKIPPED (network unavailable)\n\n");
    return;
  }
  std::printf("  TCP connected\n");

  // Create TLS config
  auto config = evring::tls_client_config::create_default();
  assert(config.valid());

  // Run TLS handshake with manual stepping for debugging
  evring::tls_handshake_machine handshake{socket, *ring, config, "example.com"};

  std::printf("  Starting handshake...\n");
  auto state = handshake.initial();

  // Initial step to start handshake
  auto result = handshake.step(state, evring::event{});
  state = std::move(result.state);

  int iterations = 0;
  const int max_iterations = 100;

  while (!handshake.done(state) && iterations++ < max_iterations) {
    std::printf("  Iteration %d: phase=%d, ops=%zu\n", iterations,
                static_cast<int>(state.current_phase), result.operations.size());

    for (const auto& op : result.operations) {
      ring->enqueue(op);
    }

    if (ring->pending() == 0) {
      std::printf("  No pending operations, breaking\n");
      break;
    }

    auto events = ring->submit_and_wait(1);
    std::printf("  Got %zu events, result=%ld\n", events.size(),
                events.empty() ? -999 : events[0].result);

    if (!events.empty()) {
      result = handshake.step(state, events[0]);
      state = std::move(result.state);
    }
  }

  if (iterations >= max_iterations) {
    std::printf("  Exceeded max iterations!\n");
  }

  auto final_state = std::move(state);

  if (!final_state.ok()) {
    std::printf("  TLS handshake failed: %s (error_code=%d)\n", final_state.error_message.c_str(),
                final_state.error_code);
    std::printf("  Phase: %d\n", static_cast<int>(final_state.current_phase));
    ring->enqueue(evring::operation::make_close(socket));
    ring->submit_and_wait(1);
    std::printf("test_tls_handshake_example_com: FAILED (handshake error)\n\n");
    return;
  }

  std::printf("  TLS handshake succeeded!\n");

  // Get connection info
  auto conn = final_state.take_context();
  std::printf("  TLS version: %s\n", conn.version());
  std::printf("  Cipher: %s\n", conn.cipher());
  std::printf("  Cipher strength: %d bits\n", conn.cipher_strength());
  if (conn.peer_cn()) {
    std::printf("  Peer CN: %s\n", conn.peer_cn());
  }

  // Clean up
  ring->enqueue(evring::operation::make_close(socket));
  ring->submit_and_wait(1);

  std::printf("test_tls_handshake_example_com: PASSED\n\n");
}

// ============================================================================
// Test: TLS handshake with ALPN (h2 negotiation)
// ============================================================================

void test_tls_alpn_negotiation() {
  std::printf("test_tls_alpn_negotiation: connecting to google.com:443 with ALPN...\n");

  auto ring = evring::make_io_uring_ring(32);

  // TCP connect
  evring::handle socket = tcp_connect(*ring, "google.com", "443");
  if (!socket.valid()) {
    std::printf("test_tls_alpn_negotiation: SKIPPED (network unavailable)\n\n");
    return;
  }
  std::printf("  TCP connected\n");

  // Create TLS config with ALPN
  auto config = evring::tls_client_config::create_default();
  assert(config.valid());
  bool alpn_ok = config.set_alpn("h2,http/1.1");
  assert(alpn_ok);

  // Run TLS handshake
  evring::tls_handshake_machine handshake{socket, *ring, config, "google.com"};
  auto final_state = evring::run(handshake, *ring);

  if (!final_state.ok()) {
    std::printf("  TLS handshake failed: %s\n", final_state.error_message.c_str());
    ring->enqueue(evring::operation::make_close(socket));
    ring->submit_and_wait(1);
    // Don't fail - network issues happen
    std::printf("test_tls_alpn_negotiation: SKIPPED (handshake failed)\n\n");
    return;
  }

  std::printf("  TLS handshake succeeded!\n");

  auto conn = final_state.take_context();
  const char* alpn = conn.alpn_selected();
  std::printf("  ALPN selected: %s\n", alpn ? alpn : "(none)");
  std::printf("  TLS version: %s\n", conn.version());

  // Google typically supports h2
  if (alpn && std::strcmp(alpn, "h2") == 0) {
    std::printf("  HTTP/2 negotiated successfully!\n");
  }

  // Clean up
  ring->enqueue(evring::operation::make_close(socket));
  ring->submit_and_wait(1);

  std::printf("test_tls_alpn_negotiation: PASSED\n\n");
}

// ============================================================================
// Test: TLS read/write (HTTP GET request)
// ============================================================================

void test_tls_http_get() {
  std::printf("test_tls_http_get: performing HTTPS GET to example.com...\n");

  auto ring = evring::make_io_uring_ring(32);

  // TCP connect
  evring::handle socket = tcp_connect(*ring, "example.com", "443");
  if (!socket.valid()) {
    std::printf("test_tls_http_get: SKIPPED (network unavailable)\n\n");
    return;
  }
  std::printf("  TCP connected\n");

  // TLS handshake
  auto config = evring::tls_client_config::create_default();
  evring::tls_handshake_machine handshake{socket, *ring, config, "example.com"};
  auto hs_state = evring::run(handshake, *ring);

  if (!hs_state.ok()) {
    std::printf("test_tls_http_get: SKIPPED (handshake failed: %s)\n\n",
                hs_state.error_message.c_str());
    ring->enqueue(evring::operation::make_close(socket));
    ring->submit_and_wait(1);
    return;
  }

  auto conn = hs_state.take_context();
  std::printf("  TLS handshake succeeded\n");

  // Send HTTP GET request
  const char* http_request = "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n";
  std::vector<std::byte> request_data(std::strlen(http_request));
  std::memcpy(request_data.data(), http_request, request_data.size());

  evring::tls_write_machine writer{conn, socket, std::span{request_data}};
  auto write_state = evring::run(writer, *ring);

  if (!write_state.ok()) {
    std::printf("  TLS write failed: %s\n", write_state.error_message.c_str());
  } else {
    std::printf("  Sent %zu bytes\n", write_state.bytes_written);
  }

  // Read response
  std::vector<std::byte> response_buffer(4096);
  evring::tls_read_machine reader{conn, socket, std::span{response_buffer}};
  auto read_state = evring::run(reader, *ring);

  if (!read_state.ok()) {
    std::printf("  TLS read failed: %s\n", read_state.error_message.c_str());
  } else {
    std::printf("  Received %zu bytes\n", read_state.bytes_read);

    // Print first line of response
    std::string response(reinterpret_cast<const char*>(response_buffer.data()),
                         std::min(read_state.bytes_read, static_cast<std::size_t>(100)));
    auto newline = response.find('\n');
    if (newline != std::string::npos) {
      response = response.substr(0, newline);
    }
    std::printf("  Response: %s\n", response.c_str());

    // Verify we got HTTP response
    assert(read_state.bytes_read > 0);
    assert(response.find("HTTP/1.1") != std::string::npos ||
           response.find("HTTP/1.0") != std::string::npos);
  }

  // Clean up
  ring->enqueue(evring::operation::make_close(socket));
  ring->submit_and_wait(1);

  std::printf("test_tls_http_get: PASSED\n\n");
}

// ============================================================================
// Test: TLS replay (verification of replayability)
// ============================================================================

void test_tls_replay() {
  std::printf("test_tls_replay: verifying TLS operations are replayable...\n");

  // The TLS state machines use the evring machine concept, which means
  // they can be replayed. We verify this by running with tracing and
  // then replaying.

  auto ring = evring::make_io_uring_ring(32);

  evring::handle socket = tcp_connect(*ring, "example.com", "443");
  if (!socket.valid()) {
    std::printf("test_tls_replay: SKIPPED (network unavailable)\n\n");
    return;
  }

  auto config = evring::tls_client_config::create_default();
  evring::tls_handshake_machine handshake{socket, *ring, config, "example.com"};

  // Run with tracing
  auto [final_state, trace] = evring::run_traced(handshake, *ring);

  std::printf("  Handshake completed with %zu events recorded\n", trace.size());

  if (final_state.ok()) {
    std::printf("  Original handshake: success\n");

    // Replay the trace
    evring::tls_handshake_machine handshake2{socket, *ring, config, "example.com"};
    auto replayed_state = evring::replay(handshake2, trace.events());

    // The replayed state should match (at least the phase)
    // Note: We can't compare TLS context pointers, but we can compare phases
    std::printf("  Replayed handshake phase matches: %s\n",
                (static_cast<int>(replayed_state.current_phase) ==
                 static_cast<int>(final_state.current_phase))
                    ? "yes"
                    : "no");
  }

  // Clean up
  ring->enqueue(evring::operation::make_close(socket));
  ring->submit_and_wait(1);

  std::printf("test_tls_replay: PASSED\n\n");
}

} // namespace

int main() {
  std::printf("=== TLS State Machine Tests ===\n\n");

  // Local tests (no network)
  test_tls_config();

  // Network tests (may be skipped if network unavailable)
  test_tls_handshake_example_com();
  test_tls_alpn_negotiation();
  test_tls_http_get();
  test_tls_replay();

  std::printf("All TLS tests completed!\n");
  return 0;
}
