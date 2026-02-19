#pragma once

/// http3.h - HTTP/3 state machines using ngtcp2 + nghttp3
///
/// Provides replayable HTTP/3 connections by modeling the protocol
/// as state machines that yield io_uring operations for UDP I/O.
///
/// Key concepts:
/// - ngtcp2 handles QUIC transport (UDP, TLS 1.3, streams, congestion control)
/// - nghttp3 handles HTTP/3 framing (QPACK, request/response)
/// - QUIC is UDP-based, so we use recvmsg/sendmsg operations
/// - Timer management for retransmission and idle timeout
///
/// Example usage:
/// @code
///   auto ring = evring::make_io_uring_ring(256);
///
///   // Create QUIC connection to server
///   http3_client_config config;
///   config.server_name = "cloudflare.com";
///   config.port = 443;
///
///   http3_client_machine client{config};
///   auto conn_state = evring::run(client, *ring);
///
///   if (conn_state.connected()) {
///     // Submit HTTP/3 request
///     http3_request req;
///     req.method = "GET";
///     req.authority = "cloudflare.com";
///     req.path = "/";
///
///     http3_request_machine request{conn_state.session(), req};
///     auto resp_state = evring::run(request, *ring);
///     // resp_state.response.status_code, .headers, .body
///   }
/// @endcode

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <sys/socket.h>

#include "straylight/evring/event.h"
#include "straylight/evring/handle.h"
#include "straylight/evring/machine.h"

// Forward declare ngtcp2 types
struct ngtcp2_conn;
struct ngtcp2_cid;
struct ngtcp2_path;
struct ngtcp2_pkt_info;

// Forward declare nghttp3 types
struct nghttp3_conn;

// Forward declare OpenSSL types
typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;

namespace evring {

// ============================================================================
// HTTP/3 constants
// ============================================================================

/// Default QUIC max UDP payload size
inline constexpr std::size_t http3_default_max_udp_payload = 1350;

/// Default max stream data (per stream)
inline constexpr std::uint64_t http3_default_max_stream_data = 256 * 1024;

/// Default max data (connection level)
inline constexpr std::uint64_t http3_default_max_data = 1 * 1024 * 1024;

/// Default max bidirectional streams
inline constexpr std::uint64_t http3_default_max_streams_bidi = 100;

/// Default idle timeout (seconds)
inline constexpr std::uint64_t http3_default_idle_timeout = 30;

/// ALPN for HTTP/3
inline constexpr const char* http3_alpn = "\x02h3";

/// Maximum UDP packet buffer size
inline constexpr std::size_t http3_max_pktlen = 1500;

// ============================================================================
// HTTP/3 error codes
// ============================================================================

enum class http3_error_code : std::uint32_t {
  no_error = 0x0100,
  general_protocol_error = 0x0101,
  internal_error = 0x0102,
  stream_creation_error = 0x0103,
  closed_critical_stream = 0x0104,
  frame_unexpected = 0x0105,
  frame_error = 0x0106,
  excessive_load = 0x0107,
  id_error = 0x0108,
  settings_error = 0x0109,
  missing_settings = 0x010a,
  request_rejected = 0x010b,
  request_cancelled = 0x010c,
  request_incomplete = 0x010d,
  message_error = 0x010e,
  connect_error = 0x010f,
  version_fallback = 0x0110,

  // Custom error codes for evring
  quic_error = 0x1000,
  tls_error = 0x1001,
  connection_closed = 0x1002,
  timeout = 0x1003,
};

/// Convert error code to string
auto http3_error_string(http3_error_code error) noexcept -> std::string_view;

// ============================================================================
// HTTP/3 header
// ============================================================================

struct http3_header {
  std::string name;
  std::string value;
};

using http3_headers = std::vector<http3_header>;

// ============================================================================
// HTTP/3 request
// ============================================================================

struct http3_request {
  std::string method = "GET";
  std::string scheme = "https";
  std::string authority; // host:port
  std::string path = "/";
  http3_headers headers;
  std::vector<std::byte> body;

