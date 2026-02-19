// file_downloader.cpp - Download a file over HTTPS with progress
//
// This example demonstrates:
// - HTTP/2 streaming download with progress reporting
// - File I/O combined with network I/O
// - Resource cleanup on error
//
// Usage: file_downloader <url> <output_file>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "straylight/evring/evring.h"
#include "straylight/evring/http1.h"
#include "straylight/evring/http2.h"
#include "straylight/evring/tls.h"

// ============================================================================
// URL parsing (same as http_get.cpp)
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
// Progress display
// ============================================================================

void print_progress(std::size_t downloaded, std::size_t total) {
  if (total > 0) {
    int percent = static_cast<int>((downloaded * 100) / total);
    int bar_width = 40;
    int filled = (percent * bar_width) / 100;

    std::printf("\r[");
    for (int i = 0; i < bar_width; ++i) {
      if (i < filled)
        std::printf("=");
      else if (i == filled)
        std::printf(">");
      else
        std::printf(" ");
    }
    std::printf("] %3d%% (%zu / %zu bytes)", percent, downloaded, total);
  } else {
    std::printf("\rDownloaded %zu bytes...", downloaded);
  }
  std::fflush(stdout);
}

// ============================================================================
// Download state machine
// ============================================================================

struct download_state {
  enum class phase { initial, opening_file, writing_chunk, done, error };

  phase current_phase{phase::initial};

  // File handle
  evring::handle file_handle;

  // Download data
  std::vector<std::byte> data;
  std::size_t write_offset{0};
  std::size_t total_size{0};

  // Error info
  std::string error_message;
};

class download_machine {
public:
  using state_type = download_state;

  download_machine(const char* output_path, std::vector<std::byte> data, std::size_t total_size)
      : output_path_(output_path), data_(std::move(data)), total_size_(total_size) {}

  [[nodiscard]] auto initial() const -> state_type {
    state_type s;
    s.data = data_;
    s.total_size = total_size_;
    return s;
  }

