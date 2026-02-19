#pragma once

/// tls.h - TLS state machine using libtls (LibreSSL)
///
/// Provides replayable TLS connections by modeling the handshake and I/O
/// as state machines that yield io_uring poll_add operations.
///
/// Key concepts:
/// - TLS operations are async: when libtls returns TLS_WANT_POLLIN/POLLOUT,
///   we yield a poll_add operation and resume when the socket is ready
/// - Uses socket-based libtls API (tls_connect_socket) for direct I/O
/// - Supports ALPN negotiation for HTTP/2
///
/// Example usage:
/// @code
///   auto ring = evring::make_io_uring_ring(256);
///   auto config = evring::tls_client_config::create_default();
///   config.set_alpn("h2,http/1.1");
///
///   // After TCP connect completes:
///   evring::tls_handshake_machine handshake{socket_handle, *ring, config, "example.com"};
///   auto final_state = evring::run(handshake, *ring);
///   auto tls_ctx = final_state.take_context(); // owns the tls* now
///
///   // For TLS read/write:
///   evring::tls_read_machine reader{tls_ctx, socket_handle, *ring, buffer};
///   auto read_state = evring::run(reader, *ring);
/// @endcode

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "straylight/evring/event.h"
#include "straylight/evring/handle.h"
#include "straylight/evring/machine.h"

// Forward declare libtls types (C structs in global namespace)
struct tls;
struct tls_config;

namespace evring {

// ============================================================================
// TLS error handling
// ============================================================================

/// TLS error codes for internal use
/// Values match libtls constants
enum class tls_error_code : int {
  none = 0,
  want_pollin = -2,  // TLS_WANT_POLLIN
  want_pollout = -3, // TLS_WANT_POLLOUT
  // Other negative values are real errors
};

/// Result of a TLS operation
struct tls_result {
  std::int64_t bytes{0}; // bytes read/written, or negative on error
  tls_error_code error{tls_error_code::none};
  std::string error_message;

  [[nodiscard]] auto ok() const noexcept -> bool {
    return error == tls_error_code::none && bytes >= 0;
  }

  [[nodiscard]] auto wants_read() const noexcept -> bool {
    return error == tls_error_code::want_pollin;
  }

  [[nodiscard]] auto wants_write() const noexcept -> bool {
    return error == tls_error_code::want_pollout;
  }
};

// ============================================================================
// TLS configuration (RAII wrapper for struct tls_config*)
// ============================================================================

/// TLS configuration - wraps struct tls_config* with RAII
///
/// Thread-safety: configuration objects should not be shared between
/// threads without external synchronization.
class tls_client_config {
public:
  /// Create a default client configuration
  /// - TLS 1.2/1.3 enabled
  /// - System CA certificates
  /// - Certificate verification enabled
  static auto create_default() -> tls_client_config;

  /// Create an insecure configuration (no verification - FOR TESTING ONLY)
  static auto create_insecure() -> tls_client_config;

  ~tls_client_config();

  // Move-only
  tls_client_config(tls_client_config&& other) noexcept;
  tls_client_config& operator=(tls_client_config&& other) noexcept;
  tls_client_config(const tls_client_config&) = delete;
  tls_client_config& operator=(const tls_client_config&) = delete;

  /// Set ALPN protocols (comma-separated, e.g., "h2,http/1.1")
  /// @return true on success
  auto set_alpn(std::string_view protocols) -> bool;

  /// Set CA file for certificate verification
  auto set_ca_file(const char* path) -> bool;

  /// Set CA certificate from memory
  auto set_ca_mem(std::span<const std::uint8_t> ca) -> bool;

  /// Set certificate and key files (for client auth)
  auto set_keypair_file(const char* cert_path, const char* key_path) -> bool;

  /// Set certificate and key from memory
  auto set_keypair_mem(std::span<const std::uint8_t> cert, std::span<const std::uint8_t> key)
      -> bool;

  /// Set TLS protocol versions (bitmask)
  auto set_protocols(std::uint32_t protocols) -> bool;

  /// Get last error message
  [[nodiscard]] auto error() const -> const char*;

  /// Get raw config pointer (for advanced use)
  [[nodiscard]] auto raw() const noexcept -> struct tls_config* { return config_; }

  /// Check if valid
  [[nodiscard]] auto valid() const noexcept -> bool { return config_ != nullptr; }

private:
  explicit tls_client_config(struct tls_config* cfg);
  struct tls_config* config_{nullptr};
};

/// TLS server configuration - wraps struct tls_config* with RAII
class tls_server_config {
public:
  /// Create a server configuration (requires cert/key setup)
  static auto create() -> tls_server_config;

  ~tls_server_config();

