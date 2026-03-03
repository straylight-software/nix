// http2.cpp - HTTP/2 state machines using nghttp2
//
// Implements HTTP/2 connection and request machines for evring

#include "straylight/evring/http2.h"

#include <cstdio>
#include <cstring>

#include <nghttp2/nghttp2.h>
#include <poll.h>
#include <tls.h>

// Debug logging - enable by defining HTTP2_DEBUG=1 before including this file
// or compile with -DHTTP2_DEBUG=1
#ifndef HTTP2_DEBUG
#  define HTTP2_DEBUG 0
#endif

#if HTTP2_DEBUG
#  define H2_LOG(...) std::fprintf(stderr, "[H2] " __VA_ARGS__)
#else
#  define H2_LOG(...) ((void)0)
#endif

#if HTTP2_DEBUG
#  define H2_LOG(...) std::fprintf(stderr, "[H2] " __VA_ARGS__)
#else
#  define H2_LOG(...) ((void)0)
#endif

namespace evring {

// ============================================================================
// Error string conversion
// ============================================================================

auto http2_error_string(http2_error_code error) noexcept -> std::string_view {
  switch (error) {
    case http2_error_code::no_error:
      return "no error";
    case http2_error_code::protocol_error:
      return "protocol error";
    case http2_error_code::internal_error:
      return "internal error";
    case http2_error_code::flow_control_error:
      return "flow control error";
    case http2_error_code::settings_timeout:
      return "settings timeout";
    case http2_error_code::stream_closed:
      return "stream closed";
    case http2_error_code::frame_size_error:
      return "frame size error";
    case http2_error_code::refused_stream:
      return "refused stream";
    case http2_error_code::cancel:
      return "cancel";
    case http2_error_code::compression_error:
      return "compression error";
    case http2_error_code::connect_error:
      return "connect error";
    case http2_error_code::enhance_your_calm:
      return "enhance your calm";
    case http2_error_code::inadequate_security:
      return "inadequate security";
    case http2_error_code::http_1_1_required:
      return "HTTP/1.1 required";
    case http2_error_code::connection_closed:
      return "connection closed";
    case http2_error_code::tls_error:
      return "TLS error";
    default:
      return "unknown error";
  }
}

// ============================================================================
// http2_request implementation
// ============================================================================

auto http2_request::all_headers() const -> http2_headers {
  http2_headers result;
  result.reserve(4 + headers.size());

  // Pseudo-headers must come first
  result.push_back({":method", method});
  result.push_back({":scheme", scheme});
  result.push_back({":authority", authority});
  result.push_back({":path", path});

  // Regular headers
  for (const auto& h : headers) {
    result.push_back(h);
  }

  return result;
}

// ============================================================================
// http2_session implementation
// ============================================================================

// nghttp2 callback forward declarations
namespace {

ssize_t nghttp2_send_cb(nghttp2_session* session, const uint8_t* data, size_t length, int flags,
                        void* user_data);

int nghttp2_header_cb(nghttp2_session* session, const nghttp2_frame* frame, const uint8_t* name,
                      size_t namelen, const uint8_t* value, size_t valuelen, uint8_t flags,
                      void* user_data);

int nghttp2_data_chunk_recv_cb(nghttp2_session* session, uint8_t flags, int32_t stream_id,
                               const uint8_t* data, size_t len, void* user_data);

int nghttp2_stream_close_cb(nghttp2_session* session, int32_t stream_id, uint32_t error_code,
                            void* user_data);

int nghttp2_frame_recv_cb(nghttp2_session* session, const nghttp2_frame* frame, void* user_data);

} // namespace

http2_session::http2_session() = default;

http2_session::~http2_session() {
  if (session_) {
    nghttp2_session_del(session_);
  }
}

http2_session::http2_session(http2_session&& other) noexcept
    : session_(other.session_),
      local_settings_(other.local_settings_),
      remote_settings_(other.remote_settings_),
      pending_headers_(std::move(other.pending_headers_)),
      on_headers_(std::move(other.on_headers_)),
      on_data_(std::move(other.on_data_)),
      on_stream_close_(std::move(other.on_stream_close_)),
      send_buffer_(std::move(other.send_buffer_)) {
  other.session_ = nullptr;
}

http2_session& http2_session::operator=(http2_session&& other) noexcept {
  if (this != &other) {
    if (session_) {
      nghttp2_session_del(session_);
    }
    session_ = other.session_;
    local_settings_ = other.local_settings_;
    remote_settings_ = other.remote_settings_;
    pending_headers_ = std::move(other.pending_headers_);
    on_headers_ = std::move(other.on_headers_);
    on_data_ = std::move(other.on_data_);
    on_stream_close_ = std::move(other.on_stream_close_);
    send_buffer_ = std::move(other.send_buffer_);
    other.session_ = nullptr;
  }
  return *this;
}

auto http2_session::init_client(const http2_settings& settings) -> bool {
  if (session_) {
    return false; // Already initialized
  }

  local_settings_ = settings;

  // Create callbacks
  nghttp2_session_callbacks* callbacks = nullptr;
  if (nghttp2_session_callbacks_new(&callbacks) != 0) {
    return false;
  }

  nghttp2_session_callbacks_set_send_callback(callbacks, nghttp2_send_cb);
  nghttp2_session_callbacks_set_on_header_callback(callbacks, nghttp2_header_cb);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, nghttp2_data_chunk_recv_cb);
  nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, nghttp2_stream_close_cb);
  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, nghttp2_frame_recv_cb);

  // Create session
  int rv = nghttp2_session_client_new(&session_, callbacks, this);
  nghttp2_session_callbacks_del(callbacks);

  if (rv != 0) {
    return false;
  }

  // Submit initial SETTINGS
  std::vector<nghttp2_settings_entry> iv;
  iv.push_back({NGHTTP2_SETTINGS_HEADER_TABLE_SIZE, local_settings_.header_table_size});
  iv.push_back({NGHTTP2_SETTINGS_ENABLE_PUSH, local_settings_.enable_push});
  iv.push_back({NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, local_settings_.max_concurrent_streams});
  iv.push_back({NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, local_settings_.initial_window_size});
  iv.push_back({NGHTTP2_SETTINGS_MAX_FRAME_SIZE, local_settings_.max_frame_size});
  iv.push_back({NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, local_settings_.max_header_list_size});

  rv = nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv.data(), iv.size());
  if (rv != 0) {
    nghttp2_session_del(session_);
    session_ = nullptr;
    return false;
  }

  // Trigger send to populate send buffer with preface + SETTINGS
  nghttp2_session_send(session_);

  return true;
}

