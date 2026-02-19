#pragma once

/// reapi.h - Remote Execution API client over HTTP/2 + gRPC
///
/// Provides evring state machines for REAPI CAS operations:
/// - FindMissingBlobs: check which blobs need uploading
/// - BatchUpdateBlobs: upload small blobs (<4MB)
/// - BatchReadBlobs: download small blobs
/// - ByteStream Read/Write: stream large blobs
///
/// Uses protobuf for message serialization (protoc-generated code)
/// and gRPC framing over HTTP/2.
///
/// Example usage:
/// @code
///   // Setup HTTP/2 connection first (see http2.h)
///   evring::http2_session session;
///   // ...
///
///   // Check for missing blobs
///   std::vector<evring::reapi_digest> digests = {...};
///   evring::find_missing_blobs_machine finder{
///     session, tls_conn, socket, "my-instance", digests
///   };
///   auto state = evring::run(finder, *ring);
///   for (const auto& missing : state.missing_digests) {
///     // upload these
///   }
/// @endcode

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "straylight/evring/handle.h"
#include "straylight/evring/http2.h"
#include "straylight/evring/machine.h"
#include "straylight/evring/tls.h"

namespace evring {

// ============================================================================
// gRPC framing
// ============================================================================

/// gRPC frame header: 1 byte compressed + 4 bytes BE length
struct grpc_frame {
  bool compressed{false};
  std::vector<std::byte> data;

  /// Encode frame with header
  [[nodiscard]] auto encode() const -> std::vector<std::byte>;

  /// Decode frame from bytes, returns bytes consumed or 0 if incomplete
  [[nodiscard]] static auto decode(std::span<const std::byte> bytes)
      -> std::pair<std::optional<grpc_frame>, std::size_t>;
};

// ============================================================================
// REAPI types
// ============================================================================

/// Content digest (hash + size)
struct reapi_digest {
  std::string hash; // SHA256 hex string (64 chars)
  std::int64_t size{0};

  bool operator==(const reapi_digest& other) const {
    return hash == other.hash && size == other.size;
  }

  /// Resource name for ByteStream Read
  [[nodiscard]] auto to_resource_name(std::string_view instance_name) const -> std::string;

  /// Resource name for ByteStream Write
  [[nodiscard]] auto to_upload_resource_name(std::string_view instance_name,
                                             std::string_view uuid = "evring") const -> std::string;
};

/// Compute digest from data (SHA256)
[[nodiscard]] auto reapi_digest_from_bytes(std::span<const std::byte> data) -> reapi_digest;

/// Compute SHA256 hash as hex string
[[nodiscard]] auto reapi_hash_bytes(std::span<const std::byte> data) -> std::string;

/// Status from REAPI response
struct reapi_status {
  std::int32_t code{0}; // 0 = OK
  std::string message;

  [[nodiscard]] auto ok() const noexcept -> bool { return code == 0; }
};

// ============================================================================
// REAPI configuration
// ============================================================================

struct reapi_config {
  std::string instance_name;
  std::size_t batch_upload_threshold{4 * 1024 * 1024}; // 4MB
  std::size_t stream_chunk_size{1024 * 1024};          // 1MB
};

// ============================================================================
// gRPC error handling
// ============================================================================

enum class grpc_status_code : std::int32_t {
  ok = 0,
  cancelled = 1,
  unknown = 2,
  invalid_argument = 3,
  deadline_exceeded = 4,
  not_found = 5,
  already_exists = 6,
  permission_denied = 7,
  resource_exhausted = 8,
  failed_precondition = 9,
  aborted = 10,
  out_of_range = 11,
  unimplemented = 12,
  internal = 13,
  unavailable = 14,
  data_loss = 15,
  unauthenticated = 16,
};

[[nodiscard]] auto grpc_status_string(grpc_status_code code) noexcept -> std::string_view;

// ============================================================================
// FindMissingBlobs
// ============================================================================

struct find_missing_blobs_state {
  enum class phase { initial, sending, waiting_write, waiting_read, receiving, done, error };

  phase current_phase{phase::initial};
  handle socket_handle;
  std::int32_t stream_id{0};

  // Input
  std::string instance_name;
  std::vector<reapi_digest> blob_digests;

  // Output
  std::vector<reapi_digest> missing_digests;

  // gRPC status
  grpc_status_code status_code{grpc_status_code::ok};
  std::string status_message;

  // I/O
  std::vector<std::byte> send_buffer;
  std::vector<std::byte> recv_buffer;
  std::uint64_t operation_id{0};

