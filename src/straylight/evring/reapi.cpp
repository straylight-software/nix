// reapi.cpp - Remote Execution API client implementation
//
// Implements REAPI CAS operations over HTTP/2 + gRPC framing.

#include "straylight/evring/reapi.h"

#include <cstring>

#include <poll.h>
#include <tls.h>

// For SHA256
#include <openssl/sha.h>

namespace evring {

// ============================================================================
// gRPC framing
// ============================================================================

auto grpc_frame::encode() const -> std::vector<std::byte> {
  std::vector<std::byte> result;
  result.reserve(5 + data.size());

  // Compressed flag (1 byte)
  result.push_back(compressed ? std::byte{1} : std::byte{0});

  // Length (4 bytes, big-endian)
  std::uint32_t len = static_cast<std::uint32_t>(data.size());
  result.push_back(static_cast<std::byte>((len >> 24) & 0xFF));
  result.push_back(static_cast<std::byte>((len >> 16) & 0xFF));
  result.push_back(static_cast<std::byte>((len >> 8) & 0xFF));
  result.push_back(static_cast<std::byte>(len & 0xFF));

  // Data
  result.insert(result.end(), data.begin(), data.end());

  return result;
}

auto grpc_frame::decode(std::span<const std::byte> bytes)
    -> std::pair<std::optional<grpc_frame>, std::size_t> {
  if (bytes.size() < 5) {
    return {std::nullopt, 0};
  }

  bool compressed = std::to_integer<std::uint8_t>(bytes[0]) != 0;

  std::uint32_t len = (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[1])) << 24) |
                      (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[2])) << 16) |
                      (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[3])) << 8) |
                      static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[4]));

  if (bytes.size() < 5 + len) {
    return {std::nullopt, 0};
  }

  grpc_frame frame;
  frame.compressed = compressed;
  frame.data.assign(bytes.begin() + 5, bytes.begin() + 5 + len);

  return {std::move(frame), 5 + len};
}

// ============================================================================
// REAPI types
// ============================================================================

auto reapi_digest::to_resource_name(std::string_view instance_name) const -> std::string {
  std::string result;
  result.reserve(instance_name.size() + 7 + hash.size() + 1 + 20);
  result += instance_name;
  result += "/blobs/";
  result += hash;
  result += "/";
  result += std::to_string(size);
  return result;
}

auto reapi_digest::to_upload_resource_name(std::string_view instance_name,
                                           std::string_view uuid) const -> std::string {
  std::string result;
  result.reserve(instance_name.size() + 9 + uuid.size() + 7 + hash.size() + 1 + 20);
  result += instance_name;
  result += "/uploads/";
  result += uuid;
  result += "/blobs/";
  result += hash;
  result += "/";
  result += std::to_string(size);
  return result;
}

auto reapi_hash_bytes(std::span<const std::byte> data) -> std::string {
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);

  static constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(SHA256_DIGEST_LENGTH * 2);
  for (std::size_t i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
    result += hex[(hash[i] >> 4) & 0xF];
    result += hex[hash[i] & 0xF];
  }
  return result;
}

auto reapi_digest_from_bytes(std::span<const std::byte> data) -> reapi_digest {
  return reapi_digest{
      .hash = reapi_hash_bytes(data),
      .size = static_cast<std::int64_t>(data.size()),
  };
}

auto grpc_status_string(grpc_status_code code) noexcept -> std::string_view {
  switch (code) {
    case grpc_status_code::ok:
      return "ok";
    case grpc_status_code::cancelled:
      return "cancelled";
    case grpc_status_code::unknown:
      return "unknown";
    case grpc_status_code::invalid_argument:
      return "invalid argument";
    case grpc_status_code::deadline_exceeded:
      return "deadline exceeded";
    case grpc_status_code::not_found:
      return "not found";
    case grpc_status_code::already_exists:
      return "already exists";
    case grpc_status_code::permission_denied:
      return "permission denied";
    case grpc_status_code::resource_exhausted:
      return "resource exhausted";
    case grpc_status_code::failed_precondition:
      return "failed precondition";
    case grpc_status_code::aborted:
      return "aborted";
    case grpc_status_code::out_of_range:
      return "out of range";
    case grpc_status_code::unimplemented:
      return "unimplemented";
    case grpc_status_code::internal:
      return "internal";
    case grpc_status_code::unavailable:
      return "unavailable";
    case grpc_status_code::data_loss:
      return "data loss";
    case grpc_status_code::unauthenticated:
      return "unauthenticated";
    default:
      return "unknown status";
  }
}

// ============================================================================
// Protobuf encoding helpers
// ============================================================================