  /// Get all headers including pseudo-headers (for submission)
  [[nodiscard]] auto all_headers() const -> http3_headers;
};

// ============================================================================
// HTTP/3 response
// ============================================================================

struct http3_response {
  int status_code = 0;
  http3_headers headers;
  std::vector<std::byte> body;

  [[nodiscard]] auto ok() const noexcept -> bool { return status_code >= 200 && status_code < 300; }

  /// Get header value by name (case-insensitive)
  [[nodiscard]] auto get_header(std::string_view name) const -> std::string_view;
};

// ============================================================================
// HTTP/3 settings
// ============================================================================

struct http3_settings {
  std::uint64_t max_field_section_size = 16384;
  std::uint64_t qpack_max_dtable_capacity = 4096;
  std::uint64_t qpack_blocked_streams = 100;
};

// ============================================================================
// QUIC connection ID
// ============================================================================

struct quic_cid {
  std::array<std::uint8_t, 20> data{};
  std::size_t len = 0;

  static auto generate() -> quic_cid;
};

// ============================================================================
// HTTP/3 session (wraps ngtcp2_conn + nghttp3_conn)
// ============================================================================

/// HTTP/3 session - manages QUIC connection and HTTP/3 layer
///
/// This class wraps both ngtcp2 (QUIC transport) and nghttp3 (HTTP/3 framing).
/// It handles:
/// - QUIC handshake and connection establishment
/// - Stream management (bidirectional and unidirectional)
/// - HTTP/3 request/response framing
/// - Timer management for retransmission
class http3_session {
public:
  http3_session();
  ~http3_session();

  // Move-only
  http3_session(http3_session&& other) noexcept;
  http3_session& operator=(http3_session&& other) noexcept;
  http3_session(const http3_session&) = delete;
  http3_session& operator=(const http3_session&) = delete;

  /// Initialize as client
  /// @param server_name SNI hostname
  /// @param local_addr Local socket address
  /// @param remote_addr Remote server address
  /// @param settings HTTP/3 settings
  /// @return true on success
  auto init_client(const char* server_name, const sockaddr* local_addr, socklen_t local_addrlen,
                   const sockaddr* remote_addr, socklen_t remote_addrlen,
                   const http3_settings& settings = {}) -> bool;

  /// Check if session is valid
  [[nodiscard]] auto valid() const noexcept -> bool { return quic_conn_ != nullptr; }

  /// Check if handshake is complete
  [[nodiscard]] auto handshake_complete() const noexcept -> bool;

  /// Submit HTTP/3 request
  /// @return stream ID (>= 0) on success, negative on error
  auto submit_request(const http3_request& req) -> std::int64_t;

  /// Get data to send (QUIC packets)
  /// @param dest Buffer for outgoing packet
  /// @param destlen Max packet size
  /// @return bytes written, 0 if nothing to send, negative on error
  auto write_pkt(std::span<std::byte> dest) -> std::int64_t;

  /// Process received QUIC packet
  /// @param data Received packet data
  /// @return 0 on success, negative on error
  auto read_pkt(std::span<const std::byte> data) -> int;

  /// Handle timer expiry
  auto handle_expiry() -> int;

  /// Get next timeout (nanoseconds from now)
  /// @return timeout in nanoseconds, UINT64_MAX if no timeout
  [[nodiscard]] auto get_timeout() const -> std::uint64_t;

  /// Check if connection wants to write (has pending data)
  [[nodiscard]] auto wants_write() const noexcept -> bool;

  /// Check if connection is in draining state
  [[nodiscard]] auto is_draining() const noexcept -> bool;

  /// Close connection gracefully
  auto close(http3_error_code error = http3_error_code::no_error) -> int;

  /// Get stream response (valid after stream closes)
  auto get_stream_response(std::int64_t stream_id) -> http3_response*;