  [[nodiscard]] auto ok() const noexcept -> bool {
    return current_phase == phase::done && status_code == grpc_status_code::ok;
  }
};

/// FindMissingBlobs machine
///
/// Queries CAS for which blobs from a list are missing.
class find_missing_blobs_machine {
public:
  using state_type = find_missing_blobs_state;

  find_missing_blobs_machine(http2_session& session, tls_connection& tls_conn, handle socket,
                             std::string instance_name, std::vector<reapi_digest> digests);

  [[nodiscard]] auto initial() const -> state_type;
  [[nodiscard]] auto step(state_type s, const event& e) const -> step_result<state_type>;
  [[nodiscard]] auto done(const state_type& s) const -> bool;

private:
  http2_session* session_;
  tls_connection* tls_conn_;
  handle socket_;
  std::string instance_name_;
  std::vector<reapi_digest> digests_;
};

// ============================================================================
// BatchUpdateBlobs
// ============================================================================

struct batch_update_blob {
  reapi_digest digest;
  std::vector<std::byte> data;
};

struct batch_update_result {
  reapi_digest digest;
  reapi_status status;
};

struct batch_update_blobs_state {
  enum class phase { initial, sending, waiting_write, waiting_read, receiving, done, error };

  phase current_phase{phase::initial};
  handle socket_handle;
  std::int32_t stream_id{0};

  // Input
  std::string instance_name;
  std::vector<batch_update_blob> blobs;

  // Output
  std::vector<batch_update_result> results;

  // gRPC status
  grpc_status_code status_code{grpc_status_code::ok};
  std::string status_message;

  // I/O
  std::vector<std::byte> send_buffer;
  std::vector<std::byte> recv_buffer;
  std::uint64_t operation_id{0};

  [[nodiscard]] auto ok() const noexcept -> bool {
    return current_phase == phase::done && status_code == grpc_status_code::ok;
  }
};

/// BatchUpdateBlobs machine
///
/// Uploads multiple small blobs to CAS in a single request.
class batch_update_blobs_machine {
public:
  using state_type = batch_update_blobs_state;

  batch_update_blobs_machine(http2_session& session, tls_connection& tls_conn, handle socket,
                             std::string instance_name, std::vector<batch_update_blob> blobs);

  [[nodiscard]] auto initial() const -> state_type;
  [[nodiscard]] auto step(state_type s, const event& e) const -> step_result<state_type>;
  [[nodiscard]] auto done(const state_type& s) const -> bool;

private:
  http2_session* session_;
  tls_connection* tls_conn_;
  handle socket_;
  std::string instance_name_;
  std::vector<batch_update_blob> blobs_;
};

// ============================================================================
// BatchReadBlobs
// ============================================================================

struct batch_read_result {
  reapi_digest digest;
  std::vector<std::byte> data;
  reapi_status status;
};

struct batch_read_blobs_state {
  enum class phase { initial, sending, waiting_write, waiting_read, receiving, done, error };

  phase current_phase{phase::initial};
  handle socket_handle;
  std::int32_t stream_id{0};

  // Input
  std::string instance_name;
  std::vector<reapi_digest> digests;

  // Output
  std::vector<batch_read_result> results;

  // gRPC status
  grpc_status_code status_code{grpc_status_code::ok};
  std::string status_message;

  // I/O
  std::vector<std::byte> send_buffer;
  std::vector<std::byte> recv_buffer;
  std::uint64_t operation_id{0};

  [[nodiscard]] auto ok() const noexcept -> bool {
    return current_phase == phase::done && status_code == grpc_status_code::ok;
  }
};

/// BatchReadBlobs machine
///
/// Downloads multiple small blobs from CAS in a single request.
class batch_read_blobs_machine {
public:
  using state_type = batch_read_blobs_state;

  batch_read_blobs_machine(http2_session& session, tls_connection& tls_conn, handle socket,
                           std::string instance_name, std::vector<reapi_digest> digests);

  [[nodiscard]] auto initial() const -> state_type;
  [[nodiscard]] auto step(state_type s, const event& e) const -> step_result<state_type>;
  [[nodiscard]] auto done(const state_type& s) const -> bool;

private:
  http2_session* session_;
  tls_connection* tls_conn_;
  handle socket_;
  std::string instance_name_;
  std::vector<reapi_digest> digests_;
};

// ============================================================================
// ByteStream Read (streaming download)
// ============================================================================

struct bytestream_read_state {
  enum class phase { initial, sending, waiting_write, waiting_read, receiving, done, error };

