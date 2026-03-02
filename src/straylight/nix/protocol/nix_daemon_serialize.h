// SPDX-License-Identifier: MIT
// Nix Daemon Protocol Serializers
// Generated from nix_daemon.ksy specification
//
// Usage:
//   std::vector<std::byte> buf;
//   straylight::protocol::Writer w{buf};
//   w.write_client_hello(0x0126);  // version 1.38
//   w.write_request(Op::QueryPathInfo, "/nix/store/...");
//
// Compile with: -std=c++23 -Wall -Wextra -Wpedantic

#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace straylight::protocol {

// =============================================================================
// Protocol Constants
// =============================================================================

inline constexpr uint64_t WORKER_MAGIC_1 = 0x6e697863; // "nixc" client->server
inline constexpr uint64_t WORKER_MAGIC_2 = 0x6478696f; // "dxio" server->client

inline constexpr uint64_t STDERR_NEXT = 0x6f6c6d67;
inline constexpr uint64_t STDERR_READ = 0x64617461;
inline constexpr uint64_t STDERR_WRITE = 0x64617416;
inline constexpr uint64_t STDERR_LAST = 0x616c7473;
inline constexpr uint64_t STDERR_ERROR = 0x63787470;
inline constexpr uint64_t STDERR_START_ACTIVITY = 0x53545254;
inline constexpr uint64_t STDERR_STOP_ACTIVITY = 0x53544f50;
inline constexpr uint64_t STDERR_RESULT = 0x52534c54;

enum class Op : uint64_t {
  IsValidPath = 1,
  HasSubstitutes = 3,
  QueryPathHash = 4,   // obsolete
  QueryReferences = 5, // obsolete
  QueryReferrers = 6,
  AddToStore = 7,
  AddTextToStore = 8, // obsolete since 1.25
  BuildPaths = 9,
  EnsurePath = 10,
  AddTempRoot = 11,
  AddIndirectRoot = 12,
  SyncWithGC = 13,
  FindRoots = 14,
  ExportPath = 16,   // obsolete
  QueryDeriver = 18, // obsolete
  SetOptions = 19,
  CollectGarbage = 20,
  QuerySubstitutablePathInfo = 21,
  QueryDerivationOutputs = 22, // obsolete
  QueryAllValidPaths = 23,
  QueryFailedPaths = 24,
  ClearFailedPaths = 25,
  QueryPathInfo = 26,
  ImportPaths = 27,                // obsolete
  QueryDerivationOutputNames = 28, // obsolete
  QueryPathFromHashPart = 29,
  QuerySubstitutablePathInfos = 30,
  QueryValidPaths = 31,
  QuerySubstitutablePaths = 32,
  QueryValidDerivers = 33,
  OptimiseStore = 34,
  VerifyStore = 35,
  BuildDerivation = 36,
  AddSignatures = 37,
  NarFromPath = 38,
  AddToStoreNar = 39,
  QueryMissing = 40,
  QueryDerivationOutputMap = 41,
  RegisterDrvOutput = 42,
  QueryRealisation = 43,
  AddMultipleToStore = 44,
  AddBuildLog = 45,
  BuildPathsWithResults = 46,
  AddPermRoot = 47,
  QueryActiveBuilds = 48,
};

enum class BuildMode : uint64_t {
  Normal = 0,
  Repair = 1,
  Check = 2,
};

enum class GcAction : uint64_t {
  ReturnLive = 0,
  ReturnDead = 1,
  DeleteDead = 2,
  DeleteSpecific = 3,
};

enum class TrustLevel : uint64_t {
  Unknown = 0,
  Trusted = 1,
  NotTrusted = 2,
};

// =============================================================================
// Shared Data Structures (used by both Reader and Writer)
// =============================================================================

// ValidPathInfo - information about a store path
struct ValidPathInfo {
  std::string deriver_; // Empty = none
  std::string nar_hash_;
  std::vector<std::string> references_;
  uint64_t registration_time_ = 0;
  uint64_t nar_size_ = 0;
  bool ultimate_ = false;
  std::vector<std::string> signatures_;
  std::string ca_; // Empty = none
};

