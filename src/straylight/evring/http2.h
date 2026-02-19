#pragma once

/// http2.h - HTTP/2 state machines using nghttp2
///
/// Provides replayable HTTP/2 connections by modeling the protocol
/// as state machines that yield io_uring operations.
///
/// Key concepts:
/// - HTTP/2 is multiplexed: multiple streams over one connection
/// - nghttp2 handles framing/HPACK; we handle I/O scheduling
/// - Uses TLS with ALPN "h2" for secure connections
///
/// Example usage:
/// @code
///   auto ring = evring::make_io_uring_ring(256);
///
///   // First: TLS handshake with ALPN
///   auto tls_config = evring::tls_client_config::create_default();
///   tls_config.set_alpn("h2");
///   evring::tls_handshake_machine tls_hs{socket, *ring, tls_config, "example.com"};
///   auto tls_state = evring::run(tls_hs, *ring);
///   auto tls_conn = tls_state.take_context();
///
///   // Second: HTTP/2 connection setup (send preface + settings)
///   evring::http2_session session;
///   evring::http2_connection_machine conn{tls_conn, socket, *ring, session};
///   auto conn_state = evring::run(conn, *ring);
///
///   // Third: send requests
///   evring::http2_request req{":method", "GET", ":path", "/", ...};
///   evring::http2_request_machine request{session, tls_conn, socket, req};
///   auto resp_state = evring::run(request, *ring);
/// @endcode

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "straylight/evring/event.h"
#include "straylight/evring/handle.h"
#include "straylight/evring/machine.h"
#include "straylight/evring/tls.h"

// Forward declare nghttp2 types
struct nghttp2_session;

namespace evring {

// ============================================================================
// HTTP/2 constants
// ============================================================================

inline constexpr std::size_t http2_default_header_table_size = 4096;
inline constexpr std::size_t http2_default_initial_window_size = 65535;
inline constexpr std::size_t http2_default_max_frame_size = 16384;
inline constexpr std::size_t http2_default_max_concurrent_streams = 100;
inline constexpr std::size_t http2_default_max_header_list_size = 8192;

// ============================================================================
// HTTP/2 error codes (RFC 7540 Section 7)
// ============================================================================

enum class http2_error_code : std::uint32_t {
  no_error = 0x0,
  protocol_error = 0x1,
  internal_error = 0x2,
  flow_control_error = 0x3,
  settings_timeout = 0x4,
  stream_closed = 0x5,
  frame_size_error = 0x6,
  refused_stream = 0x7,
  cancel = 0x8,
  compression_error = 0x9,
  connect_error = 0xa,
  enhance_your_calm = 0xb,
  inadequate_security = 0xc,
  http_1_1_required = 0xd,

  // Custom errors (not in RFC)
  connection_closed = 0x100,
  tls_error = 0x101,
};

/// Get human-readable error string
auto http2_error_string(http2_error_code error) noexcept -> std::string_view;

// ============================================================================
// HTTP/2 settings
// ============================================================================

struct http2_settings {
  std::uint32_t header_table_size = http2_default_header_table_size;
  std::uint32_t enable_push = 0; // disabled for clients
  std::uint32_t max_concurrent_streams = http2_default_max_concurrent_streams;
  std::uint32_t initial_window_size = http2_default_initial_window_size;
  std::uint32_t max_frame_size = http2_default_max_frame_size;
  std::uint32_t max_header_list_size = http2_default_max_header_list_size;
};

// ============================================================================
// HTTP/2 headers
// ============================================================================

struct http2_header {
  std::string name;
  std::string value;
};

using http2_headers = std::vector<http2_header>;

// ============================================================================
// HTTP/2 request
// ============================================================================

struct http2_request {
  std::string method = "GET";
  std::string scheme = "https";
  std::string authority; // host:port
  std::string path = "/";
  http2_headers headers;
  std::vector<std::byte> body;

  /// Build pseudo-headers + regular headers for nghttp2
  [[nodiscard]] auto all_headers() const -> http2_headers;
};

// ============================================================================
// HTTP/2 response
// ============================================================================

struct http2_response {
  int status_code = 0;
  http2_headers headers;
  std::vector<std::byte> body;

