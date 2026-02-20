// SPDX-License-Identifier: MIT
#pragma once

/// @file nar_serialize.h
/// @brief NAR (Nix Archive) format serializers and parsers - header-only C++23
///
/// NAR is a deterministic archive format used by Nix for content-addressed storage.
///
/// Key properties:
/// - Deterministic: Same filesystem content always produces identical NAR
/// - Platform-independent: Portable across Unix-like systems
/// - Content-addressed: Only content matters, not metadata like timestamps
///
/// Wire format:
/// - Strings: u64_le(length) + bytes + zero_padding_to_8_byte_boundary
/// - Magic: "nix-archive-1"
/// - Node types: regular, directory, symlink
///
/// Writing example:
/// @code
/// std::vector<std::byte> buf;
/// nar::Writer w{buf};
/// nar::dump_string(w, "hello world");
/// @endcode
///
/// Parsing example:
/// @code
/// auto result = nar::Reader::parse(data);
/// if (!nar::is_error(result)) {
///   nar::FsObject& obj = nar::get_value(result);
///   // use obj
/// }
/// @endcode

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace straylight::nar {

/// NAR version magic string
inline constexpr std::string_view NAR_VERSION_MAGIC = "nix-archive-1";

// =============================================================================
// Filesystem tree representation
// =============================================================================

/// A filesystem object that can be serialized to/parsed from NAR
struct FsObject {
  struct RegularFile {
    std::vector<std::byte> contents_;
    bool executable_ = false;

    bool operator==(const RegularFile&) const = default;
  };

  struct Symlink {
    std::string target_;

    bool operator==(const Symlink&) const = default;
  };

  struct Directory {
    std::vector<std::pair<std::string, FsObject>> entries_;

    // Declare but don't define here - FsObject is incomplete
    // Definition is after FsObject is complete
    bool operator==(const Directory& other) const;
  };

  std::variant<RegularFile, Symlink, Directory> data_;

  bool operator==(const FsObject&) const = default;

  // Private constructor
  explicit FsObject(std::variant<RegularFile, Symlink, Directory> d) : data_(std::move(d)) {}

  // Factory methods

  /// Create a regular file
  static FsObject file(std::string_view contents) {
    return FsObject{
        RegularFile{.contents_ = std::vector<std::byte>(
                        reinterpret_cast<const std::byte*>(contents.data()),
                        reinterpret_cast<const std::byte*>(contents.data() + contents.size())),
                    .executable_ = false}};
  }

  /// Create an executable file
  static FsObject executable(std::string_view contents) {
    return FsObject{
        RegularFile{.contents_ = std::vector<std::byte>(
                        reinterpret_cast<const std::byte*>(contents.data()),
                        reinterpret_cast<const std::byte*>(contents.data() + contents.size())),
                    .executable_ = true}};
  }

  /// Create a symlink
  static FsObject symlink(std::string_view target) {
    return FsObject{Symlink{.target_ = std::string(target)}};
  }

  /// Create an empty directory
  static FsObject empty_dir() { return FsObject{Directory{}}; }

  /// Create a directory with entries (will be sorted)
  static FsObject directory(std::vector<std::pair<std::string, FsObject>> entries) {
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return FsObject{Directory{.entries_ = std::move(entries)}};
  }

  /// Serialize this filesystem object to NAR format
  void to_nar(std::vector<std::byte>& buf) const;

private:
  friend class Writer;
  void write_node(class Writer& w) const;
};

// =============================================================================
// Writer
// =============================================================================

/// Low-level NAR writer that appends to a byte buffer
class Writer {
  std::vector<std::byte>& buf_;

public:
  explicit Writer(std::vector<std::byte>& buf) : buf_(buf) {}

  // ===========================================================================
  // Wire primitives
  // ===========================================================================

  /// Write a u64 in little-endian format
  void write_u64(uint64_t val) {
    auto bytes = std::bit_cast<std::array<std::byte, 8>>(val);
    buf_.insert(buf_.end(), bytes.begin(), bytes.end());
  }

  /// Write padding bytes to align to 8-byte boundary
  void write_padding(size_t len) {
    size_t pad = (8 - (len % 8)) % 8;
    for (size_t idx = 0; idx < pad; ++idx) {
      buf_.push_back(std::byte{0});
    }
  }

  /// Write a NAR string (length-prefixed with padding)
  void write_str(std::string_view s) { write_bytes(std::as_bytes(std::span{s})); }

  /// Write NAR bytes (length-prefixed with padding)
  void write_bytes(std::span<const std::byte> data) {
    write_u64(data.size());
    buf_.insert(buf_.end(), data.begin(), data.end());
    write_padding(data.size());
  }

  /// Write NAR bytes from char data
  void write_bytes(std::span<const char> data) { write_bytes(std::as_bytes(std::span{data})); }

  /// Write NAR bytes from uint8_t data
  void write_bytes(std::span<const uint8_t> data) {
    write_bytes(
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(data.data()), data.size()});
  }

  // ===========================================================================
  // NAR structure
  // ===========================================================================

  /// Write the NAR magic header
  void write_magic() { write_str(NAR_VERSION_MAGIC); }

  /// Write an opening parenthesis
  void write_open() { write_str("("); }

  /// Write a closing parenthesis
  void write_close() { write_str(")"); }

  /// Write type field
  void write_type(std::string_view type_name) {
    write_str("type");
    write_str(type_name);
  }

  // ===========================================================================
  // Node types
  // ===========================================================================

  /// Write a complete regular file node
  void write_regular_file(std::span<const std::byte> contents, bool executable = false) {
    write_open();
    write_type("regular");
    if (executable) {
      write_str("executable");
      write_str("");
    }
    write_str("contents");
    write_bytes(contents);
    write_close();
  }

  /// Write a complete regular file node from string content
  void write_regular_file(std::string_view contents, bool executable = false) {
    write_regular_file(std::as_bytes(std::span{contents}), executable);
  }

  /// Write a complete symlink node
  void write_symlink(std::string_view target) {
    write_open();
    write_type("symlink");
    write_str("target");
    write_str(target);
    write_close();
  }

  /// Begin a directory node
  void begin_directory() {
    write_open();
    write_type("directory");
  }

  /// Write a directory entry header
  void begin_entry(std::string_view name) {
    write_str("entry");
    write_open();
    write_str("name");
    write_str(name);
    write_str("node");
  }

  /// End a directory entry
  void end_entry() { write_close(); }

  /// End a directory
  void end_directory() { write_close(); }
};

