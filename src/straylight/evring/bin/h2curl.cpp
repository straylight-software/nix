// h2curl.cpp - HTTP/2 curl-style CLI using evring
//
// Usage: h2curl [options] <url>
//
// Examples:
//   h2curl https://nghttp2.org/
//   h2curl -v https://www.google.com/
//   h2curl -H "Accept: application/json" https://api.example.com/data

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
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

struct options {
  std::string url;
  std::string method = "GET";
  std::vector<std::pair<std::string, std::string>> headers;
  bool verbose = false;
  bool include_headers = false;
  bool insecure = false;
  bool head_only = false;
};

struct parsed_url {
  std::string scheme;
  std::string host;
  std::string port;
  std::string path;
  bool valid = false;
};

auto parse_url(std::string_view url) -> parsed_url {
  parsed_url result;

  // Check scheme
  if (url.starts_with("https://")) {
    result.scheme = "https";
    url.remove_prefix(8);
  } else if (url.starts_with("http://")) {
    result.scheme = "http";
    url.remove_prefix(7);
  } else {
    // Assume https
    result.scheme = "https";
  }

  // Find path
  auto path_pos = url.find('/');
  std::string_view authority;
  if (path_pos != std::string_view::npos) {
    authority = url.substr(0, path_pos);
    result.path = std::string(url.substr(path_pos));
  } else {
    authority = url;
    result.path = "/";
  }

  // Parse host:port
  auto port_pos = authority.find(':');
  if (port_pos != std::string_view::npos) {
    result.host = std::string(authority.substr(0, port_pos));
    result.port = std::string(authority.substr(port_pos + 1));
  } else {
    result.host = std::string(authority);
    result.port = (result.scheme == "https") ? "443" : "80";
  }

  result.valid = !result.host.empty();
  return result;
}

auto parse_header(std::string_view header) -> std::pair<std::string, std::string> {
  auto colon = header.find(':');
  if (colon == std::string_view::npos) {
    return {std::string(header), ""};
  }

  auto name = header.substr(0, colon);
  auto value = header.substr(colon + 1);

  // Trim leading whitespace from value
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }

  return {std::string(name), std::string(value)};
}

void print_usage(const char* prog) {
  std::fprintf(stderr, R"(Usage: %s [options] <url>

HTTP/2 client using evring state machines

Options:
  -X, --request <method>   HTTP method (GET, POST, HEAD, etc.)
  -H, --header <header>    Add header (e.g., "Accept: application/json")
  -v, --verbose            Show request/response details
  -i, --include            Include response headers in output
  -k, --insecure           Skip TLS certificate verification
  -I, --head               HEAD request (show headers only)
  -h, --help               Show this help

Examples:
  %s https://nghttp2.org/
  %s -v https://www.google.com/
  %s -H "User-Agent: myclient" https://example.com/

)",
               prog, prog, prog, prog);
}

auto parse_args(int argc, char* argv[]) -> options {
  options opts;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      std::exit(0);
    } else if (arg == "-v" || arg == "--verbose") {
      opts.verbose = true;
    } else if (arg == "-i" || arg == "--include") {
      opts.include_headers = true;
    } else if (arg == "-k" || arg == "--insecure") {
      opts.insecure = true;
    } else if (arg == "-I" || arg == "--head") {
      opts.head_only = true;
      opts.method = "HEAD";
    } else if (arg == "-X" || arg == "--request") {
      if (i + 1 < argc) {
        opts.method = argv[++i];
      } else {
        std::fprintf(stderr, "Error: %s requires an argument\n", arg.data());
        std::exit(1);
      }
    } else if (arg == "-H" || arg == "--header") {
      if (i + 1 < argc) {
        opts.headers.push_back(parse_header(argv[++i]));
      } else {
        std::fprintf(stderr, "Error: %s requires an argument\n", arg.data());
        std::exit(1);
      }
    } else if (arg.starts_with("-")) {
      std::fprintf(stderr, "Error: Unknown option: %s\n", arg.data());
      print_usage(argv[0]);
      std::exit(1);
    } else {
      opts.url = std::string(arg);
    }
  }

  if (opts.url.empty()) {
    std::fprintf(stderr, "Error: No URL specified\n");
    print_usage(argv[0]);
    std::exit(1);
  }

  return opts;
}

