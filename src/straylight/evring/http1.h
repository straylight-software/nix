#pragma once

/// http1.h - HTTP/1.1 state machines using llhttp
///
/// Provides replayable HTTP/1.1 request/response handling by modeling
/// the protocol as state machines that yield io_uring operations.
///
/// Key concepts:
/// - llhttp handles parsing; we handle I/O scheduling
/// - Supports keep-alive connections
/// - Supports chunked transfer encoding
/// - Can work over plain TCP or TLS
///
/// Example usage (plain HTTP):
/// @code
///   auto ring = evring::make_io_uring_ring(256);
///
///   // TCP connect to server
///   auto socket = tcp_connect(*ring, "httpbin.org", "80");
///
///   // Send request and receive response
///   http1_request req;
///   req.method = "GET";
///   req.path = "/get";
///   req.headers = {{"Host", "httpbin.org"}, {"Connection", "close"}};
///
///   http1_client_machine client{socket};
///   client.send_request(req);
///   auto state = evring::run(client, *ring);
///   // state.response contains the parsed response
/// @endcode
///
/// Example usage (HTTPS via TLS):
/// @code
///   // After TLS handshake...
///   http1_request req{...};
///   http1_tls_client_machine client{tls_conn, socket};
///   client.send_request(req);
///   auto state = evring::run(client, *ring);
/// @endcode

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "straylight/evring/event.h"
#include "straylight/evring/handle.h"
#include "straylight/evring/machine.h"
#include "straylight/evring/tls.h"

// Forward declare llhttp types
struct llhttp__internal_s;
typedef struct llhttp__internal_s llhttp_t;
struct llhttp_settings_s;
typedef struct llhttp_settings_s llhttp_settings_t;

namespace evring {

// ============================================================================
// HTTP/1.1 constants
// ============================================================================

inline constexpr std::size_t http1_default_max_header_size = 8192;
inline constexpr std::size_t http1_default_max_body_size = 16 * 1024 * 1024; // 16MB
inline constexpr std::size_t http1_default_buffer_size = 16384;

// ============================================================================
// HTTP/1.1 method enum (mirrors llhttp)
// ============================================================================

enum class http1_method : std::uint8_t {
  del = 0, // DELETE
  get = 1,
  head = 2,
  post = 3,
  put = 4,
  connect = 5,
  options = 6,
  trace = 7,
  patch = 28,
};

/// Convert method enum to string
auto http1_method_string(http1_method method) noexcept -> std::string_view;

/// Parse method string to enum
auto http1_method_parse(std::string_view method) noexcept -> http1_method;

// ============================================================================
// HTTP/1.1 header
// ============================================================================

struct http1_header {
  std::string name;
  std::string value;
};

using http1_headers = std::vector<http1_header>;

// ============================================================================
// HTTP/1.1 request
// ============================================================================

struct http1_request {
  http1_method method = http1_method::get;
  std::string path = "/";
  http1_headers headers;
  std::vector<std::byte> body;

  /// HTTP version (1.0 or 1.1)
  std::uint8_t version_major = 1;
  std::uint8_t version_minor = 1;

  /// Serialize request to wire format
  [[nodiscard]] auto serialize() const -> std::vector<std::byte>;

  /// Get header value by name (case-insensitive)
  [[nodiscard]] auto get_header(std::string_view name) const -> std::string_view;

  /// Set header (replaces if exists)
  void set_header(std::string name, std::string value);

  /// Check if connection should be kept alive
  [[nodiscard]] auto keep_alive() const -> bool;
};

// ============================================================================
// HTTP/1.1 response
// ============================================================================

struct http1_response {
  std::uint16_t status_code = 0;
  std::string status_message;
  http1_headers headers;
  std::vector<std::byte> body;

  std::uint8_t version_major = 1;
  std::uint8_t version_minor = 1;

  [[nodiscard]] auto ok() const noexcept -> bool { return status_code >= 200 && status_code < 300; }

  /// Get header value by name (case-insensitive)
  [[nodiscard]] auto get_header(std::string_view name) const -> std::string_view;

  /// Check if connection should be kept alive
  [[nodiscard]] auto keep_alive() const -> bool;

  /// Get Content-Length header value (-1 if not present)
  [[nodiscard]] auto content_length() const -> std::int64_t;
};

// ============================================================================
// HTTP/1.1 parser (wraps llhttp)
// ============================================================================

/// HTTP/1.1 response parser
class http1_parser {
public:
  http1_parser();
  ~http1_parser();

  // Move-only
  http1_parser(http1_parser&& other) noexcept;
  http1_parser& operator=(http1_parser&& other) noexcept;
  http1_parser(const http1_parser&) = delete;
  http1_parser& operator=(const http1_parser&) = delete;

  /// Reset parser for a new message
  void reset();

  /// Parse data, returns bytes consumed or negative error
  auto parse(std::span<const std::byte> data) -> std::int64_t;

  /// Check if headers are complete
  [[nodiscard]] auto headers_complete() const noexcept -> bool { return headers_complete_; }

  /// Check if message is complete
  [[nodiscard]] auto message_complete() const noexcept -> bool { return message_complete_; }

  /// Check if there was a parse error
  [[nodiscard]] auto has_error() const noexcept -> bool { return error_ != 0; }

  /// Get error message
  [[nodiscard]] auto error_message() const -> std::string;

