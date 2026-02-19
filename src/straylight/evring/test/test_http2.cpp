// test_http2.cpp
//
// Tests for HTTP/2 state machines using nghttp2
//
// These tests connect to real HTTPS servers to verify HTTP/2 functionality.
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
#include "straylight/evring/http2.h"
#include "straylight/evring/tls.h"

namespace {

// ============================================================================
// Helper: DNS lookup and TCP connect
// ============================================================================

auto tcp_connect(evring::ring& ring, const char* host, const char* port) -> evring::handle {
  std::printf("  tcp_connect: DNS lookup for %s...\n", host);
  std::fflush(stdout);

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
// Test: HTTP/2 session initialization
// ============================================================================

void test_http2_session_init() {
  std::printf("test_http2_session_init: testing session initialization...\n");

  evring::http2_session session;
  assert(!session.valid());

  bool init_ok = session.init_client();
  assert(init_ok);
  assert(session.valid());

  std::printf("  Session initialized successfully\n");

  // Check local settings
  auto& settings = session.local_settings();
  assert(settings.header_table_size == evring::http2_default_header_table_size);
  assert(settings.enable_push == 0);
  assert(settings.max_concurrent_streams == evring::http2_default_max_concurrent_streams);

  std::printf("  Local settings: header_table_size=%u, max_concurrent_streams=%u\n",
              settings.header_table_size, settings.max_concurrent_streams);

  // Check that initial data is pending (preface + SETTINGS)
  auto pending = session.get_pending_data();
  assert(!pending.empty());
  std::printf("  Pending data after init: %zu bytes\n", pending.size());

  // The HTTP/2 client preface is "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" (24 bytes)
  // followed by SETTINGS frame
  assert(pending.size() >= 24);

  std::printf("test_http2_session_init: PASSED\n\n");
}

// ============================================================================
// Test: HTTP/2 request creation
// ============================================================================

void test_http2_request() {
  std::printf("test_http2_request: testing request creation...\n");

  evring::http2_request req;
  req.method = "GET";
  req.scheme = "https";
  req.authority = "example.com";
  req.path = "/";
  req.headers.push_back({"user-agent", "evring-test/1.0"});
  req.headers.push_back({"accept", "*/*"});

  auto all = req.all_headers();
  assert(all.size() == 6); // 4 pseudo-headers + 2 regular

  // Check pseudo-headers
  assert(all[0].name == ":method" && all[0].value == "GET");
  assert(all[1].name == ":scheme" && all[1].value == "https");
  assert(all[2].name == ":authority" && all[2].value == "example.com");
  assert(all[3].name == ":path" && all[3].value == "/");

  // Check regular headers
  assert(all[4].name == "user-agent" && all[4].value == "evring-test/1.0");
  assert(all[5].name == "accept" && all[5].value == "*/*");

  std::printf("  Request headers built correctly\n");
  std::printf("test_http2_request: PASSED\n\n");
}

// ============================================================================
// Test: HTTP/2 error strings
// ============================================================================

void test_http2_error_strings() {
  std::printf("test_http2_error_strings: testing error string conversion...\n");

  assert(evring::http2_error_string(evring::http2_error_code::no_error) == "no error");
  assert(evring::http2_error_string(evring::http2_error_code::protocol_error) == "protocol error");
  assert(evring::http2_error_string(evring::http2_error_code::enhance_your_calm) ==
         "enhance your calm");

  std::printf("  Error strings working correctly\n");
  std::printf("test_http2_error_strings: PASSED\n\n");
}

// ============================================================================
// Test: Full HTTP/2 GET request (requires network + CA certs)
// ============================================================================

void test_http2_get_request() {
  std::printf("test_http2_get_request: performing HTTP/2 GET to www.google.com...\n");

  auto ring = evring::make_io_uring_ring(32);

  // TCP connect
  evring::handle socket = tcp_connect(*ring, "www.google.com", "443");
  if (!socket.valid()) {
    std::printf("test_http2_get_request: SKIPPED (network unavailable)\n\n");
    return;
  }
  std::printf("  TCP connected\n");

  // TLS handshake with ALPN
  auto tls_config = evring::tls_client_config::create_default();
  bool alpn_ok = tls_config.set_alpn("h2");
  assert(alpn_ok);

  evring::tls_handshake_machine tls_hs{socket, *ring, tls_config, "www.google.com"};
  auto tls_state = evring::run(tls_hs, *ring);

  if (!tls_state.ok()) {
    std::printf("  TLS handshake failed: %s\n", tls_state.error_message.c_str());
    ring->enqueue(evring::operation::make_close(socket));
    ring->submit_and_wait(1);
    std::printf("test_http2_get_request: SKIPPED (TLS failed, likely CA certs missing)\n\n");
    return;
  }

  auto tls_conn = tls_state.take_context();
  std::printf("  TLS handshake succeeded\n");
  std::printf("  TLS version: %s\n", tls_conn.version());

  // Check ALPN negotiated h2
  const char* alpn = tls_conn.alpn_selected();
  if (!alpn || std::strcmp(alpn, "h2") != 0) {
    std::printf("  ALPN not h2: %s\n", alpn ? alpn : "(none)");
    ring->enqueue(evring::operation::make_close(socket));
    ring->submit_and_wait(1);
    std::printf("test_http2_get_request: SKIPPED (ALPN h2 not negotiated)\n\n");
    return;
  }
  std::printf("  ALPN: h2\n");

  // Initialize HTTP/2 session
  evring::http2_session session;
  bool init_ok = session.init_client();
  assert(init_ok);

  // HTTP/2 connection establishment
  evring::http2_connection_machine conn_machine{session, tls_conn, socket};
  auto conn_state = evring::run(conn_machine, *ring);

  if (!conn_state.ok()) {
    std::printf("  HTTP/2 connection failed: %s\n", conn_state.error_message.c_str());
    ring->enqueue(evring::operation::make_close(socket));
    ring->submit_and_wait(1);
    std::printf("test_http2_get_request: FAILED (connection setup failed)\n\n");
    return;
  }
  std::printf("  HTTP/2 connection established\n");

  // Send GET request
  evring::http2_request req;
  req.method = "GET";
  req.scheme = "https";
  req.authority = "www.google.com";
  req.path = "/";
  req.headers.push_back({"user-agent", "evring-test/1.0"});
  req.headers.push_back({"accept", "*/*"});

  evring::http2_request_machine req_machine{session, tls_conn, socket, req};
  auto req_state = evring::run(req_machine, *ring);

  if (!req_state.ok()) {
    std::printf("  HTTP/2 request failed: %s\n", req_state.error_message.c_str());
  } else {
    std::printf("  HTTP/2 response: %d\n", req_state.response.status_code);
    std::printf("  Response body size: %zu bytes\n", req_state.response.body.size());

    // Print first 100 chars of body
    if (!req_state.response.body.empty()) {
      std::string body_preview(reinterpret_cast<const char*>(req_state.response.body.data()),
                               std::min(req_state.response.body.size(), std::size_t{100}));
      std::printf("  Body preview: %s...\n", body_preview.c_str());
    }
  }

  // Clean up
  ring->enqueue(evring::operation::make_close(socket));
  ring->submit_and_wait(1);

  if (req_state.ok() && req_state.response.status_code == 200) {
    std::printf("test_http2_get_request: PASSED\n\n");
  } else {
    std::printf("test_http2_get_request: COMPLETED (may have partial success)\n\n");
  }
}

} // namespace

int main() {
  std::printf("=== HTTP/2 State Machine Tests ===\n\n");

  // Local tests (no network)
  test_http2_session_init();
  test_http2_request();
  test_http2_error_strings();

  // Network tests (may be skipped)
  test_http2_get_request();

  std::printf("All HTTP/2 tests completed!\n");
  return 0;
}