namespace {

// Varint encoding
void encode_varint(std::vector<std::byte>& out, std::uint64_t value) {
  while (value >= 0x80) {
    out.push_back(static_cast<std::byte>((value & 0x7F) | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<std::byte>(value));
}

// Varint decoding
std::pair<std::uint64_t, std::size_t> decode_varint(std::span<const std::byte> data) {
  std::uint64_t value = 0;
  std::size_t shift = 0;
  std::size_t i = 0;

  while (i < data.size() && i < 10) {
    std::uint8_t byte = std::to_integer<std::uint8_t>(data[i]);
    value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
    ++i;
    if ((byte & 0x80) == 0) {
      return {value, i};
    }
    shift += 7;
  }

  return {0, 0}; // Error
}

// Wire types
constexpr std::uint8_t WIRE_VARINT = 0;
constexpr std::uint8_t WIRE_FIXED64 = 1;
constexpr std::uint8_t WIRE_LENGTH_DELIMITED = 2;
constexpr std::uint8_t WIRE_FIXED32 = 5;

// Tag encoding
void encode_tag(std::vector<std::byte>& out, std::uint32_t field, std::uint8_t wire_type) {
  encode_varint(out, (static_cast<std::uint64_t>(field) << 3) | wire_type);
}

// String/bytes encoding
void encode_bytes(std::vector<std::byte>& out, std::uint32_t field,
                  std::span<const std::byte> data) {
  encode_tag(out, field, WIRE_LENGTH_DELIMITED);
  encode_varint(out, data.size());
  out.insert(out.end(), data.begin(), data.end());
}

void encode_string(std::vector<std::byte>& out, std::uint32_t field, std::string_view str) {
  encode_tag(out, field, WIRE_LENGTH_DELIMITED);
  encode_varint(out, str.size());
  auto* ptr = reinterpret_cast<const std::byte*>(str.data());
  out.insert(out.end(), ptr, ptr + str.size());
}

// Int64 encoding
void encode_int64(std::vector<std::byte>& out, std::uint32_t field, std::int64_t value) {
  encode_tag(out, field, WIRE_VARINT);
  encode_varint(out, static_cast<std::uint64_t>(value));
}

// Submessage encoding (returns the data, caller wraps with length prefix)
void encode_submessage(std::vector<std::byte>& out, std::uint32_t field,
                       const std::vector<std::byte>& submsg) {
  encode_tag(out, field, WIRE_LENGTH_DELIMITED);
  encode_varint(out, submsg.size());
  out.insert(out.end(), submsg.begin(), submsg.end());
}

// Encode Digest message
std::vector<std::byte> encode_digest(const reapi_digest& digest) {
  std::vector<std::byte> out;
  encode_string(out, 1, digest.hash); // hash = field 1
  encode_int64(out, 2, digest.size);  // size_bytes = field 2
  return out;
}

// Encode FindMissingBlobsRequest
std::vector<std::byte> encode_find_missing_blobs_request(std::string_view instance_name,
                                                         const std::vector<reapi_digest>& digests) {
  std::vector<std::byte> out;
  encode_string(out, 1, instance_name); // instance_name = field 1

  for (const auto& digest : digests) {
    auto encoded = encode_digest(digest);
    encode_submessage(out, 2, encoded); // blob_digests = field 2 (repeated)
  }

  return out;
}

// Encode BatchUpdateBlobsRequest
std::vector<std::byte>
encode_batch_update_blobs_request(std::string_view instance_name,
                                  const std::vector<batch_update_blob>& blobs) {
  std::vector<std::byte> out;
  encode_string(out, 1, instance_name); // instance_name = field 1

  for (const auto& blob : blobs) {
    std::vector<std::byte> request_msg;
    auto digest_encoded = encode_digest(blob.digest);
    encode_submessage(request_msg, 1, digest_encoded); // digest = field 1
    encode_bytes(request_msg, 2, blob.data);           // data = field 2

    encode_submessage(out, 2, request_msg); // requests = field 2 (repeated)
  }

  return out;
}

// Encode BatchReadBlobsRequest
std::vector<std::byte> encode_batch_read_blobs_request(std::string_view instance_name,
                                                       const std::vector<reapi_digest>& digests) {
  std::vector<std::byte> out;
  encode_string(out, 1, instance_name); // instance_name = field 1

  for (const auto& digest : digests) {
    auto encoded = encode_digest(digest);
    encode_submessage(out, 2, encoded); // digests = field 2 (repeated)
  }

  return out;
}

// Encode ByteStream ReadRequest
std::vector<std::byte> encode_read_request(std::string_view resource_name, std::int64_t read_offset,
                                           std::int64_t read_limit) {
  std::vector<std::byte> out;
  encode_string(out, 1, resource_name); // resource_name = field 1
  if (read_offset != 0) {
    encode_int64(out, 2, read_offset); // read_offset = field 2
  }
  if (read_limit != 0) {
    encode_int64(out, 3, read_limit); // read_limit = field 3
  }
  return out;
}

// Encode ByteStream WriteRequest
std::vector<std::byte> encode_write_request(std::string_view resource_name,
                                            std::int64_t write_offset, bool finish_write,
                                            std::span<const std::byte> data) {
  std::vector<std::byte> out;
  encode_string(out, 1, resource_name); // resource_name = field 1
  encode_int64(out, 2, write_offset);   // write_offset = field 2
  if (finish_write) {
    encode_tag(out, 3, WIRE_VARINT);
    out.push_back(std::byte{1}); // finish_write = field 3
  }
  encode_bytes(out, 10, data); // data = field 10
  return out;
}

// ============================================================================
// Protobuf decoding helpers
// ============================================================================

struct proto_field {
  std::uint32_t number;
  std::uint8_t wire_type;
  std::span<const std::byte> data; // For length-delimited
  std::uint64_t value;             // For varint/fixed
};

// Parse next field from protobuf data
std::pair<std::optional<proto_field>, std::size_t> parse_field(std::span<const std::byte> data) {
  if (data.empty()) {
    return {std::nullopt, 0};
  }

  auto [tag, tag_len] = decode_varint(data);
  if (tag_len == 0) {
    return {std::nullopt, 0};
  }

  proto_field field;
  field.number = static_cast<std::uint32_t>(tag >> 3);
  field.wire_type = static_cast<std::uint8_t>(tag & 0x7);
  field.value = 0;

  std::size_t consumed = tag_len;
  auto rest = data.subspan(tag_len);

  switch (field.wire_type) {
    case WIRE_VARINT: {
      auto [value, len] = decode_varint(rest);
      if (len == 0) {
        return {std::nullopt, 0};
      }
      field.value = value;
      consumed += len;
      break;
    }

    case WIRE_FIXED64: {
      if (rest.size() < 8) {
        return {std::nullopt, 0};
      }
      std::uint64_t v = 0;
      for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(rest[i])) << (i * 8);
      }
      field.value = v;
      consumed += 8;
      break;
    }

    case WIRE_LENGTH_DELIMITED: {
      auto [len, len_bytes] = decode_varint(rest);
      if (len_bytes == 0 || rest.size() < len_bytes + len) {
        return {std::nullopt, 0};
      }
      field.data = rest.subspan(len_bytes, len);
      consumed += len_bytes + len;
      break;
    }

    case WIRE_FIXED32: {
      if (rest.size() < 4) {
        return {std::nullopt, 0};
      }
      std::uint32_t v = 0;
      for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(rest[i])) << (i * 8);
      }
      field.value = v;
      consumed += 4;
      break;
    }