  [[nodiscard]] auto ok() const noexcept -> bool { return status_code >= 200 && status_code < 300; }
};

// ============================================================================
// HTTP/2 stream state
// ============================================================================

enum class http2_stream_state : std::int8_t {
  idle = 0,
  open,
  half_closed_local,
  half_closed_remote,
  closed
};

// ============================================================================
// HTTP/2 session (owns nghttp2_session)
// ============================================================================

/// Manages nghttp2 session state
/// This is shared between connection and request machines
class http2_session {
public:
  http2_session();
  ~http2_session();

  // Move-only
  http2_session(http2_session&& other) noexcept;
  http2_session& operator=(http2_session&& other) noexcept;
  http2_session(const http2_session&) = delete;
  http2_session& operator=(const http2_session&) = delete;

  /// Initialize as client session
  auto init_client(const http2_settings& settings = {}) -> bool;

  /// Check if session is valid
  [[nodiscard]] auto valid() const noexcept -> bool { return session_ != nullptr; }

  /// Get raw session pointer
  [[nodiscard]] auto raw() noexcept -> nghttp2_session* { return session_; }
  [[nodiscard]] auto raw() const noexcept -> const nghttp2_session* { return session_; }

  /// Submit a request, returns stream ID or negative error
  auto submit_request(const http2_request& req) -> std::int32_t;

  /// Get pending data to send (from nghttp2's send buffer)
  /// Returns bytes to send, empty if nothing pending
  [[nodiscard]] auto get_pending_data() -> std::vector<std::byte>;

  /// Feed received data to nghttp2
  /// Returns bytes consumed or negative error
  auto receive_data(std::span<const std::byte> data) -> std::int64_t;

  /// Check if session wants to send data
  [[nodiscard]] auto wants_write() const noexcept -> bool;

  /// Check if session wants to read data
  [[nodiscard]] auto wants_read() const noexcept -> bool;

  /// Get local settings
  [[nodiscard]] auto local_settings() const noexcept -> const http2_settings& {
    return local_settings_;
  }

  /// Get remote settings (received from peer)
  [[nodiscard]] auto remote_settings() const noexcept -> const http2_settings& {
    return remote_settings_;
  }

  /// Stream callback: called when headers are received
  using on_headers_callback = std::function<void(std::int32_t stream_id, const http2_headers&)>;

  /// Stream callback: called when data is received
  using on_data_callback = std::function<void(std::int32_t stream_id, std::span<const std::byte>)>;

  /// Stream callback: called when stream closes
  using on_stream_close_callback = std::function<void(std::int32_t stream_id, http2_error_code)>;

  /// Set callbacks for stream events
  void set_on_headers(on_headers_callback cb) { on_headers_ = std::move(cb); }
  void set_on_data(on_data_callback cb) { on_data_ = std::move(cb); }
  void set_on_stream_close(on_stream_close_callback cb) { on_stream_close_ = std::move(cb); }

  /// Get accumulated response for a stream (headers, data, status)
  /// Returns nullptr if stream not found
  [[nodiscard]] auto get_stream_response(std::int32_t stream_id) -> http2_response*;

  /// Check if a stream is closed
  [[nodiscard]] auto is_stream_closed(std::int32_t stream_id) const -> bool;

  /// Get stream error (only valid if stream is closed with error)
  [[nodiscard]] auto get_stream_error(std::int32_t stream_id) const -> http2_error_code;

  // Internal: called by nghttp2 callbacks
  void handle_header(std::int32_t stream_id, std::string_view name, std::string_view value);
  void handle_data(std::int32_t stream_id, std::span<const std::byte> data);
  void handle_stream_close(std::int32_t stream_id, std::uint32_t error_code);
  void handle_settings(const http2_settings& settings);
  void handle_frame_recv_headers(std::int32_t stream_id);

  /// Append data to send buffer (called by nghttp2 send callback)
  void append_send_data(const std::byte* data, std::size_t length);

  /// Get pending headers for a stream
  [[nodiscard]] auto get_pending_headers(std::int32_t stream_id) const -> const http2_headers*;

private:
  nghttp2_session* session_{nullptr};
  http2_settings local_settings_;
  http2_settings remote_settings_;

  // Per-stream header accumulation
  std::map<std::int32_t, http2_headers> pending_headers_;