  /// Check if stream is closed
  [[nodiscard]] auto is_stream_closed(std::int64_t stream_id) const -> bool;

  /// Get last error message
  [[nodiscard]] auto error() const -> const char*;

  // Callbacks for streaming
  using on_headers_callback = std::function<void(std::int64_t stream_id, const http3_headers&)>;
  using on_data_callback = std::function<void(std::int64_t stream_id, std::span<const std::byte>)>;
  using on_stream_close_callback = std::function<void(std::int64_t stream_id, http3_error_code)>;

  void set_on_headers(on_headers_callback cb) { on_headers_ = std::move(cb); }
  void set_on_data(on_data_callback cb) { on_data_ = std::move(cb); }
  void set_on_stream_close(on_stream_close_callback cb) { on_stream_close_ = std::move(cb); }

  /// Get raw ngtcp2 connection (for advanced use)
  [[nodiscard]] auto raw_quic() const noexcept -> ngtcp2_conn* { return quic_conn_; }

  /// Get raw nghttp3 connection (for advanced use)
  [[nodiscard]] auto raw_http3() const noexcept -> nghttp3_conn* { return http3_conn_; }

  // Public handlers for callbacks (called from anonymous namespace callbacks)
  void handle_header(std::int64_t stream_id, std::string_view name, std::string_view value);
  void handle_data(std::int64_t stream_id, std::span<const std::byte> data);
  void handle_stream_close(std::int64_t stream_id, std::uint64_t error_code);

private:
  // QUIC connection
  ngtcp2_conn* quic_conn_ = nullptr;

  // HTTP/3 connection
  nghttp3_conn* http3_conn_ = nullptr;

  // TLS context
  SSL_CTX* ssl_ctx_ = nullptr;
  SSL* ssl_ = nullptr;

  // Connection IDs
  quic_cid scid_; // Source connection ID
  quic_cid dcid_; // Destination connection ID

  // Path (local/remote addresses)
  std::vector<std::byte> local_addr_storage_;
  std::vector<std::byte> remote_addr_storage_;

  // Stream data
  std::unordered_map<std::int64_t, http3_headers> pending_headers_;
  std::unordered_map<std::int64_t, http3_response> stream_responses_;
  std::unordered_map<std::int64_t, http3_error_code> closed_streams_;

  // Callbacks
  on_headers_callback on_headers_;
  on_data_callback on_data_;
  on_stream_close_callback on_stream_close_;

  // Error state
  std::string error_message_;

  // Internal helpers
  auto setup_ssl(const char* server_name) -> bool;
  auto setup_quic(const sockaddr* local_addr, socklen_t local_addrlen, const sockaddr* remote_addr,
                  socklen_t remote_addrlen) -> bool;
  auto setup_http3(const http3_settings& settings) -> bool;
};

// ============================================================================
// HTTP/3 client state
// ============================================================================

struct http3_client_state {
  enum class phase : std::uint8_t {
    initial,
    resolving,       // DNS resolution
    creating_socket, // Creating UDP socket
    binding_socket,  // Connecting UDP socket (for send/recv semantics)
    connecting,      // QUIC handshake in progress
    waiting_recv,    // Waiting for UDP recv
    waiting_send,    // Waiting for UDP send
    waiting_timeout, // Waiting for timer
    connected,       // Ready for requests
    draining,        // Connection closing
    done,
    error
  };

  phase current_phase{phase::initial};
  handle socket_handle;

  // NOTE: Session is stored in the machine, not the state (state must be copyable)
  // See http3_client_machine for session access

  // Connection info
  std::string server_name;
  std::uint16_t port = 443;

  // Address storage
  std::vector<std::byte> local_addr;
  std::vector<std::byte> remote_addr;

  // Error info
  http3_error_code error_code{http3_error_code::no_error};
  std::string error_message;

  // Operation tracking
  std::uint64_t operation_id{0};

  [[nodiscard]] auto ok() const noexcept -> bool {
    return current_phase == phase::connected && error_code == http3_error_code::no_error;
  }