    default:
      return {std::nullopt, 0};
  }

  return {field, consumed};
}

// Parse Digest from protobuf bytes
reapi_digest parse_digest(std::span<const std::byte> data) {
  reapi_digest digest;
  std::size_t offset = 0;

  while (offset < data.size()) {
    auto [field, consumed] = parse_field(data.subspan(offset));
    if (!field) {
      break;
    }
    offset += consumed;

    switch (field->number) {
      case 1: // hash
        digest.hash =
            std::string(reinterpret_cast<const char*>(field->data.data()), field->data.size());
        break;
      case 2: // size_bytes
        digest.size = static_cast<std::int64_t>(field->value);
        break;
    }
  }

  return digest;
}

// Parse Status from protobuf bytes
reapi_status parse_status(std::span<const std::byte> data) {
  reapi_status status;
  std::size_t offset = 0;

  while (offset < data.size()) {
    auto [field, consumed] = parse_field(data.subspan(offset));
    if (!field) {
      break;
    }
    offset += consumed;

    switch (field->number) {
      case 1: // code
        status.code = static_cast<std::int32_t>(field->value);
        break;
      case 2: // message
        status.message =
            std::string(reinterpret_cast<const char*>(field->data.data()), field->data.size());
        break;
    }
  }

  return status;
}

// Parse FindMissingBlobsResponse
std::vector<reapi_digest> parse_find_missing_blobs_response(std::span<const std::byte> data) {
  std::vector<reapi_digest> digests;
  std::size_t offset = 0;

  while (offset < data.size()) {
    auto [field, consumed] = parse_field(data.subspan(offset));
    if (!field) {
      break;
    }
    offset += consumed;

    if (field->number == 2) { // missing_blob_digests
      digests.push_back(parse_digest(field->data));
    }
  }

  return digests;
}