// BuildStatus codes
enum class BuildStatus : uint64_t {
  Built = 0,
  Substituted = 1,
  AlreadyValid = 2,
  PermanentFailure = 3,
  InputRejected = 4,
  OutputRejected = 5,
  TransientFailure = 6,
  CachedFailure = 7,
  TimedOut = 8,
  MiscFailure = 9,
  DependencyFailed = 10,
  LogLimitExceeded = 11,
  NotDeterministic = 12,
  ResolvedDrvFailed = 13,
  NoSubstituters = 14,
};

// =============================================================================
// Writer - Low-level serialization to byte buffer
// =============================================================================

class Writer {
public:
  explicit Writer(std::vector<std::byte>& buf) : buf_(buf) {}

  // Primitives
  void write_u64(uint64_t val) {
    if constexpr (std::endian::native == std::endian::little) {
      append_bytes(&val, sizeof(val));
    } else {
      uint64_t le = std::byteswap(val);
      append_bytes(&le, sizeof(le));
    }
  }

  void write_bool(bool val) { write_u64(val ? 1 : 0); }

  void write_bytes(std::span<const std::byte> data) {
    write_u64(data.size());
    buf_.insert(buf_.end(), data.begin(), data.end());
    write_padding(data.size());
  }

  void write_bytes(std::span<const uint8_t> data) {
    write_bytes(std::span{reinterpret_cast<const std::byte*>(data.data()), data.size()});
  }

  void write_string(std::string_view str) {
    write_u64(str.size());
    buf_.insert(buf_.end(), reinterpret_cast<const std::byte*>(str.data()),
                reinterpret_cast<const std::byte*>(str.data() + str.size()));
    write_padding(str.size());
  }

  // Composite types
  void write_string_list(std::span<const std::string> items) {
    write_u64(items.size());
    for (const auto& s : items) {
      write_string(s);
    }
  }

  void write_string_list(std::span<const std::string_view> items) {
    write_u64(items.size());
    for (const auto& s : items) {
      write_string(s);
    }
  }

  void write_string_set(std::span<const std::string> items) {
    // Sets are sorted on wire
    std::vector<std::string_view> sorted;
    sorted.reserve(items.size());
    for (const auto& s : items) {
      sorted.push_back(s);
    }
    std::sort(sorted.begin(), sorted.end());
    write_u64(sorted.size());
    for (const auto& s : sorted) {
      write_string(s);
    }
  }

  void write_store_path(std::string_view path) { write_string(path); }

  void write_store_path_set(std::span<const std::string> paths) { write_string_set(paths); }

  void write_optional_store_path(std::string_view path) {
    // Empty string = None
    write_string(path);
  }

  void write_derived_path(std::string_view path) { write_string(path); }

  void write_derived_path_list(std::span<const std::string> paths) {
    write_u64(paths.size());
    for (const auto& p : paths) {
      write_derived_path(p);
    }
  }

  // Raw append
  void append_raw(std::span<const std::byte> data) {
    buf_.insert(buf_.end(), data.begin(), data.end());
  }

  [[nodiscard]] size_t size() const { return buf_.size(); }
  [[nodiscard]] std::span<const std::byte> data() const { return buf_; }

private:
  void append_bytes(const void* data, size_t len) {
    const auto* p = static_cast<const std::byte*>(data);
    buf_.insert(buf_.end(), p, p + len);
  }

  void write_padding(size_t len) {
    size_t pad = (8 - (len % 8)) % 8;
    for (size_t idx = 0; idx < pad; ++idx) {
      buf_.push_back(std::byte{0});
    }
  }

  std::vector<std::byte>& buf_;
};

// =============================================================================
// Reader - Low-level deserialization from byte span
// =============================================================================

/// Protocol error types
struct ProtocolError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct UnexpectedEof : ProtocolError {
  UnexpectedEof() : ProtocolError("unexpected end of input") {}
};

struct InvalidMagic : ProtocolError {
  uint64_t magic;
  InvalidMagic(uint64_t m) : ProtocolError("invalid magic: " + std::to_string(m)), magic(m) {}
};

struct NonZeroPadding : ProtocolError {
  NonZeroPadding() : ProtocolError("non-zero padding bytes") {}
};

struct DaemonError : ProtocolError {
  DaemonError(const std::string& msg) : ProtocolError("daemon error: " + msg) {}
};

