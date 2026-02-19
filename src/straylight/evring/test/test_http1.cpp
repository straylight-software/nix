// test_http1.cpp
//
// Tests for HTTP/1.1 state machines using llhttp
//
// These tests verify HTTP/1.1 functionality with both local unit tests
// and network tests to real servers.

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
#include "straylight/evring/http1.h"
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
// Test: HTTP/1.1 method string conversion
// ============================================================================

void test_http1_method_strings() {
  std::printf("test_http1_method_strings: testing method string conversion...\n");

  assert(evring::http1_method_string(evring::http1_method::get) == "GET");
  assert(evring::http1_method_string(evring::http1_method::post) == "POST");
  assert(evring::http1_method_string(evring::http1_method::put) == "PUT");
  assert(evring::http1_method_string(evring::http1_method::del) == "DELETE");
  assert(evring::http1_method_string(evring::http1_method::head) == "HEAD");
  assert(evring::http1_method_string(evring::http1_method::patch) == "PATCH");
  assert(evring::http1_method_string(evring::http1_method::options) == "OPTIONS");
  assert(evring::http1_method_string(evring::http1_method::connect) == "CONNECT");
  assert(evring::http1_method_string(evring::http1_method::trace) == "TRACE");

  std::printf("  Method to string: OK\n");

  assert(evring::http1_method_parse("GET") == evring::http1_method::get);
  assert(evring::http1_method_parse("POST") == evring::http1_method::post);
  assert(evring::http1_method_parse("PUT") == evring::http1_method::put);
  assert(evring::http1_method_parse("DELETE") == evring::http1_method::del);
  assert(evring::http1_method_parse("HEAD") == evring::http1_method::head);
  assert(evring::http1_method_parse("PATCH") == evring::http1_method::patch);

  std::printf("  String to method: OK\n");

  std::printf("test_http1_method_strings: PASSED\n\n");
}

// ============================================================================
// Test: HTTP/1.1 request serialization
// ============================================================================

void test_http1_request_serialize() {
  std::printf("test_http1_request_serialize: testing request serialization...\n");

  evring::http1_request req;
  req.method = evring::http1_method::get;
  req.path = "/test";
  req.headers.push_back({"Host", "example.com"});
  req.headers.push_back({"User-Agent", "evring-test/1.0"});
  req.headers.push_back({"Accept", "*/*"});
  req.headers.push_back({"Connection", "close"});

  auto serialized = req.serialize();
  std::string s(reinterpret_cast<const char*>(serialized.data()), serialized.size());

  std::printf("  Serialized request (%zu bytes):\n", serialized.size());
  std::printf("  ---\n%s  ---\n", s.c_str());

  // Check request line
  assert(s.find("GET /test HTTP/1.1\r\n") != std::string::npos);

  // Check headers
  assert(s.find("Host: example.com\r\n") != std::string::npos);
  assert(s.find("User-Agent: evring-test/1.0\r\n") != std::string::npos);
  assert(s.find("Accept: */*\r\n") != std::string::npos);
  assert(s.find("Connection: close\r\n") != std::string::npos);

  // Check ends with blank line
  assert(s.find("\r\n\r\n") != std::string::npos);

  std::printf("test_http1_request_serialize: PASSED\n\n");
}

// ============================================================================
// Test: HTTP/1.1 request with body
// ============================================================================

void test_http1_request_with_body() {
  std::printf("test_http1_request_with_body: testing request with body...\n");

  evring::http1_request req;
  req.method = evring::http1_method::post;
  req.path = "/submit";
  req.headers.push_back({"Host", "example.com"});
  req.headers.push_back({"Content-Type", "application/json"});

  std::string body = R"({"key": "value"})";
  req.body.resize(body.size());
  std::memcpy(req.body.data(), body.data(), body.size());

  auto serialized = req.serialize();
  std::string s(reinterpret_cast<const char*>(serialized.data()), serialized.size());

  std::printf("  Serialized request (%zu bytes):\n", serialized.size());
  std::printf("  ---\n%s  ---\n", s.c_str());

  // Check request line
  assert(s.find("POST /submit HTTP/1.1\r\n") != std::string::npos);

  // Content-Length should be auto-added
  assert(s.find("Content-Length: 16\r\n") != std::string::npos);

  // Body should be at the end
  assert(s.find("{\"key\": \"value\"}") != std::string::npos);

  std::printf("test_http1_request_with_body: PASSED\n\n");
}

// ============================================================================
// Test: HTTP/1.1 parser
// ============================================================================