// FsObject method implementations (need Writer to be defined)

inline void FsObject::to_nar(std::vector<std::byte>& buf) const {
  Writer w{buf};
  w.write_magic();
  write_node(w);
}

inline void FsObject::write_node(Writer& w) const {
  std::visit(
      [&w](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, RegularFile>) {
          w.write_regular_file(arg.contents_, arg.executable_);
        } else if constexpr (std::is_same_v<T, Symlink>) {
          w.write_symlink(arg.target_);
        } else if constexpr (std::is_same_v<T, Directory>) {
          w.begin_directory();
          for (const auto& [name, child] : arg.entries_) {
            w.begin_entry(name);
            child.write_node(w);
            w.end_entry();
          }
          w.end_directory();
        }
      },
      data_);
}

// Directory::operator== definition (deferred because FsObject was incomplete inside struct)
inline bool FsObject::Directory::operator==(const Directory& other) const {
  return entries_ == other.entries_;
}

// =============================================================================
// Reader
// =============================================================================

/// NAR parse error types
enum class NarErrorKind {
  UnexpectedEof,
  InvalidMagic,
  ExpectedToken,
  InvalidNodeType,
  NonZeroPadding,
};

/// NAR parse error
struct NarError {
  NarErrorKind kind_;
  std::string message_;

  NarError(NarErrorKind k, std::string msg) : kind_(k), message_(std::move(msg)) {}

  static NarError unexpected_eof() {
    return {NarErrorKind::UnexpectedEof, "unexpected end of NAR"};
  }

  static NarError invalid_magic(std::string_view got) {
    return {NarErrorKind::InvalidMagic, "invalid NAR magic: " + std::string(got)};
  }