auto http2_session::submit_request(const http2_request& req) -> std::int32_t {
  if (!session_) {
    return -1;
  }

  auto all = req.all_headers();
  std::vector<nghttp2_nv> nv;
  nv.reserve(all.size());

  for (const auto& h : all) {
    nv.push_back({const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(h.name.data())),
                  const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(h.value.data())),
                  h.name.size(), h.value.size(), NGHTTP2_NV_FLAG_NONE});
  }

  // For now, no request body support (data provider)
  // TODO: Add data provider for POST/PUT bodies
  nghttp2_data_provider* data_prd = nullptr;

  std::int32_t stream_id =
      nghttp2_submit_request(session_, nullptr, nv.data(), nv.size(), data_prd, nullptr);

  if (stream_id > 0) {
    // Trigger send to populate send buffer
    nghttp2_session_send(session_);
  }

  return stream_id;
}

auto http2_session::get_pending_data() -> std::vector<std::byte> {
  std::vector<std::byte> result;
  result.swap(send_buffer_);
  return result;
}

auto http2_session::receive_data(std::span<const std::byte> data) -> std::int64_t {
  if (!session_) {
    return -1;
  }

  ssize_t consumed = nghttp2_session_mem_recv(
      session_, reinterpret_cast<const uint8_t*>(data.data()), data.size());

  if (consumed >= 0) {
    // Send any response frames (like SETTINGS ACK)
    nghttp2_session_send(session_);
  }

  return consumed;
}

