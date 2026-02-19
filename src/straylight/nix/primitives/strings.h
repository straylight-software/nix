// straylight::nix::primitives::strings
//
// High-performance string operations with adaptive backend selection.
// Uses stringzilla (SIMD-accelerated) for large strings where it helps,
// and std::string_view for small strings where SIMD overhead hurts.
//
// Configuration (via #define before including, or compiler flags):
//   STRAYLIGHT_STRINGS_FIND_THRESHOLD     - Min bytes to use SIMD find
//   STRAYLIGHT_STRINGS_PREFIX_THRESHOLD   - Min bytes to use SIMD prefix
//   STRAYLIGHT_STRINGS_FORCE_STD=1        - Always use std backend
//   STRAYLIGHT_STRINGS_FORCE_SZ=1         - Always use stringzilla
//
// See strings_config.h for full configuration options.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// StringZilla headers
#include <stringzilla/stringzilla.hpp>

// Configuration
#include "strings_config.h"

namespace straylight::nix::primitives {

// ─────────────────────────────────────────────────────────────────────────────
// Type aliases
// ─────────────────────────────────────────────────────────────────────────────

namespace sz = ashvardanian::stringzilla;

// StringZilla types (for direct SIMD access when needed)
using sz_string_view = sz::string_view;
using sz_string_span = sz::string_span;
using byteset = sz::byteset;

// Standard type alias for convenience
using std_string_view = std::string_view;

// ─────────────────────────────────────────────────────────────────────────────
// Conversion utilities
// ─────────────────────────────────────────────────────────────────────────────

/// Convert std::string_view to stringzilla string_view
[[nodiscard]] constexpr sz_string_view to_sz(std::string_view s) noexcept {
  return sz_string_view{s.data(), s.size()};
}

/// Convert stringzilla string_view to std::string_view
[[nodiscard]] constexpr std::string_view to_std(sz_string_view s) noexcept {
  return std::string_view{s.data(), s.size()};
}

/// Convert to std::string (allocates)
[[nodiscard]] inline std::string to_string(std::string_view s) {
  return std::string{s};
}

[[nodiscard]] inline std::string to_string(sz_string_view s) {
  return std::string{s.data(), s.size()};
}

// ─────────────────────────────────────────────────────────────────────────────
// Adaptive search functions
//
// These use compile-time thresholds to choose the best backend.
// For short strings: std::string_view (no SIMD overhead)
// For long strings: stringzilla (SIMD-accelerated)
// ─────────────────────────────────────────────────────────────────────────────

/// Find first occurrence of needle in haystack
/// Uses SIMD for large haystacks, std for small ones
[[nodiscard]] inline std::size_t find(std::string_view haystack, std::string_view needle) noexcept {
  if (config::use_simd_find(haystack.size())) {
    auto pos = to_sz(haystack).find(to_sz(needle));
    return pos == sz_string_view::npos ? std::string_view::npos : pos;
  }
  return haystack.find(needle);
}

/// Find last occurrence of needle in haystack
[[nodiscard]] inline std::size_t rfind(std::string_view haystack,
                                       std::string_view needle) noexcept {
  if (config::use_simd_find(haystack.size())) {
    auto pos = to_sz(haystack).rfind(to_sz(needle));
    return pos == sz_string_view::npos ? std::string_view::npos : pos;
  }
  return haystack.rfind(needle);
}

/// Check if haystack contains needle
[[nodiscard]] inline bool contains(std::string_view haystack, std::string_view needle) noexcept {
  if (config::use_simd_find(haystack.size())) {
    return to_sz(haystack).contains(to_sz(needle));
  }
  return haystack.find(needle) != std::string_view::npos;
}

/// Check if string starts with prefix
/// Uses std for short prefixes (where SIMD overhead hurts)
[[nodiscard]] inline bool starts_with(std::string_view s, std::string_view prefix) noexcept {
  if (config::use_simd_prefix(s.size())) {
    return to_sz(s).starts_with(to_sz(prefix));
  }
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

/// Check if string ends with suffix
[[nodiscard]] inline bool ends_with(std::string_view s, std::string_view suffix) noexcept {
  if (config::use_simd_prefix(s.size())) {
    return to_sz(s).ends_with(to_sz(suffix));
  }
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

// ─────────────────────────────────────────────────────────────────────────────
// Character set operations (always SIMD - these are hard to beat)
// ─────────────────────────────────────────────────────────────────────────────

/// Find first character from set (SIMD-accelerated)
[[nodiscard]] inline std::size_t find_first_of(std::string_view s,
                                               std::string_view chars) noexcept {
  auto pos = to_sz(s).find_first_of(to_sz(chars));
  return pos == sz_string_view::npos ? std::string_view::npos : pos;
}

/// Find first character not in set (SIMD-accelerated)
[[nodiscard]] inline std::size_t find_first_not_of(std::string_view s,
                                                   std::string_view chars) noexcept {
  auto pos = to_sz(s).find_first_not_of(to_sz(chars));
  return pos == sz_string_view::npos ? std::string_view::npos : pos;
}

/// Find last character from set (SIMD-accelerated)
[[nodiscard]] inline std::size_t find_last_of(std::string_view s, std::string_view chars) noexcept {
  auto pos = to_sz(s).find_last_of(to_sz(chars));
  return pos == sz_string_view::npos ? std::string_view::npos : pos;
}

/// Find last character not in set (SIMD-accelerated)
[[nodiscard]] inline std::size_t find_last_not_of(std::string_view s,
                                                  std::string_view chars) noexcept {
  auto pos = to_sz(s).find_last_not_of(to_sz(chars));
  return pos == sz_string_view::npos ? std::string_view::npos : pos;
}

// ─────────────────────────────────────────────────────────────────────────────
// String splitting
// ─────────────────────────────────────────────────────────────────────────────

/// Split string by delimiter, collecting into vector of string_views
/// Zero-copy - views point into original string
[[nodiscard]] inline std::vector<std::string_view> split_to_views(std::string_view s,
                                                                  std::string_view delimiter) {
  std::vector<std::string_view> result;

  // Empty delimiter: return each character as a separate element
  if (delimiter.empty()) {
    result.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
      result.push_back(s.substr(i, 1));
    }
    if (result.empty()) {
      result.emplace_back();
    }
    return result;
  }

  if (config::use_simd_split(s.size())) {
    auto sz_s = to_sz(s);
    auto sz_delim = to_sz(delimiter);
    for (auto part : sz::split(sz_s, sz_delim)) {
      result.emplace_back(part.data(), part.size());
    }
  } else {
    // std implementation
    std::size_t pos = 0;
    std::size_t prev = 0;
    while ((pos = s.find(delimiter, prev)) != std::string_view::npos) {
      result.push_back(s.substr(prev, pos - prev));
      prev = pos + delimiter.size();
    }
    result.push_back(s.substr(prev));
  }
  return result;
}

/// Split string by delimiter, collecting into vector of strings
[[nodiscard]] inline std::vector<std::string> split_to_strings(std::string_view s,
                                                               std::string_view delimiter) {
  std::vector<std::string> result;

  // Empty delimiter: return each character as a separate element
  if (delimiter.empty()) {
    result.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
      result.emplace_back(1, s[i]);
    }
    if (result.empty()) {
      result.emplace_back();
    }
    return result;
  }

  if (config::use_simd_split(s.size())) {
    auto sz_s = to_sz(s);
    auto sz_delim = to_sz(delimiter);
    for (auto part : sz::split(sz_s, sz_delim)) {
      result.emplace_back(part.data(), part.size());
    }
  } else {
    std::size_t pos = 0;
    std::size_t prev = 0;
    while ((pos = s.find(delimiter, prev)) != std::string_view::npos) {
      result.emplace_back(s.substr(prev, pos - prev));
      prev = pos + delimiter.size();
    }
    result.emplace_back(s.substr(prev));
  }
  return result;
}

/// Tokenize string by separator characters (like nix::tokenizeString)
/// Does NOT preserve empty strings - filters them out
[[nodiscard]] inline std::vector<std::string> tokenize(std::string_view s,
                                                       std::string_view separators) {
  std::vector<std::string> result;

  if (config::use_simd_split(s.size())) {
    auto sz_s = to_sz(s);
    byteset sep_set{separators.data(), separators.size()};
    for (auto part : sz_s.split(sep_set)) {
      if (!part.empty()) {
        result.emplace_back(part.data(), part.size());
      }
    }
  } else {
    // std implementation
    auto pos = s.find_first_not_of(separators, 0);
    while (pos != std::string_view::npos) {
      auto end_pos = s.find_first_of(separators, pos + 1);
      if (end_pos == std::string_view::npos) {
        end_pos = s.size();
      }
      result.emplace_back(s.substr(pos, end_pos - pos));
      pos = s.find_first_not_of(separators, end_pos);
    }
  }
  return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// String trimming
// ─────────────────────────────────────────────────────────────────────────────

/// Default whitespace characters
inline constexpr std::string_view whitespace_chars{" \t\n\r\f\v"};

/// Trim leading whitespace (returns view into original)
[[nodiscard]] inline std::string_view
trim_left(std::string_view s, std::string_view chars = whitespace_chars) noexcept {
  auto pos = find_first_not_of(s, chars);
  if (pos == std::string_view::npos) {
    return std::string_view{s.data() + s.size(), 0};
  }
  return s.substr(pos);
}

/// Trim trailing whitespace (returns view into original)
[[nodiscard]] inline std::string_view
trim_right(std::string_view s, std::string_view chars = whitespace_chars) noexcept {
  auto pos = find_last_not_of(s, chars);
  if (pos == std::string_view::npos) {
    return std::string_view{s.data(), 0};
  }
  return s.substr(0, pos + 1);
}

/// Trim both leading and trailing whitespace (returns view into original)
[[nodiscard]] inline std::string_view trim(std::string_view s,
                                           std::string_view chars = whitespace_chars) noexcept {
  return trim_right(trim_left(s, chars), chars);
}

// ─────────────────────────────────────────────────────────────────────────────
// String replacement
// ─────────────────────────────────────────────────────────────────────────────

/// Replace all occurrences of 'from' with 'to'
[[nodiscard]] inline std::string replace_all(std::string_view s, std::string_view from,
                                             std::string_view to) {
  if (from.empty()) {
    return std::string{s};
  }

  std::string result;
  result.reserve(s.size());

  std::size_t pos = 0;
  std::size_t prev = 0;

  if (config::use_simd_replace(s.size())) {
    auto sz_s = to_sz(s);
    auto sz_from = to_sz(from);
    while ((pos = sz_s.find(sz_from, prev)) != sz_string_view::npos) {
      result.append(s.data() + prev, pos - prev);
      result.append(to);
      prev = pos + from.size();
    }
  } else {
    while ((pos = s.find(from, prev)) != std::string_view::npos) {
      result.append(s.data() + prev, pos - prev);
      result.append(to);
      prev = pos + from.size();
    }
  }

  result.append(s.data() + prev, s.size() - prev);
  return result;
}

/// Replace first occurrence of 'from' with 'to'
[[nodiscard]] inline std::string replace_first(std::string_view s, std::string_view from,
                                               std::string_view to) {
  if (from.empty()) {
    return std::string{s};
  }

  auto pos = find(s, from);
  if (pos == std::string_view::npos) {
    return std::string{s};
  }

  std::string result;
  result.reserve(s.size() - from.size() + to.size());
  result.append(s.data(), pos);
  result.append(to);
  result.append(s.data() + pos + from.size(), s.size() - pos - from.size());
  return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// String joining
// ─────────────────────────────────────────────────────────────────────────────

/// Join strings with separator
template <typename Container>
[[nodiscard]] std::string join(std::string_view separator, const Container& strings) {
  std::size_t total_size = 0;
  bool first = true;
  for (const auto& s : strings) {
    if (!first) {
      total_size += separator.size();
    }
    total_size += std::string_view{s}.size();
    first = false;
  }

  std::string result;
  result.reserve(total_size);
  first = true;
  for (const auto& s : strings) {
    if (!first) {
      result.append(separator);
    }
    result.append(std::string_view{s});
    first = false;
  }
  return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Comparison
// ─────────────────────────────────────────────────────────────────────────────

/// Lexicographic comparison
[[nodiscard]] inline int compare(std::string_view a, std::string_view b) noexcept {
  return a.compare(b);
}

/// Check equality
[[nodiscard]] inline bool equal(std::string_view a, std::string_view b) noexcept {
  return a == b;
}

// ─────────────────────────────────────────────────────────────────────────────
// Direct SIMD access (for when you know you want SIMD)
//
// Use these when you've already determined the string is large enough,
// or when benchmarks show SIMD is always better for your use case.
// ─────────────────────────────────────────────────────────────────────────────

namespace simd {

/// Direct SIMD find (always uses stringzilla)
[[nodiscard]] inline std::size_t find(std::string_view haystack, std::string_view needle) noexcept {
  auto pos = to_sz(haystack).find(to_sz(needle));
  return pos == sz_string_view::npos ? std::string_view::npos : pos;
}

/// Direct SIMD contains (always uses stringzilla)
[[nodiscard]] inline bool contains(std::string_view haystack, std::string_view needle) noexcept {
  return to_sz(haystack).contains(to_sz(needle));
}

/// Direct SIMD replace_all (always uses stringzilla for finding)
[[nodiscard]] inline std::string replace_all(std::string_view s, std::string_view from,
                                             std::string_view to) {
  if (from.empty()) {
    return std::string{s};
  }

  std::string result;
  result.reserve(s.size());

  auto sz_s = to_sz(s);
  auto sz_from = to_sz(from);
  std::size_t prev = 0;
  std::size_t pos = 0;

  while ((pos = sz_s.find(sz_from, prev)) != sz_string_view::npos) {
    result.append(s.data() + prev, pos - prev);
    result.append(to);
    prev = pos + from.size();
  }

  result.append(s.data() + prev, s.size() - prev);
  return result;
}

} // namespace simd

} // namespace straylight::nix::primitives
