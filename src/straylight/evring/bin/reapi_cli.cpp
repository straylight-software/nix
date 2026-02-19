// reapi_cli.cpp - CLI for REAPI CAS operations
//
// Usage:
//   reapi_cli <host:port> upload <file>...
//   reapi_cli <host:port> download <hash>/<size> [output_file]
//   reapi_cli <host:port> exists <hash>/<size>...
//   reapi_cli <host:port> missing <hash>/<size>...
//
// Examples:
//   reapi_cli localhost:50051 upload /etc/passwd
//   reapi_cli localhost:50051 download abc123.../1234 out.bin
//   reapi_cli localhost:50051 exists abc123.../1234 def456.../5678

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "straylight/evring/evring.h"
#include "straylight/evring/http2.h"
#include "straylight/evring/reapi.h"
#include "straylight/evring/tls.h"

using namespace evring;

namespace {

// Parse host:port
std::pair<std::string, std::uint16_t> parse_endpoint(const char* endpoint) {
  std::string s(endpoint);
  auto pos = s.rfind(':');
  if (pos == std::string::npos) {
    return {s, 50051}; // default gRPC port
  }
  return {s.substr(0, pos), static_cast<std::uint16_t>(std::stoi(s.substr(pos + 1)))};
}

// Parse digest string "hash/size"
std::optional<reapi_digest> parse_digest(const char* str) {
  std::string s(str);
  auto pos = s.find('/');
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  reapi_digest d;
  d.hash = s.substr(0, pos);
  d.size = std::stoll(s.substr(pos + 1));
  return d;
}

// Read entire file into bytes
std::optional<std::vector<std::byte>> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    return std::nullopt;
  }
  auto size = f.tellg();
  f.seekg(0);
  std::vector<std::byte> data(size);
  f.read(reinterpret_cast<char*>(data.data()), size);
  return data;
}

// Write bytes to file
bool write_file(const char* path, std::span<const std::byte> data) {
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  f.write(reinterpret_cast<const char*>(data.data()), data.size());
  return true;
}

// TCP connect helper
int tcp_connect(const std::string& host, std::uint16_t port) {
  struct addrinfo hints{}, *res;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  std::string port_str = std::to_string(port);
  if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0) {
    return -1;
  }

  int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0) {
    freeaddrinfo(res);
    return -1;
  }

  if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
    close(fd);
    freeaddrinfo(res);
    return -1;
  }

  freeaddrinfo(res);
  return fd;
}

void usage() {
  std::cerr << R"(Usage:
  reapi_cli <host:port> upload <file>...
  reapi_cli <host:port> download <hash>/<size> [output_file]
  reapi_cli <host:port> exists <hash>/<size>...
  reapi_cli <host:port> missing <hash>/<size>...
  reapi_cli <host:port> --insecure <command> ...

Options:
  --insecure    Skip TLS verification (for local testing)

Examples:
  reapi_cli localhost:50051 --insecure upload /etc/passwd
  reapi_cli localhost:50051 --insecure download abc123.../1234
  reapi_cli cas.example.com:443 exists abc123.../1234
)";
}

struct cli_context {
  std::unique_ptr<ring> ring;
  int socket_fd{-1};
  handle socket_handle;
  std::unique_ptr<tls_client_config> tls_config;
  std::unique_ptr<tls_connection> tls_conn;
  http2_session session;
  std::string instance_name;
  bool insecure{false};

  ~cli_context() {
    if (socket_fd >= 0) {
      close(socket_fd);
    }
  }

