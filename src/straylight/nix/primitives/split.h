// straylight::nix::primitives::split - String splitting primitives
//
// Modern C++23 string splitting utilities replacing nix/util/split.h.
// Provides both eager (pair-returning) and lazy (ranges-based) splitting:
//   - split_prefix    - strip a known prefix, return remaining
//   - split_suffix    - strip a known suffix, return remaining
//   - split_first     - split on first occurrence of delimiter
//   - split_last      - split on last occurrence of delimiter
//   - split_view      - lazy range-based splitting

#pragma once

#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

namespace straylight::nix::primitives {

// ─────────────────────────────────────────────────────────────────────────────
// split_prefix - strip a known prefix from a string
// ─────────────────────────────────────────────────────────────────────────────

/// Strip a prefix from a string_view if present.
///
/// Returns the remainder of the string after the prefix, or nullopt
/// if the string doesn't start with the prefix.
///
/// Usage:
///   auto rest = split_prefix("hello world", "hello ");
///   // rest == "world"
[[nodiscard]] constexpr std::optional<std::string_view>
split_prefix(std::string_view input, std::string_view prefix) noexcept {
  if (input.starts_with(prefix)) {
    return input.substr(prefix.size());
  }
  return std::nullopt;
}

/// Strip a single character prefix from a string_view if present.
[[nodiscard]] constexpr std::optional<std::string_view> split_prefix(std::string_view input,
                                                                     char prefix) noexcept {
  if (!input.empty() && input.front() == prefix) {
    return input.substr(1);
  }
  return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// split_suffix - strip a known suffix from a string
// ─────────────────────────────────────────────────────────────────────────────

/// Strip a suffix from a string_view if present.
///
/// Returns the head of the string before the suffix, or nullopt
/// if the string doesn't end with the suffix.
///
/// Usage:
///   auto head = split_suffix("hello.txt", ".txt");
///   // head == "hello"
[[nodiscard]] constexpr std::optional<std::string_view>
split_suffix(std::string_view input, std::string_view suffix) noexcept {
  if (input.ends_with(suffix)) {
    return input.substr(0, input.size() - suffix.size());
  }
  return std::nullopt;
}

/// Strip a single character suffix from a string_view if present.
[[nodiscard]] constexpr std::optional<std::string_view> split_suffix(std::string_view input,
                                                                     char suffix) noexcept {
  if (!input.empty() && input.back() == suffix) {
    return input.substr(0, input.size() - 1);
  }
  return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// split_first - split on first occurrence of delimiter
// ─────────────────────────────────────────────────────────────────────────────

/// Split on the first occurrence of a delimiter character.
///
/// Returns a pair of (prefix, rest) where prefix is everything before
/// the first delimiter and rest is everything after. The delimiter
/// itself is not included in either part.
///
/// Returns nullopt if the delimiter is not found.
///
/// Usage:
///   auto [prefix, rest] = *split_first("foo:bar:baz", ':');
///   // prefix == "foo", rest == "bar:baz"
[[nodiscard]] constexpr std::optional<std::pair<std::string_view, std::string_view>>
split_first(std::string_view input, char delimiter) noexcept {
  auto position = input.find(delimiter);
  if (position == std::string_view::npos) {
    return std::nullopt;
  }
  return std::pair{input.substr(0, position), input.substr(position + 1)};
}

/// Split on the first occurrence of a delimiter string.
///
/// Returns a pair of (prefix, rest) where prefix is everything before
/// the first delimiter and rest is everything after. The delimiter
/// itself is not included in either part.
///
/// Returns nullopt if the delimiter is not found.
[[nodiscard]] constexpr std::optional<std::pair<std::string_view, std::string_view>>
split_first(std::string_view input, std::string_view delimiter) noexcept {
  auto position = input.find(delimiter);
  if (position == std::string_view::npos) {
    return std::nullopt;
  }
  return std::pair{input.substr(0, position), input.substr(position + delimiter.size())};
}

// ─────────────────────────────────────────────────────────────────────────────
// split_last - split on last occurrence of delimiter
// ─────────────────────────────────────────────────────────────────────────────

/// Split on the last occurrence of a delimiter character.
///
/// Returns a pair of (head, suffix) where head is everything before
/// the last delimiter and suffix is everything after. The delimiter
/// itself is not included in either part.
///
/// Returns nullopt if the delimiter is not found.
///
/// Usage:
///   auto [head, suffix] = *split_last("foo:bar:baz", ':');
///   // head == "foo:bar", suffix == "baz"
[[nodiscard]] constexpr std::optional<std::pair<std::string_view, std::string_view>>
split_last(std::string_view input, char delimiter) noexcept {
  auto position = input.rfind(delimiter);
  if (position == std::string_view::npos) {
    return std::nullopt;
  }
  return std::pair{input.substr(0, position), input.substr(position + 1)};
}

/// Split on the last occurrence of a delimiter string.
///
/// Returns a pair of (head, suffix) where head is everything before
/// the last delimiter and suffix is everything after. The delimiter
/// itself is not included in either part.
///
/// Returns nullopt if the delimiter is not found.
[[nodiscard]] constexpr std::optional<std::pair<std::string_view, std::string_view>>
split_last(std::string_view input, std::string_view delimiter) noexcept {
  auto position = input.rfind(delimiter);
  if (position == std::string_view::npos) {
    return std::nullopt;
  }
  return std::pair{input.substr(0, position), input.substr(position + delimiter.size())};
}

// ─────────────────────────────────────────────────────────────────────────────
// split_first_to / split_last_to - mutating versions
// ─────────────────────────────────────────────────────────────────────────────

/// Split on first occurrence, modifying the input to contain the rest.
///
/// If the delimiter is found, returns the prefix and modifies the input
/// to contain only the part after the delimiter. Otherwise, returns
/// nullopt and leaves the input unchanged.
///
/// This is useful for iterative parsing:
///   while (auto part = split_first_to(remaining, ':')) {
///     process(*part);
///   }
///   process(remaining); // Handle the last part
[[nodiscard]] constexpr std::optional<std::string_view> split_first_to(std::string_view& input,
                                                                       char delimiter) noexcept {
  auto position = input.find(delimiter);
  if (position == std::string_view::npos) {
    return std::nullopt;
  }
  auto prefix = input.substr(0, position);
  input.remove_prefix(position + 1);
  return prefix;
}

/// Split on first occurrence of a string delimiter, modifying the input.
[[nodiscard]] constexpr std::optional<std::string_view>
split_first_to(std::string_view& input, std::string_view delimiter) noexcept {
  auto position = input.find(delimiter);
  if (position == std::string_view::npos) {
    return std::nullopt;
  }
  auto prefix = input.substr(0, position);
  input.remove_prefix(position + delimiter.size());
  return prefix;
}

/// Split on last occurrence, modifying the input to contain the head.
///
/// If the delimiter is found, returns the suffix and modifies the input
/// to contain only the part before the delimiter. Otherwise, returns
/// nullopt and leaves the input unchanged.
[[nodiscard]] constexpr std::optional<std::string_view> split_last_to(std::string_view& input,
                                                                      char delimiter) noexcept {
  auto position = input.rfind(delimiter);
  if (position == std::string_view::npos) {
    return std::nullopt;
  }
  auto suffix = input.substr(position + 1);
  input.remove_suffix(input.size() - position);
  return suffix;
}

/// Split on last occurrence of a string delimiter, modifying the input.
[[nodiscard]] constexpr std::optional<std::string_view>
split_last_to(std::string_view& input, std::string_view delimiter) noexcept {
  auto position = input.rfind(delimiter);
  if (position == std::string_view::npos) {
    return std::nullopt;
  }
  auto suffix = input.substr(position + delimiter.size());
  input.remove_suffix(input.size() - position);
  return suffix;
}

// ─────────────────────────────────────────────────────────────────────────────
// strip_prefix / strip_suffix - mutating prefix/suffix removal
// ─────────────────────────────────────────────────────────────────────────────

/// Strip a known prefix from the input, modifying it in place.
///
/// Returns true if the prefix was found and removed, false otherwise.
/// The input is unchanged if the prefix is not found.
[[nodiscard]] constexpr bool strip_prefix(std::string_view& input,
                                          std::string_view prefix) noexcept {
  if (input.starts_with(prefix)) {
    input.remove_prefix(prefix.size());
    return true;
  }
  return false;
}

/// Strip a known character prefix from the input, modifying it in place.
[[nodiscard]] constexpr bool strip_prefix(std::string_view& input, char prefix) noexcept {
  if (!input.empty() && input.front() == prefix) {
    input.remove_prefix(1);
    return true;
  }
  return false;
}

/// Strip a known suffix from the input, modifying it in place.
///
/// Returns true if the suffix was found and removed, false otherwise.
/// The input is unchanged if the suffix is not found.
[[nodiscard]] constexpr bool strip_suffix(std::string_view& input,
                                          std::string_view suffix) noexcept {
  if (input.ends_with(suffix)) {
    input.remove_suffix(suffix.size());
    return true;
  }
  return false;
}

/// Strip a known character suffix from the input, modifying it in place.
[[nodiscard]] constexpr bool strip_suffix(std::string_view& input, char suffix) noexcept {
  if (!input.empty() && input.back() == suffix) {
    input.remove_suffix(1);
    return true;
  }
  return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// split_view - lazy range-based splitting (C++23)
// ─────────────────────────────────────────────────────────────────────────────

/// Create a lazy view that splits the input on each occurrence of delimiter.
///
/// Returns a range of string_views, one for each segment between delimiters.
/// Empty segments are preserved.
///
/// Usage:
///   for (auto part : split_view("a:b:c", ':')) {
///     process(part);
///   }
[[nodiscard]] constexpr auto split_view(std::string_view input, char delimiter) noexcept {
  return input | std::views::split(delimiter) |
         std::views::transform([](auto&& range) { return std::string_view(range); });
}

/// Create a lazy view that splits the input on each occurrence of a string delimiter.
[[nodiscard]] constexpr auto split_view(std::string_view input,
                                        std::string_view delimiter) noexcept {
  return input | std::views::split(delimiter) |
         std::views::transform([](auto&& range) { return std::string_view(range); });
}

// ─────────────────────────────────────────────────────────────────────────────
// split_lines - lazy line-based splitting
// ─────────────────────────────────────────────────────────────────────────────

/// Create a lazy view that splits the input into lines.
///
/// Splits on '\n'. Does not handle '\r\n' specially - use a trim
/// if needed. Empty lines are preserved.
[[nodiscard]] constexpr auto split_lines(std::string_view input) noexcept {
  return split_view(input, '\n');
}

} // namespace straylight::nix::primitives
