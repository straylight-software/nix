// http1.cpp - HTTP/1.1 implementation using llhttp
//
// Callback-based parsing with state machine I/O

#include "straylight/evring/http1.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <llhttp.h>
#include <poll.h>
#include <tls.h>

#include "straylight/evring/evring.h"

namespace evring {

// ============================================================================
// Method string conversion
// ============================================================================

auto http1_method_string(http1_method method) noexcept -> std::string_view {
  switch (method) {
    case http1_method::del:
      return "DELETE";
    case http1_method::get:
      return "GET";
    case http1_method::head:
      return "HEAD";
    case http1_method::post:
      return "POST";
    case http1_method::put:
      return "PUT";
    case http1_method::connect:
      return "CONNECT";
    case http1_method::options:
      return "OPTIONS";
    case http1_method::trace:
      return "TRACE";
    case http1_method::patch:
      return "PATCH";
    default:
      return "GET";
  }
}

auto http1_method_parse(std::string_view method) noexcept -> http1_method {
  if (method == "GET") {
    return http1_method::get;
  }
  if (method == "POST") {
    return http1_method::post;
  }
  if (method == "PUT") {
    return http1_method::put;
  }
  if (method == "DELETE") {
    return http1_method::del;
  }
  if (method == "HEAD") {
    return http1_method::head;
  }
  if (method == "PATCH") {
    return http1_method::patch;
  }
  if (method == "OPTIONS") {
    return http1_method::options;
  }
  if (method == "CONNECT") {
    return http1_method::connect;
  }
  if (method == "TRACE") {
    return http1_method::trace;
  }
  return http1_method::get;
}

// ============================================================================
// http1_request implementation
// ============================================================================

auto http1_request::serialize() const -> std::vector<std::byte> {
  std::string result;

  // Request line
  result += http1_method_string(method);
  result += ' ';
  result += path;
  result += " HTTP/";
  result += std::to_string(version_major);
  result += '.';
  result += std::to_string(version_minor);
  result += "\r\n";

  // Headers
  for (const auto& header : headers) {
    result += header.name;
    result += ": ";
    result += header.value;
    result += "\r\n";
  }

  // Content-Length if body present
  if (!body.empty()) {
    bool has_content_length = false;
    for (const auto& h : headers) {
      if (strcasecmp(h.name.c_str(), "content-length") == 0) {
        has_content_length = true;
        break;
      }
    }
    if (!has_content_length) {
      result += "Content-Length: ";
      result += std::to_string(body.size());
      result += "\r\n";
    }
  }

  // End of headers
  result += "\r\n";

  // Convert to bytes
  std::vector<std::byte> output(result.size() + body.size());
  std::memcpy(output.data(), result.data(), result.size());
  if (!body.empty()) {
    std::memcpy(output.data() + result.size(), body.data(), body.size());
  }

  return output;
}

auto http1_request::get_header(std::string_view name) const -> std::string_view {
  for (const auto& h : headers) {
    if (strcasecmp(h.name.c_str(), std::string(name).c_str()) == 0) {
      return h.value;
    }
  }
  return {};
}

void http1_request::set_header(std::string name, std::string value) {
  for (auto& h : headers) {
    if (strcasecmp(h.name.c_str(), name.c_str()) == 0) {
      h.value = std::move(value);
      return;
    }
  }
  headers.push_back({std::move(name), std::move(value)});
}

auto http1_request::keep_alive() const -> bool {
  auto conn = get_header("Connection");
  if (version_minor >= 1) {
    // HTTP/1.1 defaults to keep-alive
    return conn.empty() || strcasecmp(std::string(conn).c_str(), "close") != 0;
  } else {
    // HTTP/1.0 defaults to close
    return strcasecmp(std::string(conn).c_str(), "keep-alive") == 0;
  }
}

// ============================================================================
// http1_response implementation
// ============================================================================

auto http1_response::get_header(std::string_view name) const -> std::string_view {
  for (const auto& h : headers) {
    if (strcasecmp(h.name.c_str(), std::string(name).c_str()) == 0) {
      return h.value;
    }
  }
  return {};
}

auto http1_response::keep_alive() const -> bool {
  auto conn = get_header("Connection");
  if (version_minor >= 1) {
    return conn.empty() || strcasecmp(std::string(conn).c_str(), "close") != 0;
  } else {
    return strcasecmp(std::string(conn).c_str(), "keep-alive") == 0;
  }
}

auto http1_response::content_length() const -> std::int64_t {
  auto cl = get_header("Content-Length");
  if (cl.empty()) {
    return -1;
  }
  try {
    return std::stoll(std::string(cl));
  } catch (...) {
    return -1;
  }
}

// ============================================================================
// http1_parser implementation
// ============================================================================

http1_parser::http1_parser() {
  parser_ = new llhttp_t;
  settings_ = new llhttp_settings_t;

  llhttp_settings_init(settings_);

  // Set up callbacks
  settings_->on_message_begin = on_message_begin;
  settings_->on_status = on_status;
  settings_->on_header_field = on_header_field;
  settings_->on_header_value = on_header_value;
  settings_->on_headers_complete = on_headers_complete;
  settings_->on_body = on_body;
  settings_->on_message_complete = on_message_complete;

  llhttp_init(parser_, HTTP_RESPONSE, settings_);
  parser_->data = this;
}

http1_parser::~http1_parser() {
  delete parser_;
  delete settings_;
}

http1_parser::http1_parser(http1_parser&& other) noexcept
    : parser_(other.parser_),
      settings_(other.settings_),
      response_(std::move(other.response_)),
      current_header_field_(std::move(other.current_header_field_)),
      current_header_value_(std::move(other.current_header_value_)),
      in_header_value_(other.in_header_value_),
      headers_complete_(other.headers_complete_),
      message_complete_(other.message_complete_),
      error_(other.error_) {
  other.parser_ = nullptr;
  other.settings_ = nullptr;
  if (parser_) {
    parser_->data = this;
  }
}

http1_parser& http1_parser::operator=(http1_parser&& other) noexcept {
  if (this != &other) {
    delete parser_;
    delete settings_;

    parser_ = other.parser_;
    settings_ = other.settings_;
    response_ = std::move(other.response_);
    current_header_field_ = std::move(other.current_header_field_);
    current_header_value_ = std::move(other.current_header_value_);
    in_header_value_ = other.in_header_value_;
    headers_complete_ = other.headers_complete_;
    message_complete_ = other.message_complete_;
    error_ = other.error_;

    other.parser_ = nullptr;
    other.settings_ = nullptr;
    if (parser_) {
      parser_->data = this;
    }
  }
  return *this;
}

void http1_parser::reset() {
  llhttp_reset(parser_);
  parser_->data = this;

  response_ = http1_response{};
  current_header_field_.clear();
  current_header_value_.clear();
  in_header_value_ = false;
  headers_complete_ = false;
  message_complete_ = false;
  error_ = 0;
}

auto http1_parser::parse(std::span<const std::byte> data) -> std::int64_t {
  if (data.empty()) {
    return 0;
  }

  const char* input = reinterpret_cast<const char*>(data.data());
  llhttp_errno_t err = llhttp_execute(parser_, input, data.size());

  if (err != HPE_OK && err != HPE_PAUSED) {
    error_ = static_cast<int>(err);
    return -1;
  }

  return static_cast<std::int64_t>(data.size());
}

auto http1_parser::error_message() const -> std::string {
  if (parser_ && error_ != 0) {
    return llhttp_errno_name(static_cast<llhttp_errno_t>(error_));
  }
  return {};
}

auto http1_parser::should_keep_alive() const noexcept -> bool {
  return parser_ && llhttp_should_keep_alive(parser_) != 0;
}

auto http1_parser::is_upgrade() const noexcept -> bool {
  return parser_ && (parser_->flags & F_UPGRADE) != 0;
}

// Parser callbacks
int http1_parser::on_message_begin(llhttp_t* parser) {
  auto* self = static_cast<http1_parser*>(parser->data);
  self->response_ = http1_response{};
  self->current_header_field_.clear();
  self->current_header_value_.clear();
  self->in_header_value_ = false;
  return 0;
}

int http1_parser::on_status(llhttp_t* parser, const char* at, std::size_t length) {
  auto* self = static_cast<http1_parser*>(parser->data);
  self->response_.status_message.append(at, length);
  return 0;
}

int http1_parser::on_header_field(llhttp_t* parser, const char* at, std::size_t length) {
  auto* self = static_cast<http1_parser*>(parser->data);

  // If we were reading a value, save the previous header
  if (self->in_header_value_ && !self->current_header_field_.empty()) {
    self->response_.headers.push_back(
        {std::move(self->current_header_field_), std::move(self->current_header_value_)});
    self->current_header_field_.clear();
    self->current_header_value_.clear();
  }

  self->current_header_field_.append(at, length);
  self->in_header_value_ = false;
  return 0;
}

int http1_parser::on_header_value(llhttp_t* parser, const char* at, std::size_t length) {
  auto* self = static_cast<http1_parser*>(parser->data);
  self->current_header_value_.append(at, length);
  self->in_header_value_ = true;
  return 0;
}

int http1_parser::on_headers_complete(llhttp_t* parser) {
  auto* self = static_cast<http1_parser*>(parser->data);

  // Save last header if any
  if (!self->current_header_field_.empty()) {
    self->response_.headers.push_back(
        {std::move(self->current_header_field_), std::move(self->current_header_value_)});
    self->current_header_field_.clear();
    self->current_header_value_.clear();
  }

  self->response_.status_code = parser->status_code;
  self->response_.version_major = parser->http_major;
  self->response_.version_minor = parser->http_minor;
  self->headers_complete_ = true;

  return 0;
}

int http1_parser::on_body(llhttp_t* parser, const char* at, std::size_t length) {
  auto* self = static_cast<http1_parser*>(parser->data);
  const auto* bytes = reinterpret_cast<const std::byte*>(at);
  self->response_.body.insert(self->response_.body.end(), bytes, bytes + length);
  return 0;
}

int http1_parser::on_message_complete(llhttp_t* parser) {
  auto* self = static_cast<http1_parser*>(parser->data);
  self->message_complete_ = true;
  return 0;
}

// ============================================================================
// http1_client_machine implementation (plain TCP)
// ============================================================================

http1_client_machine::http1_client_machine(handle socket, http1_request request)
    : socket_(socket), request_(std::move(request)) {
  recv_buffer_.resize(http1_default_buffer_size);
}

auto http1_client_machine::initial() const -> state_type {
  state_type s;
  s.socket_handle = socket_;
  s.send_buffer = request_.serialize();
  return s;
}

auto http1_client_machine::step(state_type s, const event& e) const -> step_result<state_type> {
  std::vector<operation> ops;

  switch (s.current_phase) {
    case state_type::phase::initial: {
      // Start sending request
      s.current_phase = state_type::phase::sending;
      s.bytes_sent = 0;
      [[fallthrough]];
    }

    case state_type::phase::sending: {
      // Send remaining data
      std::size_t remaining = s.send_buffer.size() - s.bytes_sent;
      if (remaining > 0) {
        std::span<const std::byte> to_send(s.send_buffer.data() + s.bytes_sent, remaining);
        ops.push_back(operation::make_send(s.socket_handle, to_send, 0, ++s.operation_id));
        s.current_phase = state_type::phase::waiting_write;
      } else {
        // All sent, start receiving
        s.current_phase = state_type::phase::receiving;
        ops.push_back(
            operation::make_recv(s.socket_handle, recv_buffer_span(), 0, ++s.operation_id));
        s.current_phase = state_type::phase::waiting_read;
      }
      break;
    }

    case state_type::phase::waiting_write: {
      if (!e.ok()) {
        s.current_phase = state_type::phase::error;
        s.error_code = e.error_code();
        s.error_message = "send failed";
        break;
      }

      s.bytes_sent += static_cast<std::size_t>(e.result);

      // Check if all sent
      if (s.bytes_sent >= s.send_buffer.size()) {
        // Start receiving
        ops.push_back(
            operation::make_recv(s.socket_handle, recv_buffer_span(), 0, ++s.operation_id));
        s.current_phase = state_type::phase::waiting_read;
      } else {
        // Send more
        std::size_t remaining = s.send_buffer.size() - s.bytes_sent;
        std::span<const std::byte> to_send(s.send_buffer.data() + s.bytes_sent, remaining);
        ops.push_back(operation::make_send(s.socket_handle, to_send, 0, ++s.operation_id));
      }
      break;
    }

    case state_type::phase::waiting_read: {
      if (e.result < 0) {
        s.current_phase = state_type::phase::error;
        s.error_code = e.error_code();
        s.error_message = "recv failed";
        break;
      }

      if (e.result == 0) {
        // Connection closed
        if (parser_.message_complete()) {
          s.response = parser_.response();
          s.message_complete = true;
          s.current_phase = state_type::phase::done;
        } else {
          s.current_phase = state_type::phase::error;
          s.error_code = -1;
          s.error_message = "connection closed before response complete";
        }
        break;
      }

      // Parse received data
      std::span<const std::byte> data(recv_buffer_.data(), static_cast<std::size_t>(e.result));
      auto parsed = parser_.parse(data);

      if (parsed < 0 || parser_.has_error()) {
        s.current_phase = state_type::phase::error;
        s.error_code = -1;
        s.error_message = "parse error: " + parser_.error_message();
        break;
      }

      if (parser_.message_complete()) {
        s.response = parser_.response();
        s.message_complete = true;
        s.current_phase = state_type::phase::done;
      } else {
        // Need more data
        ops.push_back(
            operation::make_recv(s.socket_handle, recv_buffer_span(), 0, ++s.operation_id));
      }
      break;
    }

    case state_type::phase::receiving:
    case state_type::phase::done:
    case state_type::phase::error:
      // Terminal states
      break;
  }

  return {std::move(s), std::move(ops)};
}

auto http1_client_machine::done(const state_type& s) const -> bool {
  return s.current_phase == state_type::phase::done || s.current_phase == state_type::phase::error;
}

// ============================================================================
// http1_tls_client_machine implementation
// ============================================================================

http1_tls_client_machine::http1_tls_client_machine(tls_connection& tls_conn, handle socket,
                                                   http1_request request)
    : tls_conn_(&tls_conn), socket_(socket), request_(std::move(request)) {
  recv_buffer_.resize(http1_default_buffer_size);
}

auto http1_tls_client_machine::initial() const -> state_type {
  state_type s;
  s.socket_handle = socket_;
  s.send_buffer = request_.serialize();
  return s;
}

auto http1_tls_client_machine::step(state_type s, [[maybe_unused]] const event& e) const
    -> step_result<state_type> {
  std::vector<operation> ops;

  switch (s.current_phase) {
    case state_type::phase::initial: {
      // Start sending via TLS
      s.current_phase = state_type::phase::sending;
      s.bytes_sent = 0;
      [[fallthrough]];
    }

    case state_type::phase::sending: {
      // TLS write using raw libtls C API
      std::size_t remaining = s.send_buffer.size() - s.bytes_sent;
      if (remaining > 0) {
        ssize_t result =
            tls_write(tls_conn_->raw(), s.send_buffer.data() + s.bytes_sent, remaining);

        if (result < 0) {
          if (result == TLS_WANT_POLLIN) {
            ops.push_back(operation::make_poll_add(s.socket_handle, POLLIN, ++s.operation_id));
            s.current_phase = state_type::phase::waiting_write;
          } else if (result == TLS_WANT_POLLOUT) {
            ops.push_back(operation::make_poll_add(s.socket_handle, POLLOUT, ++s.operation_id));
            s.current_phase = state_type::phase::waiting_write;
          } else {
            s.current_phase = state_type::phase::error;
            s.error_code = static_cast<int>(result);
            s.error_message = tls_error(tls_conn_->raw());
          }
        } else {
          s.bytes_sent += static_cast<std::size_t>(result);
          if (s.bytes_sent >= s.send_buffer.size()) {
            // All sent, start receiving
            s.current_phase = state_type::phase::receiving;
          }
          // Continue in same step to send more or start receiving
          return step(std::move(s), event{});
        }
      } else {
        s.current_phase = state_type::phase::receiving;
        return step(std::move(s), event{});
      }
      break;
    }

    case state_type::phase::waiting_write: {
      // Poll completed, retry write
      s.current_phase = state_type::phase::sending;
      return step(std::move(s), event{});
    }

    case state_type::phase::receiving: {
      // TLS read using raw libtls C API
      ssize_t result = tls_read(tls_conn_->raw(), recv_buffer_.data(), recv_buffer_.size());

      if (result < 0) {
        if (result == TLS_WANT_POLLIN) {
          ops.push_back(operation::make_poll_add(s.socket_handle, POLLIN, ++s.operation_id));
          s.current_phase = state_type::phase::waiting_read;
        } else if (result == TLS_WANT_POLLOUT) {
          ops.push_back(operation::make_poll_add(s.socket_handle, POLLOUT, ++s.operation_id));
          s.current_phase = state_type::phase::waiting_read;
        } else {
          s.current_phase = state_type::phase::error;
          s.error_code = static_cast<int>(result);
          s.error_message = tls_error(tls_conn_->raw());
        }
      } else if (result == 0) {
        // EOF
        if (parser_.message_complete()) {
          s.response = parser_.response();
          s.message_complete = true;
          s.current_phase = state_type::phase::done;
        } else {
          s.current_phase = state_type::phase::error;
          s.error_code = -1;
          s.error_message = "connection closed before response complete";
        }
      } else {
        // Parse data
        std::span<const std::byte> data(recv_buffer_.data(), static_cast<std::size_t>(result));
        auto parsed = parser_.parse(data);

        if (parsed < 0 || parser_.has_error()) {
          s.current_phase = state_type::phase::error;
          s.error_code = -1;
          s.error_message = "parse error: " + parser_.error_message();
        } else if (parser_.message_complete()) {
          s.response = parser_.response();
          s.message_complete = true;
          s.current_phase = state_type::phase::done;
        } else {
          // Continue reading
          return step(std::move(s), event{});
        }
      }
      break;
    }

    case state_type::phase::waiting_read: {
      // Poll completed, retry read
      s.current_phase = state_type::phase::receiving;
      return step(std::move(s), event{});
    }

    case state_type::phase::done:
    case state_type::phase::error:
      break;
  }

  return {std::move(s), std::move(ops)};
}

auto http1_tls_client_machine::done(const state_type& s) const -> bool {
  return s.current_phase == state_type::phase::done || s.current_phase == state_type::phase::error;
}

} // namespace evring