// Parse BatchUpdateBlobsResponse
std::vector<batch_update_result>
parse_batch_update_blobs_response(std::span<const std::byte> data) {
  std::vector<batch_update_result> results;
  std::size_t offset = 0;

  while (offset < data.size()) {
    auto [field, consumed] = parse_field(data.subspan(offset));
    if (!field) {
      break;
    }
    offset += consumed;

    if (field->number == 1) { // responses
      batch_update_result result;

      // Parse Response submessage
      std::size_t sub_offset = 0;
      while (sub_offset < field->data.size()) {
        auto [sub_field, sub_consumed] = parse_field(field->data.subspan(sub_offset));
        if (!sub_field) {
          break;
        }
        sub_offset += sub_consumed;

        switch (sub_field->number) {
          case 1: // digest
            result.digest = parse_digest(sub_field->data);
            break;
          case 2: // status
            result.status = parse_status(sub_field->data);
            break;
        }
      }

      results.push_back(std::move(result));
    }
  }

  return results;
}

// Parse BatchReadBlobsResponse
std::vector<batch_read_result> parse_batch_read_blobs_response(std::span<const std::byte> data) {
  std::vector<batch_read_result> results;
  std::size_t offset = 0;

  while (offset < data.size()) {
    auto [field, consumed] = parse_field(data.subspan(offset));
    if (!field) {
      break;
    }
    offset += consumed;

    if (field->number == 1) { // responses
      batch_read_result result;

      // Parse Response submessage
      std::size_t sub_offset = 0;
      while (sub_offset < field->data.size()) {
        auto [sub_field, sub_consumed] = parse_field(field->data.subspan(sub_offset));
        if (!sub_field) {
          break;
        }
        sub_offset += sub_consumed;

        switch (sub_field->number) {
          case 1: // digest
            result.digest = parse_digest(sub_field->data);
            break;
          case 2: // data
            result.data.assign(sub_field->data.begin(), sub_field->data.end());
            break;
          case 3: // status
            result.status = parse_status(sub_field->data);
            break;
        }
      }

      results.push_back(std::move(result));
    }
  }

  return results;
}

// Parse ByteStream ReadResponse - extracts data field
std::vector<std::byte> parse_read_response(std::span<const std::byte> data) {
  std::vector<std::byte> result;
  std::size_t offset = 0;

  while (offset < data.size()) {
    auto [field, consumed] = parse_field(data.subspan(offset));
    if (!field) {
      break;
    }
    offset += consumed;

    if (field->number == 10) { // data
      result.insert(result.end(), field->data.begin(), field->data.end());
    }
  }

  return result;
}

// Parse ByteStream WriteResponse
std::int64_t parse_write_response(std::span<const std::byte> data) {
  std::size_t offset = 0;

  while (offset < data.size()) {
    auto [field, consumed] = parse_field(data.subspan(offset));
    if (!field) {
      break;
    }
    offset += consumed;

    if (field->number == 1) { // committed_size
      return static_cast<std::int64_t>(field->value);
    }
  }

  return 0;
}

// Parse grpc-status from trailers
grpc_status_code parse_grpc_status(const http2_headers& headers) {
  for (const auto& h : headers) {
    if (h.name == "grpc-status") {
      return static_cast<grpc_status_code>(std::stoi(h.value));
    }
  }
  return grpc_status_code::ok;
}

// Parse grpc-message from trailers
std::string parse_grpc_message(const http2_headers& headers) {
  for (const auto& h : headers) {
    if (h.name == "grpc-message") {
      return h.value;
    }
  }
  return "";
}

} // namespace

// ============================================================================
// Common gRPC request helpers
// ============================================================================