class Reader {
public:
  explicit Reader(std::span<const std::byte> data) : data_(data), pos_(0) {}
  explicit Reader(std::span<const uint8_t> data)
      : data_(reinterpret_cast<const std::byte*>(data.data()), data.size()), pos_(0) {}

  // ===========================================================================
  // Primitives
  // ===========================================================================

  uint64_t read_u64() {
    require(8);
    uint64_t val;
    std::memcpy(&val, data_.data() + pos_, 8);
    pos_ += 8;
    if constexpr (std::endian::native != std::endian::little) {
      val = std::byteswap(val);
    }
    return val;
  }

  bool read_bool() { return read_u64() != 0; }

  std::vector<std::byte> read_bytes() {
    uint64_t len = read_u64();
    require(len);
    std::vector<std::byte> result(data_.begin() + pos_, data_.begin() + pos_ + len);
    pos_ += len;
    read_padding(len);
    return result;
  }

  std::string read_string() {
    auto bytes = read_bytes();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }

  std::string read_store_path() { return read_string(); }

  std::vector<std::string> read_string_list() {
    uint64_t count = read_u64();
    std::vector<std::string> result;
    result.reserve(count);
    for (uint64_t idx = 0; idx < count; ++idx) {
      result.push_back(read_string());
    }
    return result;
  }

  std::vector<std::string> read_string_set() { return read_string_list(); }
  std::vector<std::string> read_store_path_set() { return read_string_list(); }

  // ===========================================================================
  // Stderr message handling
  // ===========================================================================

  /// Read stderr messages until STDERR_LAST, handling logs and errors
  void read_stderr() {
    while (true) {
      uint64_t msg = read_u64();
      switch (msg) {
        case STDERR_LAST:
          return;
        case STDERR_ERROR: {
          std::string error_msg = read_string();
          throw DaemonError(error_msg);
        }
        case STDERR_NEXT:
          // Log message - read and discard
          (void)read_string();
          break;
        case STDERR_WRITE:
          // Data from daemon - read and discard
          (void)read_bytes();
          break;
        case STDERR_START_ACTIVITY:
          (void)read_u64();    // activity id
          (void)read_u64();    // verbosity
          (void)read_u64();    // activity type
          (void)read_string(); // text
          read_activity_fields();
          (void)read_u64(); // parent
          break;
        case STDERR_STOP_ACTIVITY:
          (void)read_u64(); // activity id
          break;
        case STDERR_RESULT:
          (void)read_u64(); // activity id
          (void)read_u64(); // result type
          read_activity_fields();
          break;
        default:
          throw ProtocolError("invalid stderr message: " + std::to_string(msg));
      }
    }
  }

  // ===========================================================================
  // Position management
  // ===========================================================================

  [[nodiscard]] size_t position() const { return pos_; }
  [[nodiscard]] size_t remaining() const { return data_.size() - pos_; }
  [[nodiscard]] bool at_end() const { return pos_ >= data_.size(); }

private:
  void require(size_t n) {
    if (pos_ + n > data_.size()) {
      throw UnexpectedEof();
    }
  }

  void read_padding(size_t len) {
    size_t pad = (8 - (len % 8)) % 8;
    if (pad > 0) {
      require(pad);
      for (size_t idx = 0; idx < pad; ++idx) {
        if (data_[pos_ + idx] != std::byte{0}) {
          throw NonZeroPadding();
        }
      }
      pos_ += pad;
    }
  }

  void read_activity_fields() {
    uint64_t count = read_u64();
    for (uint64_t idx = 0; idx < count; ++idx) {
      uint64_t field_type = read_u64();
      if (field_type == 0) {
        (void)read_u64(); // int field
      } else if (field_type == 1) {
        (void)read_string(); // string field
      }
    }
  }

  std::span<const std::byte> data_;
  size_t pos_;
};

// =============================================================================
// Response Readers
// =============================================================================

/// Server hello response
struct ServerHello {
  uint64_t magic_;
  uint64_t version_;
};

inline ServerHello read_server_hello(Reader& r) {
  ServerHello hello;
  hello.magic_ = r.read_u64();
  if (hello.magic_ != WORKER_MAGIC_2) {
    throw InvalidMagic(hello.magic_);
  }
  hello.version_ = r.read_u64();
  return hello;
}

/// Read IsValidPath response
inline bool read_is_valid_path_response(Reader& r) {
  return r.read_bool();
}