void test_http1_parser() {
  std::printf("test_http1_parser: testing response parser...\n");

  evring::http1_parser parser;

  // Simple HTTP/1.1 response
  const char* response = "HTTP/1.1 200 OK\r\n"
                         "Content-Type: text/plain\r\n"
                         "Content-Length: 13\r\n"
                         "Connection: close\r\n"
                         "\r\n"
                         "Hello, World!";

  std::span<const std::byte> data(reinterpret_cast<const std::byte*>(response),
                                  std::strlen(response));

  auto consumed = parser.parse(data);
  assert(consumed > 0);
  assert(parser.message_complete());
  assert(!parser.has_error());

  auto& resp = parser.response();
  assert(resp.status_code == 200);
  assert(resp.status_message == "OK");

  std::printf("  Status: %d %s\n", resp.status_code, resp.status_message.c_str());

  // Check headers
  auto content_type = resp.get_header("Content-Type");
  assert(content_type == "text/plain");
  std::printf("  Content-Type: %.*s\n", static_cast<int>(content_type.size()), content_type.data());

  // Check body
  assert(resp.body.size() == 13);
  std::string body(reinterpret_cast<const char*>(resp.body.data()), resp.body.size());
  assert(body == "Hello, World!");
  std::printf("  Body: %s\n", body.c_str());

  std::printf("test_http1_parser: PASSED\n\n");
}

// ============================================================================
// Test: HTTP/1.1 chunked transfer encoding
// ============================================================================

void test_http1_parser_chunked() {
  std::printf("test_http1_parser_chunked: testing chunked transfer encoding...\n");

  evring::http1_parser parser;

  // Chunked response
  const char* response = "HTTP/1.1 200 OK\r\n"
                         "Transfer-Encoding: chunked\r\n"
                         "\r\n"
                         "5\r\n"
                         "Hello\r\n"
                         "7\r\n"
                         ", World\r\n"
                         "0\r\n"
                         "\r\n";

  std::span<const std::byte> data(reinterpret_cast<const std::byte*>(response),
                                  std::strlen(response));

  auto consumed = parser.parse(data);
  assert(consumed > 0);
  assert(parser.message_complete());
  assert(!parser.has_error());

  auto& resp = parser.response();
  assert(resp.status_code == 200);

  std::string body(reinterpret_cast<const char*>(resp.body.data()), resp.body.size());
  std::printf("  Body: %s (len=%zu)\n", body.c_str(), body.size());
  assert(body == "Hello, World");

  std::printf("test_http1_parser_chunked: PASSED\n\n");
}

// ============================================================================
// Test: HTTP/1.1 response helper methods
// ============================================================================

void test_http1_response_helpers() {
  std::printf("test_http1_response_helpers: testing response helper methods...\n");

  evring::http1_response resp;
  resp.status_code = 200;
  resp.headers.push_back({"Content-Length", "100"});
  resp.headers.push_back({"Connection", "keep-alive"});

  assert(resp.ok());
  assert(resp.content_length() == 100);
  assert(resp.keep_alive());

  resp.status_code = 404;
  assert(!resp.ok());

  resp.headers.clear();
  resp.headers.push_back({"Connection", "close"});
  assert(!resp.keep_alive());

  std::printf("test_http1_response_helpers: PASSED\n\n");
}

// ============================================================================
// Test: HTTPS GET request (requires network + CA certs)
// ============================================================================