  bool connect(const std::string& host, std::uint16_t port) {
    // Create io_uring
    ring = make_io_uring_ring(256);
    if (!ring) {
      std::cerr << "error: failed to create io_uring\n";
      return false;
    }

    // TCP connect
    std::cerr << "connecting to " << host << ":" << port << "...\n";
    socket_fd = tcp_connect(host, port);
    if (socket_fd < 0) {
      std::cerr << "error: TCP connect failed\n";
      return false;
    }
    socket_handle = handle{static_cast<std::uint32_t>(socket_fd)};
    std::cerr << "TCP connected\n";

    // TLS setup
    if (insecure) {
      tls_config = std::make_unique<tls_client_config>(tls_client_config::create_insecure());
    } else {
      tls_config = std::make_unique<tls_client_config>(tls_client_config::create_default());
    }
    tls_config->set_alpn("h2");

    // TLS handshake
    std::cerr << "TLS handshake...\n";
    tls_handshake_machine hs{socket_handle, *ring, *tls_config, host};
    auto hs_state = run(hs, *ring);
    if (!hs_state.ok()) {
      std::cerr << "error: TLS handshake failed: " << hs_state.error_message << "\n";
      return false;
    }
    tls_conn = std::make_unique<tls_connection>(hs_state.take_context());
    std::cerr << "TLS connected: " << tls_conn->version() << "\n";

    // HTTP/2 setup
    if (!session.init_client()) {
      std::cerr << "error: HTTP/2 session init failed\n";
      return false;
    }

    std::cerr << "HTTP/2 connection...\n";
    http2_connection_machine conn{session, *tls_conn, socket_handle};
    auto conn_state = run(conn, *ring);
    if (!conn_state.ok()) {
      std::cerr << "error: HTTP/2 connection failed: " << conn_state.error_message << "\n";
      return false;
    }
    std::cerr << "HTTP/2 connected\n";

    return true;
  }
};

int cmd_upload(cli_context& ctx, int argc, char** argv) {
  if (argc < 1) {
    std::cerr << "error: upload requires at least one file\n";
    return 1;
  }

  std::vector<batch_update_blob> blobs;

  for (int i = 0; i < argc; ++i) {
    auto data = read_file(argv[i]);
    if (!data) {
      std::cerr << "error: cannot read file: " << argv[i] << "\n";
      return 1;
    }

    auto digest = reapi_digest_from_bytes(*data);
    std::cerr << "uploading " << argv[i] << " (" << data->size() << " bytes)\n";
    std::cerr << "  digest: " << digest.hash << "/" << digest.size << "\n";

    blobs.push_back({digest, std::move(*data)});
  }

  // Check which are missing first
  std::vector<reapi_digest> digests;
  for (const auto& b : blobs) {
    digests.push_back(b.digest);
  }

  find_missing_blobs_machine finder{ctx.session, *ctx.tls_conn, ctx.socket_handle,
                                    ctx.instance_name, digests};
  auto find_state = run(finder, *ctx.ring);

  if (!find_state.ok()) {
    std::cerr << "error: FindMissingBlobs failed: " << find_state.status_message << "\n";
    return 1;
  }

  std::cerr << "missing: " << find_state.missing_digests.size() << " of " << digests.size() << "\n";

  if (find_state.missing_digests.empty()) {
    std::cerr << "all blobs already exist\n";
    return 0;
  }

  // Filter to only missing blobs
  std::vector<batch_update_blob> to_upload;
  for (auto& b : blobs) {
    for (const auto& missing : find_state.missing_digests) {
      if (b.digest == missing) {
        to_upload.push_back(std::move(b));
        break;
      }
    }
  }

  // Upload
  batch_update_blobs_machine uploader{ctx.session, *ctx.tls_conn, ctx.socket_handle,
                                      ctx.instance_name, std::move(to_upload)};
  auto upload_state = run(uploader, *ctx.ring);

  if (!upload_state.ok()) {
    std::cerr << "error: BatchUpdateBlobs failed: " << upload_state.status_message << "\n";
    return 1;
  }

  std::cerr << "uploaded " << upload_state.results.size() << " blobs\n";
  for (const auto& r : upload_state.results) {
    std::cout << r.digest.hash << "/" << r.digest.size;
    if (!r.status.ok()) {
      std::cout << " ERROR: " << r.status.message;
    }
    std::cout << "\n";
  }

  return 0;
}

int cmd_download(cli_context& ctx, int argc, char** argv) {
  if (argc < 1) {
    std::cerr << "error: download requires a digest\n";
    return 1;
  }

  auto digest = parse_digest(argv[0]);
  if (!digest) {
    std::cerr << "error: invalid digest format (expected hash/size)\n";
    return 1;
  }

  const char* output = (argc > 1) ? argv[1] : nullptr;

  std::cerr << "downloading " << digest->hash << "/" << digest->size << "...\n";

  // Try batch read first (simpler for small blobs)
  batch_read_blobs_machine reader{
      ctx.session, *ctx.tls_conn, ctx.socket_handle, ctx.instance_name, {*digest}};
  auto read_state = run(reader, *ctx.ring);

  if (!read_state.ok()) {
    std::cerr << "error: BatchReadBlobs failed: " << read_state.status_message << "\n";
    return 1;
  }

  if (read_state.results.empty()) {
    std::cerr << "error: no results returned\n";
    return 1;
  }

  const auto& result = read_state.results[0];
  if (!result.status.ok()) {
    std::cerr << "error: " << result.status.message << "\n";
    return 1;
  }

  std::cerr << "downloaded " << result.data.size() << " bytes\n";

  if (output) {
    if (!write_file(output, result.data)) {
      std::cerr << "error: cannot write to " << output << "\n";
      return 1;
    }
    std::cerr << "written to " << output << "\n";
  } else {
    // Write to stdout
    std::cout.write(reinterpret_cast<const char*>(result.data.data()), result.data.size());
  }

  return 0;
}