  phase current_phase{phase::initial};
  handle socket_handle;
  std::int32_t stream_id{0};

  // Input
  std::string resource_name;
  std::int64_t read_offset{0};
  std::int64_t read_limit{0}; // 0 = read all

  // Output
  std::vector<std::byte> data;

  // gRPC status
  grpc_status_code status_code{grpc_status_code::ok};
  std::string status_message;

  // I/O
  std::vector<std::byte> send_buffer;
  std::vector<std::byte> recv_buffer;
  std::uint64_t operation_id{0};

  [[nodiscard]] auto ok() const noexcept -> bool {
    return current_phase == phase::done && status_code == grpc_status_code::ok;
  }
};

/// ByteStream Read machine
///
/// Streams a large blob from CAS.
class bytestream_read_machine {
public:
  using state_type = bytestream_read_state;

  bytestream_read_machine(http2_session& session, tls_connection& tls_conn, handle socket,
                          std::string resource_name, std::int64_t read_offset = 0,
                          std::int64_t read_limit = 0);

  [[nodiscard]] auto initial() const -> state_type;
  [[nodiscard]] auto step(state_type s, const event& e) const -> step_result<state_type>;
  [[nodiscard]] auto done(const state_type& s) const -> bool;

private:
  http2_session* session_;
  tls_connection* tls_conn_;
  handle socket_;
  std::string resource_name_;
  std::int64_t read_offset_;
  std::int64_t read_limit_;
};

// ============================================================================
// ByteStream Write (streaming upload)
// ============================================================================

struct bytestream_write_state {
  enum class phase { initial, sending, waiting_write, waiting_read, receiving, done, error };

  phase current_phase{phase::initial};
  handle socket_handle;
  std::int32_t stream_id{0};

  // Input
  std::string resource_name;
  std::vector<std::byte> data;
  std::size_t chunk_size{1024 * 1024};

  // Progress
  std::size_t bytes_sent{0};

  // Output
  std::int64_t committed_size{0};

  // gRPC status
  grpc_status_code status_code{grpc_status_code::ok};
  std::string status_message;

  // I/O
  std::vector<std::byte> send_buffer;
  std::vector<std::byte> recv_buffer;
  std::uint64_t operation_id{0};

  [[nodiscard]] auto ok() const noexcept -> bool {
    return current_phase == phase::done && status_code == grpc_status_code::ok;
  }
};

/// ByteStream Write machine
///
/// Streams a large blob to CAS.
class bytestream_write_machine {
public:
  using state_type = bytestream_write_state;

  bytestream_write_machine(http2_session& session, tls_connection& tls_conn, handle socket,
                           std::string resource_name, std::vector<std::byte> data,
                           std::size_t chunk_size = 1024 * 1024);

  [[nodiscard]] auto initial() const -> state_type;
  [[nodiscard]] auto step(state_type s, const event& e) const -> step_result<state_type>;
  [[nodiscard]] auto done(const state_type& s) const -> bool;

private:
  http2_session* session_;
  tls_connection* tls_conn_;
  handle socket_;
  std::string resource_name_;
  std::vector<std::byte> data_;
  std::size_t chunk_size_;
};

// ============================================================================
// High-level CAS client
// ============================================================================

/// REAPI CAS client - convenience wrapper around the state machines
///
/// For most use cases, use the individual machines directly for
/// better control over the async flow. This class is for simple
/// synchronous-style usage.
class reapi_cas_client {
public:
  reapi_cas_client(http2_session& session, tls_connection& tls_conn, handle socket,
                   reapi_config config);

  /// Upload a blob, returns digest
  /// Uses BatchUpdateBlobs for small blobs, ByteStream for large
  [[nodiscard]] auto upload_blob(std::span<const std::byte> data) -> reapi_digest;

  /// Download a blob by digest
  [[nodiscard]] auto download_blob(const reapi_digest& digest)
      -> std::optional<std::vector<std::byte>>;

  /// Check which blobs are missing from a list
  [[nodiscard]] auto find_missing_blobs(const std::vector<reapi_digest>& digests)
      -> std::vector<reapi_digest>;

  /// Check if a single blob exists
  [[nodiscard]] auto blob_exists(const reapi_digest& digest) -> bool;

  [[nodiscard]] auto config() const noexcept -> const reapi_config& { return config_; }

private:
  http2_session* session_;
  tls_connection* tls_conn_;
  handle socket_;
  reapi_config config_;
};

} // namespace evring
