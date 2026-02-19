// straylight::nix::primitives::git - Git object parsing primitives
//
// Pure parsing of Git objects without filesystem/repo access.
// Provides:
//   - GitHash          - 20-byte SHA-1 hash with hex conversion
//   - GitObjectType    - blob, tree, commit, tag
//   - GitBlob          - parsed blob object
//   - GitTree          - parsed tree object with entries
//   - GitCommit        - parsed commit with tree, parents, author, committer, message
//   - GitTag           - parsed annotated tag
//   - parse_git_object - parse raw git object data

#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace straylight::nix::primitives {

// ─────────────────────────────────────────────────────────────────────────────
// GitHash - 20-byte SHA-1 hash
// ─────────────────────────────────────────────────────────────────────────────

/// A 20-byte SHA-1 hash used for Git object identification.
///
/// Provides conversion to/from hex string representation.
/// Supports comparison for use in containers.
class GitHash {
public:
  static constexpr std::size_t byte_length = 20;
  static constexpr std::size_t hex_length = 40;

  /// Default constructor - zero hash.
  constexpr GitHash() noexcept : bytes_{} {}

  /// Construct from raw bytes.
  explicit constexpr GitHash(std::array<std::uint8_t, byte_length> bytes) noexcept
      : bytes_(bytes) {}

  /// Construct from a span of exactly 20 bytes.
  explicit constexpr GitHash(std::span<const std::uint8_t, byte_length> bytes) noexcept {
    std::copy(bytes.begin(), bytes.end(), bytes_.begin());
  }

  /// Parse from hex string (40 characters).
  /// Returns std::nullopt if the string is invalid.
  [[nodiscard]] static std::optional<GitHash> from_hex(std::string_view hex_string) noexcept {
    if (hex_string.size() != hex_length) {
      return std::nullopt;
    }

    GitHash result;
    for (std::size_t index = 0; index < byte_length; ++index) {
      std::uint8_t high_nibble = hex_digit_value(hex_string[index * 2]);
      std::uint8_t low_nibble = hex_digit_value(hex_string[index * 2 + 1]);
      if (high_nibble > 15 || low_nibble > 15) {
        return std::nullopt;
      }
      result.bytes_[index] = static_cast<std::uint8_t>((high_nibble << 4) | low_nibble);
    }
    return result;
  }

  /// Convert to hex string representation.
  [[nodiscard]] std::string to_hex() const noexcept {
    static constexpr char hex_digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(hex_length);
    for (std::uint8_t byte : bytes_) {
      result.push_back(hex_digits[byte >> 4]);
      result.push_back(hex_digits[byte & 0x0F]);
    }
    return result;
  }

  /// Access raw bytes.
  [[nodiscard]] constexpr const std::array<std::uint8_t, byte_length>& bytes() const noexcept {
    return bytes_;
  }

  /// Access raw bytes as span.
  [[nodiscard]] constexpr std::span<const std::uint8_t, byte_length> as_span() const noexcept {
    return bytes_;
  }

  /// Check if this is the zero hash.
  [[nodiscard]] constexpr bool is_zero() const noexcept {
    for (std::uint8_t byte : bytes_) {
      if (byte != 0) {
        return false;
      }
    }
    return true;
  }

  bool operator==(const GitHash&) const = default;
  auto operator<=>(const GitHash&) const = default;

private:
  [[nodiscard]] static constexpr std::uint8_t hex_digit_value(char character) noexcept {
    if (character >= '0' && character <= '9') {
      return static_cast<std::uint8_t>(character - '0');
    }
    if (character >= 'a' && character <= 'f') {
      return static_cast<std::uint8_t>(character - 'a' + 10);
    }
    if (character >= 'A' && character <= 'F') {
      return static_cast<std::uint8_t>(character - 'A' + 10);
    }
    return 255; // Invalid
  }

  std::array<std::uint8_t, byte_length> bytes_;
};

// ─────────────────────────────────────────────────────────────────────────────
// GitObjectType - type of git object
// ─────────────────────────────────────────────────────────────────────────────

/// Git object types.
enum class GitObjectType {
  blob,
  tree,
  commit,
  tag,
};

/// Convert object type to string representation.
[[nodiscard]] constexpr std::string_view git_object_type_to_string(GitObjectType type) noexcept {
  switch (type) {
    case GitObjectType::blob:
      return "blob";
    case GitObjectType::tree:
      return "tree";
    case GitObjectType::commit:
      return "commit";
    case GitObjectType::tag:
      return "tag";
  }
  return "unknown";
}