namespace {

http2_request make_grpc_request(std::string_view authority, std::string_view method,
                                const std::vector<std::byte>& payload) {
  grpc_frame frame;
  frame.compressed = false;
  frame.data = payload;
  auto encoded = frame.encode();

  http2_request req;
  req.method = "POST";
  req.scheme = "https";
  req.authority = std::string(authority);
  req.path = std::string(method);
  req.headers = {
      {"content-type", "application/grpc"},
      {"te", "trailers"},
  };
  req.body = std::move(encoded);

  return req;
}

// Common step logic for gRPC machines
template <typename State>
auto do_grpc_tls_write(State s, http2_session* session, tls_connection* tls_conn)
    -> step_result<State> {
  if (s.send_buffer.empty()) {
    s.send_buffer = session->get_pending_data();
  }

  if (s.send_buffer.empty()) {
    s.current_phase = State::phase::waiting_read;
    std::vector<operation> ops;
    ops.push_back(operation::make_poll_add(s.socket_handle, POLLIN, ++s.operation_id));
    return {std::move(s), std::move(ops)};
  }

  ssize_t written = tls_write(tls_conn->raw(), s.send_buffer.data(), s.send_buffer.size());

  if (written > 0) {
    s.send_buffer.erase(s.send_buffer.begin(), s.send_buffer.begin() + written);
    if (s.send_buffer.empty()) {
      s.current_phase = State::phase::waiting_read;
      std::vector<operation> ops;
      ops.push_back(operation::make_poll_add(s.socket_handle, POLLIN, ++s.operation_id));
      return {std::move(s), std::move(ops)};
    }
    s.current_phase = State::phase::waiting_write;
    std::vector<operation> ops;
    ops.push_back(operation::make_poll_add(s.socket_handle, POLLOUT, ++s.operation_id));
    return {std::move(s), std::move(ops)};
  }

  if (written == TLS_WANT_POLLIN || written == TLS_WANT_POLLOUT) {
    s.current_phase = State::phase::waiting_write;
    short poll_events = (written == TLS_WANT_POLLIN) ? POLLIN : POLLOUT;
    std::vector<operation> ops;
    ops.push_back(operation::make_poll_add(s.socket_handle, poll_events, ++s.operation_id));
    return {std::move(s), std::move(ops)};
  }

  s.current_phase = State::phase::error;
  s.status_code = grpc_status_code::unavailable;
  s.status_message = tls_error(tls_conn->raw());
  return {std::move(s), {}};
}

template <typename State, typename ResponseParser>
auto do_grpc_tls_read(State s, http2_session* session, tls_connection* tls_conn,
                      ResponseParser&& parse_response) -> step_result<State> {
  std::byte buffer[16384];
  ssize_t nread = tls_read(tls_conn->raw(), buffer, sizeof(buffer));

  if (nread > 0) {
    auto consumed = session->receive_data(std::span<const std::byte>(buffer, nread));
    if (consumed < 0) {
      s.current_phase = State::phase::error;
      s.status_code = grpc_status_code::internal;
      s.status_message = "nghttp2 receive error";
      return {std::move(s), {}};
    }

    // Check if stream is closed
    if (session->is_stream_closed(s.stream_id)) {
      auto* resp = session->get_stream_response(s.stream_id);
      if (resp) {
        // Extract gRPC status from trailers
        s.status_code = parse_grpc_status(resp->headers);
        s.status_message = parse_grpc_message(resp->headers);

        // Parse response body
        if (!resp->body.empty()) {
          auto [frame, frame_len] = grpc_frame::decode(resp->body);
          if (frame && !frame->data.empty()) {
            parse_response(s, frame->data);
          }
        }
      }

      s.current_phase = State::phase::done;
      return {std::move(s), {}};
    }

    // Check for pending data to send
    auto pending = session->get_pending_data();
    if (!pending.empty()) {
      s.send_buffer = std::move(pending);
      s.current_phase = State::phase::waiting_write;
      std::vector<operation> ops;
      ops.push_back(operation::make_poll_add(s.socket_handle, POLLOUT, ++s.operation_id));
      return {std::move(s), std::move(ops)};
    }

    // Keep reading
    s.current_phase = State::phase::waiting_read;
    std::vector<operation> ops;
    ops.push_back(operation::make_poll_add(s.socket_handle, POLLIN, ++s.operation_id));
    return {std::move(s), std::move(ops)};
  }

  if (nread == TLS_WANT_POLLIN || nread == TLS_WANT_POLLOUT) {
    s.current_phase = State::phase::waiting_read;
    short poll_events = (nread == TLS_WANT_POLLIN) ? POLLIN : POLLOUT;
    std::vector<operation> ops;
    ops.push_back(operation::make_poll_add(s.socket_handle, poll_events, ++s.operation_id));
    return {std::move(s), std::move(ops)};
  }

  if (nread == 0) {
    s.current_phase = State::phase::error;
    s.status_code = grpc_status_code::unavailable;
    s.status_message = "connection closed";
    return {std::move(s), {}};
  }

  s.current_phase = State::phase::error;
  s.status_code = grpc_status_code::unavailable;
  s.status_message = tls_error(tls_conn->raw());
  return {std::move(s), {}};
}

} // namespace

// ============================================================================
// FindMissingBlobs machine
// ============================================================================

find_missing_blobs_machine::find_missing_blobs_machine(http2_session& session,
                                                       tls_connection& tls_conn, handle socket,
                                                       std::string instance_name,
                                                       std::vector<reapi_digest> digests)
    : session_(&session),
      tls_conn_(&tls_conn),
      socket_(socket),
      instance_name_(std::move(instance_name)),
      digests_(std::move(digests)) {}

auto find_missing_blobs_machine::initial() const -> state_type {
  state_type s;
  s.socket_handle = socket_;
  s.instance_name = instance_name_;
  s.blob_digests = digests_;
  return s;
}