  // Move-only
  tls_server_config(tls_server_config&& other) noexcept;
  tls_server_config& operator=(tls_server_config&& other) noexcept;
  tls_server_config(const tls_server_config&) = delete;
  tls_server_config& operator=(const tls_server_config&) = delete;

  /// Set ALPN protocols
  auto set_alpn(std::string_view protocols) -> bool;

  /// Set certificate and key files
  auto set_keypair_file(const char* cert_path, const char* key_path) -> bool;

  /// Set certificate and key from memory
  auto set_keypair_mem(std::span<const std::uint8_t> cert, std::span<const std::uint8_t> key)
      -> bool;

  /// Get raw config pointer
  [[nodiscard]] auto raw() const noexcept -> struct tls_config* { return config_; }

  /// Check if valid
  [[nodiscard]] auto valid() const noexcept -> bool { return config_ != nullptr; }

private:
  explicit tls_server_config(struct tls_config* cfg);
  struct tls_config* config_{nullptr};
};

// ============================================================================
// TLS context (RAII wrapper for struct tls*)
// ============================================================================

/// TLS connection context - wraps struct tls* with RAII
///
/// Created by tls_handshake_machine after successful handshake.
class tls_connection {
public:
  ~tls_connection();

  // Move-only
  tls_connection(tls_connection&& other) noexcept;
  tls_connection& operator=(tls_connection&& other) noexcept;
  tls_connection(const tls_connection&) = delete;
  tls_connection& operator=(const tls_connection&) = delete;

  /// Get ALPN protocol selected during handshake (or nullptr if none)
  [[nodiscard]] auto alpn_selected() const -> const char*;

  /// Get TLS version string
  [[nodiscard]] auto version() const -> const char*;

  /// Get cipher suite name
  [[nodiscard]] auto cipher() const -> const char*;

  /// Get cipher strength in bits
  [[nodiscard]] auto cipher_strength() const -> int;

  /// Get peer certificate common name (or nullptr)
  [[nodiscard]] auto peer_cn() const -> const char*;

  /// Get last error message
  [[nodiscard]] auto error() const -> const char*;

  /// Get raw context pointer
  [[nodiscard]] auto raw() const noexcept -> struct tls* { return ctx_; }

  /// Release ownership of raw pointer (caller takes ownership)
  auto release() noexcept -> struct tls*;

  /// Create from raw pointer (takes ownership)
  static auto from_raw(struct tls* ctx) -> tls_connection;

  /// Check if valid
  [[nodiscard]] auto valid() const noexcept -> bool { return ctx_ != nullptr; }

private:
  explicit tls_connection(struct tls* ctx);
  struct tls* ctx_{nullptr};

  friend class tls_handshake_machine;
};

// ============================================================================
// TLS handshake state machine
// ============================================================================

/// State for TLS handshake machine
struct tls_handshake_state {
  enum class phase { initial, handshaking, waiting_for_poll, done, error };

  phase current_phase{phase::initial};

  // The socket we're wrapping
  handle socket_handle;

  // Socket file descriptor (for libtls)
  int socket_fd{-1};

  // TLS context being established (owned)
  struct tls* ctx{nullptr};

  // Error information
  int error_code{0};
  std::string error_message;

  // Operation tracking
  std::uint64_t operation_id{0};

  [[nodiscard]] auto ok() const -> bool { return current_phase == phase::done && error_code == 0; }

  /// Take ownership of the TLS context (only valid after successful handshake)
  auto take_context() -> tls_connection;
};

// Forward declaration
class ring;

/// TLS handshake state machine
///
/// Models the TLS handshake as a replayable state machine that yields
/// poll_add operations when the TLS library needs I/O.
///
/// Usage:
/// @code
///   tls_handshake_machine handshake{socket_handle, ring, config, "example.com"};
///   auto final_state = evring::run(handshake, ring);
///   if (final_state.ok()) {
///     auto ctx = final_state.take_context();
///     // use ctx for TLS read/write
///   }
/// @endcode
class tls_handshake_machine {
public:
  using state_type = tls_handshake_state;

  /// Create a client handshake machine
  /// @param socket Connected TCP socket handle
  /// @param ring_ref Ring to get file descriptor from handle
  /// @param config TLS configuration (must outlive machine)
  /// @param servername SNI hostname for verification
  tls_handshake_machine(handle socket, ring& ring_ref, const tls_client_config& config,
                        std::string_view servername);

  /// Create a server handshake machine (for accepting connections)
  /// @param socket Connected TCP socket handle (from accept)
  /// @param ring_ref Ring to get file descriptor from handle
  /// @param server_ctx Server TLS context (from tls_server())
  tls_handshake_machine(handle socket, ring& ring_ref, tls_connection& server_ctx);

  [[nodiscard]] auto initial() const -> state_type;

  [[nodiscard]] auto step(state_type s, const event& e) const -> step_result<state_type>;

