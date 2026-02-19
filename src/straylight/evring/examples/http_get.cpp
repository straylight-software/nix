// http_get.cpp - HTTP GET requests using HTTP/1.1 and HTTP/2
//
// This example demonstrates the HTTP protocol support in evring:
// - HTTP/1.1 over TLS (using llhttp)
// - HTTP/2 over TLS with ALPN (using nghttp2)
//
// Usage: http_get <url>
//        http_get -1 <url>  (force HTTP/1.1)
//        http_get -2 <url>  (force HTTP/2)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "straylight/evring/evring.h"
#include "straylight/evring/http1.h"
#include "straylight/evring/http2.h"
#include "straylight/evring/tls.h"

// ============================================================================
// URL parsing
// ============================================================================

struct parsed_url {
  std::string scheme;
  std::string host;
  std::string port;
  std::string path;
  bool valid = false;
};

auto parse_url(std::string_view url) -> parsed_url {
  parsed_url result;

  if (url.starts_with("https://")) {
    result.scheme = "https";
    url.remove_prefix(8);
  } else if (url.starts_with("http://")) {
    result.scheme = "http";
    url.remove_prefix(7);
  } else {
    result.scheme = "https";
  }

  auto path_pos = url.find('/');
  std::string_view authority;
  if (path_pos != std::string_view::npos) {
    authority = url.substr(0, path_pos);
    result.path = std::string(url.substr(path_pos));
  } else {
    authority = url;
    result.path = "/";
  }

  auto port_pos = authority.find(':');
  if (port_pos != std::string_view::npos) {
    result.host = std::string(authority.substr(0, port_pos));
    result.port = std::string(authority.substr(port_pos + 1));
  } else {
    result.host = std::string(authority);
    result.port = result.scheme == "https" ? "443" : "80";
  }

  result.valid = !result.host.empty();
  return result;
}

// ============================================================================
// TCP connect helper
// ============================================================================

auto tcp_connect(evring::ring& ring, const char* host, const char* port) -> evring::handle {
  struct addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* result = nullptr;
  if (getaddrinfo(host, port, &hints, &result) != 0) {
    return evring::handle::invalid();
  }

  ring.enqueue(evring::operation::make_socket(AF_INET, SOCK_STREAM, 0, SOCK_CLOEXEC));
  auto events = ring.submit_and_wait(1);
  if (!events[0].ok()) {
    freeaddrinfo(result);
    return evring::handle::invalid();
  }

  evring::handle socket_handle = events[0].resource_handle;

  ring.enqueue(evring::operation::make_connect(socket_handle, result->ai_addr,
                                               static_cast<std::uint32_t>(result->ai_addrlen)));
  events = ring.submit_and_wait(1);
  freeaddrinfo(result);

  if (!events[0].ok()) {
    ring.enqueue(evring::operation::make_close(socket_handle));
    ring.submit_and_wait(1);
    return evring::handle::invalid();
  }

  return socket_handle;
}

// ============================================================================
// HTTP/1.1 request
// ============================================================================

void do_http1_request(evring::ring& ring, const parsed_url& url, evring::handle socket,
                      evring::tls_connection& tls) {
  std::printf("Sending HTTP/1.1 request...\n");

  evring::http1_request req;
  req.method = evring::http1_method::get;
  req.path = url.path;
  req.headers = {
      {"Host", url.host},
      {"User-Agent", "evring-example/1.0"},
      {"Accept", "*/*"},
      {"Connection", "close"},
  };

  evring::http1_tls_client_machine client{tls, socket, req};
  auto state = evring::run(client, ring);

  if (state.current_phase == evring::http1_tls_client_state::phase::error) {
    std::fprintf(stderr, "HTTP/1.1 error: %s\n", state.error_message.c_str());
    return;
  }

  std::printf("\n--- Response ---\n");
  std::printf("Status: %d %s\n", state.response.status_code, state.response.status_message.c_str());
  std::printf("Headers:\n");
  for (const auto& h : state.response.headers) {
    std::printf("  %s: %s\n", h.name.c_str(), h.value.c_str());
  }
  std::printf("\nBody (%zu bytes):\n", state.response.body.size());
  if (state.response.body.size() <= 2048) {
    std::fwrite(state.response.body.data(), 1, state.response.body.size(), stdout);
  } else {
    std::fwrite(state.response.body.data(), 1, 2048, stdout);
    std::printf("\n... (truncated)\n");
  }
  std::printf("\n");
}

// ============================================================================
// HTTP/2 request
// ============================================================================