auto http2_session::wants_write() const noexcept -> bool {
  return session_ && nghttp2_session_want_write(session_);
}

auto http2_session::wants_read() const noexcept -> bool {
  return session_ && nghttp2_session_want_read(session_);
}

void http2_session::handle_header(std::int32_t stream_id, std::string_view name,
                                  std::string_view value) {
  pending_headers_[stream_id].push_back({std::string(name), std::string(value)});

  // Also store in response
  auto& resp = stream_responses_[stream_id];
  if (name == ":status") {
    resp.status_code = std::stoi(std::string(value));
  } else if (!name.empty() && name[0] != ':') {
    resp.headers.push_back({std::string(name), std::string(value)});
  }
}

void http2_session::handle_data(std::int32_t stream_id, std::span<const std::byte> data) {
  // Store in response
  auto& resp = stream_responses_[stream_id];
  resp.body.insert(resp.body.end(), data.begin(), data.end());

  if (on_data_) {
    on_data_(stream_id, data);
  }
}

void http2_session::handle_stream_close(std::int32_t stream_id, std::uint32_t error_code) {
  // Mark stream as closed
  closed_streams_[stream_id] = static_cast<http2_error_code>(error_code);

  // Deliver any pending headers first
  auto it = pending_headers_.find(stream_id);
  if (it != pending_headers_.end() && on_headers_) {
    on_headers_(stream_id, it->second);
    pending_headers_.erase(it);
  }

  if (on_stream_close_) {
    on_stream_close_(stream_id, static_cast<http2_error_code>(error_code));
  }
}

auto http2_session::get_stream_response(std::int32_t stream_id) -> http2_response* {
  auto it = stream_responses_.find(stream_id);
  if (it != stream_responses_.end()) {
    return &it->second;
  }
  return nullptr;
}

auto http2_session::is_stream_closed(std::int32_t stream_id) const -> bool {
  return closed_streams_.find(stream_id) != closed_streams_.end();
}

auto http2_session::get_stream_error(std::int32_t stream_id) const -> http2_error_code {
  auto it = closed_streams_.find(stream_id);
  if (it != closed_streams_.end()) {
    return it->second;
  }
  return http2_error_code::no_error;
}

void http2_session::handle_settings(const http2_settings& settings) {
  remote_settings_ = settings;
}

void http2_session::handle_frame_recv_headers(std::int32_t stream_id) {
  auto it = pending_headers_.find(stream_id);
  if (it != pending_headers_.end() && on_headers_) {
    on_headers_(stream_id, it->second);
    // Keep headers for potential later use
  }
}

void http2_session::append_send_data(const std::byte* data, std::size_t length) {
  send_buffer_.insert(send_buffer_.end(), data, data + length);
}

auto http2_session::get_pending_headers(std::int32_t stream_id) const -> const http2_headers* {
  auto it = pending_headers_.find(stream_id);
  if (it != pending_headers_.end()) {
    return &it->second;
  }
  return nullptr;
}

// ============================================================================
// nghttp2 callbacks
// ============================================================================