  [[nodiscard]] auto step(state_type s, const evring::event& e) const
      -> evring::step_result<state_type> {
    std::vector<evring::operation> ops;

    switch (s.current_phase) {
      case state_type::phase::initial:
        s.current_phase = state_type::phase::opening_file;
        ops.push_back(
            evring::operation::make_open(output_path_, O_WRONLY | O_CREAT | O_TRUNC, 0644));
        break;

      case state_type::phase::opening_file:
        if (!e.ok()) {
          s.current_phase = state_type::phase::error;
          s.error_message = "Failed to create output file";
        } else {
          s.file_handle = e.resource_handle;
          if (s.data.empty()) {
            // No data to write, close file
            s.current_phase = state_type::phase::done;
            ops.push_back(evring::operation::make_close(s.file_handle));
          } else {
            // Write data in chunks
            s.current_phase = state_type::phase::writing_chunk;
            std::size_t chunk_size = std::min(s.data.size() - s.write_offset, std::size_t{65536});
            ops.push_back(evring::operation::make_write(
                s.file_handle,
                std::span<const std::byte>{s.data.data() + s.write_offset, chunk_size},
                static_cast<std::int64_t>(s.write_offset)));
          }
        }
        break;

      case state_type::phase::writing_chunk:
        if (!e.ok()) {
          s.current_phase = state_type::phase::error;
          s.error_message = "Write error";
          ops.push_back(evring::operation::make_close(s.file_handle));
        } else {
          s.write_offset += static_cast<std::size_t>(e.result);
          print_progress(s.write_offset, s.data.size());

          if (s.write_offset >= s.data.size()) {
            // Done writing
            s.current_phase = state_type::phase::done;
            ops.push_back(evring::operation::make_close(s.file_handle));
          } else {
            // Write next chunk
            std::size_t chunk_size = std::min(s.data.size() - s.write_offset, std::size_t{65536});
            ops.push_back(evring::operation::make_write(
                s.file_handle,
                std::span<const std::byte>{s.data.data() + s.write_offset, chunk_size},
                static_cast<std::int64_t>(s.write_offset)));
          }
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

private:
  const char* output_path_;
  std::vector<std::byte> data_;
  std::size_t total_size_;
};

// ============================================================================
// HTTP download using HTTP/1.1 (simpler for download)
// ============================================================================

auto do_http_download(evring::ring& ring, const parsed_url& url, evring::handle socket,
                      evring::tls_connection& tls) -> std::pair<bool, std::vector<std::byte>> {
  evring::http1_request req;
  req.method = evring::http1_method::get;
  req.path = url.path;
  req.headers = {
      {"Host", url.host},
      {"User-Agent", "evring-downloader/1.0"},
      {"Accept", "*/*"},
      {"Connection", "close"},
  };

  evring::http1_tls_client_machine client{tls, socket, req};
  auto state = evring::run(client, ring);

  if (state.current_phase == evring::http1_tls_client_state::phase::error) {
    std::fprintf(stderr, "HTTP error: %s\n", state.error_message.c_str());
    return {false, {}};
  }

  if (state.response.status_code >= 300) {
    std::fprintf(stderr, "HTTP %d: %s\n", state.response.status_code,
                 state.response.status_message.c_str());
    return {false, {}};
  }

  // Convert string body to bytes
  std::vector<std::byte> data;
  data.resize(state.response.body.size());
  std::memcpy(data.data(), state.response.body.data(), state.response.body.size());

  return {true, std::move(data)};
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
  if (argc != 3) {
    std::fprintf(stderr, "Usage: %s <url> <output_file>\n", argv[0]);
    std::fprintf(stderr, "\nExample:\n");
    std::fprintf(stderr, "  %s https://httpbin.org/bytes/1024 data.bin\n", argv[0]);
    return 1;
  }

  const char* url_arg = argv[1];
  const char* output_path = argv[2];

  // Parse URL
  auto url = parse_url(url_arg);
  if (!url.valid) {
    std::fprintf(stderr, "Invalid URL: %s\n", url_arg);
    return 1;
  }

  std::printf("Downloading: %s://%s:%s%s\n", url.scheme.c_str(), url.host.c_str(), url.port.c_str(),
              url.path.c_str());
  std::printf("Output: %s\n\n", output_path);

  // Create ring
  auto ring = evring::make_io_uring_ring(256);
  if (!ring) {
    std::fprintf(stderr, "Failed to create io_uring\n");
    return 1;
  }

  // TCP connect
  std::printf("Connecting...\n");
  auto socket = tcp_connect(*ring, url.host.c_str(), url.port.c_str());
  if (!socket.valid()) {
    std::fprintf(stderr, "TCP connect failed\n");
    return 1;
  }

  // TLS handshake
  auto tls_config = evring::tls_client_config::create_default();
  tls_config.set_alpn("http/1.1"); // Use HTTP/1.1 for simplicity

  std::printf("TLS handshake...\n");
  evring::tls_handshake_machine handshake{socket, *ring, tls_config, url.host.c_str()};
  auto tls_state = evring::run(handshake, *ring);

  if (!tls_state.ok()) {
    std::fprintf(stderr, "TLS handshake failed: %s\n", tls_state.error_message.c_str());
    return 1;
  }

  auto tls_conn = tls_state.take_context();
  std::printf("TLS: %s\n", tls_conn.version());

  // Download
  std::printf("Downloading...\n");
  auto [success, data] = do_http_download(*ring, url, socket, tls_conn);

  // Close socket
  ring->enqueue(evring::operation::make_close(socket));
  ring->submit_and_wait(1);

  if (!success) {
    return 1;
  }

  std::printf("Downloaded %zu bytes\n", data.size());

  // Write to file
  std::printf("Saving to %s...\n", output_path);
  download_machine downloader{output_path, std::move(data), 0};
  auto dl_state = evring::run(downloader, *ring);

  std::printf("\n");

  if (dl_state.current_phase == download_state::phase::error) {
    std::fprintf(stderr, "Error: %s\n", dl_state.error_message.c_str());
    return 1;
  }

  std::printf("Successfully saved %zu bytes to %s\n", dl_state.write_offset, output_path);
  return 0;
}