void do_http2_request(evring::ring& ring, const parsed_url& url, evring::handle socket,
                      evring::tls_connection& tls) {
  std::printf("Sending HTTP/2 request...\n");

  // Initialize HTTP/2 session
  evring::http2_session session;
  if (!session.init_client()) {
    std::fprintf(stderr, "Failed to initialize HTTP/2 session\n");
    return;
  }

  // HTTP/2 connection setup
  evring::http2_connection_machine conn{session, tls, socket};
  auto conn_state = evring::run(conn, ring);
  if (!conn_state.ok()) {
    std::fprintf(stderr, "HTTP/2 connection failed: %s\n", conn_state.error_message.c_str());
    return;
  }

  // Build request
  evring::http2_request req;
  req.method = "GET";
  req.scheme = url.scheme;
  req.authority = url.host;
  req.path = url.path;
  req.headers = {{"user-agent", "evring-example/1.0"}, {"accept", "*/*"}};

  // Send request
  evring::http2_request_machine request{session, tls, socket, req};
  auto resp_state = evring::run(request, ring);

  if (!resp_state.ok()) {
    std::fprintf(stderr, "HTTP/2 request failed: %s\n", resp_state.error_message.c_str());
    return;
  }

  std::printf("\n--- Response ---\n");
  std::printf("Status: %d\n", resp_state.response.status_code);
  std::printf("Headers:\n");
  for (const auto& h : resp_state.response.headers) {
    std::printf("  %s: %s\n", h.name.c_str(), h.value.c_str());
  }
  std::printf("\nBody (%zu bytes):\n", resp_state.response.body.size());
  if (resp_state.response.body.size() <= 2048) {
    std::fwrite(resp_state.response.body.data(), 1, resp_state.response.body.size(), stdout);
  } else {
    std::fwrite(resp_state.response.body.data(), 1, 2048, stdout);
    std::printf("\n... (truncated)\n");
  }
  std::printf("\n");
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
  // Parse arguments
  bool force_http1 = false;
  bool force_http2 = false;
  const char* url_arg = nullptr;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-1") == 0) {
      force_http1 = true;
    } else if (std::strcmp(argv[i], "-2") == 0) {
      force_http2 = true;
    } else {
      url_arg = argv[i];
    }
  }

  if (!url_arg) {
    std::fprintf(stderr, "Usage: %s [-1|-2] <url>\n", argv[0]);
    std::fprintf(stderr, "\nOptions:\n");
    std::fprintf(stderr, "  -1  Force HTTP/1.1\n");
    std::fprintf(stderr, "  -2  Force HTTP/2\n");
    std::fprintf(stderr, "\nExamples:\n");
    std::fprintf(stderr, "  %s https://httpbin.org/get\n", argv[0]);
    std::fprintf(stderr, "  %s -2 https://www.google.com/\n", argv[0]);
    return 1;
  }

  // Parse URL
  auto url = parse_url(url_arg);
  if (!url.valid) {
    std::fprintf(stderr, "Invalid URL: %s\n", url_arg);
    return 1;
  }

  std::printf("URL: %s://%s:%s%s\n", url.scheme.c_str(), url.host.c_str(), url.port.c_str(),
              url.path.c_str());

  // Create ring
  auto ring = evring::make_io_uring_ring(256);
  if (!ring) {
    std::fprintf(stderr, "Failed to create io_uring\n");
    return 1;
  }

  // TCP connect
  std::printf("Connecting to %s:%s...\n", url.host.c_str(), url.port.c_str());
  auto socket = tcp_connect(*ring, url.host.c_str(), url.port.c_str());
  if (!socket.valid()) {
    std::fprintf(stderr, "TCP connect failed\n");
    return 1;
  }
  std::printf("Connected.\n");

  // TLS handshake
  auto tls_config = evring::tls_client_config::create_default();
  if (force_http1) {
    tls_config.set_alpn("http/1.1");
  } else if (force_http2) {
    tls_config.set_alpn("h2");
  } else {
    tls_config.set_alpn("h2,http/1.1"); // Prefer HTTP/2
  }

  std::printf("TLS handshake...\n");
  evring::tls_handshake_machine handshake{socket, *ring, tls_config, url.host.c_str()};
  auto tls_state = evring::run(handshake, *ring);

  if (!tls_state.ok()) {
    std::fprintf(stderr, "TLS handshake failed: %s\n", tls_state.error_message.c_str());
    return 1;
  }

  auto tls_conn = tls_state.take_context();
  std::printf("TLS: %s, cipher: %s\n", tls_conn.version(), tls_conn.cipher());

  // Check ALPN result
  const char* alpn = tls_conn.alpn_selected();
  if (alpn) {
    std::printf("ALPN: %s\n", alpn);
  }

  // Route to appropriate HTTP version
  bool use_http2 = false;
  if (force_http2) {
    use_http2 = true;
  } else if (force_http1) {
    use_http2 = false;
  } else if (alpn && std::strcmp(alpn, "h2") == 0) {
    use_http2 = true;
  }

  if (use_http2) {
    do_http2_request(*ring, url, socket, tls_conn);
  } else {
    do_http1_request(*ring, url, socket, tls_conn);
  }

  // Clean up
  ring->enqueue(evring::operation::make_close(socket));
  ring->submit_and_wait(1);

  return 0;
}