namespace {

ssize_t nghttp2_send_cb([[maybe_unused]] nghttp2_session* session, const uint8_t* data,
                        size_t length, [[maybe_unused]] int flags, void* user_data) {
  auto* sess = static_cast<http2_session*>(user_data);
  auto* byte_data = reinterpret_cast<const std::byte*>(data);
  sess->append_send_data(byte_data, length);
  return static_cast<ssize_t>(length);
}

int nghttp2_header_cb([[maybe_unused]] nghttp2_session* session, const nghttp2_frame* frame,
                      const uint8_t* name, size_t namelen, const uint8_t* value, size_t valuelen,
                      [[maybe_unused]] uint8_t flags, void* user_data) {
  if (frame->hd.type != NGHTTP2_HEADERS) {
    return 0;
  }

  auto* sess = static_cast<http2_session*>(user_data);
  sess->handle_header(frame->hd.stream_id,
                      std::string_view(reinterpret_cast<const char*>(name), namelen),
                      std::string_view(reinterpret_cast<const char*>(value), valuelen));
  return 0;
}

int nghttp2_data_chunk_recv_cb([[maybe_unused]] nghttp2_session* session,
                               [[maybe_unused]] uint8_t flags, int32_t stream_id,
                               const uint8_t* data, size_t len, void* user_data) {
  auto* sess = static_cast<http2_session*>(user_data);
  sess->handle_data(stream_id,
                    std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), len));
  return 0;
}

int nghttp2_stream_close_cb([[maybe_unused]] nghttp2_session* session, int32_t stream_id,
                            uint32_t error_code, void* user_data) {
  auto* sess = static_cast<http2_session*>(user_data);
  sess->handle_stream_close(stream_id, error_code);
  return 0;
}

int nghttp2_frame_recv_cb([[maybe_unused]] nghttp2_session* session, const nghttp2_frame* frame,
                          void* user_data) {
  auto* sess = static_cast<http2_session*>(user_data);

  switch (frame->hd.type) {
    case NGHTTP2_SETTINGS: {
      if (!(frame->hd.flags & NGHTTP2_FLAG_ACK)) {
        // Update remote settings
        http2_settings settings = sess->remote_settings();
        for (size_t i = 0; i < frame->settings.niv; ++i) {
          auto& iv = frame->settings.iv[i];
          switch (iv.settings_id) {
            case NGHTTP2_SETTINGS_HEADER_TABLE_SIZE:
              settings.header_table_size = iv.value;
              break;
            case NGHTTP2_SETTINGS_ENABLE_PUSH:
              settings.enable_push = iv.value;
              break;
            case NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS:
              settings.max_concurrent_streams = iv.value;
              break;
            case NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE:
              settings.initial_window_size = iv.value;
              break;
            case NGHTTP2_SETTINGS_MAX_FRAME_SIZE:
              settings.max_frame_size = iv.value;
              break;
            case NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE:
              settings.max_header_list_size = iv.value;
              break;
          }
        }
        sess->handle_settings(settings);
      }
      break;
    }

    case NGHTTP2_HEADERS: {
      // End of headers - deliver if END_HEADERS flag set
      if (frame->hd.flags & NGHTTP2_FLAG_END_HEADERS) {
        sess->handle_frame_recv_headers(frame->hd.stream_id);
      }
      break;
    }

    default:
      break;
  }

  return 0;
}

} // namespace

// ============================================================================
// http2_connection_machine implementation
// ============================================================================

http2_connection_machine::http2_connection_machine(http2_session& session, tls_connection& tls_conn,
                                                   handle socket)
    : session_(&session), tls_conn_(&tls_conn), socket_(socket) {}

auto http2_connection_machine::initial() const -> state_type {
  state_type s;
  s.socket_handle = socket_;
  return s;
}