  // Per-stream response accumulation
  std::map<std::int32_t, http2_response> stream_responses_;

  // Per-stream close tracking
  std::map<std::int32_t, http2_error_code> closed_streams_;

  // Callbacks
  on_headers_callback on_headers_;
  on_data_callback on_data_;
  on_stream_close_callback on_stream_close_;

  // Send buffer for nghttp2's output
  std::vector<std::byte> send_buffer_;
};

// ============================================================================
// HTTP/2 connection machine state
// ============================================================================

struct http2_connection_state {
  enum class phase {
    initial,
    sending_preface, // sending client preface + SETTINGS
    waiting_write,   // waiting for TLS write to complete
    waiting_read,    // waiting for server SETTINGS
    connected,       // ready for requests
    error
  };

  phase current_phase{phase::initial};
  handle socket_handle;

  // Error info
  http2_error_code error_code{http2_error_code::no_error};
  std::string error_message;

  // I/O buffers
  std::vector<std::byte> send_buffer;
  std::vector<std::byte> recv_buffer;

  // Operation tracking
  std::uint64_t operation_id{0};

  [[nodiscard]] auto ok() const noexcept -> bool {
    return current_phase == phase::connected && error_code == http2_error_code::no_error;
  }
};

// Forward declaration
class ring;

/// HTTP/2 connection establishment machine
///
/// Sends the HTTP/2 client preface and initial SETTINGS,
/// waits for server SETTINGS response.
///
/// Requires: TLS already established with ALPN "h2"
class http2_connection_machine {
public:
  using state_type = http2_connection_state;

  /// Create connection machine
  /// @param session HTTP/2 session (must be initialized)
  /// @param tls_conn TLS connection from completed handshake
  /// @param socket Socket handle
  http2_connection_machine(http2_session& session, tls_connection& tls_conn, handle socket);

  [[nodiscard]] auto initial() const -> state_type;
  [[nodiscard]] auto step(state_type s, const event& e) const -> step_result<state_type>;
  [[nodiscard]] auto done(const state_type& s) const -> bool;

private:
  http2_session* session_;
  tls_connection* tls_conn_;
  handle socket_;

  [[nodiscard]] auto do_tls_write(state_type s) const -> step_result<state_type>;
  [[nodiscard]] auto do_tls_read(state_type s) const -> step_result<state_type>;
};

// ============================================================================
// HTTP/2 request machine state
// ============================================================================

struct http2_request_state {
  enum class phase {
    initial,
    submitting,    // submitting request to nghttp2
    sending,       // sending request frames
    waiting_write, // waiting for TLS write
    waiting_read,  // waiting for response
    receiving,     // receiving response
    done,
    error
  };

  phase current_phase{phase::initial};
  handle socket_handle;
  std::int32_t stream_id{0};

  // Request
  http2_request request;

  // Response (accumulated)
  http2_response response;

  // Error info
  http2_error_code error_code{http2_error_code::no_error};
  std::string error_message;

  // I/O buffers
  std::vector<std::byte> send_buffer;
  std::vector<std::byte> recv_buffer;

  // Operation tracking
  std::uint64_t operation_id{0};

  [[nodiscard]] auto ok() const noexcept -> bool {
    return current_phase == phase::done && error_code == http2_error_code::no_error;
  }
};

/// HTTP/2 request machine
///
/// Submits a request and collects the response.
/// Multiple request machines can run concurrently on the same session.
class http2_request_machine {
public:
  using state_type = http2_request_state;

  /// Create request machine
  /// @param session HTTP/2 session (must be connected)
  /// @param tls_conn TLS connection
  /// @param socket Socket handle
  /// @param request The request to send
  http2_request_machine(http2_session& session, tls_connection& tls_conn, handle socket,
                        http2_request request);

  [[nodiscard]] auto initial() const -> state_type;
  [[nodiscard]] auto step(state_type s, const event& e) const -> step_result<state_type>;
  [[nodiscard]] auto done(const state_type& s) const -> bool;

private:
  http2_session* session_;
  tls_connection* tls_conn_;
  handle socket_;
  http2_request request_;

  [[nodiscard]] auto do_tls_write(state_type s) const -> step_result<state_type>;
  [[nodiscard]] auto do_tls_read(state_type s) const -> step_result<state_type>;
};

} // namespace evring
