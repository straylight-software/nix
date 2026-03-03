// straylight::nix::build::vm_protocol
//
// Wire protocol for Firecracker build VM communication via vsock.
//
// The protocol is simple and designed for build operations:
//
// Host → Guest (Commands):
//   BUILD_EXEC    - Execute a builder with environment
//   BUILD_ABORT   - Cancel current build
//   PING          - Health check
//
// Guest → Host (Responses/Events):
//   BUILD_STDOUT  - Builder stdout data
//   BUILD_STDERR  - Builder stderr data
//   BUILD_EXIT    - Builder completed (with exit code)
//   PONG          - Response to PING
//   WITNESS       - Filesystem/syscall witness event
//
// Wire format (all little-endian):
//   [magic: u32]       - 0x4E495842 ("NIXB")
//   [version: u16]     - Protocol version (1)
//   [msg_type: u16]    - Message type
//   [payload_len: u32] - Length of payload
//   [payload: bytes]   - Message-specific data
//
// BUILD_EXEC payload:
//   [builder_len: u32] [builder: utf8]
//   [args_count: u32]  [args: (u32, utf8)*]
//   [env_count: u32]   [env: (u32, utf8, u32, utf8)*]  // key, value pairs
//   [workdir_len: u32] [workdir: utf8]
//   [outputs_count: u32] [outputs: (u32, utf8)*]       // expected output paths

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace straylight::nix::build {

// ============================================================================
// Protocol Constants
// ============================================================================

constexpr uint32_t VM_PROTOCOL_MAGIC = 0x4E495842; // "NIXB"
constexpr uint16_t VM_PROTOCOL_VERSION = 1;

// vsock configuration
constexpr uint32_t VM_VSOCK_GUEST_CID = 3;     // Guest CID (host is always 2)
constexpr uint32_t VM_VSOCK_BUILD_PORT = 5000; // Port for build communication
constexpr uint32_t VM_VSOCK_LOG_PORT = 5001;   // Port for log streaming

// ============================================================================
// Message Types
// ============================================================================

enum class vm_msg_type : uint16_t {
  // Host → Guest
  BUILD_EXEC = 0x0001,
  BUILD_ABORT = 0x0002,
  PING = 0x0003,

  // Guest → Host
  BUILD_STDOUT = 0x0101,
  BUILD_STDERR = 0x0102,
  BUILD_EXIT = 0x0103,
  PONG = 0x0104,
  WITNESS_EVENT = 0x0105,
};

// ============================================================================
// Wire Header
// ============================================================================

#pragma pack(push, 1)
struct vm_wire_header {
  uint32_t magic;
  uint16_t version;
  uint16_t msg_type;
  uint32_t payload_len;
};
#pragma pack(pop)

static_assert(sizeof(vm_wire_header) == 12, "wire header must be 12 bytes");

// ============================================================================
// Message Payloads
// ============================================================================

/// Build execution request (host → guest)
struct build_exec_request {
  std::string builder;                                  // Path to builder executable
  std::vector<std::string> args;                        // Command-line arguments
  std::vector<std::pair<std::string, std::string>> env; // Environment variables
  std::string workdir;                                  // Working directory
  std::vector<std::string> outputs;                     // Expected output paths

  // Serialize to wire format
  auto serialize() const -> std::vector<uint8_t>;

  // Deserialize from wire format
  static auto deserialize(std::span<const uint8_t> data) -> build_exec_request;
};

/// Build exit notification (guest → host)
struct build_exit_response {
  int32_t exit_code;
  bool success;
  std::string error_msg; // Empty if success

  auto serialize() const -> std::vector<uint8_t>;
  static auto deserialize(std::span<const uint8_t> data) -> build_exit_response;
};

/// Witness event (guest → host) - reports filesystem/syscall activity
struct witness_event {
  enum class event_type : uint8_t {
    FILE_READ = 1,
    FILE_WRITE = 2,
    FILE_STAT = 3,
    NET_CONNECT = 4,
    SYSCALL = 5,
  };

  event_type type;
  std::string path;      // For file events
  std::string syscall;   // For syscall events
  uint64_t timestamp_ns; // Nanoseconds since build start

  auto serialize() const -> std::vector<uint8_t>;
  static auto deserialize(std::span<const uint8_t> data) -> witness_event;
};

// ============================================================================
// Message Building/Parsing
// ============================================================================

/// Build a complete wire message from header + payload
auto build_message(vm_msg_type type, std::span<const uint8_t> payload) -> std::vector<uint8_t>;

/// Parse header from wire data (returns nullopt if invalid/incomplete)
auto parse_header(std::span<const uint8_t> data) -> std::optional<vm_wire_header>;

/// Validate header magic and version
auto validate_header(const vm_wire_header& hdr) -> bool;

} // namespace straylight::nix::build