/// Read QueryPathInfo response
inline std::optional<ValidPathInfo> read_query_path_info_response(Reader& r,
                                                                  uint16_t protocol_version) {
  bool valid = r.read_bool();
  if (!valid) {
    return std::nullopt;
  }

  ValidPathInfo info;
  info.deriver_ = r.read_string();
  info.nar_hash_ = r.read_string();
  info.references_ = r.read_store_path_set();
  info.registration_time_ = r.read_u64();
  info.nar_size_ = r.read_u64();

  if (protocol_version >= 16) {
    info.ultimate_ = r.read_bool();
    info.signatures_ = r.read_string_set();
    info.ca_ = r.read_string();
  }

  return info;
}

/// QueryMissing result
struct QueryMissingResult {
  std::vector<std::string> will_build_;
  std::vector<std::string> will_substitute_;
  std::vector<std::string> unknown_;
  uint64_t download_size_;
  uint64_t nar_size_;
};

/// Read QueryMissing response
inline QueryMissingResult read_query_missing_response(Reader& r) {
  QueryMissingResult result;
  result.will_build_ = r.read_store_path_set();
  result.will_substitute_ = r.read_store_path_set();
  result.unknown_ = r.read_store_path_set();
  result.download_size_ = r.read_u64();
  result.nar_size_ = r.read_u64();
  return result;
}

/// Read QueryReferrers response
inline std::vector<std::string> read_query_referrers_response(Reader& r) {
  return r.read_store_path_set();
}

/// GC root entry
struct GcRoot {
  std::string link_;
  std::string target_;
};

/// Read FindRoots response
inline std::vector<GcRoot> read_find_roots_response(Reader& r) {
  uint64_t count = r.read_u64();
  std::vector<GcRoot> roots;
  roots.reserve(count);
  for (uint64_t idx = 0; idx < count; ++idx) {
    GcRoot root;
    root.link_ = r.read_string();
    root.target_ = r.read_store_path();
    roots.push_back(std::move(root));
  }
  return roots;
}

/// Realisation (parsed from JSON)
struct Realisation {
  std::string id_;
  std::string out_path_;
  std::vector<std::string> signatures_;
};

/// Parse realisation from JSON string (minimal parsing)
inline Realisation parse_realisation_json(const std::string& json) {
  Realisation r;

  // Extract "id" field
  auto pos = json.find("\"id\":\"");
  if (pos != std::string::npos) {
    pos += 6;
    auto end = json.find('"', pos);
    if (end != std::string::npos) {
      r.id_ = json.substr(pos, end - pos);
    }
  }

  // Extract "outPath" field
  pos = json.find("\"outPath\":\"");
  if (pos != std::string::npos) {
    pos += 11;
    auto end = json.find('"', pos);
    if (end != std::string::npos) {
      r.out_path_ = json.substr(pos, end - pos);
    }
  }

  return r;
}

/// BuildResult with path (from BuildPathsWithResults)
struct BuildResultWithPath {
  std::string path_;
  uint64_t status_;
  std::string error_msg_;
  uint64_t times_built_;
  bool is_non_deterministic_;
  uint64_t start_time_;
  uint64_t stop_time_;
  uint64_t cpu_user_;
  uint64_t cpu_system_;
  std::vector<std::pair<std::string, Realisation>> built_outputs_;
};

/// Read BuildPathsWithResults response
inline std::vector<BuildResultWithPath>
read_build_paths_with_results_response(Reader& r, uint16_t protocol_version) {
  uint64_t count = r.read_u64();
  std::vector<BuildResultWithPath> results;
  results.reserve(count);

  for (uint64_t idx = 0; idx < count; ++idx) {
    BuildResultWithPath result;
    result.path_ = r.read_string();
    result.status_ = r.read_u64();
    result.error_msg_ = r.read_string();

    if (protocol_version >= 0x11d) {
      result.times_built_ = r.read_u64();
      result.is_non_deterministic_ = r.read_bool();
    } else {
      result.times_built_ = 0;
      result.is_non_deterministic_ = false;
    }

    if (protocol_version >= 0x11e) {
      result.start_time_ = r.read_u64();
      result.stop_time_ = r.read_u64();
    } else {
      result.start_time_ = 0;
      result.stop_time_ = 0;
    }

    // CPU times - read unconditionally for high protocol versions
    // (observed in captures even at 1.38)
    if (protocol_version >= 0x11c) {
      result.cpu_user_ = r.read_u64();
      result.cpu_system_ = r.read_u64();
    } else {
      result.cpu_user_ = 0;
      result.cpu_system_ = 0;
    }

    // Built outputs
    if (protocol_version >= 0x11c) {
      uint64_t out_count = r.read_u64();
      result.built_outputs_.reserve(out_count);
      for (uint64_t jdx = 0; jdx < out_count; ++jdx) {
        std::string output_name = r.read_string();
        std::string json = r.read_string();
        result.built_outputs_.emplace_back(output_name, parse_realisation_json(json));
      }
    }

    results.push_back(std::move(result));
  }

  return results;
}

