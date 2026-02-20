// straylight::nix::text::split tests
//
// Tests for string splitting primitives: split_prefix, split_suffix,
// split_first, split_last, strip_prefix, strip_suffix, split_view

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "straylight/nix/text/split.h"

namespace split = straylight::nix::text;

// ─────────────────────────────────────────────────────────────────────────────
// split_prefix - string_view prefix
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("split_prefix with matching string prefix", "[split][split_prefix]") {
  auto result = split::split_prefix("hello world", "hello ");
  REQUIRE(result.has_value());
  REQUIRE(*result == "world");
}

TEST_CASE("split_prefix with non-matching string prefix", "[split][split_prefix]") {
  auto result = split::split_prefix("hello world", "goodbye ");
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("split_prefix with empty prefix matches everything", "[split][split_prefix]") {
  auto result = split::split_prefix("hello", "");
  REQUIRE(result.has_value());
  REQUIRE(*result == "hello");
}

TEST_CASE("split_prefix with empty input and empty prefix", "[split][split_prefix]") {
  auto result = split::split_prefix("", "");
  REQUIRE(result.has_value());
  REQUIRE(*result == "");
}

TEST_CASE("split_prefix with empty input and non-empty prefix", "[split][split_prefix]") {
  auto result = split::split_prefix("", "prefix");
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("split_prefix with prefix equal to input", "[split][split_prefix]") {
  auto result = split::split_prefix("exact", "exact");
  REQUIRE(result.has_value());
  REQUIRE(*result == "");
}

TEST_CASE("split_prefix with prefix longer than input", "[split][split_prefix]") {
  auto result = split::split_prefix("short", "shorterlonger");
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("split_prefix with character prefix matching", "[split][split_prefix]") {
  auto result = split::split_prefix("/path/to/file", '/');
  REQUIRE(result.has_value());
  REQUIRE(*result == "path/to/file");
}

TEST_CASE("split_prefix with character prefix not matching", "[split][split_prefix]") {
  auto result = split::split_prefix("path/to/file", '/');
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("split_prefix with character on empty input", "[split][split_prefix]") {
  auto result = split::split_prefix("", '/');
  REQUIRE_FALSE(result.has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// split_suffix - string_view suffix
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("split_suffix with matching string suffix", "[split][split_suffix]") {
  auto result = split::split_suffix("hello.txt", ".txt");
  REQUIRE(result.has_value());
  REQUIRE(*result == "hello");
}

TEST_CASE("split_suffix with non-matching string suffix", "[split][split_suffix]") {
  auto result = split::split_suffix("hello.txt", ".md");
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("split_suffix with empty suffix matches everything", "[split][split_suffix]") {
  auto result = split::split_suffix("hello", "");
  REQUIRE(result.has_value());
  REQUIRE(*result == "hello");
}

TEST_CASE("split_suffix with empty input and empty suffix", "[split][split_suffix]") {
  auto result = split::split_suffix("", "");
  REQUIRE(result.has_value());
  REQUIRE(*result == "");
}

TEST_CASE("split_suffix with empty input and non-empty suffix", "[split][split_suffix]") {
  auto result = split::split_suffix("", "suffix");
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("split_suffix with suffix equal to input", "[split][split_suffix]") {
  auto result = split::split_suffix("exact", "exact");
  REQUIRE(result.has_value());
  REQUIRE(*result == "");
}

TEST_CASE("split_suffix with suffix longer than input", "[split][split_suffix]") {
  auto result = split::split_suffix("short", "shorterlonger");
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("split_suffix with character suffix matching", "[split][split_suffix]") {
  auto result = split::split_suffix("path/", '/');
  REQUIRE(result.has_value());
  REQUIRE(*result == "path");
}

TEST_CASE("split_suffix with character suffix not matching", "[split][split_suffix]") {
  auto result = split::split_suffix("path", '/');
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("split_suffix with character on empty input", "[split][split_suffix]") {
  auto result = split::split_suffix("", '/');
  REQUIRE_FALSE(result.has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// split_first - split on first occurrence
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("split_first with single delimiter", "[split][split_first]") {
  auto result = split::split_first("foo:bar", ':');
  REQUIRE(result.has_value());
  auto [prefix, rest] = *result;
  REQUIRE(prefix == "foo");
  REQUIRE(rest == "bar");
}

TEST_CASE("split_first with multiple delimiters", "[split][split_first]") {
  auto result = split::split_first("foo:bar:baz", ':');
  REQUIRE(result.has_value());
  auto [prefix, rest] = *result;
  REQUIRE(prefix == "foo");
  REQUIRE(rest == "bar:baz");
}

TEST_CASE("split_first with no delimiter", "[split][split_first]") {
  auto result = split::split_first("no separator here", ':');
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("split_first with delimiter at start", "[split][split_first]") {
  auto result = split::split_first(":rest", ':');
  REQUIRE(result.has_value());
  auto [prefix, rest] = *result;
  REQUIRE(prefix == "");
  REQUIRE(rest == "rest");
}

TEST_CASE("split_first with delimiter at end", "[split][split_first]") {
  auto result = split::split_first("prefix:", ':');
  REQUIRE(result.has_value());
  auto [prefix, rest] = *result;
  REQUIRE(prefix == "prefix");
  REQUIRE(rest == "");
}

TEST_CASE("split_first with empty input", "[split][split_first]") {
  auto result = split::split_first("", ':');
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("split_first with string delimiter", "[split][split_first]") {
  auto result = split::split_first("foo::bar::baz", "::");
  REQUIRE(result.has_value());
  auto [prefix, rest] = *result;
  REQUIRE(prefix == "foo");
  REQUIRE(rest == "bar::baz");
}

TEST_CASE("split_first with empty string delimiter", "[split][split_first]") {
  auto result = split::split_first("hello", std::string_view{});
  REQUIRE(result.has_value());
  auto [prefix, rest] = *result;
  REQUIRE(prefix == "");
  REQUIRE(rest == "hello");
}

// ─────────────────────────────────────────────────────────────────────────────
// split_last - split on last occurrence
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("split_last with single delimiter", "[split][split_last]") {
  auto result = split::split_last("foo:bar", ':');
  REQUIRE(result.has_value());
  auto [head, suffix] = *result;
  REQUIRE(head == "foo");
  REQUIRE(suffix == "bar");
}

TEST_CASE("split_last with multiple delimiters", "[split][split_last]") {
  auto result = split::split_last("foo:bar:baz", ':');
  REQUIRE(result.has_value());
  auto [head, suffix] = *result;
  REQUIRE(head == "foo:bar");
  REQUIRE(suffix == "baz");
}

TEST_CASE("split_last with no delimiter", "[split][split_last]") {
  auto result = split::split_last("no separator here", ':');
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("split_last with delimiter at start", "[split][split_last]") {
  auto result = split::split_last(":rest", ':');
  REQUIRE(result.has_value());
  auto [head, suffix] = *result;
  REQUIRE(head == "");
  REQUIRE(suffix == "rest");
}

TEST_CASE("split_last with delimiter at end", "[split][split_last]") {
  auto result = split::split_last("prefix:", ':');
  REQUIRE(result.has_value());
  auto [head, suffix] = *result;
  REQUIRE(head == "prefix");
  REQUIRE(suffix == "");
}

TEST_CASE("split_last with empty input", "[split][split_last]") {
  auto result = split::split_last("", ':');
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("split_last with string delimiter", "[split][split_last]") {
  auto result = split::split_last("foo::bar::baz", "::");
  REQUIRE(result.has_value());
  auto [head, suffix] = *result;
  REQUIRE(head == "foo::bar");
  REQUIRE(suffix == "baz");
}

// ─────────────────────────────────────────────────────────────────────────────
// split_first_to - mutating split on first occurrence
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("split_first_to basic usage", "[split][split_first_to]") {
  std::string_view input = "foo:bar:baz";
  auto prefix = split::split_first_to(input, ':');
  REQUIRE(prefix.has_value());
  REQUIRE(*prefix == "foo");
  REQUIRE(input == "bar:baz");
}

TEST_CASE("split_first_to chained calls", "[split][split_first_to]") {
  std::string_view input = "a:b:c";

  auto first = split::split_first_to(input, ':');
  REQUIRE(first.has_value());
  REQUIRE(*first == "a");
  REQUIRE(input == "b:c");

  auto second = split::split_first_to(input, ':');
  REQUIRE(second.has_value());
  REQUIRE(*second == "b");
  REQUIRE(input == "c");

  auto third = split::split_first_to(input, ':');
  REQUIRE_FALSE(third.has_value());
  REQUIRE(input == "c");
}

TEST_CASE("split_first_to with no delimiter leaves input unchanged", "[split][split_first_to]") {
  std::string_view input = "noseparator";
  auto result = split::split_first_to(input, ':');
  REQUIRE_FALSE(result.has_value());
  REQUIRE(input == "noseparator");
}

TEST_CASE("split_first_to with string delimiter", "[split][split_first_to]") {
  std::string_view input = "foo::bar::baz";
  auto prefix = split::split_first_to(input, std::string_view{"::"});
  REQUIRE(prefix.has_value());
  REQUIRE(*prefix == "foo");
  REQUIRE(input == "bar::baz");
}

TEST_CASE("split_first_to with empty prefix result", "[split][split_first_to]") {
  std::string_view input = ":rest";
  auto prefix = split::split_first_to(input, ':');
  REQUIRE(prefix.has_value());
  REQUIRE(*prefix == "");
  REQUIRE(input == "rest");
}

TEST_CASE("split_first_to with empty suffix result", "[split][split_first_to]") {
  std::string_view input = "prefix:";
  auto prefix = split::split_first_to(input, ':');
  REQUIRE(prefix.has_value());
  REQUIRE(*prefix == "prefix");
  REQUIRE(input == "");
}

// ─────────────────────────────────────────────────────────────────────────────
// split_last_to - mutating split on last occurrence
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("split_last_to basic usage", "[split][split_last_to]") {
  std::string_view input = "foo:bar:baz";
  auto suffix = split::split_last_to(input, ':');
  REQUIRE(suffix.has_value());
  REQUIRE(*suffix == "baz");
  REQUIRE(input == "foo:bar");
}

TEST_CASE("split_last_to chained calls", "[split][split_last_to]") {
  std::string_view input = "a:b:c";

  auto first = split::split_last_to(input, ':');
  REQUIRE(first.has_value());
  REQUIRE(*first == "c");
  REQUIRE(input == "a:b");

  auto second = split::split_last_to(input, ':');
  REQUIRE(second.has_value());
  REQUIRE(*second == "b");
  REQUIRE(input == "a");

  auto third = split::split_last_to(input, ':');
  REQUIRE_FALSE(third.has_value());
  REQUIRE(input == "a");
}

TEST_CASE("split_last_to with no delimiter leaves input unchanged", "[split][split_last_to]") {
  std::string_view input = "noseparator";
  auto result = split::split_last_to(input, ':');
  REQUIRE_FALSE(result.has_value());
  REQUIRE(input == "noseparator");
}

TEST_CASE("split_last_to with string delimiter", "[split][split_last_to]") {
  std::string_view input = "foo::bar::baz";
  auto suffix = split::split_last_to(input, std::string_view{"::"});
  REQUIRE(suffix.has_value());
  REQUIRE(*suffix == "baz");
  REQUIRE(input == "foo::bar");
}

// ─────────────────────────────────────────────────────────────────────────────
// strip_prefix - mutating prefix removal
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("strip_prefix with matching prefix", "[split][strip_prefix]") {
  std::string_view input = "prefix:suffix";
  bool result = split::strip_prefix(input, "prefix:");
  REQUIRE(result);
  REQUIRE(input == "suffix");
}

TEST_CASE("strip_prefix with non-matching prefix", "[split][strip_prefix]") {
  std::string_view input = "other:suffix";
  bool result = split::strip_prefix(input, "prefix:");
  REQUIRE_FALSE(result);
  REQUIRE(input == "other:suffix");
}

TEST_CASE("strip_prefix with empty prefix", "[split][strip_prefix]") {
  std::string_view input = "anything";
  bool result = split::strip_prefix(input, "");
  REQUIRE(result);
  REQUIRE(input == "anything");
}

TEST_CASE("strip_prefix with character prefix", "[split][strip_prefix]") {
  std::string_view input = "/path";
  bool result = split::strip_prefix(input, '/');
  REQUIRE(result);
  REQUIRE(input == "path");
}

TEST_CASE("strip_prefix with non-matching character prefix", "[split][strip_prefix]") {
  std::string_view input = "path";
  bool result = split::strip_prefix(input, '/');
  REQUIRE_FALSE(result);
  REQUIRE(input == "path");
}

// ─────────────────────────────────────────────────────────────────────────────
// strip_suffix - mutating suffix removal
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("strip_suffix with matching suffix", "[split][strip_suffix]") {
  std::string_view input = "hello.txt";
  bool result = split::strip_suffix(input, ".txt");
  REQUIRE(result);
  REQUIRE(input == "hello");
}

TEST_CASE("strip_suffix with non-matching suffix", "[split][strip_suffix]") {
  std::string_view input = "hello.txt";
  bool result = split::strip_suffix(input, ".md");
  REQUIRE_FALSE(result);
  REQUIRE(input == "hello.txt");
}

TEST_CASE("strip_suffix with empty suffix", "[split][strip_suffix]") {
  std::string_view input = "anything";
  bool result = split::strip_suffix(input, "");
  REQUIRE(result);
  REQUIRE(input == "anything");
}

TEST_CASE("strip_suffix with character suffix", "[split][strip_suffix]") {
  std::string_view input = "path/";
  bool result = split::strip_suffix(input, '/');
  REQUIRE(result);
  REQUIRE(input == "path");
}

TEST_CASE("strip_suffix with non-matching character suffix", "[split][strip_suffix]") {
  std::string_view input = "path";
  bool result = split::strip_suffix(input, '/');
  REQUIRE_FALSE(result);
  REQUIRE(input == "path");
}

// ─────────────────────────────────────────────────────────────────────────────
// split_view - lazy range-based splitting
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("split_view basic usage", "[split][split_view]") {
  std::vector<std::string_view> parts;
  for (auto part : split::split_view("a:b:c", ':')) {
    parts.push_back(part);
  }
  REQUIRE(parts.size() == 3);
  REQUIRE(parts[0] == "a");
  REQUIRE(parts[1] == "b");
  REQUIRE(parts[2] == "c");
}

TEST_CASE("split_view with empty parts", "[split][split_view]") {
  std::vector<std::string_view> parts;
  for (auto part : split::split_view(":a::b:", ':')) {
    parts.push_back(part);
  }
  REQUIRE(parts.size() == 5);
  REQUIRE(parts[0] == "");
  REQUIRE(parts[1] == "a");
  REQUIRE(parts[2] == "");
  REQUIRE(parts[3] == "b");
  REQUIRE(parts[4] == "");
}

TEST_CASE("split_view with no delimiter", "[split][split_view]") {
  std::vector<std::string_view> parts;
  for (auto part : split::split_view("noseparator", ':')) {
    parts.push_back(part);
  }
  REQUIRE(parts.size() == 1);
  REQUIRE(parts[0] == "noseparator");
}

TEST_CASE("split_view with empty input", "[split][split_view]") {
  std::vector<std::string_view> parts;
  for (auto part : split::split_view("", ':')) {
    parts.push_back(part);
  }
  REQUIRE(parts.size() == 1);
  REQUIRE(parts[0] == "");
}

TEST_CASE("split_view with string delimiter", "[split][split_view]") {
  std::vector<std::string_view> parts;
  for (auto part : split::split_view("foo::bar::baz", std::string_view{"::"})) {
    parts.push_back(part);
  }
  REQUIRE(parts.size() == 3);
  REQUIRE(parts[0] == "foo");
  REQUIRE(parts[1] == "bar");
  REQUIRE(parts[2] == "baz");
}

// ─────────────────────────────────────────────────────────────────────────────
// split_lines - line splitting
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("split_lines basic usage", "[split][split_lines]") {
  std::vector<std::string_view> lines;
  for (auto line : split::split_lines("line1\nline2\nline3")) {
    lines.push_back(line);
  }
  REQUIRE(lines.size() == 3);
  REQUIRE(lines[0] == "line1");
  REQUIRE(lines[1] == "line2");
  REQUIRE(lines[2] == "line3");
}

TEST_CASE("split_lines with trailing newline", "[split][split_lines]") {
  std::vector<std::string_view> lines;
  for (auto line : split::split_lines("line1\nline2\n")) {
    lines.push_back(line);
  }
  REQUIRE(lines.size() == 3);
  REQUIRE(lines[0] == "line1");
  REQUIRE(lines[1] == "line2");
  REQUIRE(lines[2] == "");
}

TEST_CASE("split_lines with empty lines", "[split][split_lines]") {
  std::vector<std::string_view> lines;
  for (auto line : split::split_lines("line1\n\nline3")) {
    lines.push_back(line);
  }
  REQUIRE(lines.size() == 3);
  REQUIRE(lines[0] == "line1");
  REQUIRE(lines[1] == "");
  REQUIRE(lines[2] == "line3");
}

// ─────────────────────────────────────────────────────────────────────────────
// constexpr verification
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("split_prefix is constexpr", "[split][constexpr]") {
  constexpr auto result = split::split_prefix("hello world", "hello ");
  static_assert(result.has_value());
  static_assert(*result == "world");
  REQUIRE(result.has_value());
}

TEST_CASE("split_suffix is constexpr", "[split][constexpr]") {
  constexpr auto result = split::split_suffix("hello.txt", ".txt");
  static_assert(result.has_value());
  static_assert(*result == "hello");
  REQUIRE(result.has_value());
}

TEST_CASE("split_first is constexpr", "[split][constexpr]") {
  constexpr auto result = split::split_first("foo:bar", ':');
  static_assert(result.has_value());
  static_assert(result->first == "foo");
  static_assert(result->second == "bar");
  REQUIRE(result.has_value());
}

TEST_CASE("split_last is constexpr", "[split][constexpr]") {
  constexpr auto result = split::split_last("foo:bar:baz", ':');
  static_assert(result.has_value());
  static_assert(result->first == "foo:bar");
  static_assert(result->second == "baz");
  REQUIRE(result.has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Real-world usage patterns
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("split_first for URL parsing", "[split][patterns]") {
  std::string_view url = "https://example.com/path/to/resource";

  auto protocol_split = split::split_first(url, std::string_view{"://"});
  REQUIRE(protocol_split.has_value());
  auto [protocol, rest] = *protocol_split;
  REQUIRE(protocol == "https");
  REQUIRE(rest == "example.com/path/to/resource");

  auto host_split = split::split_first(rest, '/');
  REQUIRE(host_split.has_value());
  auto [host, path] = *host_split;
  REQUIRE(host == "example.com");
  REQUIRE(path == "path/to/resource");
}

TEST_CASE("split_last for file extension", "[split][patterns]") {
  std::string_view filename = "archive.tar.gz";

  auto ext_split = split::split_last(filename, '.');
  REQUIRE(ext_split.has_value());
  auto [base, extension] = *ext_split;
  REQUIRE(base == "archive.tar");
  REQUIRE(extension == "gz");
}

TEST_CASE("split_prefix for store path parsing", "[split][patterns]") {
  std::string_view path = "/nix/store/abc123-package-1.0";

  auto rest = split::split_prefix(path, "/nix/store/");
  REQUIRE(rest.has_value());
  REQUIRE(*rest == "abc123-package-1.0");
}

TEST_CASE("split_first_to for iterative parsing", "[split][patterns]") {
  std::string_view input = "key1=value1;key2=value2;key3=value3";
  std::vector<std::pair<std::string_view, std::string_view>> pairs;

  while (auto pair_string = split::split_first_to(input, ';')) {
    if (auto kv = split::split_first(*pair_string, '=')) {
      pairs.push_back(*kv);
    }
  }
  // Handle the last pair (no trailing semicolon)
  if (auto kv = split::split_first(input, '=')) {
    pairs.push_back(*kv);
  }

  REQUIRE(pairs.size() == 3);
  REQUIRE(pairs[0].first == "key1");
  REQUIRE(pairs[0].second == "value1");
  REQUIRE(pairs[1].first == "key2");
  REQUIRE(pairs[1].second == "value2");
  REQUIRE(pairs[2].first == "key3");
  REQUIRE(pairs[2].second == "value3");
}

TEST_CASE("split_view with algorithm integration", "[split][patterns]") {
  std::string_view csv = "apple,banana,cherry,date";

  auto view = split::split_view(csv, ',');
  auto count = std::ranges::distance(view);

  REQUIRE(count == 4);
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge cases and boundary conditions
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("split_first with only delimiter", "[split][edge_cases]") {
  auto result = split::split_first(":", ':');
  REQUIRE(result.has_value());
  auto [prefix, rest] = *result;
  REQUIRE(prefix == "");
  REQUIRE(rest == "");
}

TEST_CASE("split_last with only delimiter", "[split][edge_cases]") {
  auto result = split::split_last(":", ':');
  REQUIRE(result.has_value());
  auto [head, suffix] = *result;
  REQUIRE(head == "");
  REQUIRE(suffix == "");
}

TEST_CASE("split_first with consecutive delimiters", "[split][edge_cases]") {
  auto result = split::split_first(":::", ':');
  REQUIRE(result.has_value());
  auto [prefix, rest] = *result;
  REQUIRE(prefix == "");
  REQUIRE(rest == "::");
}

TEST_CASE("split_last with consecutive delimiters", "[split][edge_cases]") {
  auto result = split::split_last(":::", ':');
  REQUIRE(result.has_value());
  auto [head, suffix] = *result;
  REQUIRE(head == "::");
  REQUIRE(suffix == "");
}

TEST_CASE("split_view preserves all empty segments", "[split][edge_cases]") {
  std::vector<std::string_view> parts;
  for (auto part : split::split_view(":::", ':')) {
    parts.push_back(part);
  }
  REQUIRE(parts.size() == 4);
  for (const auto& part : parts) {
    REQUIRE(part == "");
  }
}

TEST_CASE("split functions are noexcept", "[split][noexcept]") {
  std::string_view input = "test";
  std::string_view prefix = "te";

  static_assert(noexcept(split::split_prefix(input, prefix)));
  static_assert(noexcept(split::split_prefix(input, 't')));
  static_assert(noexcept(split::split_suffix(input, prefix)));
  static_assert(noexcept(split::split_suffix(input, 't')));
  static_assert(noexcept(split::split_first(input, ':')));
  static_assert(noexcept(split::split_first(input, prefix)));
  static_assert(noexcept(split::split_last(input, ':')));
  static_assert(noexcept(split::split_last(input, prefix)));
  static_assert(noexcept(split::split_first_to(input, ':')));
  static_assert(noexcept(split::split_last_to(input, ':')));
  static_assert(noexcept(split::strip_prefix(input, prefix)));
  static_assert(noexcept(split::strip_suffix(input, prefix)));

  REQUIRE(true);
}