auto find_missing_blobs_machine::step(state_type s, const event& e) const
    -> step_result<state_type> {
  switch (s.current_phase) {
    case find_missing_blobs_state::phase::initial: {
      // Build and submit request
      auto payload = encode_find_missing_blobs_request(s.instance_name, s.blob_digests);
      auto req = make_grpc_request(
          "", "/build.bazel.remote.execution.v2.ContentAddressableStorage/FindMissingBlobs",
          payload);

      s.stream_id = session_->submit_request(req);
      if (s.stream_id < 0) {
        s.current_phase = find_missing_blobs_state::phase::error;
        s.status_code = grpc_status_code::internal;
        s.status_message = "failed to submit request";
        return {std::move(s), {}};
      }

      s.current_phase = find_missing_blobs_state::phase::sending;
      return do_grpc_tls_write(std::move(s), session_, tls_conn_);
    }

    case find_missing_blobs_state::phase::sending:
    case find_missing_blobs_state::phase::waiting_write: {
      if (!e.ok()) {
        s.current_phase = find_missing_blobs_state::phase::error;
        s.status_code = grpc_status_code::unavailable;
        s.status_message = "poll failed";
        return {std::move(s), {}};
      }
      return do_grpc_tls_write(std::move(s), session_, tls_conn_);
    }

    case find_missing_blobs_state::phase::waiting_read:
    case find_missing_blobs_state::phase::receiving: {
      if (!e.ok()) {
        s.current_phase = find_missing_blobs_state::phase::error;
        s.status_code = grpc_status_code::unavailable;
        s.status_message = "poll failed";
        return {std::move(s), {}};
      }
      return do_grpc_tls_read(std::move(s), session_, tls_conn_,
                              [](find_missing_blobs_state& state, std::span<const std::byte> data) {
                                state.missing_digests = parse_find_missing_blobs_response(data);
                              });
    }

    case find_missing_blobs_state::phase::done:
    case find_missing_blobs_state::phase::error:
      return {std::move(s), {}};
  }

  return {std::move(s), {}};
}

auto find_missing_blobs_machine::done(const state_type& s) const -> bool {
  return s.current_phase == find_missing_blobs_state::phase::done ||
         s.current_phase == find_missing_blobs_state::phase::error;
}

// ============================================================================
// BatchUpdateBlobs machine
// ============================================================================

batch_update_blobs_machine::batch_update_blobs_machine(http2_session& session,
                                                       tls_connection& tls_conn, handle socket,
                                                       std::string instance_name,
                                                       std::vector<batch_update_blob> blobs)
    : session_(&session),
      tls_conn_(&tls_conn),
      socket_(socket),
      instance_name_(std::move(instance_name)),
      blobs_(std::move(blobs)) {}

auto batch_update_blobs_machine::initial() const -> state_type {
  state_type s;
  s.socket_handle = socket_;
  s.instance_name = instance_name_;
  s.blobs = blobs_;
  return s;
}

auto batch_update_blobs_machine::step(state_type s, const event& e) const
    -> step_result<state_type> {
  switch (s.current_phase) {
    case batch_update_blobs_state::phase::initial: {
      auto payload = encode_batch_update_blobs_request(s.instance_name, s.blobs);
      auto req = make_grpc_request(
          "", "/build.bazel.remote.execution.v2.ContentAddressableStorage/BatchUpdateBlobs",
          payload);

      s.stream_id = session_->submit_request(req);
      if (s.stream_id < 0) {
        s.current_phase = batch_update_blobs_state::phase::error;
        s.status_code = grpc_status_code::internal;
        s.status_message = "failed to submit request";
        return {std::move(s), {}};
      }

      s.current_phase = batch_update_blobs_state::phase::sending;
      return do_grpc_tls_write(std::move(s), session_, tls_conn_);
    }

    case batch_update_blobs_state::phase::sending:
    case batch_update_blobs_state::phase::waiting_write: {
      if (!e.ok()) {
        s.current_phase = batch_update_blobs_state::phase::error;
        s.status_code = grpc_status_code::unavailable;
        s.status_message = "poll failed";
        return {std::move(s), {}};
      }
      return do_grpc_tls_write(std::move(s), session_, tls_conn_);
    }

    case batch_update_blobs_state::phase::waiting_read:
    case batch_update_blobs_state::phase::receiving: {
      if (!e.ok()) {
        s.current_phase = batch_update_blobs_state::phase::error;
        s.status_code = grpc_status_code::unavailable;
        s.status_message = "poll failed";
        return {std::move(s), {}};
      }
      return do_grpc_tls_read(std::move(s), session_, tls_conn_,
                              [](batch_update_blobs_state& state, std::span<const std::byte> data) {
                                state.results = parse_batch_update_blobs_response(data);
                              });
    }

    case batch_update_blobs_state::phase::done:
    case batch_update_blobs_state::phase::error:
      return {std::move(s), {}};
  }

  return {std::move(s), {}};
}

auto batch_update_blobs_machine::done(const state_type& s) const -> bool {
  return s.current_phase == batch_update_blobs_state::phase::done ||
         s.current_phase == batch_update_blobs_state::phase::error;
}

// ============================================================================
// BatchReadBlobs machine
// ============================================================================