/// Parse object type from string.
[[nodiscard]] constexpr std::optional<GitObjectType>
git_object_type_from_string(std::string_view type_string) noexcept {
  if (type_string == "blob") {
    return GitObjectType::blob;
  }
  if (type_string == "tree") {
    return GitObjectType::tree;
  }
  if (type_string == "commit") {
    return GitObjectType::commit;
  }
  if (type_string == "tag") {
    return GitObjectType::tag;
  }
  return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// GitTreeEntryMode - file mode for tree entries
// ─────────────────────────────────────────────────────────────────────────────

/// File mode in a git tree entry.
enum class GitTreeEntryMode : std::uint32_t {
  directory = 040000,
  regular_file = 0100644,
  executable_file = 0100755,
  symbolic_link = 0120000,
  gitlink = 0160000, // submodule
};

/// Parse tree entry mode from octal string.
[[nodiscard]] constexpr std::optional<GitTreeEntryMode>
git_tree_entry_mode_from_octal(std::string_view octal_string) noexcept {
  std::uint32_t value = 0;
  for (char character : octal_string) {
    if (character < '0' || character > '7') {
      return std::nullopt;
    }
    value = value * 8 + static_cast<std::uint32_t>(character - '0');
  }

  switch (value) {
    case 040000:
      return GitTreeEntryMode::directory;
    case 0100644:
      return GitTreeEntryMode::regular_file;
    case 0100755:
      return GitTreeEntryMode::executable_file;
    case 0120000:
      return GitTreeEntryMode::symbolic_link;
    case 0160000:
      return GitTreeEntryMode::gitlink;
    default:
      return std::nullopt;
  }
}

/// Check if mode represents a directory.
[[nodiscard]] constexpr bool is_directory_mode(GitTreeEntryMode mode) noexcept {
  return mode == GitTreeEntryMode::directory;
}

/// Check if mode represents a file (regular or executable).
[[nodiscard]] constexpr bool is_file_mode(GitTreeEntryMode mode) noexcept {
  return mode == GitTreeEntryMode::regular_file || mode == GitTreeEntryMode::executable_file;
}

// ─────────────────────────────────────────────────────────────────────────────
// GitSignature - author/committer information
// ─────────────────────────────────────────────────────────────────────────────

/// A signature (author or committer) in a git commit or tag.
struct GitSignature {
  std::string name;
  std::string email;
  std::int64_t timestamp;       // Unix timestamp
  std::int32_t timezone_offset; // Offset in minutes from UTC

  bool operator==(const GitSignature&) const = default;
};

// ─────────────────────────────────────────────────────────────────────────────
// GitTreeEntry - single entry in a tree
// ─────────────────────────────────────────────────────────────────────────────

/// A single entry in a git tree object.
struct GitTreeEntry {
  GitTreeEntryMode mode;
  std::string name;
  GitHash hash;

  bool operator==(const GitTreeEntry&) const = default;
};

// ─────────────────────────────────────────────────────────────────────────────
// GitBlob - parsed blob object
// ─────────────────────────────────────────────────────────────────────────────

/// A parsed git blob object.
struct GitBlob {
  std::vector<std::uint8_t> content;

  bool operator==(const GitBlob&) const = default;
};

// ─────────────────────────────────────────────────────────────────────────────
// GitTree - parsed tree object
// ─────────────────────────────────────────────────────────────────────────────

/// A parsed git tree object.
struct GitTree {
  std::vector<GitTreeEntry> entries;

  bool operator==(const GitTree&) const = default;
};

// ─────────────────────────────────────────────────────────────────────────────
// GitCommit - parsed commit object
// ─────────────────────────────────────────────────────────────────────────────

/// A parsed git commit object.
struct GitCommit {
  GitHash tree_hash;
  std::vector<GitHash> parent_hashes;
  GitSignature author;
  GitSignature committer;
  std::string message;
  std::optional<std::string> gpg_signature;

  bool operator==(const GitCommit&) const = default;
};

// ─────────────────────────────────────────────────────────────────────────────
// GitTag - parsed tag object
// ─────────────────────────────────────────────────────────────────────────────

/// A parsed git annotated tag object.
struct GitTag {
  GitHash object_hash;
  GitObjectType object_type;
  std::string tag_name;
  GitSignature tagger;
  std::string message;
  std::optional<std::string> gpg_signature;

  bool operator==(const GitTag&) const = default;
};

// ─────────────────────────────────────────────────────────────────────────────
// GitObject - variant of all object types
// ─────────────────────────────────────────────────────────────────────────────

/// A parsed git object (blob, tree, commit, or tag).
using GitObject = std::variant<GitBlob, GitTree, GitCommit, GitTag>;

// ─────────────────────────────────────────────────────────────────────────────
// GitObjectHeader - parsed object header
// ─────────────────────────────────────────────────────────────────────────────

/// Header of a git object (type and size).
struct GitObjectHeader {
  GitObjectType type;
  std::size_t size;

  bool operator==(const GitObjectHeader&) const = default;
};

// ─────────────────────────────────────────────────────────────────────────────
// Parse error type
// ─────────────────────────────────────────────────────────────────────────────

/// Error codes for git object parsing.
enum class GitParseError {
  invalid_header,
  invalid_object_type,
  invalid_size,
  truncated_data,
  invalid_tree_entry,
  invalid_tree_mode,
  invalid_hash,
  invalid_commit_format,
  invalid_signature,
  invalid_tag_format,
  missing_required_field,
};

/// Convert parse error to string.
[[nodiscard]] constexpr std::string_view git_parse_error_to_string(GitParseError error) noexcept {
  switch (error) {
    case GitParseError::invalid_header:
      return "invalid object header";
    case GitParseError::invalid_object_type:
      return "invalid object type";
    case GitParseError::invalid_size:
      return "invalid size field";
    case GitParseError::truncated_data:
      return "truncated data";
    case GitParseError::invalid_tree_entry:
      return "invalid tree entry";
    case GitParseError::invalid_tree_mode:
      return "invalid tree entry mode";
    case GitParseError::invalid_hash:
      return "invalid hash";
    case GitParseError::invalid_commit_format:
      return "invalid commit format";
    case GitParseError::invalid_signature:
      return "invalid signature format";
    case GitParseError::invalid_tag_format:
      return "invalid tag format";
    case GitParseError::missing_required_field:
      return "missing required field";
  }
  return "unknown error";
}

// ─────────────────────────────────────────────────────────────────────────────
// Parsing functions
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// Find the position of a character in a span.
[[nodiscard]] inline std::optional<std::size_t> find_byte(std::span<const std::uint8_t> data,
                                                          std::uint8_t byte) noexcept {
  auto iterator = std::find(data.begin(), data.end(), byte);
  if (iterator == data.end()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(iterator - data.begin());
}

/// Find the position of a newline in a span.
[[nodiscard]] inline std::optional<std::size_t>
find_newline(std::span<const std::uint8_t> data) noexcept {
  return find_byte(data, '\n');
}

/// Convert span to string_view.
[[nodiscard]] inline std::string_view
span_to_string_view(std::span<const std::uint8_t> data) noexcept {
  return std::string_view(reinterpret_cast<const char*>(data.data()), data.size());
}

/// Parse a signature line (author/committer).
/// Format: "name <email> timestamp timezone"
[[nodiscard]] inline std::expected<GitSignature, GitParseError>
parse_signature(std::string_view line) noexcept {
  // Find email boundaries
  std::size_t email_start = line.find('<');
  std::size_t email_end = line.find('>');
  if (email_start == std::string_view::npos || email_end == std::string_view::npos ||
      email_end <= email_start) {
    return std::unexpected(GitParseError::invalid_signature);
  }

  GitSignature signature;

  // Name is everything before '<', trimmed
  std::string_view name_part = line.substr(0, email_start);
  while (!name_part.empty() && name_part.back() == ' ') {
    name_part.remove_suffix(1);
  }
  signature.name = std::string(name_part);

  // Email is between '<' and '>'
  signature.email = std::string(line.substr(email_start + 1, email_end - email_start - 1));

  // Parse timestamp and timezone after '> '
  std::string_view remainder = line.substr(email_end + 1);
  while (!remainder.empty() && remainder.front() == ' ') {
    remainder.remove_prefix(1);
  }

  // Find space between timestamp and timezone
  std::size_t space_position = remainder.find(' ');
  if (space_position == std::string_view::npos) {
    return std::unexpected(GitParseError::invalid_signature);
  }

  // Parse timestamp
  std::string_view timestamp_string = remainder.substr(0, space_position);
  auto timestamp_result =
      std::from_chars(timestamp_string.data(), timestamp_string.data() + timestamp_string.size(),
                      signature.timestamp);
  if (timestamp_result.ec != std::errc{}) {
    return std::unexpected(GitParseError::invalid_signature);
  }

  // Parse timezone offset (e.g., +0100 or -0500)
  std::string_view timezone_string = remainder.substr(space_position + 1);
  if (timezone_string.empty()) {
    return std::unexpected(GitParseError::invalid_signature);
  }

  std::int32_t timezone_sign = 1;
  if (timezone_string[0] == '-') {
    timezone_sign = -1;
    timezone_string.remove_prefix(1);
  } else if (timezone_string[0] == '+') {
    timezone_string.remove_prefix(1);
  }

  std::int32_t timezone_value = 0;
  auto timezone_result = std::from_chars(
      timezone_string.data(), timezone_string.data() + timezone_string.size(), timezone_value);
  if (timezone_result.ec != std::errc{}) {
    return std::unexpected(GitParseError::invalid_signature);
  }

  // Convert HHMM to minutes
  std::int32_t hours = timezone_value / 100;
  std::int32_t minutes = timezone_value % 100;
  signature.timezone_offset = timezone_sign * (hours * 60 + minutes);

  return signature;
}

} // namespace detail

/// Parse a git object header from raw data.
/// Format: "type size\0"
///
/// Returns the header and the position after the null byte.
[[nodiscard]] inline std::expected<std::pair<GitObjectHeader, std::size_t>, GitParseError>
parse_git_object_header(std::span<const std::uint8_t> data) noexcept {
  // Find the null byte that terminates the header
  auto null_position = detail::find_byte(data, '\0');
  if (!null_position) {
    return std::unexpected(GitParseError::invalid_header);
  }

  std::string_view header_string = detail::span_to_string_view(data.subspan(0, *null_position));

  // Find the space between type and size
  std::size_t space_position = header_string.find(' ');
  if (space_position == std::string_view::npos) {
    return std::unexpected(GitParseError::invalid_header);
  }

  // Parse type
  std::string_view type_string = header_string.substr(0, space_position);
  auto object_type = git_object_type_from_string(type_string);
  if (!object_type) {
    return std::unexpected(GitParseError::invalid_object_type);
  }

  // Parse size
  std::string_view size_string = header_string.substr(space_position + 1);
  std::size_t size = 0;
  auto size_result =
      std::from_chars(size_string.data(), size_string.data() + size_string.size(), size);
  if (size_result.ec != std::errc{}) {
    return std::unexpected(GitParseError::invalid_size);
  }

  GitObjectHeader header{*object_type, size};
  return std::pair{header, *null_position + 1};
}

/// Parse a git blob from raw data (after header).
[[nodiscard]] inline std::expected<GitBlob, GitParseError>
parse_git_blob(std::span<const std::uint8_t> data, std::size_t expected_size) noexcept {
  if (data.size() < expected_size) {
    return std::unexpected(GitParseError::truncated_data);
  }

  GitBlob blob;
  blob.content.assign(data.begin(), data.begin() + expected_size);
  return blob;
}

/// Parse a git tree from raw data (after header).
/// Tree format: repeated entries of "mode name\0hash(20 bytes)"
[[nodiscard]] inline std::expected<GitTree, GitParseError>
parse_git_tree(std::span<const std::uint8_t> data, std::size_t expected_size) noexcept {
  if (data.size() < expected_size) {
    return std::unexpected(GitParseError::truncated_data);
  }

  GitTree tree;
  std::size_t offset = 0;
  std::span<const std::uint8_t> remaining = data.subspan(0, expected_size);

  while (offset < expected_size) {
    // Find space after mode
    std::span<const std::uint8_t> entry_data = remaining.subspan(offset);
    auto space_position = detail::find_byte(entry_data, ' ');
    if (!space_position) {
      return std::unexpected(GitParseError::invalid_tree_entry);
    }

    // Parse mode
    std::string_view mode_string =
        detail::span_to_string_view(entry_data.subspan(0, *space_position));
    auto mode = git_tree_entry_mode_from_octal(mode_string);
    if (!mode) {
      return std::unexpected(GitParseError::invalid_tree_mode);
    }

    // Find null after name
    std::span<const std::uint8_t> after_space = entry_data.subspan(*space_position + 1);
    auto null_position = detail::find_byte(after_space, '\0');
    if (!null_position) {
      return std::unexpected(GitParseError::invalid_tree_entry);
    }

    // Extract name
    std::string name(detail::span_to_string_view(after_space.subspan(0, *null_position)));

    // Extract hash (20 bytes after null)
    std::size_t hash_start = *space_position + 1 + *null_position + 1;
    if (hash_start + GitHash::byte_length > entry_data.size()) {
      return std::unexpected(GitParseError::truncated_data);
    }

    std::span<const std::uint8_t, GitHash::byte_length> hash_bytes(entry_data.data() + hash_start,
                                                                   GitHash::byte_length);
    GitHash hash(hash_bytes);

    tree.entries.push_back(GitTreeEntry{*mode, std::move(name), hash});
    offset += hash_start + GitHash::byte_length;
  }

  return tree;
}

/// Parse a git commit from raw data (after header).
[[nodiscard]] inline std::expected<GitCommit, GitParseError>
parse_git_commit(std::span<const std::uint8_t> data, std::size_t expected_size) noexcept {
  if (data.size() < expected_size) {
    return std::unexpected(GitParseError::truncated_data);
  }

  GitCommit commit;
  std::string_view content = detail::span_to_string_view(data.subspan(0, expected_size));

  // Parse header lines until empty line
  std::size_t position = 0;
  bool has_tree = false;
  bool has_author = false;
  bool has_committer = false;
  std::string gpg_sig;
  bool in_gpg_sig = false;

  while (position < content.size()) {
    std::size_t line_end = content.find('\n', position);
    if (line_end == std::string_view::npos) {
      line_end = content.size();
    }

    std::string_view line = content.substr(position, line_end - position);

    // Empty line marks start of message
    if (line.empty()) {
      position = line_end + 1;
      break;
    }

    // Handle multiline GPG signature
    if (in_gpg_sig) {
      if (line.starts_with(' ')) {
        gpg_sig += '\n';
        gpg_sig += line.substr(1);
        position = line_end + 1;
        continue;
      } else {
        in_gpg_sig = false;
        commit.gpg_signature = std::move(gpg_sig);
        gpg_sig.clear();
      }
    }

    // Parse header field
    std::size_t space_pos = line.find(' ');
    if (space_pos == std::string_view::npos) {
      return std::unexpected(GitParseError::invalid_commit_format);
    }

    std::string_view field_name = line.substr(0, space_pos);
    std::string_view field_value = line.substr(space_pos + 1);

    if (field_name == "tree") {
      auto hash = GitHash::from_hex(field_value);
      if (!hash) {
        return std::unexpected(GitParseError::invalid_hash);
      }
      commit.tree_hash = *hash;
      has_tree = true;
    } else if (field_name == "parent") {
      auto hash = GitHash::from_hex(field_value);
      if (!hash) {
        return std::unexpected(GitParseError::invalid_hash);
      }
      commit.parent_hashes.push_back(*hash);
    } else if (field_name == "author") {
      auto sig = detail::parse_signature(field_value);
      if (!sig) {
        return std::unexpected(sig.error());
      }
      commit.author = *sig;
      has_author = true;
    } else if (field_name == "committer") {
      auto sig = detail::parse_signature(field_value);
      if (!sig) {
        return std::unexpected(sig.error());
      }
      commit.committer = *sig;
      has_committer = true;
    } else if (field_name == "gpgsig") {
      in_gpg_sig = true;
      gpg_sig = std::string(field_value);
    }
    // Ignore unknown fields

    position = line_end + 1;
  }

  // Handle GPG signature that extends to end of headers
  if (in_gpg_sig) {
    commit.gpg_signature = std::move(gpg_sig);
  }

  // Validate required fields
  if (!has_tree || !has_author || !has_committer) {
    return std::unexpected(GitParseError::missing_required_field);
  }

  // Rest is the commit message
  if (position < content.size()) {
    commit.message = std::string(content.substr(position));
  }

  return commit;
}

/// Parse a git tag from raw data (after header).
[[nodiscard]] inline std::expected<GitTag, GitParseError>
parse_git_tag(std::span<const std::uint8_t> data, std::size_t expected_size) noexcept {
  if (data.size() < expected_size) {
    return std::unexpected(GitParseError::truncated_data);
  }

  GitTag tag;
  std::string_view content = detail::span_to_string_view(data.subspan(0, expected_size));

  // Parse header lines until empty line
  std::size_t position = 0;
  bool has_object = false;
  bool has_type = false;
  bool has_tag = false;
  bool has_tagger = false;
  std::string gpg_sig;
  bool in_gpg_sig = false;

  while (position < content.size()) {
    std::size_t line_end = content.find('\n', position);
    if (line_end == std::string_view::npos) {
      line_end = content.size();
    }

    std::string_view line = content.substr(position, line_end - position);

    // Empty line marks start of message
    if (line.empty()) {
      position = line_end + 1;
      break;
    }

    // Handle multiline GPG signature
    if (in_gpg_sig) {
      if (line.starts_with(' ')) {
        gpg_sig += '\n';
        gpg_sig += line.substr(1);
        position = line_end + 1;
        continue;
      } else {
        in_gpg_sig = false;
        tag.gpg_signature = std::move(gpg_sig);
        gpg_sig.clear();
      }
    }

    // Parse header field
    std::size_t space_pos = line.find(' ');
    if (space_pos == std::string_view::npos) {
      return std::unexpected(GitParseError::invalid_tag_format);
    }

    std::string_view field_name = line.substr(0, space_pos);
    std::string_view field_value = line.substr(space_pos + 1);

    if (field_name == "object") {
      auto hash = GitHash::from_hex(field_value);
      if (!hash) {
        return std::unexpected(GitParseError::invalid_hash);
      }
      tag.object_hash = *hash;
      has_object = true;
    } else if (field_name == "type") {
      auto object_type = git_object_type_from_string(field_value);
      if (!object_type) {
        return std::unexpected(GitParseError::invalid_object_type);
      }
      tag.object_type = *object_type;
      has_type = true;
    } else if (field_name == "tag") {
      tag.tag_name = std::string(field_value);
      has_tag = true;
    } else if (field_name == "tagger") {
      auto sig = detail::parse_signature(field_value);
      if (!sig) {
        return std::unexpected(sig.error());
      }
      tag.tagger = *sig;
      has_tagger = true;
    } else if (field_name == "gpgsig") {
      in_gpg_sig = true;
      gpg_sig = std::string(field_value);
    }
    // Ignore unknown fields

    position = line_end + 1;
  }

  // Handle GPG signature that extends to end of headers
  if (in_gpg_sig) {
    tag.gpg_signature = std::move(gpg_sig);
  }

  // Validate required fields (tagger is optional)
  if (!has_object || !has_type || !has_tag) {
    return std::unexpected(GitParseError::missing_required_field);
  }

  // Ensure tagger has default if not present
  if (!has_tagger) {
    tag.tagger = GitSignature{};
  }

  // Rest is the tag message
  if (position < content.size()) {
    tag.message = std::string(content.substr(position));
  }

  return tag;
}

/// Parse a complete git object from raw data.
/// The data should include the header ("type size\0content").
[[nodiscard]] inline std::expected<GitObject, GitParseError>
parse_git_object(std::span<const std::uint8_t> data) noexcept {
  // Parse header
  auto header_result = parse_git_object_header(data);
  if (!header_result) {
    return std::unexpected(header_result.error());
  }

  auto [header, content_offset] = *header_result;
  std::span<const std::uint8_t> content = data.subspan(content_offset);

  // Parse body based on type
  switch (header.type) {
    case GitObjectType::blob: {
      auto blob = parse_git_blob(content, header.size);
      if (!blob) {
        return std::unexpected(blob.error());
      }
      return *blob;
    }
    case GitObjectType::tree: {
      auto tree = parse_git_tree(content, header.size);
      if (!tree) {
        return std::unexpected(tree.error());
      }
      return *tree;
    }
    case GitObjectType::commit: {
      auto commit = parse_git_commit(content, header.size);
      if (!commit) {
        return std::unexpected(commit.error());
      }
      return *commit;
    }
    case GitObjectType::tag: {
      auto tag_obj = parse_git_tag(content, header.size);
      if (!tag_obj) {
        return std::unexpected(tag_obj.error());
      }
      return *tag_obj;
    }
  }

  return std::unexpected(GitParseError::invalid_object_type);
}

/// Parse a git object from raw data, returning the content without the header.
/// Convenience overload that accepts string_view.
[[nodiscard]] inline std::expected<GitObject, GitParseError>
parse_git_object(std::string_view data) noexcept {
  return parse_git_object(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
}

} // namespace straylight::nix::primitives
