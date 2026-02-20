// straylight::nix::adapters::nix_strings_adapter
//
// Compatibility adapter that allows Nix code to use straylight::nix::text
// string operations while maintaining API compatibility with existing Nix code.
//
// Migration strategy:
// 1. Include this header instead of (or in addition to) nix/util/strings.h
// 2. Use `using namespace straylight::nix::adapters;` in .cpp files
// 3. Gradually migrate call sites to use primitives directly
//
// This adapter provides:
// - Drop-in replacements for nix::tokenizeString, nix::splitString, nix::concatStringsSep
// - Drop-in replacements for nix::hasPrefix, nix::hasSuffix (from util.h)
// - Drop-in replacements for nix::trim, nix::chomp, nix::replaceStrings (from util.h)
//
// All functions delegate to straylight::nix::primitives for SIMD acceleration.

#pragma once

#include <list>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// TODO[b7r6]: consider absl::InlinedVector if perf regresses

#include "straylight/nix/text/strings.h"

namespace straylight::nix::adapters {

// ─────────────────────────────────────────────────────────────────────────────
// Prefix/Suffix checks (nix::hasPrefix, nix::hasSuffix from util.h)
//
// Nix API:
//   bool hasPrefix(std::string_view s, std::string_view prefix);
//   bool hasSuffix(std::string_view s, std::string_view suffix);
//
// Primitives API:
//   bool starts_with(std::string_view s, std::string_view prefix);
//   bool ends_with(std::string_view s, std::string_view suffix);
// ─────────────────────────────────────────────────────────────────────────────

/// Check if string starts with prefix (compatible with nix::hasPrefix)
[[nodiscard]] inline bool hasPrefix(std::string_view s, std::string_view prefix) noexcept {
  return straylight::nix::text::starts_with(s, prefix);
}

/// Check if string ends with suffix (compatible with nix::hasSuffix)
[[nodiscard]] inline bool hasSuffix(std::string_view s, std::string_view suffix) noexcept {
  return straylight::nix::text::ends_with(s, suffix);
}

// ─────────────────────────────────────────────────────────────────────────────
// Trim functions (nix::trim, nix::chomp from util.h)
//
// Nix API:
//   std::string trim(std::string_view s, std::string_view whitespace = " \n\r\t");
//   std::string chomp(std::string_view s);  // trailing only
//
// Primitives API:
//   std::string_view trim(std::string_view s, std::string_view chars);
//   std::string_view trim_left(std::string_view s, std::string_view chars);
//   std::string_view trim_right(std::string_view s, std::string_view chars);
//
// Note: Nix returns std::string, primitives return std::string_view.
// The adapter allocates to maintain compatibility.
// ─────────────────────────────────────────────────────────────────────────────

/// Nix default whitespace (slightly different from primitives default)
inline constexpr std::string_view nix_whitespace{" \n\r\t"};

/// Remove whitespace from start and end (compatible with nix::trim)
/// Note: Returns std::string for Nix compatibility; use text::trim directly
/// for zero-copy when possible.
[[nodiscard]] inline std::string trim(std::string_view s,
                                      std::string_view whitespace = nix_whitespace) {
  auto trimmed = straylight::nix::text::trim(s, whitespace);
  return std::string{trimmed};
}

/// Remove trailing whitespace (compatible with nix::chomp)
/// Note: Nix chomp uses " \n\r\t" as whitespace
[[nodiscard]] inline std::string chomp(std::string_view s) {
  auto trimmed = straylight::nix::text::trim_right(s, nix_whitespace);
  return std::string{trimmed};
}

// ─────────────────────────────────────────────────────────────────────────────
// String replacement (nix::replaceStrings from util.h)
//
// Nix API:
//   std::string replaceStrings(std::string s, std::string_view from, std::string_view to);
//
// Primitives API:
//   std::string replace_all(std::string_view s, std::string_view from, std::string_view to);
// ─────────────────────────────────────────────────────────────────────────────

/// Replace all occurrences (compatible with nix::replaceStrings)
/// Note: Accepts std::string_view but std::string also works via implicit conversion
[[nodiscard]] inline std::string replaceStrings(std::string_view s, std::string_view from,
                                                std::string_view to) {
  return straylight::nix::text::replace_all(s, from, to);
}

// ─────────────────────────────────────────────────────────────────────────────
// Tokenize (nix::tokenizeString from strings.h)
//
// Nix API:
//   template<class C>
//   C tokenizeString(std::string_view s, std::string_view separators = " \t\n\r");
//
// Primitives API:
//   std::vector<std::string> tokenize(std::string_view s, std::string_view separators);
//
// Note: Nix tokenize filters out empty strings (same as primitives).
// Nix uses character-set separators (each char is a separator).
// ─────────────────────────────────────────────────────────────────────────────

/// Default separators for tokenize (matches Nix default)
inline constexpr std::string_view default_separators{" \t\n\r"};

/// Tokenize string into vector (compatible with nix::tokenizeString<std::vector<std::string>>)
template <typename C = std::vector<std::string>>
[[nodiscard]] C tokenizeString(std::string_view s,
                               std::string_view separators = default_separators) {
  auto tokens = straylight::nix::text::tokenize(s, separators);

  // Convert to requested container type
  if constexpr (std::is_same_v<C, std::vector<std::string>>) {
    return tokens;
  } else {
    C result;
    for (auto& token : tokens) {
      if constexpr (requires { result.insert(result.end(), std::move(token)); }) {
        result.insert(result.end(), std::move(token));
      } else {
        result.push_back(std::move(token));
      }
    }
    return result;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Split (nix::splitString from strings.h)
//
// Nix API:
//   template<class C>
//   C splitString(std::string_view s, std::string_view separators);
//
// Primitives API:
//   std::vector<std::string> split_to_strings(std::string_view s, std::string_view delimiter);
//
// KEY DIFFERENCE: Nix splitString treats `separators` as a CHARACTER SET
// (each character is a separator), while primitives split_to_strings treats
// `delimiter` as a single DELIMITER STRING.
//
// We implement the Nix character-set semantics here for compatibility.
// ─────────────────────────────────────────────────────────────────────────────

/// Split by character set, preserving empty strings (compatible with nix::splitString)
template <typename C = std::vector<std::string>>
[[nodiscard]] C splitString(std::string_view s, std::string_view separators) {
  C result;
  std::size_t pos = 0;

  while (pos <= s.size()) {
    auto end = straylight::nix::text::find_first_of(s.substr(pos), separators);
    if (end == std::string_view::npos) {
      end = s.size() - pos;
    }

    std::string part{s.substr(pos, end)};
    if constexpr (requires { result.insert(result.end(), std::move(part)); }) {
      result.insert(result.end(), std::move(part));
    } else {
      result.push_back(std::move(part));
    }

    pos = pos + end + 1;
  }

  return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Concat/Join (nix::concatStringsSep from strings.h)
//
// Nix API:
//   template<class C>
//   std::string concatStringsSep(std::string_view sep, const C& ss);
//
// Primitives API:
//   template<typename Container>
//   std::string join(std::string_view separator, const Container& strings);
// ─────────────────────────────────────────────────────────────────────────────

/// Concatenate strings with separator (compatible with nix::concatStringsSep)
template <typename C>
[[nodiscard]] std::string concatStringsSep(std::string_view sep, const C& ss) {
  return straylight::nix::text::join(sep, ss);
}

/// Apply function and concatenate (compatible with nix::concatMapStringsSep)
template <typename C, typename F>
[[nodiscard]] std::string concatMapStringsSep(std::string_view separator, const C& iterable, F fn) {
  std::vector<std::string> strings; // TODO[b7r6]: consider absl::InlinedVector<std::string, 64>
  strings.reserve(iterable.size());
  for (const auto& elem : iterable) {
    strings.push_back(fn(elem));
  }
  return straylight::nix::text::join(separator, strings);
}

/// Deprecated: concat with empty string dropping (compatible with
/// nix::dropEmptyInitThenConcatStringsSep)
template <typename C>
[[deprecated("Use concatStringsSep instead")]] [[nodiscard]] std::string
dropEmptyInitThenConcatStringsSep(std::string_view sep, const C& ss) {
  std::size_t total_size = 0;
  for (const auto& s : ss) {
    total_size += sep.size() + std::string_view(s).size();
  }

  std::string result;
  result.reserve(total_size);
  for (const auto& s : ss) {
    if (!result.empty()) {
      result.append(sep);
    }
    result.append(std::string_view(s));
  }
  return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Additional utilities from primitives (not in original Nix API)
// These are exposed for convenience when migrating code.
// ─────────────────────────────────────────────────────────────────────────────

/// Check if string contains substring (not in Nix API, but commonly needed)
[[nodiscard]] inline bool contains(std::string_view haystack, std::string_view needle) noexcept {
  return straylight::nix::text::contains(haystack, needle);
}

/// Find substring position
[[nodiscard]] inline std::size_t find(std::string_view haystack, std::string_view needle) noexcept {
  return straylight::nix::text::find(haystack, needle);
}

[[nodiscard]] inline std::size_t rfind(std::string_view haystack,
                                       std::string_view needle) noexcept {
  return straylight::nix::text::rfind(haystack, needle);
}

/// Character set search
[[nodiscard]] inline std::size_t find_first_of(std::string_view s,
                                               std::string_view chars) noexcept {
  return straylight::nix::text::find_first_of(s, chars);
}

[[nodiscard]] inline std::size_t find_first_not_of(std::string_view s,
                                                   std::string_view chars) noexcept {
  return straylight::nix::text::find_first_not_of(s, chars);
}

[[nodiscard]] inline std::size_t find_last_of(std::string_view s, std::string_view chars) noexcept {
  return straylight::nix::text::find_last_of(s, chars);
}

[[nodiscard]] inline std::size_t find_last_not_of(std::string_view s,
                                                  std::string_view chars) noexcept {
  return straylight::nix::text::find_last_not_of(s, chars);
}

/// Zero-copy trim (returns view, not in Nix API)
/// Use these for better performance when you don't need std::string
[[nodiscard]] inline std::string_view trim_view(std::string_view s,
                                                std::string_view whitespace = nix_whitespace) {
  return straylight::nix::text::trim(s, whitespace);
}

[[nodiscard]] inline std::string_view trim_left_view(std::string_view s,
                                                     std::string_view whitespace = nix_whitespace) {
  return straylight::nix::text::trim_left(s, whitespace);
}

[[nodiscard]] inline std::string_view
trim_right_view(std::string_view s, std::string_view whitespace = nix_whitespace) {
  return straylight::nix::text::trim_right(s, whitespace);
}

/// Zero-copy split (returns views, not in Nix API)
/// Use this for better performance when you don't need owned strings
[[nodiscard]] inline std::vector<std::string_view> split_to_views(std::string_view s,
                                                                  std::string_view delimiter) {
  return straylight::nix::text::split_to_views(s, delimiter);
}

/// Replace first occurrence only (not in Nix API)
[[nodiscard]] inline std::string replace_first(std::string_view s, std::string_view from,
                                               std::string_view to) {
  return straylight::nix::text::replace_first(s, from, to);
}

} // namespace straylight::nix::adapters

// ─────────────────────────────────────────────────────────────────────────────
// Convenience namespace alias for migration
// ─────────────────────────────────────────────────────────────────────────────

namespace straylight::nix::adapters {
namespace strings = straylight::nix::adapters;
}
