// tls.cpp - TLS connection wrappers using libtls
//
// Socket-based TLS with poll integration for io_uring async support

#include "straylight/evring/tls.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <poll.h>
#include <tls.h>

#include "straylight/evring/evring.h"

namespace evring {

// ============================================================================
// tls_client_config implementation
// ============================================================================

tls_client_config::tls_client_config(struct tls_config* cfg) : config_(cfg) {}

auto tls_client_config::create_default() -> tls_client_config {
  struct tls_config* cfg = tls_config_new();
  if (cfg) {
    tls_config_set_protocols(cfg, TLS_PROTOCOLS_DEFAULT);
    tls_config_set_ciphers(cfg, "secure");

    // libressl's default CA path may be incorrect (relative path compiled in).
    // Check environment variables and common system paths.
    const char* ca_file = std::getenv("SSL_CERT_FILE");
    if (!ca_file) {
      ca_file = std::getenv("NIX_SSL_CERT_FILE");
    }
    if (!ca_file) {
      // Common system paths
      static const char* const kCaPaths[] = {
          "/etc/ssl/certs/ca-certificates.crt", // Debian/Ubuntu/NixOS
          "/etc/pki/tls/certs/ca-bundle.crt",   // Fedora/RHEL
          "/etc/ssl/ca-bundle.pem",             // OpenSUSE
          "/etc/ssl/cert.pem",                  // Alpine/FreeBSD
          nullptr,
      };
      for (const char* const* path = kCaPaths; *path; ++path) {
        if (std::filesystem::exists(*path)) {
          ca_file = *path;
          break;
        }
      }
    }
    if (ca_file) {
      tls_config_set_ca_file(cfg, ca_file);
    }
  }
  return tls_client_config{cfg};
}

auto tls_client_config::create_insecure() -> tls_client_config {
  struct tls_config* cfg = tls_config_new();
  if (cfg) {
    tls_config_set_protocols(cfg, TLS_PROTOCOLS_DEFAULT);
    tls_config_set_ciphers(cfg, "secure");
    tls_config_insecure_noverifycert(cfg);
    tls_config_insecure_noverifyname(cfg);
    tls_config_insecure_noverifytime(cfg);
  }
  return tls_client_config{cfg};
}

tls_client_config::~tls_client_config() {
  if (config_) {
    tls_config_free(config_);
  }
}

tls_client_config::tls_client_config(tls_client_config&& other) noexcept : config_(other.config_) {
  other.config_ = nullptr;
}

auto tls_client_config::operator=(tls_client_config&& other) noexcept -> tls_client_config& {
  if (this != &other) {
    if (config_) {
      tls_config_free(config_);
    }
    config_ = other.config_;
    other.config_ = nullptr;
  }
  return *this;
}

auto tls_client_config::set_alpn(std::string_view protocols) -> bool {
  if (!config_) {
    return false;
  }
  // tls_config_set_alpn expects null-terminated string
  std::string proto_str{protocols};
  return tls_config_set_alpn(config_, proto_str.c_str()) == 0;
}

auto tls_client_config::set_ca_file(const char* path) -> bool {
  return config_ && tls_config_set_ca_file(config_, path) == 0;
}

auto tls_client_config::set_ca_mem(std::span<const std::uint8_t> ca) -> bool {
  return config_ && tls_config_set_ca_mem(config_, ca.data(), ca.size()) == 0;
}

auto tls_client_config::set_keypair_file(const char* cert_path, const char* key_path) -> bool {
  return config_ && tls_config_set_keypair_file(config_, cert_path, key_path) == 0;
}

auto tls_client_config::set_keypair_mem(std::span<const std::uint8_t> cert,
                                        std::span<const std::uint8_t> key) -> bool {
  return config_ &&
         tls_config_set_keypair_mem(config_, cert.data(), cert.size(), key.data(), key.size()) == 0;
}

auto tls_client_config::set_protocols(std::uint32_t protocols) -> bool {
  return config_ && tls_config_set_protocols(config_, protocols) == 0;
}

auto tls_client_config::error() const -> const char* {
  // tls_config doesn't have an error function in libtls
  // Return nullptr to indicate no error info available
  return nullptr;
}

// ============================================================================
// tls_server_config implementation
// ============================================================================

tls_server_config::tls_server_config(struct tls_config* cfg) : config_(cfg) {}

auto tls_server_config::create() -> tls_server_config {
  struct tls_config* cfg = tls_config_new();
  if (cfg) {
    tls_config_set_protocols(cfg, TLS_PROTOCOLS_DEFAULT);
    tls_config_set_ciphers(cfg, "secure");
  }
  return tls_server_config{cfg};
}

tls_server_config::~tls_server_config() {
  if (config_) {
    tls_config_free(config_);
  }
}

tls_server_config::tls_server_config(tls_server_config&& other) noexcept : config_(other.config_) {
  other.config_ = nullptr;
}

auto tls_server_config::operator=(tls_server_config&& other) noexcept -> tls_server_config& {
  if (this != &other) {
    if (config_) {
      tls_config_free(config_);
    }
    config_ = other.config_;
    other.config_ = nullptr;
  }
  return *this;
}

auto tls_server_config::set_alpn(std::string_view protocols) -> bool {
  if (!config_) {
    return false;
  }
  std::string proto_str{protocols};
  return tls_config_set_alpn(config_, proto_str.c_str()) == 0;
}

auto tls_server_config::set_keypair_file(const char* cert_path, const char* key_path) -> bool {
  return config_ && tls_config_set_keypair_file(config_, cert_path, key_path) == 0;
}

auto tls_server_config::set_keypair_mem(std::span<const std::uint8_t> cert,
                                        std::span<const std::uint8_t> key) -> bool {
  return config_ &&
         tls_config_set_keypair_mem(config_, cert.data(), cert.size(), key.data(), key.size()) == 0;
}

// ============================================================================
// tls_connection implementation
// ============================================================================

tls_connection::tls_connection(struct tls* ctx) : ctx_(ctx) {}

tls_connection::~tls_connection() {
  if (ctx_) {
    tls_close(ctx_);
    tls_free(ctx_);
  }
}

tls_connection::tls_connection(tls_connection&& other) noexcept : ctx_(other.ctx_) {
  other.ctx_ = nullptr;
}

auto tls_connection::operator=(tls_connection&& other) noexcept -> tls_connection& {
  if (this != &other) {
    if (ctx_) {
      tls_close(ctx_);
      tls_free(ctx_);
    }
    ctx_ = other.ctx_;
    other.ctx_ = nullptr;
  }
  return *this;
}

auto tls_connection::alpn_selected() const -> const char* {
  if (!ctx_) {
    return nullptr;
  }
  return tls_conn_alpn_selected(ctx_);
}

auto tls_connection::version() const -> const char* {
  if (!ctx_) {
    return nullptr;
  }
  return tls_conn_version(ctx_);
}

auto tls_connection::cipher() const -> const char* {
  if (!ctx_) {
    return nullptr;
  }
  return tls_conn_cipher(ctx_);
}

auto tls_connection::cipher_strength() const -> int {
  if (!ctx_) {
    return 0;
  }
  return tls_conn_cipher_strength(ctx_);
}

auto tls_connection::peer_cn() const -> const char* {
  if (!ctx_) {
    return nullptr;
  }
  return tls_peer_cert_subject(ctx_);
}

auto tls_connection::error() const -> const char* {
  if (!ctx_) {
    return "no context";
  }
  return tls_error(ctx_);
}

auto tls_connection::release() noexcept -> struct tls* {
  struct tls* p = ctx_;
  ctx_ = nullptr;
  return p;
}

auto tls_connection::from_raw(struct tls* ctx) -> tls_connection {
  return tls_connection{ctx};
}

// ============================================================================
// TLS handshake state - take_context
// ============================================================================

auto tls_handshake_state::take_context() -> tls_connection {
  struct tls* p = ctx;
  ctx = nullptr;
  return tls_connection::from_raw(p);
}

// ============================================================================
// TLS handshake machine implementation (socket-based with poll)
// ============================================================================

tls_handshake_machine::tls_handshake_machine(handle socket, ring& ring_ref,
                                             const tls_client_config& config,
                                             std::string_view servername)
    : socket_(socket),
      socket_fd_(ring_ref.get_file_descriptor(socket)),
      config_(&config),
      servername_(servername),
      is_client_(true) {}

tls_handshake_machine::tls_handshake_machine(handle socket, ring& ring_ref,
                                             tls_connection& server_ctx)
    : socket_(socket),
      socket_fd_(ring_ref.get_file_descriptor(socket)),
      server_ctx_(&server_ctx),
      is_client_(false) {}

auto tls_handshake_machine::initial() const -> state_type {
  state_type s;
  s.socket_handle = socket_;
  s.socket_fd = socket_fd_;
  return s;
}

auto tls_handshake_machine::continue_handshake(state_type s) const -> step_result<state_type> {
  // Attempt handshake - with socket-based TLS, this does blocking I/O internally
  // but the socket is non-blocking so it will return TLS_WANT_POLLIN/POLLOUT
  int result = tls_handshake(s.ctx);

  if (result == 0) {
    // Handshake complete!
    s.current_phase = tls_handshake_state::phase::done;
    return {std::move(s), {}};
  }

  if (result == TLS_WANT_POLLIN || result == TLS_WANT_POLLOUT) {
    // TLS needs socket I/O - use poll_add to wait for readability/writability
    s.current_phase = tls_handshake_state::phase::waiting_for_poll;

    // Determine poll events based on what TLS wants
    short poll_events = (result == TLS_WANT_POLLIN) ? POLLIN : POLLOUT;

    std::vector<operation> ops;
    ops.push_back(operation::make_poll_add(s.socket_handle, poll_events, ++s.operation_id));
    return {std::move(s), std::move(ops)};
  }

  // Error
  s.current_phase = tls_handshake_state::phase::error;
  s.error_code = result;
  s.error_message = tls_error(s.ctx);
  return {std::move(s), {}};
}

auto tls_handshake_machine::step(state_type s, const event& e) const -> step_result<state_type> {
  switch (s.current_phase) {
    case tls_handshake_state::phase::initial: {
      // Create TLS context and connect to socket
      if (is_client_) {
        s.ctx = tls_client();
        if (!s.ctx) {
          s.current_phase = tls_handshake_state::phase::error;
          s.error_code = -1;
          s.error_message = "failed to create TLS client context";
          return {std::move(s), {}};
        }

        if (tls_configure(s.ctx, config_->raw()) != 0) {
          s.current_phase = tls_handshake_state::phase::error;
          s.error_code = -1;
          s.error_message = tls_error(s.ctx);
          return {std::move(s), {}};
        }

        // Connect using socket fd - this sets up TLS to use the connected socket
        if (tls_connect_socket(s.ctx, s.socket_fd, servername_.c_str()) != 0) {
          s.current_phase = tls_handshake_state::phase::error;
          s.error_code = -1;
          s.error_message = tls_error(s.ctx);
          return {std::move(s), {}};
        }
      } else {
        // Server mode - accept on socket
        struct tls* client_ctx = nullptr;
        if (tls_accept_socket(server_ctx_->raw(), &client_ctx, s.socket_fd) != 0) {
          s.current_phase = tls_handshake_state::phase::error;
          s.error_code = -1;
          s.error_message = tls_error(server_ctx_->raw());
          return {std::move(s), {}};
        }
        s.ctx = client_ctx;
      }

      s.current_phase = tls_handshake_state::phase::handshaking;
      return continue_handshake(std::move(s));
    }

    case tls_handshake_state::phase::waiting_for_poll: {
      // Poll completed - socket is ready for I/O
      if (!e.ok()) {
        s.current_phase = tls_handshake_state::phase::error;
        s.error_code = e.error_code();
        s.error_message = "poll failed";
        return {std::move(s), {}};
      }

      // Resume handshake
      s.current_phase = tls_handshake_state::phase::handshaking;
      return continue_handshake(std::move(s));
    }

    case tls_handshake_state::phase::handshaking:
      // Called directly - continue handshake
      return continue_handshake(std::move(s));

    case tls_handshake_state::phase::done:
    case tls_handshake_state::phase::error:
      // Terminal states
      return {std::move(s), {}};
  }

  // Should not reach here
  return {std::move(s), {}};
}

auto tls_handshake_machine::done(const state_type& s) const -> bool {
  return s.current_phase == tls_handshake_state::phase::done ||
         s.current_phase == tls_handshake_state::phase::error;
}

// ============================================================================
// TLS read machine implementation
// ============================================================================

tls_read_machine::tls_read_machine(tls_connection& ctx, handle socket, std::span<std::byte> buffer)
    : ctx_(&ctx), socket_(socket), buffer_(buffer) {}

auto tls_read_machine::initial() const -> state_type {
  state_type s;
  s.socket_handle = socket_;
  s.output_buffer = buffer_;
  return s;
}

auto tls_read_machine::continue_read(state_type s) const -> step_result<state_type> {
  ssize_t result = tls_read(ctx_->raw(), s.output_buffer.data(), s.output_buffer.size());

  if (result > 0) {
    s.bytes_read = static_cast<std::size_t>(result);
    s.current_phase = tls_read_state::phase::done;
    return {std::move(s), {}};
  }

  if (result == TLS_WANT_POLLIN || result == TLS_WANT_POLLOUT) {
    s.current_phase = tls_read_state::phase::waiting_for_poll;
    short poll_events = (result == TLS_WANT_POLLIN) ? POLLIN : POLLOUT;
    std::vector<operation> ops;
    ops.push_back(operation::make_poll_add(s.socket_handle, poll_events, 0));
    return {std::move(s), std::move(ops)};
  }

  if (result == 0) {
    // EOF
    s.bytes_read = 0;
    s.current_phase = tls_read_state::phase::done;
    return {std::move(s), {}};
  }

  // Error
  s.current_phase = tls_read_state::phase::error;
  s.error_code = static_cast<int>(result);
  s.error_message = tls_error(ctx_->raw());
  return {std::move(s), {}};
}

auto tls_read_machine::step(state_type s, const event& e) const -> step_result<state_type> {
  switch (s.current_phase) {
    case tls_read_state::phase::initial:
      s.current_phase = tls_read_state::phase::reading;
      return continue_read(std::move(s));

    case tls_read_state::phase::waiting_for_poll:
      if (!e.ok()) {
        s.current_phase = tls_read_state::phase::error;
        s.error_code = e.error_code();
        s.error_message = "poll failed";
        return {std::move(s), {}};
      }
      s.current_phase = tls_read_state::phase::reading;
      return continue_read(std::move(s));

    case tls_read_state::phase::reading:
      return continue_read(std::move(s));

    case tls_read_state::phase::done:
    case tls_read_state::phase::error:
      return {std::move(s), {}};
  }

  return {std::move(s), {}};
}

auto tls_read_machine::done(const state_type& s) const -> bool {
  return s.current_phase == tls_read_state::phase::done ||
         s.current_phase == tls_read_state::phase::error;
}

// ============================================================================
// TLS write machine implementation
// ============================================================================

tls_write_machine::tls_write_machine(tls_connection& ctx, handle socket,
                                     std::span<const std::byte> data)
    : ctx_(&ctx), socket_(socket), data_(data) {}

auto tls_write_machine::initial() const -> state_type {
  state_type s;
  s.socket_handle = socket_;
  s.input_buffer = data_;
  return s;
}

auto tls_write_machine::continue_write(state_type s) const -> step_result<state_type> {
  // Calculate remaining data to write
  const std::byte* ptr = s.input_buffer.data() + s.bytes_written;
  std::size_t remaining = s.input_buffer.size() - s.bytes_written;

  if (remaining == 0) {
    s.current_phase = tls_write_state::phase::done;
    return {std::move(s), {}};
  }

  ssize_t result = tls_write(ctx_->raw(), ptr, remaining);

  if (result > 0) {
    s.bytes_written += static_cast<std::size_t>(result);
    if (s.bytes_written >= s.input_buffer.size()) {
      s.current_phase = tls_write_state::phase::done;
    }
    return {std::move(s), {}};
  }

  if (result == TLS_WANT_POLLIN || result == TLS_WANT_POLLOUT) {
    s.current_phase = tls_write_state::phase::waiting_for_poll;
    short poll_events = (result == TLS_WANT_POLLIN) ? POLLIN : POLLOUT;
    std::vector<operation> ops;
    ops.push_back(operation::make_poll_add(s.socket_handle, poll_events, 0));
    return {std::move(s), std::move(ops)};
  }

  // Error
  s.current_phase = tls_write_state::phase::error;
  s.error_code = static_cast<int>(result);
  s.error_message = tls_error(ctx_->raw());
  return {std::move(s), {}};
}

auto tls_write_machine::step(state_type s, const event& e) const -> step_result<state_type> {
  switch (s.current_phase) {
    case tls_write_state::phase::initial:
      s.current_phase = tls_write_state::phase::writing;
      return continue_write(std::move(s));

    case tls_write_state::phase::waiting_for_poll:
      if (!e.ok()) {
        s.current_phase = tls_write_state::phase::error;
        s.error_code = e.error_code();
        s.error_message = "poll failed";
        return {std::move(s), {}};
      }
      s.current_phase = tls_write_state::phase::writing;
      return continue_write(std::move(s));

    case tls_write_state::phase::writing:
      return continue_write(std::move(s));

    case tls_write_state::phase::done:
    case tls_write_state::phase::error:
      return {std::move(s), {}};
  }

  return {std::move(s), {}};
}

auto tls_write_machine::done(const state_type& s) const -> bool {
  return s.current_phase == tls_write_state::phase::done ||
         s.current_phase == tls_write_state::phase::error;
}

// ============================================================================
// TLS close machine implementation
// ============================================================================

tls_close_machine::tls_close_machine(tls_connection& ctx, handle socket)
    : ctx_(&ctx), socket_(socket) {}

auto tls_close_machine::initial() const -> state_type {
  state_type s;
  s.socket_handle = socket_;
  return s;
}

auto tls_close_machine::continue_close(state_type s) const -> step_result<state_type> {
  int result = tls_close(ctx_->raw());

  if (result == 0) {
    s.current_phase = tls_close_state::phase::done;
    return {std::move(s), {}};
  }

  if (result == TLS_WANT_POLLIN || result == TLS_WANT_POLLOUT) {
    s.current_phase = tls_close_state::phase::waiting_for_poll;
    short poll_events = (result == TLS_WANT_POLLIN) ? POLLIN : POLLOUT;
    std::vector<operation> ops;
    ops.push_back(operation::make_poll_add(s.socket_handle, poll_events, 0));
    return {std::move(s), std::move(ops)};
  }

  // Error (but often okay to ignore for close)
  s.current_phase = tls_close_state::phase::error;
  s.error_code = result;
  s.error_message = tls_error(ctx_->raw());
  return {std::move(s), {}};
}

auto tls_close_machine::step(state_type s, const event& e) const -> step_result<state_type> {
  switch (s.current_phase) {
    case tls_close_state::phase::initial:
      s.current_phase = tls_close_state::phase::closing;
      return continue_close(std::move(s));

    case tls_close_state::phase::waiting_for_poll:
      if (!e.ok()) {
        s.current_phase = tls_close_state::phase::error;
        s.error_code = e.error_code();
        s.error_message = "poll failed";
        return {std::move(s), {}};
      }
      s.current_phase = tls_close_state::phase::closing;
      return continue_close(std::move(s));

    case tls_close_state::phase::closing:
      return continue_close(std::move(s));

    case tls_close_state::phase::done:
    case tls_close_state::phase::error:
      return {std::move(s), {}};
  }

  return {std::move(s), {}};
}

auto tls_close_machine::done(const state_type& s) const -> bool {
  return s.current_phase == tls_close_state::phase::done ||
         s.current_phase == tls_close_state::phase::error;
}

} // namespace evring