  /// Get parsed response (valid after message_complete)
  [[nodiscard]] auto response() -> http1_response& { return response_; }
  [[nodiscard]] auto response() const -> const http1_response& { return response_; }

  /// Check if connection should be kept alive
  [[nodiscard]] auto should_keep_alive() const noexcept -> bool;

  /// Check if this is an upgrade response
  [[nodiscard]] auto is_upgrade() const noexcept -> bool;

private:
  llhttp_t* parser_{nullptr};
  llhttp_settings_t* settings_{nullptr};

  http1_response response_;
  std::string current_header_field_;
  std::string current_header_value_;
  bool in_header_value_{false};

  bool headers_complete_{false};
  bool message_complete_{false};
  int error_{0};

  // Callbacks
  static int on_message_begin(llhttp_t* parser);
  static int on_status(llhttp_t* parser, const char* at, std::size_t length);
  static int on_header_field(llhttp_t* parser, const char* at, std::size_t length);
  static int on_header_value(llhttp_t* parser, const char* at, std::size_t length);
  static int on_headers_complete(llhttp_t* parser);
  static int on_body(llhttp_t* parser, const char* at, std::size_t length);
  static int on_message_complete(llhttp_t* parser);
};

// ============================================================================
// HTTP/1.1 client state (plain TCP)
// ============================================================================

struct http1_client_state {
  enum class phase : std::uint8_t {
    initial,
    sending,       // sending request
    waiting_write, // waiting for send to complete
    receiving,     // receiving response
    waiting_read,  // waiting for recv to complete
    done,
    error
  };

  phase current_phase{phase::initial};
  handle socket_handle;

  // Request being sent
  std::vector<std::byte> send_buffer;
  std::size_t bytes_sent{0};

  // Note: recv_buffer is in the machine (not state) for buffer lifetime safety

  // Response (copied from parser when complete)
  http1_response response;

  // Parser state flags
  bool headers_complete{false};
  bool message_complete{false};

  // Error info
  int error_code{0};
  std::string error_message;

  // Operation tracking
  std::uint64_t operation_id{0};

  [[nodiscard]] auto ok() const noexcept -> bool {
    return current_phase == phase::done && error_code == 0;
  }
};

// ============================================================================
// HTTP/1.1 client machine (plain TCP)
// ============================================================================

/// HTTP/1.1 client over plain TCP
///
/// Sends a request and receives the response.
class http1_client_machine {
public:
  using state_type = http1_client_state;

  /// Create client machine
  /// @param socket Connected TCP socket handle
  /// @param request The request to send
  http1_client_machine(handle socket, http1_request request);

  [[nodiscard]] auto initial() const -> state_type;
  [[nodiscard]] auto step(state_type s, const event& e) const -> step_result<state_type>;
  [[nodiscard]] auto done(const state_type& s) const -> bool;

  /// Get stable span to recv buffer (for use in operations)
  [[nodiscard]] auto recv_buffer_span() const -> stable_span<std::byte> {
    return make_stable_span(std::span{recv_buffer_});
  }

private:
  handle socket_;
  http1_request request_;
  mutable http1_parser parser_;                // Parser is mutable to allow updates in const step()
  mutable std::vector<std::byte> recv_buffer_; // Stable buffer for receive operations
};

// ============================================================================
// HTTP/1.1 TLS client state
// ============================================================================

struct http1_tls_client_state {
  enum class phase : std::uint8_t {
    initial,
    sending,       // sending request via TLS
    waiting_write, // waiting for TLS write to complete
    receiving,     // receiving response via TLS
    waiting_read,  // waiting for TLS read to complete
    done,
    error
  };

  phase current_phase{phase::initial};
  handle socket_handle;

  // Request being sent
  std::vector<std::byte> send_buffer;
  std::size_t bytes_sent{0};

  // Note: recv_buffer is in the machine (not state) for buffer lifetime safety

  // Response (copied from parser when complete)
  http1_response response;

  // Parser state flags
  bool headers_complete{false};
  bool message_complete{false};

  // Error info
  int error_code{0};
  std::string error_message;

  // Operation tracking
  std::uint64_t operation_id{0};

  [[nodiscard]] auto ok() const noexcept -> bool {
    return current_phase == phase::done && error_code == 0;
  }
};

// ============================================================================
// HTTP/1.1 TLS client machine
// ============================================================================

/// HTTP/1.1 client over TLS
///
/// Sends a request and receives the response over an established TLS connection.
class http1_tls_client_machine {
public:
  using state_type = http1_tls_client_state;

  /// Create TLS client machine
  /// @param tls_conn Established TLS connection
  /// @param socket Socket handle
  /// @param request The request to send
  http1_tls_client_machine(tls_connection& tls_conn, handle socket, http1_request request);

  [[nodiscard]] auto initial() const -> state_type;
  [[nodiscard]] auto step(state_type s, const event& e) const -> step_result<state_type>;
  [[nodiscard]] auto done(const state_type& s) const -> bool;

  /// Get stable span to recv buffer (for use in operations)
  [[nodiscard]] auto recv_buffer_span() const -> stable_span<std::byte> {
    return make_stable_span(std::span{recv_buffer_});
  }

private:
  tls_connection* tls_conn_;
  handle socket_;
  http1_request request_;
  mutable http1_parser parser_;                // Parser is mutable to allow updates in const step()
  mutable std::vector<std::byte> recv_buffer_; // Stable buffer for receive operations
};

} // namespace evring