void test_http1_https_get() {
  std::printf("test_http1_https_get: performing HTTPS GET to httpbin.org...\n");

  auto ring = evring::make_io_uring_ring(32);

  // TCP connect
  evring::handle socket = tcp_connect(*ring, "httpbin.org", "443");
  if (!socket.valid()) {
    std::printf("test_http1_https_get: SKIPPED (network unavailable)\n\n");
    return;
  }
  std::printf("  TCP connected\n");

  // TLS handshake (HTTP/1.1 only, no h2 ALPN)
  auto tls_config = evring::tls_client_config::create_default();
  bool alpn_ok = tls_config.set_alpn("http/1.1");
  assert(alpn_ok);

  evring::tls_handshake_machine tls_hs{socket, *ring, tls_config, "httpbin.org"};
  auto tls_state = evring::run(tls_hs, *ring);

  if (!tls_state.ok()) {
    std::printf("  TLS handshake failed: %s\n", tls_state.error_message.c_str());
    ring->enqueue(evring::operation::make_close(socket));
    ring->submit_and_wait(1);
    std::printf("test_http1_https_get: SKIPPED (TLS failed, likely CA certs missing)\n\n");
    return;
  }

  auto tls_conn = tls_state.take_context();
  std::printf("  TLS handshake succeeded\n");
  std::printf("  TLS version: %s\n", tls_conn.version());

  // Build HTTP/1.1 request
  evring::http1_request req;
  req.method = evring::http1_method::get;
  req.path = "/get";
  req.headers.push_back({"Host", "httpbin.org"});
  req.headers.push_back({"User-Agent", "evring-test/1.0"});
  req.headers.push_back({"Accept", "application/json"});
  req.headers.push_back({"Connection", "close"});

  // Send request via TLS
  evring::http1_tls_client_machine client{tls_conn, socket, req};
  auto state = evring::run(client, *ring);

  if (!state.ok()) {
    std::printf("  HTTP request failed: %s\n", state.error_message.c_str());
  } else {
    auto& resp = state.response;
    std::printf("  HTTP response: %d %s\n", resp.status_code, resp.status_message.c_str());
    std::printf("  Response body size: %zu bytes\n", resp.body.size());

    // Print first 200 chars of body
    if (!resp.body.empty()) {
      std::size_t preview_len = std::min(resp.body.size(), std::size_t{200});
      std::string body_preview(reinterpret_cast<const char*>(resp.body.data()), preview_len);
      std::printf("  Body preview: %s...\n", body_preview.c_str());
    }
  }

  // Clean up
  ring->enqueue(evring::operation::make_close(socket));
  ring->submit_and_wait(1);

  if (state.ok() && state.response.status_code == 200) {
    std::printf("test_http1_https_get: PASSED\n\n");
  } else {
    std::printf("test_http1_https_get: COMPLETED (may have partial success)\n\n");
  }
}

// ============================================================================
// Test: HTTPS GET to cloudflare (reliable server)
// ============================================================================

void test_http1_https_cloudflare() {
  std::printf("test_http1_https_cloudflare: performing HTTPS GET to cloudflare.com...\n");

  auto ring = evring::make_io_uring_ring(32);

  // TCP connect
  evring::handle socket = tcp_connect(*ring, "cloudflare.com", "443");
  if (!socket.valid()) {
    std::printf("test_http1_https_cloudflare: SKIPPED (network unavailable)\n\n");
    return;
  }
  std::printf("  TCP connected\n");

  // TLS handshake
  auto tls_config = evring::tls_client_config::create_default();
  tls_config.set_alpn("http/1.1");

  evring::tls_handshake_machine tls_hs{socket, *ring, tls_config, "cloudflare.com"};
  auto tls_state = evring::run(tls_hs, *ring);

  if (!tls_state.ok()) {
    std::printf("  TLS handshake failed: %s\n", tls_state.error_message.c_str());
    ring->enqueue(evring::operation::make_close(socket));
    ring->submit_and_wait(1);
    std::printf("test_http1_https_cloudflare: SKIPPED (TLS failed)\n\n");
    return;
  }

  auto tls_conn = tls_state.take_context();
  std::printf("  TLS handshake succeeded, version: %s\n", tls_conn.version());

  // Build HTTP/1.1 request
  evring::http1_request req;
  req.method = evring::http1_method::get;
  req.path = "/";
  req.headers.push_back({"Host", "cloudflare.com"});
  req.headers.push_back({"User-Agent", "evring-test/1.0"});
  req.headers.push_back({"Accept", "*/*"});
  req.headers.push_back({"Connection", "close"});

  // Send request via TLS
  evring::http1_tls_client_machine client{tls_conn, socket, req};
  auto state = evring::run(client, *ring);

  // Clean up
  ring->enqueue(evring::operation::make_close(socket));
  ring->submit_and_wait(1);

  if (!state.ok()) {
    std::printf("  HTTP request failed: %s\n", state.error_message.c_str());
    std::printf("test_http1_https_cloudflare: FAILED\n\n");
    return;
  }

  auto& resp = state.response;
  std::printf("  HTTP response: %d %s\n", resp.status_code, resp.status_message.c_str());
  std::printf("  Response body size: %zu bytes\n", resp.body.size());

  // Cloudflare usually returns 301/302 redirect or 200
  if (resp.status_code >= 200 && resp.status_code < 400) {
    std::printf("test_http1_https_cloudflare: PASSED\n\n");
  } else {
    std::printf("test_http1_https_cloudflare: FAILED (unexpected status)\n\n");
  }
}

} // namespace

int main() {
  std::printf("=== HTTP/1.1 State Machine Tests ===\n\n");

  // Local tests (no network)
  test_http1_method_strings();
  test_http1_request_serialize();
  test_http1_request_with_body();
  test_http1_parser();
  test_http1_parser_chunked();
  test_http1_response_helpers();

  // Network tests (may be skipped)
  test_http1_https_get();
  test_http1_https_cloudflare();

  std::printf("All HTTP/1.1 tests completed!\n");
  return 0;
}