  static NarError expected_token(std::string_view expected, std::string_view got) {
    return {NarErrorKind::ExpectedToken,
            "expected '" + std::string(expected) + "', got '" + std::string(got) + "'"};
  }

  static NarError invalid_node_type(std::string_view got) {
    return {NarErrorKind::InvalidNodeType, "invalid node type: " + std::string(got)};
  }

  static NarError non_zero_padding() {
    return {NarErrorKind::NonZeroPadding, "non-zero padding bytes"};
  }
};

/// Result type for NAR parsing
template <typename T>
using NarResult = std::variant<T, NarError>;

/// Check if result is an error
template <typename T>
bool is_error(const NarResult<T>& r) {
  return std::holds_alternative<NarError>(r);
}

/// Get error from result
template <typename T>
const NarError& get_error(const NarResult<T>& r) {
  return std::get<NarError>(r);
}

/// Get value from result
template <typename T>
T& get_value(NarResult<T>& r) {
  return std::get<T>(r);
}

template <typename T>
const T& get_value(const NarResult<T>& r) {
  return std::get<T>(r);
}

/// Low-level NAR reader that parses from a byte span
class Reader {
  std::span<const std::byte> data_;
  size_t pos_ = 0;

public:
  explicit Reader(std::span<const std::byte> data) : data_(data) {}

  // ===========================================================================
  // Wire primitives
  // ===========================================================================

  /// Read a u64 in little-endian format
  NarResult<uint64_t> read_u64() {
    if (pos_ + 8 > data_.size()) {
      return NarError::unexpected_eof();
    }
    uint64_t val;
    std::memcpy(&val, data_.data() + pos_, 8);
    pos_ += 8;
    return val;
  }

  /// Read and verify padding bytes (must be zeros)
  NarResult<std::monostate> read_padding(size_t len) {
    size_t pad = (8 - (len % 8)) % 8;
    if (pos_ + pad > data_.size()) {
      return NarError::unexpected_eof();
    }
    for (size_t idx = 0; idx < pad; ++idx) {
      if (data_[pos_ + idx] != std::byte{0}) {
        return NarError::non_zero_padding();
      }
    }
    pos_ += pad;
    return std::monostate{};
  }

  /// Read NAR bytes (length-prefixed with padding)
  NarResult<std::vector<std::byte>> read_bytes() {
    auto len_result = read_u64();
    if (is_error(len_result)) {
      return get_error(len_result);
    }
    size_t len = static_cast<size_t>(get_value(len_result));

    if (pos_ + len > data_.size()) {
      return NarError::unexpected_eof();
    }

    std::vector<std::byte> result(data_.begin() + pos_, data_.begin() + pos_ + len);
    pos_ += len;

    auto pad_result = read_padding(len);
    if (is_error(pad_result)) {
      return get_error(pad_result);
    }

    return result;
  }

  /// Read a NAR string (length-prefixed with padding)
  NarResult<std::string> read_str() {
    auto bytes_result = read_bytes();
    if (is_error(bytes_result)) {
      return get_error(bytes_result);
    }
    auto& bytes = get_value(bytes_result);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }

  /// Read and expect a specific string token
  NarResult<std::monostate> expect(std::string_view expected) {
    auto str_result = read_str();
    if (is_error(str_result)) {
      return get_error(str_result);
    }
    auto& got = get_value(str_result);
    if (got != expected) {
      return NarError::expected_token(expected, got);
    }
    return std::monostate{};
  }

  // ===========================================================================
  // NAR structure parsing
  // ===========================================================================

  /// Read and validate the NAR magic header
  NarResult<std::monostate> read_magic() {
    auto str_result = read_str();
    if (is_error(str_result)) {
      return get_error(str_result);
    }
    auto& magic = get_value(str_result);
    if (magic != NAR_VERSION_MAGIC) {
      return NarError::invalid_magic(magic);
    }
    return std::monostate{};
  }

  /// Read a complete node (file, directory, or symlink)
  NarResult<FsObject> read_node() {
    auto r1 = expect("(");
    if (is_error(r1))
      return get_error(r1);

    auto r2 = expect("type");
    if (is_error(r2))
      return get_error(r2);

    auto type_result = read_str();
    if (is_error(type_result))
      return get_error(type_result);
    auto& type_name = get_value(type_result);

    if (type_name == "regular") {
      return read_regular_file();
    } else if (type_name == "directory") {
      return read_directory();
    } else if (type_name == "symlink") {
      return read_symlink();
    } else {
      return NarError::invalid_node_type(type_name);
    }
  }