// =============================================================================
// Handshake Serializers
// =============================================================================

inline void write_client_hello(Writer& w, uint64_t version) {
  w.write_u64(WORKER_MAGIC_1);
  w.write_u64(version);
}

inline void write_server_hello(Writer& w, uint64_t version) {
  w.write_u64(WORKER_MAGIC_2);
  w.write_u64(version);
}

inline void write_handshake_continuation(Writer& w, uint16_t protocol_version,
                                         bool cpu_affinity = false, bool reserve_space = false) {
  if (protocol_version >= 14) {
    w.write_u64(cpu_affinity ? 1 : 0);
    // If cpu_affinity is set, would write actual affinity value here
  }
  if (protocol_version >= 11) {
    w.write_u64(reserve_space ? 1 : 0);
  }
}

inline void write_server_handshake_info(Writer& w, uint16_t protocol_version,
                                        std::string_view daemon_version,
                                        TrustLevel trust = TrustLevel::Trusted) {
  if (protocol_version >= 33) {
    w.write_string(daemon_version);
  }
  if (protocol_version >= 35) {
    w.write_u64(static_cast<uint64_t>(trust));
  }
}

// =============================================================================
// Request Serializers
// =============================================================================

// Generic operation header
inline void write_op(Writer& w, Op op) {
  w.write_u64(static_cast<uint64_t>(op));
}

// IsValidPath (op 1)
inline void write_is_valid_path_request(Writer& w, std::string_view path) {
  write_op(w, Op::IsValidPath);
  w.write_store_path(path);
}

// QueryPathInfo (op 26)
inline void write_query_path_info_request(Writer& w, std::string_view path) {
  write_op(w, Op::QueryPathInfo);
  w.write_store_path(path);
}

// QueryReferrers (op 6)
inline void write_query_referrers_request(Writer& w, std::string_view path) {
  write_op(w, Op::QueryReferrers);
  w.write_store_path(path);
}

// QueryValidPaths (op 31)
inline void write_query_valid_paths_request(Writer& w, std::span<const std::string> paths,
                                            uint16_t protocol_version, bool substitute = false) {
  write_op(w, Op::QueryValidPaths);
  w.write_store_path_set(paths);
  if (protocol_version >= 27) {
    w.write_bool(substitute);
  }
}

// QueryMissing (op 40)
inline void write_query_missing_request(Writer& w, std::span<const std::string> targets) {
  write_op(w, Op::QueryMissing);
  w.write_derived_path_list(targets);
}

// QueryPathFromHashPart (op 29)
inline void write_query_path_from_hash_part_request(Writer& w, std::string_view hash_part) {
  write_op(w, Op::QueryPathFromHashPart);
  w.write_string(hash_part);
}

// AddTempRoot (op 11)
inline void write_add_temp_root_request(Writer& w, std::string_view path) {
  write_op(w, Op::AddTempRoot);
  w.write_store_path(path);
}

// AddIndirectRoot (op 12)
inline void write_add_indirect_root_request(Writer& w, std::string_view path) {
  write_op(w, Op::AddIndirectRoot);
  w.write_string(path); // Filesystem path, not store path
}

// FindRoots (op 14)
inline void write_find_roots_request(Writer& w) {
  write_op(w, Op::FindRoots);
}

// SyncWithGC (op 13)
inline void write_sync_with_gc_request(Writer& w) {
  write_op(w, Op::SyncWithGC);
}

