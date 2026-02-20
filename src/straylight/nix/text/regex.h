// straylight::nix::text
//
// High-performance regex engine using RE2 with ERE (Extended Regular Expression)
// compatibility for Nix builtins.match and builtins.split.
//
// RE2 advantages over std::regex:
//   - Linear time complexity (no backtracking, immune to ReDoS)
//   - 10-100x faster than std::regex for typical patterns
//   - Memory efficient with shared compiled regex
//   - Thread-safe compiled pattern sharing
//
// ERE compatibility:
//   RE2 with POSIX mode supports POSIX Extended Regular Expressions as
//   specified by Nix for builtins.match/builtins.split. This includes:
//     - Character classes: [abc], [^abc], [a-z]
//     - POSIX classes: [[:alpha:]], [[:digit:]], [[:space:]], etc.
//     - Quantifiers: *, +, ?, {n}, {n,}, {n,m}
//     - Alternation: a|b
//     - Grouping: (abc)
//     - Anchors: ^, $
//
//   NOT supported (and not used in Nix patterns):
//     - Backreferences: \1, \2 (verified: no Nix patterns use these)
//
// Usage:
//   // Compile once, match many
//   auto re = regex::compile("([[:alpha:]]+)([[:digit:]]+)");
//   if (!re) { /* handle error */ }
//
//   // Full match (like builtins.match - must match entire string)
//   auto match = re->full_match("abc123");
//   if (match) {
//     for (auto& group : match->groups()) { ... }
//   }
//
//   // Partial match (find first occurrence)
//   auto match = re->partial_match("prefix abc123 suffix");
//
//   // Find all matches (for builtins.split)
//   for (auto& match : re->find_all("abc123 def456")) { ... }
//
// Thread-safe caching:
//   regex::Cache cache;
//   auto& re = cache.get("pattern");  // Compiled once, reused

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Forward declare RE2 to avoid header pollution
namespace re2 {
class RE2;
}

namespace straylight::nix::text {

// ─────────────────────────────────────────────────────────────────────────────
// Error handling
// ─────────────────────────────────────────────────────────────────────────────

/// Error codes for regex operations
enum class ErrorCode : uint8_t {
  Ok = 0,
  BadPattern,    // Syntax error in regex pattern
  BadCharClass,  // Invalid character class
  BadEscape,     // Invalid escape sequence
  BadRepeat,     // Invalid repetition operator
  TooMuchMemory, // Pattern requires too much memory
  InternalError, // Unexpected internal error
};

/// Human-readable error description
[[nodiscard]] std::string_view error_message(ErrorCode code) noexcept;

/// Regex compilation/match error
struct Error {
  ErrorCode code;
  std::string message; // Detailed error from RE2

  explicit Error(ErrorCode c) : code(c), message(std::string(error_message(c))) {}
  Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Match result
// ─────────────────────────────────────────────────────────────────────────────

/// A single captured group (string_view into original input)
struct Group {
  std::string_view text; // Captured text (empty if unmatched)
  bool matched = false;  // Whether this group participated in match

  /// Implicit conversion for convenience
  operator std::string_view() const noexcept { return text; }
  [[nodiscard]] bool empty() const noexcept { return text.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return text.size(); }
};

/// Match result containing captured groups
/// Group 0 is the entire match, groups 1..N are capture groups
class Match {
public:
  Match() = default;
  explicit Match(std::vector<Group> groups) : groups_(std::move(groups)) {}

  /// Check if match succeeded
  [[nodiscard]] explicit operator bool() const noexcept { return !groups_.empty(); }

  /// Number of groups (including group 0 = full match)
  [[nodiscard]] std::size_t size() const noexcept { return groups_.size(); }

  /// Number of capture groups (excluding group 0)
  [[nodiscard]] std::size_t num_captures() const noexcept {
    return groups_.empty() ? 0 : groups_.size() - 1;
  }

  /// Access group by index (0 = full match)
  [[nodiscard]] const Group& operator[](std::size_t i) const noexcept { return groups_[i]; }

  /// Get all groups as span
  [[nodiscard]] std::span<const Group> groups() const noexcept { return groups_; }

  /// Get capture groups only (excluding full match)
  [[nodiscard]] std::span<const Group> captures() const noexcept {
    if (groups_.size() <= 1)
      return {};
    return std::span<const Group>(groups_).subspan(1);
  }

  /// Get prefix (text before match) - only valid for partial_match
  [[nodiscard]] std::string_view prefix() const noexcept { return prefix_; }

  /// Get suffix (text after match) - only valid for partial_match
  [[nodiscard]] std::string_view suffix() const noexcept { return suffix_; }