  /// Parse a NAR from a byte span
  static NarResult<FsObject> parse(std::span<const std::byte> data) {
    Reader r{data};
    auto magic_result = r.read_magic();
    if (is_error(magic_result)) {
      return get_error(magic_result);
    }
    return r.read_node();
  }

private:
  /// Read a regular file node (after "type" "regular")
  NarResult<FsObject> read_regular_file() {
    // Next token is either "executable" or "contents"
    auto first_result = read_str();
    if (is_error(first_result))
      return get_error(first_result);
    auto& first = get_value(first_result);

    bool executable = (first == "executable");

    if (executable) {
      // Read empty executable marker
      auto r1 = expect("");
      if (is_error(r1))
        return get_error(r1);
      // Then "contents"
      auto r2 = expect("contents");
      if (is_error(r2))
        return get_error(r2);
    } else if (first != "contents") {
      return NarError::expected_token("contents", first);
    }

    auto contents_result = read_bytes();
    if (is_error(contents_result))
      return get_error(contents_result);

    auto r3 = expect(")");
    if (is_error(r3))
      return get_error(r3);

    return FsObject{FsObject::RegularFile{.contents_ = std::move(get_value(contents_result)),
                                          .executable_ = executable}};
  }

  /// Read a directory node (after "type" "directory")
  NarResult<FsObject> read_directory() {
    std::vector<std::pair<std::string, FsObject>> entries;

    while (true) {
      auto tag_result = read_str();
      if (is_error(tag_result))
        return get_error(tag_result);
      auto& tag = get_value(tag_result);

      if (tag == ")") {
        // End of directory
        break;
      } else if (tag != "entry") {
        return NarError::expected_token("entry", tag);
      }

      // Read entry: "(" "name" <name> "node" <node> ")"
      auto r1 = expect("(");
      if (is_error(r1))
        return get_error(r1);
      auto r2 = expect("name");
      if (is_error(r2))
        return get_error(r2);

      auto name_result = read_str();
      if (is_error(name_result))
        return get_error(name_result);

      auto r3 = expect("node");
      if (is_error(r3))
        return get_error(r3);

      auto child_result = read_node();
      if (is_error(child_result))
        return get_error(child_result);

      auto r4 = expect(")");
      if (is_error(r4))
        return get_error(r4);

      entries.emplace_back(std::move(get_value(name_result)), std::move(get_value(child_result)));
    }

    return FsObject{FsObject::Directory{.entries_ = std::move(entries)}};
  }

  /// Read a symlink node (after "type" "symlink")
  NarResult<FsObject> read_symlink() {
    auto r1 = expect("target");
    if (is_error(r1))
      return get_error(r1);

    auto target_result = read_str();
    if (is_error(target_result))
      return get_error(target_result);

    auto r2 = expect(")");
    if (is_error(r2))
      return get_error(r2);

    return FsObject{FsObject::Symlink{.target_ = std::move(get_value(target_result))}};
  }
};

// =============================================================================
// High-level API
// =============================================================================

/// Dump a string/bytes as a simple NAR (single regular file at root)
inline void dump_string(Writer& w, std::string_view contents) {
  w.write_magic();
  w.write_regular_file(contents, false);
}

inline void dump_string(Writer& w, std::span<const std::byte> contents) {
  w.write_magic();
  w.write_regular_file(contents, false);
}

/// Dump an executable file as a NAR
inline void dump_executable(Writer& w, std::string_view contents) {
  w.write_magic();
  w.write_regular_file(contents, true);
}

/// Dump a symlink as a NAR
inline void dump_symlink(Writer& w, std::string_view target) {
  w.write_magic();
  w.write_symlink(target);
}

/// Dump an empty directory as a NAR
inline void dump_empty_directory(Writer& w) {
  w.write_magic();
  w.begin_directory();
  w.end_directory();
}

} // namespace straylight::nar