// BuildPaths (op 9)
inline void write_build_paths_request(Writer& w, std::span<const std::string> paths,
                                      BuildMode mode = BuildMode::Normal) {
  write_op(w, Op::BuildPaths);
  w.write_derived_path_list(paths);
  w.write_u64(static_cast<uint64_t>(mode));
}

// BuildPathsWithResults (op 46)
inline void write_build_paths_with_results_request(Writer& w, std::span<const std::string> paths,
                                                   BuildMode mode = BuildMode::Normal) {
  write_op(w, Op::BuildPathsWithResults);
  w.write_derived_path_list(paths);
  w.write_u64(static_cast<uint64_t>(mode));
}

// EnsurePath (op 10)
inline void write_ensure_path_request(Writer& w, std::string_view path) {
  write_op(w, Op::EnsurePath);
  w.write_store_path(path);
}

// NarFromPath (op 38)
inline void write_nar_from_path_request(Writer& w, std::string_view path) {
  write_op(w, Op::NarFromPath);
  w.write_store_path(path);
}

// OptimiseStore (op 34)
inline void write_optimise_store_request(Writer& w) {
  write_op(w, Op::OptimiseStore);
}

// VerifyStore (op 35)
inline void write_verify_store_request(Writer& w, bool check_contents, bool repair) {
  write_op(w, Op::VerifyStore);
  w.write_bool(check_contents);
  w.write_bool(repair);
}

// CollectGarbage (op 20)
inline void write_collect_garbage_request(Writer& w, GcAction action,
                                          std::span<const std::string> paths_to_delete,
                                          bool ignore_liveness, uint64_t max_freed) {
  write_op(w, Op::CollectGarbage);
  w.write_u64(static_cast<uint64_t>(action));
  w.write_store_path_set(paths_to_delete);
  w.write_bool(ignore_liveness);
  w.write_u64(max_freed);
  w.write_u64(0); // obsolete1
  w.write_u64(0); // obsolete2
  w.write_u64(0); // obsolete3
}

// SetOptions (op 19)
struct ClientSettings {
  bool keep_failed_ = false;
  bool keep_going_ = false;
  bool try_fallback_ = false;
  uint64_t verbosity_ = 0;
  uint64_t max_build_jobs_ = 1;
  uint64_t max_silent_time_ = 0;
  bool use_build_hook_ = true;
  uint64_t verbose_build_ = 0;
  uint64_t log_type_ = 0;
  uint64_t print_build_trace_ = 0;
  uint64_t build_cores_ = 1;
  bool use_substitutes_ = true;
  std::vector<std::pair<std::string, std::string>> overrides_;
};

inline void write_set_options_request(Writer& w, const ClientSettings& settings,
                                      uint16_t protocol_version) {
  write_op(w, Op::SetOptions);
  w.write_bool(settings.keep_failed_);
  w.write_bool(settings.keep_going_);
  w.write_bool(settings.try_fallback_);
  w.write_u64(settings.verbosity_);
  w.write_u64(settings.max_build_jobs_);
  w.write_u64(settings.max_silent_time_);
  w.write_bool(settings.use_build_hook_);
  w.write_u64(settings.verbose_build_);
  w.write_u64(settings.log_type_);
  w.write_u64(settings.print_build_trace_);
  w.write_u64(settings.build_cores_);
  w.write_bool(settings.use_substitutes_);

  if (protocol_version >= 12) {
    w.write_u64(settings.overrides_.size());
    for (const auto& [name, value] : settings.overrides_) {
      w.write_string(name);
      w.write_string(value);
    }
  }
}

// AddToStoreNar (op 39)
struct AddToStoreNarRequest {
  std::string path_;
  std::string deriver_;  // Empty = none
  std::string nar_hash_; // SHA256 in base16/32
  std::vector<std::string> references_;
  uint64_t registration_time_ = 0;
  uint64_t nar_size_ = 0;
  bool ultimate_ = false;
  std::vector<std::string> signatures_;
  std::string ca_; // Empty = none
  bool repair_ = false;
  bool dont_check_sigs_ = false;
};