auto tcp_connect(evring::ring& ring, const char* host, const char* port, bool verbose)
    -> evring::handle {
  if (verbose) {
    std::fprintf(stderr, "* Resolving %s...\n", host);
  }

  struct addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* result = nullptr;
  int gai_result = getaddrinfo(host, port, &hints, &result);
  if (gai_result != 0) {
    std::fprintf(stderr, "* Failed to resolve host: %s\n", gai_strerror(gai_result));
    return evring::handle::invalid();
  }

  // Get IP address for display
  char ip_str[INET_ADDRSTRLEN];
  auto* addr_in = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
  inet_ntop(AF_INET, &addr_in->sin_addr, ip_str, sizeof(ip_str));

  if (verbose) {
    std::fprintf(stderr, "* Connecting to %s:%s...\n", ip_str, port);
  }

  ring.enqueue(evring::operation::make_socket(AF_INET, SOCK_STREAM, 0, SOCK_CLOEXEC));
  auto events = ring.submit_and_wait(1);
  if (!events[0].ok()) {
    std::fprintf(stderr, "* Socket creation failed: %d\n", events[0].error_code());
    freeaddrinfo(result);
    return evring::handle::invalid();
  }

  evring::handle socket_handle = events[0].resource_handle;

  ring.enqueue(evring::operation::make_connect(socket_handle, result->ai_addr,
                                               static_cast<std::uint32_t>(result->ai_addrlen)));
  events = ring.submit_and_wait(1);
  freeaddrinfo(result);

  if (!events[0].ok()) {
    std::fprintf(stderr, "* Connection failed: %d\n", events[0].error_code());
    ring.enqueue(evring::operation::make_close(socket_handle));
    ring.submit_and_wait(1);
    return evring::handle::invalid();
  }

  if (verbose) {
    std::fprintf(stderr, "* Connected to %s:%s\n", ip_str, port);
  }

  return socket_handle;
}

} // namespace