auto http2_connection_machine::do_tls_write(state_type s) const -> step_result<state_type> {
  H2_LOG("conn::do_tls_write: phase=%d, send_buffer.size=%zu, settings_received=%d\n",
         static_cast<int>(s.current_phase), s.send_buffer.size(), s.settings_received);

  // Write pending data via TLS
  if (s.send_buffer.empty()) {
    s.send_buffer = session_->get_pending_data();
    H2_LOG("conn::do_tls_write: got pending data, size=%zu\n", s.send_buffer.size());
  }

  if (s.send_buffer.empty()) {
    // Nothing to send.
    // If we've already received server's SETTINGS and sent our ACK,
    // the connection is ready - we don't need to wait for the server's ACK of our SETTINGS.
    if (s.settings_received) {
      H2_LOG("conn::do_tls_write: nothing to send, settings received -> connected!\n");
      s.current_phase = http2_connection_state::phase::connected;
      return {std::move(s), {}};
    }
    // Otherwise, try to read the server's SETTINGS
    H2_LOG("conn::do_tls_write: nothing to send, calling do_tls_read\n");
    return do_tls_read(std::move(s));
  }

  H2_LOG("conn::do_tls_write: calling tls_write with %zu bytes\n", s.send_buffer.size());
  ssize_t written = tls_write(tls_conn_->raw(), s.send_buffer.data(), s.send_buffer.size());
  H2_LOG("conn::do_tls_write: tls_write returned %zd\n", written);

  if (written > 0) {
    s.send_buffer.erase(s.send_buffer.begin(), s.send_buffer.begin() + written);
    if (s.send_buffer.empty()) {
      // All sent.
      // If we've received server's SETTINGS and just sent our ACK, we're done.
      if (s.settings_received) {
        H2_LOG("conn::do_tls_write: all sent, settings received -> connected!\n");
        s.current_phase = http2_connection_state::phase::connected;
        return {std::move(s), {}};
      }
      // Otherwise, try to read the server's SETTINGS
      H2_LOG("conn::do_tls_write: all sent, calling do_tls_read\n");
      return do_tls_read(std::move(s));
    }
    // More to send - poll for write readiness
    H2_LOG("conn::do_tls_write: partial write, polling for POLLOUT\n");
    s.current_phase = http2_connection_state::phase::waiting_write;
    std::vector<operation> ops;
    ops.push_back(operation::make_poll_add(s.socket_handle, POLLOUT, ++s.operation_id));
    return {std::move(s), std::move(ops)};
  }

  if (written == TLS_WANT_POLLIN || written == TLS_WANT_POLLOUT) {
    H2_LOG("conn::do_tls_write: TLS_WANT_%s\n", written == TLS_WANT_POLLIN ? "POLLIN" : "POLLOUT");
    s.current_phase = http2_connection_state::phase::waiting_write;
    short poll_events = (written == TLS_WANT_POLLIN) ? POLLIN : POLLOUT;
    std::vector<operation> ops;
    ops.push_back(operation::make_poll_add(s.socket_handle, poll_events, ++s.operation_id));
    return {std::move(s), std::move(ops)};
  }

  // Error
  H2_LOG("conn::do_tls_write: ERROR - %s\n", tls_error(tls_conn_->raw()));
  s.current_phase = http2_connection_state::phase::error;
  s.error_code = http2_error_code::tls_error;
  s.error_message = tls_error(tls_conn_->raw());
  return {std::move(s), {}};
}