inline void write_add_to_store_nar_request(Writer& w, const AddToStoreNarRequest& req) {
  write_op(w, Op::AddToStoreNar);
  w.write_store_path(req.path_);
  w.write_optional_store_path(req.deriver_);
  w.write_string(req.nar_hash_);
  w.write_store_path_set(req.references_);
  w.write_u64(req.registration_time_);
  w.write_u64(req.nar_size_);
  w.write_bool(req.ultimate_);
  w.write_string_set(req.signatures_);
  w.write_string(req.ca_);
  w.write_bool(req.repair_);
  w.write_bool(req.dont_check_sigs_);
}

// Framed NAR data (protocol >= 1.23)
inline void write_framed_data(Writer& w, std::span<const std::byte> data) {
  w.write_u64(data.size());
  w.append_raw(data);
}

inline void write_framed_end(Writer& w) {
  w.write_u64(0);
}

// AddSignatures (op 37)
inline void write_add_signatures_request(Writer& w, std::string_view path,
                                         std::span<const std::string> sigs) {
  write_op(w, Op::AddSignatures);
  w.write_store_path(path);
  w.write_string_set(sigs);
}

// =============================================================================
// Response Serializers (for daemon implementation)
// =============================================================================

inline void write_stderr_last(Writer& w) {
  w.write_u64(STDERR_LAST);
}

inline void write_stderr_error(Writer& w, std::string_view msg) {
  w.write_u64(STDERR_ERROR);
  w.write_string(msg);
}

inline void write_bool_response(Writer& w, bool value) {
  write_stderr_last(w);
  w.write_bool(value);
}

inline void write_store_path_response(Writer& w, std::string_view path) {
  write_stderr_last(w);
  w.write_store_path(path);
}

inline void write_store_path_set_response(Writer& w, std::span<const std::string> paths) {
  write_stderr_last(w);
  w.write_store_path_set(paths);
}

inline void write_query_path_info_response(Writer& w, const ValidPathInfo* info,
                                           uint16_t protocol_version) {
  write_stderr_last(w);
  if (!info) {
    w.write_bool(false);
    return;
  }
  w.write_bool(true);
  w.write_optional_store_path(info->deriver_);
  w.write_string(info->nar_hash_);
  w.write_store_path_set(info->references_);
  w.write_u64(info->registration_time_);
  w.write_u64(info->nar_size_);
  if (protocol_version >= 16) {
    w.write_bool(info->ultimate_);
    w.write_string_set(info->signatures_);
    w.write_string(info->ca_);
  }
}

// QueryMissing response
struct QueryMissingResponse {
  std::vector<std::string> will_build_;
  std::vector<std::string> will_substitute_;
  std::vector<std::string> unknown_;
  uint64_t download_size_ = 0;
  uint64_t nar_size_ = 0;
};

inline void write_query_missing_response(Writer& w, const QueryMissingResponse& resp) {
  write_stderr_last(w);
  w.write_store_path_set(resp.will_build_);
  w.write_store_path_set(resp.will_substitute_);
  w.write_store_path_set(resp.unknown_);
  w.write_u64(resp.download_size_);
  w.write_u64(resp.nar_size_);
}

// BuildResult (for write_build_result)
struct BuildResult {
  BuildStatus status_ = BuildStatus::Built;
  std::string error_msg_;
  uint64_t times_built_ = 0;
  bool is_non_deterministic_ = false;
  uint64_t start_time_ = 0;
  uint64_t stop_time_ = 0;
  // CPU times (protocol >= 1.37)
  std::optional<int64_t> cpu_user_us_;
  std::optional<int64_t> cpu_system_us_;
};

inline void write_build_result(Writer& w, const BuildResult& result, uint16_t protocol_version) {
  w.write_u64(static_cast<uint64_t>(result.status_));
  w.write_string(result.error_msg_);
  if (protocol_version >= 29) {
    w.write_u64(result.times_built_);
    w.write_bool(result.is_non_deterministic_);
    w.write_u64(result.start_time_);
    w.write_u64(result.stop_time_);
  }
  if (protocol_version >= 37) {
    // Optional duration
    if (result.cpu_user_us_) {
      w.write_u64(1);
      w.write_u64(static_cast<uint64_t>(*result.cpu_user_us_));
    } else {
      w.write_u64(0);
    }
    if (result.cpu_system_us_) {
      w.write_u64(1);
      w.write_u64(static_cast<uint64_t>(*result.cpu_system_us_));
    } else {
      w.write_u64(0);
    }
  }
  // built_outputs would go here for protocol >= 28
}

} // namespace straylight::protocol