batch_read_blobs_machine::batch_read_blobs_machine(http2_session& session, tls_connection& tls_conn,
                                                   handle socket, std::string instance_name,
                                                   std::vector<reapi_digest> digests)
    : session_(&session),
      tls_conn_(&tls_conn),
      socket_(socket),
      instance_name_(std::move(instance_name)),
      digests_(std::move(digests)) {}

auto batch_read_blobs_machine::initial() const -> state_type {
  state_type s;
  s.socket_handle = socket_;
  s.instance_name = instance_name_;
  s.digests = digests_;
  return s;
}

auto batch_read_blobs_machine::step(state_type s, const event& e) const -> step_result<state_type> {
  switch (s.current_phase) {
    case batch_read_blobs_state::phase::initial: {
      auto payload = encode_batch_read_blobs_request(s.instance_name, s.digests);
      auto req = make_grpc_request(
          "", "/build.bazel.remote.execution.v2.ContentAddressableStorage/BatchReadBlobs", payload);

      s.stream_id = session_->submit_request(req);
      if (s.stream_id < 0) {
        s.current_phase = batch_read_blobs_state::phase::error;
        s.status_code = grpc_status_code::internal;
        s.status_message = "failed to submit request";
        return {std::move(s), {}};
      }

      s.current_phase = batch_read_blobs_state::phase::sending;
      return do_grpc_tls_write(std::move(s), session_, tls_conn_);
    }

    case batch_read_blobs_state::phase::sending:
    case batch_read_blobs_state::phase::waiting_write: {
      if (!e.ok()) {
        s.current_phase = batch_read_blobs_state::phase::error;
        s.status_code = grpc_status_code::unavailable;
        s.status_message = "poll failed";
        return {std::move(s), {}};
      }
      return do_grpc_tls_write(std::move(s), session_, tls_conn_);
    }

    case batch_read_blobs_state::phase::waiting_read:
    case batch_read_blobs_state::phase::receiving: {
      if (!e.ok()) {
        s.current_phase = batch_read_blobs_state::phase::error;
        s.status_code = grpc_status_code::unavailable;
        s.status_message = "poll failed";
        return {std::move(s), {}};
      }
      return do_grpc_tls_read(std::move(s), session_, tls_conn_,
                              [](batch_read_blobs_state& state, std::span<const std::byte> data) {
                                state.results = parse_batch_read_blobs_response(data);
                              });
    }

    case batch_read_blobs_state::phase::done:
    case batch_read_blobs_state::phase::error:
      return {std::move(s), {}};
  }

  return {std::move(s), {}};
}

auto batch_read_blobs_machine::done(const state_type& s) const -> bool {
  return s.current_phase == batch_read_blobs_state::phase::done ||
         s.current_phase == batch_read_blobs_state::phase::error;
}

// ============================================================================
// ByteStream Read machine
// ============================================================================

bytestream_read_machine::bytestream_read_machine(http2_session& session, tls_connection& tls_conn,
                                                 handle socket, std::string resource_name,
                                                 std::int64_t read_offset, std::int64_t read_limit)
    : session_(&session),
      tls_conn_(&tls_conn),
      socket_(socket),
      resource_name_(std::move(resource_name)),
      read_offset_(read_offset),
      read_limit_(read_limit) {}

auto bytestream_read_machine::initial() const -> state_type {
  state_type s;
  s.socket_handle = socket_;
  s.resource_name = resource_name_;
  s.read_offset = read_offset_;
  s.read_limit = read_limit_;
  return s;
}

auto bytestream_read_machine::step(state_type s, const event& e) const -> step_result<state_type> {
  switch (s.current_phase) {
    case bytestream_read_state::phase::initial: {
      auto payload = encode_read_request(s.resource_name, s.read_offset, s.read_limit);
      auto req = make_grpc_request("", "/google.bytestream.ByteStream/Read", payload);

      s.stream_id = session_->submit_request(req);
      if (s.stream_id < 0) {
        s.current_phase = bytestream_read_state::phase::error;
        s.status_code = grpc_status_code::internal;
        s.status_message = "failed to submit request";
        return {std::move(s), {}};
      }

      s.current_phase = bytestream_read_state::phase::sending;
      return do_grpc_tls_write(std::move(s), session_, tls_conn_);
    }

    case bytestream_read_state::phase::sending:
    case bytestream_read_state::phase::waiting_write: {
      if (!e.ok()) {
        s.current_phase = bytestream_read_state::phase::error;
        s.status_code = grpc_status_code::unavailable;
        s.status_message = "poll failed";
        return {std::move(s), {}};
      }
      return do_grpc_tls_write(std::move(s), session_, tls_conn_);
    }

    case bytestream_read_state::phase::waiting_read:
    case bytestream_read_state::phase::receiving: {
      if (!e.ok()) {
        s.current_phase = bytestream_read_state::phase::error;
        s.status_code = grpc_status_code::unavailable;
        s.status_message = "poll failed";
        return {std::move(s), {}};
      }
      // For streaming, we accumulate data from multiple frames
      return do_grpc_tls_read(std::move(s), session_, tls_conn_,
                              [](bytestream_read_state& state, std::span<const std::byte> data) {
                                auto chunk = parse_read_response(data);
                                state.data.insert(state.data.end(), chunk.begin(), chunk.end());
                              });
    }

    case bytestream_read_state::phase::done:
    case bytestream_read_state::phase::error:
      return {std::move(s), {}};
  }

  return {std::move(s), {}};
}