auto http2_connection_machine::do_tls_read(state_type s) const -> step_result<state_type> {
  H2_LOG("conn::do_tls_read: phase=%d, settings_received=%d\n", static_cast<int>(s.current_phase),
         s.settings_received);

  // Read server SETTINGS
  std::byte buffer[4096];
  H2_LOG("conn::do_tls_read: calling tls_read...\n");
  ssize_t nread = tls_read(tls_conn_->raw(), buffer, sizeof(buffer));
  H2_LOG("conn::do_tls_read: tls_read returned %zd\n", nread);

  if (nread > 0) {
    H2_LOG("conn::do_tls_read: received %zd bytes, passing to nghttp2\n", nread);
    auto consumed = session_->receive_data(std::span<const std::byte>(buffer, nread));
    H2_LOG("conn::do_tls_read: nghttp2 consumed %ld bytes\n", consumed);
    if (consumed < 0) {
      H2_LOG("conn::do_tls_read: nghttp2 receive error\n");
      s.current_phase = http2_connection_state::phase::error;
      s.error_code = http2_error_code::protocol_error;
      s.error_message = "nghttp2 receive error";
      return {std::move(s), {}};
    }

    // Mark that we've received server's SETTINGS (if we haven't already)
    // This is detected by nghttp2 generating a SETTINGS ACK
    auto pending = session_->get_pending_data();
    H2_LOG("conn::do_tls_read: pending data to send: %zu bytes\n", pending.size());

    // If nghttp2 wants to send data (like SETTINGS ACK), the server sent us something
    if (!pending.empty() && !s.settings_received) {
      H2_LOG("conn::do_tls_read: marking settings_received=true\n");
      s.settings_received = true;
    }

    if (!pending.empty()) {
      s.send_buffer = std::move(pending);
      // Poll for write readiness to send the ACK
      s.current_phase = http2_connection_state::phase::waiting_write;
      std::vector<operation> ops;
      ops.push_back(operation::make_poll_add(s.socket_handle, POLLOUT, ++s.operation_id));
      return {std::move(s), std::move(ops)};
    }

    // No pending data - if we've received SETTINGS, we're done
    if (s.settings_received) {
      H2_LOG("conn::do_tls_read: connection established!\n");
      s.current_phase = http2_connection_state::phase::connected;
      return {std::move(s), {}};
    }

    // Otherwise poll for more data (waiting for server's SETTINGS)
    H2_LOG("conn::do_tls_read: waiting for more data...\n");
    s.current_phase = http2_connection_state::phase::waiting_read;
    std::vector<operation> ops;
    ops.push_back(operation::make_poll_add(s.socket_handle, POLLIN, ++s.operation_id));
    return {std::move(s), std::move(ops)};
  }

  if (nread == TLS_WANT_POLLIN || nread == TLS_WANT_POLLOUT) {
    H2_LOG("conn::do_tls_read: TLS_WANT_%s - polling...\n",
           nread == TLS_WANT_POLLIN ? "POLLIN" : "POLLOUT");
    s.current_phase = http2_connection_state::phase::waiting_read;
    short poll_events = (nread == TLS_WANT_POLLIN) ? POLLIN : POLLOUT;
    std::vector<operation> ops;
    ops.push_back(operation::make_poll_add(s.socket_handle, poll_events, ++s.operation_id));
    return {std::move(s), std::move(ops)};
  }

  if (nread == 0) {
    H2_LOG("conn::do_tls_read: connection closed by peer\n");
    s.current_phase = http2_connection_state::phase::error;
    s.error_code = http2_error_code::connection_closed;
    s.error_message = "connection closed by peer";
    return {std::move(s), {}};
  }

  // Error
  H2_LOG("conn::do_tls_read: ERROR - %s\n", tls_error(tls_conn_->raw()));
  s.current_phase = http2_connection_state::phase::error;
  s.error_code = http2_error_code::tls_error;
  s.error_message = tls_error(tls_conn_->raw());
  return {std::move(s), {}};
}

auto http2_connection_machine::step(state_type s, const event& e) const -> step_result<state_type> {
  H2_LOG("conn::step: phase=%d, event.ok=%d, event.result=%d\n", static_cast<int>(s.current_phase),
         e.ok(), e.result);

  switch (s.current_phase) {
    case http2_connection_state::phase::initial: {
      H2_LOG("conn::step: initial -> sending_preface\n");
      // Start sending preface + SETTINGS
      s.current_phase = http2_connection_state::phase::sending_preface;
      return do_tls_write(std::move(s));
    }

    case http2_connection_state::phase::sending_preface: {
      H2_LOG("conn::step: sending_preface\n");
      return do_tls_write(std::move(s));
    }

    case http2_connection_state::phase::waiting_write: {
      H2_LOG("conn::step: waiting_write, event.ok=%d\n", e.ok());
      if (!e.ok()) {
        s.current_phase = http2_connection_state::phase::error;
        s.error_code = http2_error_code::tls_error;
        s.error_message = "poll failed";
        return {std::move(s), {}};
      }
      return do_tls_write(std::move(s));
    }

    case http2_connection_state::phase::waiting_read: {
      H2_LOG("conn::step: waiting_read, event.ok=%d\n", e.ok());
      if (!e.ok()) {
        s.current_phase = http2_connection_state::phase::error;
        s.error_code = http2_error_code::tls_error;
        s.error_message = "poll failed";
        return {std::move(s), {}};
      }
      return do_tls_read(std::move(s));
    }

    case http2_connection_state::phase::connected:
      H2_LOG("conn::step: connected (done)\n");
      return {std::move(s), {}};
    case http2_connection_state::phase::error:
      H2_LOG("conn::step: error (done)\n");
      return {std::move(s), {}};
  }

  return {std::move(s), {}};
}