  [[nodiscard]] auto connected() const noexcept -> bool {
    return current_phase == phase::connected;
  }
};

// ============================================================================
// HTTP/3 client config
// ============================================================================

struct http3_client_config {
  std::string server_name;
  std::uint16_t port = 443;
  http3_settings settings;
};

// ============================================================================
// HTTP/3 client machine
// ============================================================================

/// HTTP/3 client connection machine
///
/// Establishes a QUIC connection and performs HTTP/3 setup.
/// After successful completion, the session is ready for requests.
///
/// NOTE: The session is stored in the machine (as mutable) because
/// states must be copyable for the machine concept to work.
class http3_client_machine {
public:
  using state_type = http3_client_state;

  /// Create HTTP/3 client machine
  /// @param config Connection configuration
  http3_client_machine(http3_client_config config);

  [[nodiscard]] auto initial() const -> state_type;
  [[nodiscard]] auto step(state_type s, const event& e) const -> step_result<state_type>;
  [[nodiscard]] auto done(const state_type& s) const -> bool;

  /// Get the HTTP/3 session (valid after connection established)
  [[nodiscard]] auto session() -> http3_session& { return session_; }
  [[nodiscard]] auto session() const -> const http3_session& { return session_; }

private:
  http3_client_config config_;

  // Session stored in machine, not state (state must be copyable)
  mutable http3_session session_;

  // I/O buffers (must outlive the operations that reference them)
  mutable std::array<std::byte, http3_max_pktlen> recv_buffer_{};
  mutable std::array<std::byte, http3_max_pktlen> send_buffer_{};

  auto do_send(state_type s) const -> step_result<state_type>;
  auto do_recv(state_type s, const event& e) const -> step_result<state_type>;
  auto do_timeout(state_type s) const -> step_result<state_type>;
};

// ============================================================================
// HTTP/3 request state
// ============================================================================

struct http3_request_state {
  enum class phase : std::uint8_t {
    initial,
    submitting,      // Submitting request
    waiting_send,    // Waiting to send request data
    waiting_recv,    // Waiting for response data
    waiting_timeout, // Waiting for timer
    receiving,       // Receiving response
    done,
    error
  };

  phase current_phase{phase::initial};
  handle socket_handle;
  std::int64_t stream_id{-1};

  // Request/Response
  http3_request request;
  http3_response response;

  // Error info
  http3_error_code error_code{http3_error_code::no_error};
  std::string error_message;

  // Operation tracking
  std::uint64_t operation_id{0};

  [[nodiscard]] auto ok() const noexcept -> bool {
    return current_phase == phase::done && error_code == http3_error_code::no_error;
  }
};

// ============================================================================
// HTTP/3 request machine
// ============================================================================

/// HTTP/3 request machine
///
/// Submits an HTTP/3 request and collects the response.
/// Requires an already-connected http3_session.
class http3_request_machine {
public:
  using state_type = http3_request_state;

  /// Create HTTP/3 request machine
  /// @param session Connected HTTP/3 session
  /// @param socket UDP socket handle
  /// @param request The request to send
  http3_request_machine(http3_session& session, handle socket, http3_request request);

  [[nodiscard]] auto initial() const -> state_type;
  [[nodiscard]] auto step(state_type s, const event& e) const -> step_result<state_type>;
  [[nodiscard]] auto done(const state_type& s) const -> bool;

  /// Get stable span to recv buffer (for use in operations)
  [[nodiscard]] auto recv_buffer_span() const -> stable_span<std::byte> {
    return make_stable_span(std::span{recv_buffer_});
  }

private:
  http3_session* session_;
  handle socket_;
  http3_request request_;
  mutable std::array<std::byte, http3_max_pktlen> recv_buffer_{}; // Stable buffer for recv

  auto do_send(state_type s) const -> step_result<state_type>;
  auto do_recv(state_type s, const event& e) const -> step_result<state_type>;
};

} // namespace evring