auto bytestream_read_machine::done(const state_type& s) const -> bool {
  return s.current_phase == bytestream_read_state::phase::done ||
         s.current_phase == bytestream_read_state::phase::error;
}

// ============================================================================
// ByteStream Write machine
// ============================================================================

bytestream_write_machine::bytestream_write_machine(http2_session& session, tls_connection& tls_conn,
                                                   handle socket, std::string resource_name,
                                                   std::vector<std::byte> data,
                                                   std::size_t chunk_size)
    : session_(&session),
      tls_conn_(&tls_conn),
      socket_(socket),
      resource_name_(std::move(resource_name)),
      data_(std::move(data)),
      chunk_size_(chunk_size) {}

auto bytestream_write_machine::initial() const -> state_type {
  state_type s;
  s.socket_handle = socket_;
  s.resource_name = resource_name_;
  s.data = data_;
  s.chunk_size = chunk_size_;
  return s;
}

auto bytestream_write_machine::step(state_type s, const event& e) const -> step_result<state_type> {
  switch (s.current_phase) {
    case bytestream_write_state::phase::initial: {
      // For simplicity, send all data in one request
      // A full implementation would chunk for very large blobs
      bool finish = true;
      auto payload =
          encode_write_request(s.resource_name, 0, finish, std::span<const std::byte>(s.data));
      auto req = make_grpc_request("", "/google.bytestream.ByteStream/Write", payload);

      s.stream_id = session_->submit_request(req);
      if (s.stream_id < 0) {
        s.current_phase = bytestream_write_state::phase::error;
        s.status_code = grpc_status_code::internal;
        s.status_message = "failed to submit request";
        return {std::move(s), {}};
      }

      s.current_phase = bytestream_write_state::phase::sending;
      return do_grpc_tls_write(std::move(s), session_, tls_conn_);
    }

    case bytestream_write_state::phase::sending:
    case bytestream_write_state::phase::waiting_write: {
      if (!e.ok()) {
        s.current_phase = bytestream_write_state::phase::error;
        s.status_code = grpc_status_code::unavailable;
        s.status_message = "poll failed";
        return {std::move(s), {}};
      }
      return do_grpc_tls_write(std::move(s), session_, tls_conn_);
    }

    case bytestream_write_state::phase::waiting_read:
    case bytestream_write_state::phase::receiving: {
      if (!e.ok()) {
        s.current_phase = bytestream_write_state::phase::error;
        s.status_code = grpc_status_code::unavailable;
        s.status_message = "poll failed";
        return {std::move(s), {}};
      }
      return do_grpc_tls_read(std::move(s), session_, tls_conn_,
                              [](bytestream_write_state& state, std::span<const std::byte> data) {
                                state.committed_size = parse_write_response(data);
                              });
    }

    case bytestream_write_state::phase::done:
    case bytestream_write_state::phase::error:
      return {std::move(s), {}};
  }

  return {std::move(s), {}};
}

auto bytestream_write_machine::done(const state_type& s) const -> bool {
  return s.current_phase == bytestream_write_state::phase::done ||
         s.current_phase == bytestream_write_state::phase::error;
}

// ============================================================================
// High-level CAS client
// ============================================================================

reapi_cas_client::reapi_cas_client(http2_session& session, tls_connection& tls_conn, handle socket,
                                   reapi_config config)
    : session_(&session), tls_conn_(&tls_conn), socket_(socket), config_(std::move(config)) {}

// Note: These methods are stubs - they would need a ring to actually run
// In practice, users should use the machines directly with evring::run()

auto reapi_cas_client::upload_blob(std::span<const std::byte> data) -> reapi_digest {
  return reapi_digest_from_bytes(data);
}

auto reapi_cas_client::download_blob(const reapi_digest& /*digest*/)
    -> std::optional<std::vector<std::byte>> {
  return std::nullopt;
}

auto reapi_cas_client::find_missing_blobs(const std::vector<reapi_digest>& digests)
    -> std::vector<reapi_digest> {
  return digests; // Stub: assume all missing
}

auto reapi_cas_client::blob_exists(const reapi_digest& digest) -> bool {
  auto missing = find_missing_blobs({digest});
  return missing.empty();
}

} // namespace evring