int main(int argc, char* argv[]) {
  auto opts = parse_args(argc, argv);
  auto url = parse_url(opts.url);

  if (!url.valid) {
    std::fprintf(stderr, "Error: Invalid URL: %s\n", opts.url.c_str());
    return 1;
  }

  if (url.scheme != "https") {
    std::fprintf(stderr, "Error: Only HTTPS is supported (HTTP/2 requires TLS)\n");
    return 1;
  }

  if (opts.verbose) {
    std::fprintf(stderr, "* URL: %s\n", opts.url.c_str());
    std::fprintf(stderr, "* Host: %s\n", url.host.c_str());
    std::fprintf(stderr, "* Port: %s\n", url.port.c_str());
    std::fprintf(stderr, "* Path: %s\n", url.path.c_str());
  }

  // Create io_uring ring
  auto ring = evring::make_io_uring_ring(64);

  // TCP connect
  evring::handle socket = tcp_connect(*ring, url.host.c_str(), url.port.c_str(), opts.verbose);
  if (!socket.valid()) {
    return 1;
  }

  // TLS handshake with ALPN h2
  if (opts.verbose) {
    std::fprintf(stderr, "* TLS handshake...\n");
  }

  auto tls_config = opts.insecure ? evring::tls_client_config::create_insecure()
                                  : evring::tls_client_config::create_default();

  if (!tls_config.set_alpn("h2")) {
    std::fprintf(stderr, "Error: Failed to set ALPN\n");
    return 1;
  }

  evring::tls_handshake_machine tls_hs{socket, *ring, tls_config, url.host};
  auto tls_state = evring::run(tls_hs, *ring);

  if (!tls_state.ok()) {
    std::fprintf(stderr, "* TLS handshake failed: %s\n", tls_state.error_message.c_str());
    ring->enqueue(evring::operation::make_close(socket));
    ring->submit_and_wait(1);
    return 1;
  }

  auto tls_conn = tls_state.take_context();

  if (opts.verbose) {
    std::fprintf(stderr, "* TLS version: %s\n", tls_conn.version());
    std::fprintf(stderr, "* Cipher: %s (%d bits)\n", tls_conn.cipher(), tls_conn.cipher_strength());
    const char* alpn = tls_conn.alpn_selected();
    std::fprintf(stderr, "* ALPN: %s\n", alpn ? alpn : "(none)");
  }

  // Verify ALPN is h2
  const char* alpn = tls_conn.alpn_selected();
  if (!alpn || std::strcmp(alpn, "h2") != 0) {
    std::fprintf(stderr, "Error: Server does not support HTTP/2 (ALPN: %s)\n",
                 alpn ? alpn : "none");
    ring->enqueue(evring::operation::make_close(socket));
    ring->submit_and_wait(1);
    return 1;
  }

  // HTTP/2 connection setup
  if (opts.verbose) {
    std::fprintf(stderr, "* HTTP/2 connection...\n");
  }

  evring::http2_session session;
  if (!session.init_client()) {
    std::fprintf(stderr, "Error: Failed to initialize HTTP/2 session\n");
    return 1;
  }

  evring::http2_connection_machine conn_machine{session, tls_conn, socket};
  auto conn_state = evring::run(conn_machine, *ring);

  if (!conn_state.ok()) {
    std::fprintf(stderr, "* HTTP/2 connection failed: %s\n", conn_state.error_message.c_str());
    ring->enqueue(evring::operation::make_close(socket));
    ring->submit_and_wait(1);
    return 1;
  }

  if (opts.verbose) {
    std::fprintf(stderr, "* HTTP/2 connection established\n");
  }

  // Build request
  evring::http2_request req;
  req.method = opts.method;
  req.scheme = url.scheme;
  req.authority = url.host;
  if (url.port != "443") {
    req.authority += ":" + url.port;
  }
  req.path = url.path;

  // Add default headers
  req.headers.push_back({"user-agent", "h2curl/1.0 (evring)"});
  req.headers.push_back({"accept", "*/*"});

  // Add custom headers
  for (const auto& [name, value] : opts.headers) {
    req.headers.push_back({name, value});
  }

  if (opts.verbose) {
    std::fprintf(stderr, "> %s %s\n", req.method.c_str(), req.path.c_str());
    std::fprintf(stderr, "> :authority: %s\n", req.authority.c_str());
    for (const auto& h : req.headers) {
      std::fprintf(stderr, "> %s: %s\n", h.name.c_str(), h.value.c_str());
    }
    std::fprintf(stderr, ">\n");
  }

  // Send request
  evring::http2_request_machine req_machine{session, tls_conn, socket, req};
  auto req_state = evring::run(req_machine, *ring);

  // Clean up socket
  ring->enqueue(evring::operation::make_close(socket));
  ring->submit_and_wait(1);

  if (!req_state.ok()) {
    std::fprintf(stderr, "Error: Request failed: %s\n", req_state.error_message.c_str());
    return 1;
  }

  // Output response
  if (opts.verbose || opts.include_headers) {
    std::fprintf(stderr, "< HTTP/2 %d\n", req_state.response.status_code);
    for (const auto& h : req_state.response.headers) {
      std::fprintf(stderr, "< %s: %s\n", h.name.c_str(), h.value.c_str());
    }
    std::fprintf(stderr, "<\n");
  }

  // Output body (unless HEAD request)
  if (!opts.head_only && !req_state.response.body.empty()) {
    std::fwrite(req_state.response.body.data(), 1, req_state.response.body.size(), stdout);
    // Add newline if body doesn't end with one
    if (req_state.response.body.back() != std::byte{'\n'}) {
      std::putchar('\n');
    }
  }

  // Exit code based on HTTP status
  if (req_state.response.status_code >= 400) {
    return 22; // curl uses 22 for HTTP errors
  }

  return 0;
}