auto http2_connection_machine::done(const state_type& s) const -> bool {
  return s.current_phase == http2_connection_state::phase::connected ||
         s.current_phase == http2_connection_state::phase::error;
}

// ============================================================================
// http2_request_machine implementation
// ============================================================================

http2_request_machine::http2_request_machine(http2_session& session, tls_connection& tls_conn,
                                             handle socket, http2_request request)
    : session_(&session), tls_conn_(&tls_conn), socket_(socket), request_(std::move(request)) {}

auto http2_request_machine::initial() const -> state_type {
  state_type s;
  s.socket_handle = socket_;
  s.request = request_;
  return s;
}

auto http2_request_machine::do_tls_write(state_type s) const -> step_result<state_type> {
  if (s.send_buffer.empty()) {
    s.send_buffer = session_->get_pending_data();
  }

  if (s.send_buffer.empty()) {
    // Nothing to send - try to read immediately instead of polling.
    // This is critical: libtls may have buffered data from a previous read,
    // and poll(POLLIN) won't fire if data is already in the TLS buffer.
    s.current_phase = http2_request_state::phase::receiving;
    return do_tls_read(std::move(s));
  }

  ssize_t written = tls_write(tls_conn_->raw(), s.send_buffer.data(), s.send_buffer.size());

  if (written > 0) {
    s.send_buffer.erase(s.send_buffer.begin(), s.send_buffer.begin() + written);
    if (s.send_buffer.empty()) {
      // All sent - try to read immediately instead of polling.
      // libtls may have buffered data from a previous read.
      s.current_phase = http2_request_state::phase::receiving;
      return do_tls_read(std::move(s));
    }
    // More to send - poll for write readiness
    s.current_phase = http2_request_state::phase::waiting_write;
    std::vector<operation> ops;
    ops.push_back(operation::make_poll_add(s.socket_handle, POLLOUT, ++s.operation_id));
    return {std::move(s), std::move(ops)};
  }

  if (written == TLS_WANT_POLLIN || written == TLS_WANT_POLLOUT) {
    s.current_phase = http2_request_state::phase::waiting_write;
    short poll_events = (written == TLS_WANT_POLLIN) ? POLLIN : POLLOUT;
    std::vector<operation> ops;
    ops.push_back(operation::make_poll_add(s.socket_handle, poll_events, ++s.operation_id));
    return {std::move(s), std::move(ops)};
  }

  s.current_phase = http2_request_state::phase::error;
  s.error_code = http2_error_code::tls_error;
  s.error_message = tls_error(tls_conn_->raw());
  return {std::move(s), {}};
}