int cmd_exists(cli_context& ctx, int argc, char** argv) {
  if (argc < 1) {
    std::cerr << "error: exists requires at least one digest\n";
    return 1;
  }

  std::vector<reapi_digest> digests;
  for (int i = 0; i < argc; ++i) {
    auto d = parse_digest(argv[i]);
    if (!d) {
      std::cerr << "error: invalid digest: " << argv[i] << "\n";
      return 1;
    }
    digests.push_back(*d);
  }

  find_missing_blobs_machine finder{ctx.session, *ctx.tls_conn, ctx.socket_handle,
                                    ctx.instance_name, digests};
  auto state = run(finder, *ctx.ring);

  if (!state.ok()) {
    std::cerr << "error: FindMissingBlobs failed: " << state.status_message << "\n";
    return 1;
  }

  // Build set of missing
  std::set<std::string> missing;
  for (const auto& d : state.missing_digests) {
    missing.insert(d.hash);
  }

  int exit_code = 0;
  for (const auto& d : digests) {
    bool exists = missing.find(d.hash) == missing.end();
    std::cout << d.hash << "/" << d.size << ": " << (exists ? "EXISTS" : "MISSING") << "\n";
    if (!exists) {
      exit_code = 1;
    }
  }

  return exit_code;
}

int cmd_missing(cli_context& ctx, int argc, char** argv) {
  if (argc < 1) {
    std::cerr << "error: missing requires at least one digest\n";
    return 1;
  }

  std::vector<reapi_digest> digests;
  for (int i = 0; i < argc; ++i) {
    auto d = parse_digest(argv[i]);
    if (!d) {
      std::cerr << "error: invalid digest: " << argv[i] << "\n";
      return 1;
    }
    digests.push_back(*d);
  }

  find_missing_blobs_machine finder{ctx.session, *ctx.tls_conn, ctx.socket_handle,
                                    ctx.instance_name, digests};
  auto state = run(finder, *ctx.ring);

  if (!state.ok()) {
    std::cerr << "error: FindMissingBlobs failed: " << state.status_message << "\n";
    return 1;
  }

  for (const auto& d : state.missing_digests) {
    std::cout << d.hash << "/" << d.size << "\n";
  }

  return 0;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    usage();
    return 1;
  }

  auto [host, port] = parse_endpoint(argv[1]);

  int arg_offset = 2;
  bool insecure = false;

  if (std::strcmp(argv[arg_offset], "--insecure") == 0) {
    insecure = true;
    ++arg_offset;
    if (argc < arg_offset + 1) {
      usage();
      return 1;
    }
  }

  const char* cmd = argv[arg_offset];
  ++arg_offset;

  cli_context ctx;
  ctx.insecure = insecure;
  ctx.instance_name = ""; // Empty for default instance

  if (!ctx.connect(host, port)) {
    return 1;
  }

  int remaining_argc = argc - arg_offset;
  char** remaining_argv = argv + arg_offset;

  if (std::strcmp(cmd, "upload") == 0) {
    return cmd_upload(ctx, remaining_argc, remaining_argv);
  } else if (std::strcmp(cmd, "download") == 0) {
    return cmd_download(ctx, remaining_argc, remaining_argv);
  } else if (std::strcmp(cmd, "exists") == 0) {
    return cmd_exists(ctx, remaining_argc, remaining_argv);
  } else if (std::strcmp(cmd, "missing") == 0) {
    return cmd_missing(ctx, remaining_argc, remaining_argv);
  } else {
    std::cerr << "error: unknown command: " << cmd << "\n";
    usage();
    return 1;
  }
}