  [[nodiscard]] auto done(const state_type& s) const -> bool;

private:
  handle socket_;
  int socket_fd_;
  const tls_client_config* config_{nullptr};
  tls_connection* server_ctx_{nullptr};
  std::string servername_;
  bool is_client_{true};

  // Helper to continue handshake
  [[nodiscard]] auto continue_handshake(state_type s) const -> step_result<state_type>;
};

// ============================================================================
// TLS read state machine
// ============================================================================

/// State for TLS read machine
struct tls_read_state {
  enum class phase {
    initial,
    reading,
    waiting_for_poll, // TLS needs to wait for socket I/O
    done,
    error
  };

  phase current_phase{phase::initial};
  handle socket_handle;

  // Output
  std::span<std::byte> output_buffer;
  std::size_t bytes_read{0};

  // Error info
  int error_code{0};
  std::string error_message;

  [[nodiscard]] auto ok() const -> bool { return current_phase == phase::done && error_code == 0; }
};

/// TLS read state machine
///
/// Reads decrypted data from a TLS connection.
class tls_read_machine {
public:
  using state_type = tls_read_state;

  /// @param ctx TLS connection from completed handshake
  /// @param socket Socket handle
  /// @param buffer Output buffer for decrypted data
  tls_read_machine(tls_connection& ctx, handle socket, std::span<std::byte> buffer);

  [[nodiscard]] auto initial() const -> state_type;
  [[nodiscard]] auto step(state_type s, const event& e) const -> step_result<state_type>;
  [[nodiscard]] auto done(const state_type& s) const -> bool;

private:
  tls_connection* ctx_;
  handle socket_;
  std::span<std::byte> buffer_;

  [[nodiscard]] auto continue_read(state_type s) const -> step_result<state_type>;
};

// ============================================================================
// TLS write state machine
// ============================================================================

/// State for TLS write machine
struct tls_write_state {
  enum class phase {
    initial,
    writing,
    waiting_for_poll, // TLS needs to wait for socket I/O
    done,
    error
  };

  phase current_phase{phase::initial};
  handle socket_handle;

  // Input
  std::span<const std::byte> input_buffer;
  std::size_t bytes_written{0};

  // Error info
  int error_code{0};
  std::string error_message;

  [[nodiscard]] auto ok() const -> bool { return current_phase == phase::done && error_code == 0; }
};

/// TLS write state machine
///
/// Writes encrypted data to a TLS connection.
class tls_write_machine {
public:
  using state_type = tls_write_state;

  /// @param ctx TLS connection from completed handshake
  /// @param socket Socket handle
  /// @param data Data to encrypt and send
  tls_write_machine(tls_connection& ctx, handle socket, std::span<const std::byte> data);

  [[nodiscard]] auto initial() const -> state_type;
  [[nodiscard]] auto step(state_type s, const event& e) const -> step_result<state_type>;
  [[nodiscard]] auto done(const state_type& s) const -> bool;

private:
  tls_connection* ctx_;
  handle socket_;
  std::span<const std::byte> data_;

  [[nodiscard]] auto continue_write(state_type s) const -> step_result<state_type>;
};

// ============================================================================
// TLS close state machine
// ============================================================================

/// State for TLS close machine
struct tls_close_state {
  enum class phase {
    initial,
    closing,
    waiting_for_poll, // TLS needs to wait for socket I/O
    done,
    error
  };

  phase current_phase{phase::initial};
  handle socket_handle;

  int error_code{0};
  std::string error_message;

  [[nodiscard]] auto ok() const -> bool { return current_phase == phase::done && error_code == 0; }
};

/// TLS close state machine
///
/// Performs TLS shutdown handshake.
class tls_close_machine {
public:
  using state_type = tls_close_state;

  tls_close_machine(tls_connection& ctx, handle socket);

  [[nodiscard]] auto initial() const -> state_type;
  [[nodiscard]] auto step(state_type s, const event& e) const -> step_result<state_type>;
  [[nodiscard]] auto done(const state_type& s) const -> bool;

private:
  tls_connection* ctx_;
  handle socket_;

  [[nodiscard]] auto continue_close(state_type s) const -> step_result<state_type>;
};

// ============================================================================
// Protocol constants
// ============================================================================

/// TLS protocol version flags (for set_protocols)
/// Values match libtls constants
inline constexpr std::uint32_t TLS_PROTO_1_2 = (1 << 3);                // TLS_PROTOCOL_TLSv1_2
inline constexpr std::uint32_t TLS_PROTO_1_3 = (1 << 4);                // TLS_PROTOCOL_TLSv1_3
inline constexpr std::uint32_t TLS_PROTO_DEFAULT = (1 << 3) | (1 << 4); // TLS_PROTOCOLS_DEFAULT

} // namespace evring
