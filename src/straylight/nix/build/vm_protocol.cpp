// straylight::nix::build::vm_protocol
//
// Wire protocol serialization for Firecracker build VM communication.

#include "vm_protocol.h"

#include <cstring>
#include <stdexcept>

namespace straylight::nix::build {

// ============================================================================
// Helper functions
// ============================================================================

namespace {

// Write a u32 in little-endian
void write_u32(std::vector<uint8_t>& buf, uint32_t val) {
  buf.push_back(static_cast<uint8_t>(val & 0xFF));
  buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
  buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
}

// Write a u64 in little-endian
void write_u64(std::vector<uint8_t>& buf, uint64_t val) {
  for (int i = 0; i < 8; ++i) {
    buf.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
  }
}

// Write a length-prefixed string
void write_string(std::vector<uint8_t>& buf, std::string_view str) {
  write_u32(buf, static_cast<uint32_t>(str.size()));
  buf.insert(buf.end(), str.begin(), str.end());
}

// Read a u32 in little-endian
auto read_u32(std::span<const uint8_t> data, size_t& offset) -> uint32_t {
  if (offset + 4 > data.size()) {
    throw std::runtime_error("read_u32: buffer underflow");
  }
  uint32_t val = static_cast<uint32_t>(data[offset]) |
                 (static_cast<uint32_t>(data[offset + 1]) << 8) |
                 (static_cast<uint32_t>(data[offset + 2]) << 16) |
                 (static_cast<uint32_t>(data[offset + 3]) << 24);
  offset += 4;
  return val;
}

// Read a u64 in little-endian
auto read_u64(std::span<const uint8_t> data, size_t& offset) -> uint64_t {
  if (offset + 8 > data.size()) {
    throw std::runtime_error("read_u64: buffer underflow");
  }
  uint64_t val = 0;
  for (int i = 0; i < 8; ++i) {
    val |= static_cast<uint64_t>(data[offset + i]) << (i * 8);
  }
  offset += 8;
  return val;
}

// Read a length-prefixed string
auto read_string(std::span<const uint8_t> data, size_t& offset) -> std::string {
  uint32_t len = read_u32(data, offset);
  if (offset + len > data.size()) {
    throw std::runtime_error("read_string: buffer underflow");
  }
  std::string result(reinterpret_cast<const char*>(data.data() + offset), len);
  offset += len;
  return result;
}

} // namespace

// ============================================================================
// build_exec_request
// ============================================================================

auto build_exec_request::serialize() const -> std::vector<uint8_t> {
  std::vector<uint8_t> buf;
  buf.reserve(256); // Pre-allocate reasonable size

  // Builder path
  write_string(buf, builder);

  // Args
  write_u32(buf, static_cast<uint32_t>(args.size()));
  for (const auto& arg : args) {
    write_string(buf, arg);
  }

  // Environment variables
  write_u32(buf, static_cast<uint32_t>(env.size()));
  for (const auto& [key, value] : env) {
    write_string(buf, key);
    write_string(buf, value);
  }

  // Working directory
  write_string(buf, workdir);

  // Expected outputs
  write_u32(buf, static_cast<uint32_t>(outputs.size()));
  for (const auto& output : outputs) {
    write_string(buf, output);
  }

  return buf;
}

auto build_exec_request::deserialize(std::span<const uint8_t> data) -> build_exec_request {
  build_exec_request req;
  size_t offset = 0;

  // Builder path
  req.builder = read_string(data, offset);

  // Args
  uint32_t args_count = read_u32(data, offset);
  req.args.reserve(args_count);
  for (uint32_t i = 0; i < args_count; ++i) {
    req.args.push_back(read_string(data, offset));
  }

  // Environment variables
  uint32_t env_count = read_u32(data, offset);
  req.env.reserve(env_count);
  for (uint32_t i = 0; i < env_count; ++i) {
    std::string key = read_string(data, offset);
    std::string value = read_string(data, offset);
    req.env.emplace_back(std::move(key), std::move(value));
  }

  // Working directory
  req.workdir = read_string(data, offset);

  // Expected outputs
  uint32_t outputs_count = read_u32(data, offset);
  req.outputs.reserve(outputs_count);
  for (uint32_t i = 0; i < outputs_count; ++i) {
    req.outputs.push_back(read_string(data, offset));
  }

  return req;
}

// ============================================================================
// build_exit_response
// ============================================================================

auto build_exit_response::serialize() const -> std::vector<uint8_t> {
  std::vector<uint8_t> buf;
  buf.reserve(16 + error_msg.size());

  // Exit code (4 bytes)
  write_u32(buf, static_cast<uint32_t>(exit_code));

  // Success flag (1 byte)
  buf.push_back(success ? 1 : 0);

  // Padding (3 bytes for alignment)
  buf.push_back(0);
  buf.push_back(0);
  buf.push_back(0);

  // Error message
  write_string(buf, error_msg);

  return buf;
}

auto build_exit_response::deserialize(std::span<const uint8_t> data) -> build_exit_response {
  build_exit_response resp;
  size_t offset = 0;

  // Exit code
  resp.exit_code = static_cast<int32_t>(read_u32(data, offset));

  // Success flag
  if (offset >= data.size()) {
    throw std::runtime_error("build_exit_response: buffer underflow");
  }
  resp.success = data[offset] != 0;
  offset += 4; // Skip success byte + 3 padding bytes

  // Error message
  resp.error_msg = read_string(data, offset);

  return resp;
}

// ============================================================================
// witness_event
// ============================================================================

auto witness_event::serialize() const -> std::vector<uint8_t> {
  std::vector<uint8_t> buf;
  buf.reserve(32 + path.size() + syscall.size());

  // Event type (1 byte)
  buf.push_back(static_cast<uint8_t>(type));

  // Padding (3 bytes)
  buf.push_back(0);
  buf.push_back(0);
  buf.push_back(0);

  // Timestamp (8 bytes)
  write_u64(buf, timestamp_ns);

  // Path
  write_string(buf, path);

  // Syscall name
  write_string(buf, syscall);

  return buf;
}

auto witness_event::deserialize(std::span<const uint8_t> data) -> witness_event {
  witness_event evt;
  size_t offset = 0;

  // Event type
  if (offset >= data.size()) {
    throw std::runtime_error("witness_event: buffer underflow");
  }
  evt.type = static_cast<event_type>(data[offset]);
  offset += 4; // Skip type byte + 3 padding bytes

  // Timestamp
  evt.timestamp_ns = read_u64(data, offset);

  // Path
  evt.path = read_string(data, offset);

  // Syscall name
  evt.syscall = read_string(data, offset);

  return evt;
}

// ============================================================================
// Message Building/Parsing
// ============================================================================

auto build_message(vm_msg_type type, std::span<const uint8_t> payload) -> std::vector<uint8_t> {
  std::vector<uint8_t> msg;
  msg.reserve(sizeof(vm_wire_header) + payload.size());

  // Magic
  write_u32(msg, VM_PROTOCOL_MAGIC);

  // Version
  msg.push_back(static_cast<uint8_t>(VM_PROTOCOL_VERSION & 0xFF));
  msg.push_back(static_cast<uint8_t>((VM_PROTOCOL_VERSION >> 8) & 0xFF));

  // Message type
  msg.push_back(static_cast<uint8_t>(static_cast<uint16_t>(type) & 0xFF));
  msg.push_back(static_cast<uint8_t>((static_cast<uint16_t>(type) >> 8) & 0xFF));

  // Payload length
  write_u32(msg, static_cast<uint32_t>(payload.size()));

  // Payload
  msg.insert(msg.end(), payload.begin(), payload.end());

  return msg;
}

auto parse_header(std::span<const uint8_t> data) -> std::optional<vm_wire_header> {
  if (data.size() < sizeof(vm_wire_header)) {
    return std::nullopt;
  }

  vm_wire_header hdr;
  size_t offset = 0;

  hdr.magic = read_u32(data, offset);
  hdr.version =
      static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8);
  offset += 2;
  hdr.msg_type =
      static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8);
  offset += 2;
  hdr.payload_len = read_u32(data, offset);

  return hdr;
}

auto validate_header(const vm_wire_header& hdr) -> bool {
  return hdr.magic == VM_PROTOCOL_MAGIC && hdr.version == VM_PROTOCOL_VERSION;
}

} // namespace straylight::nix::build