  /// Set prefix/suffix (used internally by find operations)
  void set_context(std::string_view pre, std::string_view suf) noexcept {
    prefix_ = pre;
    suffix_ = suf;
  }

private:
  std::vector<Group> groups_;
  std::string_view prefix_;
  std::string_view suffix_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Compiled regex pattern
// ─────────────────────────────────────────────────────────────────────────────

/// Compiled regular expression (immutable, thread-safe to use)
class Regex {
public:
  /// Compile a pattern with ERE (POSIX extended) semantics
  /// Returns error if pattern is invalid
  [[nodiscard]] static std::expected<Regex, Error> compile(std::string_view pattern);

  /// Compile with ECMAScript semantics (for internal static patterns)
  [[nodiscard]] static std::expected<Regex, Error> compile_ecma(std::string_view pattern);

  // Move-only (shared_ptr internally)
  Regex(Regex&&) noexcept = default;
  Regex& operator=(Regex&&) noexcept = default;
  Regex(const Regex&) = default;
  Regex& operator=(const Regex&) = default;
  ~Regex() = default;

  /// Check if regex is valid
  [[nodiscard]] explicit operator bool() const noexcept { return impl_ != nullptr; }

  /// Get the original pattern string
  [[nodiscard]] std::string_view pattern() const noexcept;

  /// Get number of capture groups (excluding group 0)
  [[nodiscard]] std::size_t num_captures() const noexcept;

  // ───────────────────────────────────────────────────────────────────────────
  // Matching operations
  // ───────────────────────────────────────────────────────────────────────────

  /// Full match: regex must match the ENTIRE string (like builtins.match)
  /// Returns nullopt if no match, Match with groups if successful
  [[nodiscard]] std::optional<Match> full_match(std::string_view text) const;

  /// Partial match: find first occurrence anywhere in string
  /// Returns nullopt if no match, Match with prefix/suffix context if successful
  [[nodiscard]] std::optional<Match> partial_match(std::string_view text) const;

  /// Find all non-overlapping matches (for builtins.split)
  /// Each match includes prefix context for split semantics
  [[nodiscard]] std::vector<Match> find_all(std::string_view text) const;

  /// Test if pattern matches (no capture, faster)
  [[nodiscard]] bool test(std::string_view text) const;

  /// Test if pattern matches anywhere (no capture, faster)
  [[nodiscard]] bool test_partial(std::string_view text) const;

private:
  Regex() = default;
  explicit Regex(std::shared_ptr<re2::RE2> impl) : impl_(std::move(impl)) {}

  std::shared_ptr<re2::RE2> impl_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Thread-safe compiled regex cache
// ─────────────────────────────────────────────────────────────────────────────

/// Thread-safe cache for compiled regex patterns
/// Uses boost::concurrent_flat_map internally for lock-free reads
class Cache {
public:
  Cache();
  ~Cache();

  // Non-copyable, non-movable (contains concurrent data structure)
  Cache(const Cache&) = delete;
  Cache& operator=(const Cache&) = delete;
  Cache(Cache&&) = delete;
  Cache& operator=(Cache&&) = delete;

  /// Get or compile a regex pattern (ERE mode)
  /// Returns reference to cached compiled regex
  /// Throws regex::Error if pattern is invalid
  [[nodiscard]] const Regex& get(std::string_view pattern);

  /// Get or compile, returning expected instead of throwing
  [[nodiscard]] std::expected<std::reference_wrapper<const Regex>, Error>
  try_get(std::string_view pattern);

  /// Clear all cached patterns
  void clear();

  /// Number of cached patterns
  [[nodiscard]] std::size_t size() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Convenience functions (compile + match in one call)
// ─────────────────────────────────────────────────────────────────────────────

/// Full match with pattern string (compiles on each call - prefer Regex for reuse)
[[nodiscard]] inline std::expected<std::optional<Match>, Error> full_match(std::string_view pattern,
                                                                           std::string_view text) {
  auto re = Regex::compile(pattern);
  if (!re)
    return std::unexpected(re.error());
  return re->full_match(text);
}

/// Partial match with pattern string
[[nodiscard]] inline std::expected<std::optional<Match>, Error>
partial_match(std::string_view pattern, std::string_view text) {
  auto re = Regex::compile(pattern);
  if (!re)
    return std::unexpected(re.error());
  return re->partial_match(text);
}

/// Test if pattern matches (compiles on each call)
[[nodiscard]] inline std::expected<bool, Error> test(std::string_view pattern,
                                                     std::string_view text) {
  auto re = Regex::compile(pattern);
  if (!re)
    return std::unexpected(re.error());
  return re->test(text);
}

} // namespace straylight::nix::text