auto http2_request_machine::do_tls_read(state_type s) const -> step_result<state_type> {
  std::byte buffer[16384];
  ssize_t nread = tls_read(tls_conn_->raw(), buffer, sizeof(buffer));

  if (nread > 0) {
    auto consumed = session_->receive_data(std::span<const std::byte>(buffer, nread));
    if (consumed < 0) {
      s.current_phase = http2_request_state::phase::error;
      s.error_code = http2_error_code::protocol_error;
      s.error_message = "nghttp2 receive error";
      return {std::move(s), {}};
    }

    // Check if stream is closed
    if (session_->is_stream_closed(s.stream_id)) {
      // Copy response from session
      auto* resp = session_->get_stream_response(s.stream_id);
      if (resp) {
        s.response = *resp;
      }

      auto err = session_->get_stream_error(s.stream_id);
      if (err == http2_error_code::no_error) {
        s.current_phase = http2_request_state::phase::done;
      } else {
        s.current_phase = http2_request_state::phase::error;
        s.error_code = err;
      }
      return {std::move(s), {}};
    }

    // Check if we need to send anything (e.g., WINDOW_UPDATE)
    auto pending = session_->get_pending_data();
    if (!pending.empty()) {
      s.send_buffer = std::move(pending);
      s.current_phase = http2_request_state::phase::waiting_write;
      std::vector<operation> ops;
      ops.push_back(operation::make_poll_add(s.socket_handle, POLLOUT, ++s.operation_id));
      return {std::move(s), std::move(ops)};
    }

    // Keep reading - poll for more data
    s.current_phase = http2_request_state::phase::waiting_read;
    std::vector<operation> ops;
    ops.push_back(operation::make_poll_add(s.socket_handle, POLLIN, ++s.operation_id));
    return {std::move(s), std::move(ops)};
  }

  if (nread == TLS_WANT_POLLIN || nread == TLS_WANT_POLLOUT) {
    s.current_phase = http2_request_state::phase::waiting_read;
    short poll_events = (nread == TLS_WANT_POLLIN) ? POLLIN : POLLOUT;
    std::vector<operation> ops;
    ops.push_back(operation::make_poll_add(s.socket_handle, poll_events, ++s.operation_id));
    return {std::move(s), std::move(ops)};
  }

  if (nread == 0) {
    // EOF - connection closed, check if we have a response
    if (s.response.status_code > 0) {
      s.current_phase = http2_request_state::phase::done;
    } else {
      s.current_phase = http2_request_state::phase::error;
      s.error_code = http2_error_code::connection_closed;
      s.error_message = "connection closed before response";
    }
    return {std::move(s), {}};
  }

  s.current_phase = http2_request_state::phase::error;
  s.error_code = http2_error_code::tls_error;
  s.error_message = tls_error(tls_conn_->raw());
  return {std::move(s), {}};
}

auto http2_request_machine::step(state_type s, const event& e) const -> step_result<state_type> {
  switch (s.current_phase) {
    case http2_request_state::phase::initial: {
      // Submit request to nghttp2
      s.stream_id = session_->submit_request(s.request);
      if (s.stream_id < 0) {
        s.current_phase = http2_request_state::phase::error;
        s.error_code = http2_error_code::internal_error;
        s.error_message = "failed to submit request";
        return {std::move(s), {}};
      }

      s.current_phase = http2_request_state::phase::sending;
      return do_tls_write(std::move(s));
    }

    case http2_request_state::phase::submitting:
    case http2_request_state::phase::sending: {
      return do_tls_write(std::move(s));
    }

    case http2_request_state::phase::waiting_write: {
      if (!e.ok()) {
        s.current_phase = http2_request_state::phase::error;
        s.error_code = http2_error_code::tls_error;
        s.error_message = "poll failed";
        return {std::move(s), {}};
      }
      return do_tls_write(std::move(s));
    }

    case http2_request_state::phase::waiting_read: {
      if (!e.ok()) {
        s.current_phase = http2_request_state::phase::error;
        s.error_code = http2_error_code::tls_error;
        s.error_message = "poll failed";
        return {std::move(s), {}};
      }
      return do_tls_read(std::move(s));
    }

    case http2_request_state::phase::receiving: {
      return do_tls_read(std::move(s));
    }

    case http2_request_state::phase::done:
    case http2_request_state::phase::error:
      return {std::move(s), {}};
  }

  return {std::move(s), {}};
}

auto http2_request_machine::done(const state_type& s) const -> bool {
  return s.current_phase == http2_request_state::phase::done ||
         s.current_phase == http2_request_state::phase::error;
}

} // namespace evring
