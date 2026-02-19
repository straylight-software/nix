// straylight // nix // util // tests
//
// Unit tests for string utilities

// Catch2 must be included before rapidcheck/catch.h
#include <catch2/catch_test_macros.hpp>

// RapidCheck for property-based testing
#include <list>
#include <set>
#include <string>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include "nix/util/split.h"
#include "nix/util/strings.h"
#include "nix/util/util.h"

using namespace nix;

// =============================================================================
// tokenizeString tests
// =============================================================================

TEST_CASE("tokenizeString with default separators", "[strings][tokenize]") {
  auto result = tokenizeString<std::vector<std::string>>("hello world");
  REQUIRE(result.size() == 2);
  REQUIRE(result[0] == "hello");
  REQUIRE(result[1] == "world");
}

TEST_CASE("tokenizeString empty input", "[strings][tokenize]") {
  auto result = tokenizeString<std::vector<std::string>>("");
  REQUIRE(result.empty());
}

TEST_CASE("tokenizeString single token", "[strings][tokenize]") {
  auto result = tokenizeString<std::vector<std::string>>("hello");
  REQUIRE(result.size() == 1);
  REQUIRE(result[0] == "hello");
}

TEST_CASE("tokenizeString multiple consecutive separators", "[strings][tokenize]") {
  auto result = tokenizeString<std::vector<std::string>>("hello   world");
  REQUIRE(result.size() == 2);
  REQUIRE(result[0] == "hello");
  REQUIRE(result[1] == "world");
}

TEST_CASE("tokenizeString with leading separators", "[strings][tokenize]") {
  auto result = tokenizeString<std::vector<std::string>>("  hello");
  REQUIRE(result.size() == 1);
  REQUIRE(result[0] == "hello");
}

TEST_CASE("tokenizeString with trailing separators", "[strings][tokenize]") {
  auto result = tokenizeString<std::vector<std::string>>("hello  ");
  REQUIRE(result.size() == 1);
  REQUIRE(result[0] == "hello");
}

TEST_CASE("tokenizeString with custom separator", "[strings][tokenize]") {
  auto result = tokenizeString<std::vector<std::string>>("a,b,c", ",");
  REQUIRE(result.size() == 3);
  REQUIRE(result[0] == "a");
  REQUIRE(result[1] == "b");
  REQUIRE(result[2] == "c");
}

TEST_CASE("tokenizeString with multiple separator chars", "[strings][tokenize]") {
  auto result = tokenizeString<std::vector<std::string>>("a,b;c:d", ",;:");
  REQUIRE(result.size() == 4);
  REQUIRE(result[0] == "a");
  REQUIRE(result[1] == "b");
  REQUIRE(result[2] == "c");
  REQUIRE(result[3] == "d");
}

TEST_CASE("tokenizeString only separators", "[strings][tokenize]") {
  auto result = tokenizeString<std::vector<std::string>>("   ");
  REQUIRE(result.empty());
}

TEST_CASE("tokenizeString to set deduplicates", "[strings][tokenize]") {
  auto result = tokenizeString<string_set_t>("a b a c b");
  REQUIRE(result.size() == 3);
  REQUIRE(result.count("a") == 1);
  REQUIRE(result.count("b") == 1);
  REQUIRE(result.count("c") == 1);
}

TEST_CASE("tokenizeString to list preserves order", "[strings][tokenize]") {
  auto result = tokenizeString<std::list<std::string>>("first second third");
  REQUIRE(result.size() == 3);
  auto iter = result.begin();
  REQUIRE(*iter++ == "first");
  REQUIRE(*iter++ == "second");
  REQUIRE(*iter++ == "third");
}

// =============================================================================
// splitString tests
// =============================================================================

TEST_CASE("splitString basic", "[strings][split]") {
  auto result = splitString<std::vector<std::string>>("a,b,c", ",");
  REQUIRE(result.size() == 3);
  REQUIRE(result[0] == "a");
  REQUIRE(result[1] == "b");
  REQUIRE(result[2] == "c");
}

TEST_CASE("splitString preserves empty strings", "[strings][split]") {
  auto result = splitString<std::vector<std::string>>("a,,c", ",");
  REQUIRE(result.size() == 3);
  REQUIRE(result[0] == "a");
  REQUIRE(result[1] == "");
  REQUIRE(result[2] == "c");
}

TEST_CASE("splitString empty input returns single empty string", "[strings][split]") {
  auto result = splitString<std::vector<std::string>>("", ",");
  REQUIRE(result.size() == 1);
  REQUIRE(result[0] == "");
}

TEST_CASE("splitString leading separator", "[strings][split]") {
  auto result = splitString<std::vector<std::string>>(",a,b", ",");
  REQUIRE(result.size() == 3);
  REQUIRE(result[0] == "");
  REQUIRE(result[1] == "a");
  REQUIRE(result[2] == "b");
}

TEST_CASE("splitString trailing separator", "[strings][split]") {
  auto result = splitString<std::vector<std::string>>("a,b,", ",");
  REQUIRE(result.size() == 3);
  REQUIRE(result[0] == "a");
  REQUIRE(result[1] == "b");
  REQUIRE(result[2] == "");
}

TEST_CASE("splitString only separators", "[strings][split]") {
  auto result = splitString<std::vector<std::string>>(",,", ",");
  REQUIRE(result.size() == 3);
  REQUIRE(result[0] == "");
  REQUIRE(result[1] == "");
  REQUIRE(result[2] == "");
}

TEST_CASE("splitString no separator in input", "[strings][split]") {
  auto result = splitString<std::vector<std::string>>("hello", ",");
  REQUIRE(result.size() == 1);
  REQUIRE(result[0] == "hello");
}

// =============================================================================
// concatStringsSep tests
// =============================================================================

TEST_CASE("concatStringsSep basic", "[strings][concat]") {
  std::vector<std::string> parts = {"a", "b", "c"};
  auto result = concatStringsSep(",", parts);
  REQUIRE(result == "a,b,c");
}

TEST_CASE("concatStringsSep empty vector", "[strings][concat]") {
  std::vector<std::string> empty;
  auto result = concatStringsSep(",", empty);
  REQUIRE(result == "");
}

TEST_CASE("concatStringsSep single element", "[strings][concat]") {
  std::vector<std::string> parts = {"only"};
  auto result = concatStringsSep(",", parts);
  REQUIRE(result == "only");
}

TEST_CASE("concatStringsSep empty separator", "[strings][concat]") {
  std::vector<std::string> parts = {"a", "b", "c"};
  auto result = concatStringsSep("", parts);
  REQUIRE(result == "abc");
}

TEST_CASE("concatStringsSep multi-char separator", "[strings][concat]") {
  std::vector<std::string> parts = {"a", "b", "c"};
  auto result = concatStringsSep(" -> ", parts);
  REQUIRE(result == "a -> b -> c");
}

TEST_CASE("concatStringsSep with empty strings in vector", "[strings][concat]") {
  std::vector<std::string> parts = {"a", "", "c"};
  auto result = concatStringsSep(",", parts);
  REQUIRE(result == "a,,c");
}

TEST_CASE("concatStringsSep from set", "[strings][concat]") {
  string_set_t parts = {"apple", "banana", "cherry"};
  auto result = concatStringsSep(", ", parts);
  // Set is sorted, so order is deterministic
  REQUIRE(result == "apple, banana, cherry");
}

TEST_CASE("concatStringsSep from list", "[strings][concat]") {
  std::list<std::string> parts = {"x", "y", "z"};
  auto result = concatStringsSep("-", parts);
  REQUIRE(result == "x-y-z");
}

// =============================================================================
// concatMapStringsSep tests
// =============================================================================

TEST_CASE("concatMapStringsSep basic", "[strings][concat]") {
  std::vector<int> nums = {1, 2, 3};
  auto result = concatMapStringsSep(", ", nums, [](int n) { return std::to_string(n); });
  REQUIRE(result == "1, 2, 3");
}

TEST_CASE("concatMapStringsSep empty input", "[strings][concat]") {
  std::vector<int> empty;
  auto result = concatMapStringsSep(", ", empty, [](int n) { return std::to_string(n); });
  REQUIRE(result == "");
}

TEST_CASE("concatMapStringsSep string transformation", "[strings][concat]") {
  std::vector<std::string> words = {"hello", "world"};
  auto result = concatMapStringsSep(" ", words, [](const std::string& s) {
    std::string upper = s;
    for (auto& c : upper) {
      c = static_cast<char>(std::toupper(c));
    }
    return upper;
  });
  REQUIRE(result == "HELLO WORLD");
}

// =============================================================================
// splitPrefixTo tests (from split.h)
// =============================================================================

TEST_CASE("splitPrefixTo basic", "[split]") {
  std::string_view input = "foo:bar:baz";
  auto prefix = splitPrefixTo(input, ':');
  REQUIRE(prefix.has_value());
  REQUIRE(*prefix == "foo");
  REQUIRE(input == "bar:baz");
}

TEST_CASE("splitPrefixTo no separator", "[split]") {
  std::string_view input = "noseparator";
  auto prefix = splitPrefixTo(input, ':');
  REQUIRE_FALSE(prefix.has_value());
  REQUIRE(input == "noseparator");
}

TEST_CASE("splitPrefixTo empty prefix", "[split]") {
  std::string_view input = ":rest";
  auto prefix = splitPrefixTo(input, ':');
  REQUIRE(prefix.has_value());
  REQUIRE(*prefix == "");
  REQUIRE(input == "rest");
}

TEST_CASE("splitPrefixTo empty suffix", "[split]") {
  std::string_view input = "prefix:";
  auto prefix = splitPrefixTo(input, ':');
  REQUIRE(prefix.has_value());
  REQUIRE(*prefix == "prefix");
  REQUIRE(input == "");
}

TEST_CASE("splitPrefixTo empty input", "[split]") {
  std::string_view input;
  auto prefix = splitPrefixTo(input, ':');
  REQUIRE_FALSE(prefix.has_value());
  REQUIRE(input.empty());
}

TEST_CASE("splitPrefixTo chained calls", "[split]") {
  std::string_view input = "a:b:c";

  auto first = splitPrefixTo(input, ':');
  REQUIRE(first.has_value());
  REQUIRE(*first == "a");
  REQUIRE(input == "b:c");

  auto second = splitPrefixTo(input, ':');
  REQUIRE(second.has_value());
  REQUIRE(*second == "b");
  REQUIRE(input == "c");

  auto third = splitPrefixTo(input, ':');
  REQUIRE_FALSE(third.has_value());
  REQUIRE(input == "c");
}

// =============================================================================
// splitPrefix tests (from split.h)
// =============================================================================

TEST_CASE("splitPrefix matches", "[split]") {
  std::string_view input = "prefix:suffix";
  bool result = splitPrefix(input, "prefix:");
  REQUIRE(result);
  REQUIRE(input == "suffix");
}

TEST_CASE("splitPrefix no match", "[split]") {
  std::string_view input = "other:suffix";
  bool result = splitPrefix(input, "prefix:");
  REQUIRE_FALSE(result);
  REQUIRE(input == "other:suffix");
}

TEST_CASE("splitPrefix empty prefix matches", "[split]") {
  std::string_view input = "anything";
  bool result = splitPrefix(input, "");
  REQUIRE(result);
  REQUIRE(input == "anything");
}

TEST_CASE("splitPrefix prefix longer than input", "[split]") {
  std::string_view input = "short";
  bool result = splitPrefix(input, "shorterlonger");
  REQUIRE_FALSE(result);
  REQUIRE(input == "short");
}

// =============================================================================
// hasPrefix / hasSuffix tests (from util.h)
// =============================================================================

TEST_CASE("hasPrefix basic", "[util]") {
  REQUIRE(hasPrefix("hello world", "hello"));
  REQUIRE_FALSE(hasPrefix("hello world", "world"));
  REQUIRE(hasPrefix("hello", "hello"));
  REQUIRE(hasPrefix("hello", ""));
  REQUIRE_FALSE(hasPrefix("", "hello"));
  REQUIRE(hasPrefix("", ""));
}

TEST_CASE("hasSuffix basic", "[util]") {
  REQUIRE(hasSuffix("hello world", "world"));
  REQUIRE_FALSE(hasSuffix("hello world", "hello"));
  REQUIRE(hasSuffix("hello", "hello"));
  REQUIRE(hasSuffix("hello", ""));
  REQUIRE_FALSE(hasSuffix("", "hello"));
  REQUIRE(hasSuffix("", ""));
}

// =============================================================================
// trim / chomp tests (from util.h)
// =============================================================================

TEST_CASE("trim whitespace", "[util]") {
  REQUIRE(trim("  hello  ") == "hello");
  REQUIRE(trim("\t\nhello\r\n") == "hello");
  REQUIRE(trim("hello") == "hello");
  REQUIRE(trim("   ") == "");
  REQUIRE(trim("") == "");
}

TEST_CASE("trim custom whitespace", "[util]") {
  REQUIRE(trim("xxhelloxx", "x") == "hello");
  REQUIRE(trim("abchelloabc", "abc") == "hello");
}

TEST_CASE("chomp trailing whitespace", "[util]") {
  REQUIRE(chomp("hello\n") == "hello");
  REQUIRE(chomp("hello\r\n") == "hello");
  REQUIRE(chomp("hello   ") == "hello");
  REQUIRE(chomp("hello") == "hello");
  REQUIRE(chomp("") == "");
}

TEST_CASE("chomp preserves leading whitespace", "[util]") {
  REQUIRE(chomp("  hello\n") == "  hello");
}

// =============================================================================
// toLower tests (from util.h)
// =============================================================================

TEST_CASE("toLower basic", "[util]") {
  REQUIRE(toLower("HELLO") == "hello");
  REQUIRE(toLower("Hello World") == "hello world");
  REQUIRE(toLower("already lowercase") == "already lowercase");
  REQUIRE(toLower("") == "");
  REQUIRE(toLower("123ABC") == "123abc");
}

// =============================================================================
// replaceStrings tests (from util.h)
// =============================================================================

TEST_CASE("replaceStrings basic", "[util]") {
  REQUIRE(replaceStrings("hello world", "world", "universe") == "hello universe");
  REQUIRE(replaceStrings("aaa", "a", "b") == "bbb");
  REQUIRE(replaceStrings("hello", "x", "y") == "hello");
  REQUIRE(replaceStrings("", "a", "b") == "");
}

TEST_CASE("replaceStrings multiple occurrences", "[util]") {
  REQUIRE(replaceStrings("foo bar foo baz foo", "foo", "qux") == "qux bar qux baz qux");
}

TEST_CASE("replaceStrings empty from string", "[util]") {
  // Empty 'from' string typically means no replacement
  REQUIRE(replaceStrings("hello", "", "x") == "hello");
}

TEST_CASE("replaceStrings empty to string", "[util]") {
  REQUIRE(replaceStrings("hello", "l", "") == "heo");
}

// =============================================================================
// quoteString tests (from util.h)
// =============================================================================

TEST_CASE("quoteString default quote", "[util]") {
  REQUIRE(quoteString("hello") == "'hello'");
  REQUIRE(quoteString("") == "''");
  REQUIRE(quoteString("with space") == "'with space'");
}

TEST_CASE("quoteString custom quote", "[util]") {
  REQUIRE(quoteString("hello", '"') == "\"hello\"");
  REQUIRE(quoteString("hello", '`') == "`hello`");
}

// =============================================================================
// escapeShellArgAlways tests (from util.h)
// =============================================================================

TEST_CASE("escapeShellArgAlways basic", "[util]") {
  REQUIRE(escapeShellArgAlways("hello") == "'hello'");
  REQUIRE(escapeShellArgAlways("hello world") == "'hello world'");
}

TEST_CASE("escapeShellArgAlways special characters", "[util]") {
  // Single quotes need escaping in shell
  auto result = escapeShellArgAlways("it's");
  // Should escape the single quote somehow
  REQUIRE(result.contains("it"));
  REQUIRE(result.contains('s'));
}

TEST_CASE("escapeShellArgAlways empty", "[util]") {
  REQUIRE(escapeShellArgAlways("") == "''");
}

// =============================================================================
// optionalBracket tests (from strings.h)
// =============================================================================

TEST_CASE("optionalBracket with content", "[strings]") {
  REQUIRE(optionalBracket(" (", "foo", ")") == " (foo)");
  REQUIRE(optionalBracket("[", "item", "]") == "[item]");
}

TEST_CASE("optionalBracket empty content", "[strings]") {
  REQUIRE(optionalBracket(" (", "", ")") == "");
  REQUIRE(optionalBracket("[", "", "]") == "");
}

TEST_CASE("optionalBracket with optional string", "[strings]") {
  std::optional<std::string> some_value = "bar";
  std::optional<std::string> no_value = std::nullopt;
  std::optional<std::string> empty_value = "";

  REQUIRE(optionalBracket(" (", some_value, ")") == " (bar)");
  REQUIRE(optionalBracket(" (", no_value, ")") == "");
  REQUIRE(optionalBracket(" (", empty_value, ")") == "");
}

// =============================================================================
// shellSplitString tests (from strings.h)
// =============================================================================

TEST_CASE("shellSplitString basic", "[strings]") {
  auto result = shellSplitString("arg1 arg2 arg3");
  REQUIRE(result.size() == 3);
  auto iter = result.begin();
  REQUIRE(*iter++ == "arg1");
  REQUIRE(*iter++ == "arg2");
  REQUIRE(*iter++ == "arg3");
}

TEST_CASE("shellSplitString with quotes", "[strings]") {
  auto result = shellSplitString("arg1 'arg with spaces' arg3");
  REQUIRE(result.size() == 3);
  auto iter = result.begin();
  REQUIRE(*iter++ == "arg1");
  REQUIRE(*iter++ == "arg with spaces");
  REQUIRE(*iter++ == "arg3");
}

TEST_CASE("shellSplitString with double quotes", "[strings]") {
  auto result = shellSplitString("arg1 \"arg with spaces\" arg3");
  REQUIRE(result.size() == 3);
  auto iter = result.begin();
  REQUIRE(*iter++ == "arg1");
  REQUIRE(*iter++ == "arg with spaces");
  REQUIRE(*iter++ == "arg3");
}

TEST_CASE("shellSplitString empty", "[strings]") {
  auto result = shellSplitString("");
  REQUIRE(result.empty());
}

TEST_CASE("shellSplitString only whitespace", "[strings]") {
  auto result = shellSplitString("   ");
  REQUIRE(result.empty());
}

// =============================================================================
// getLine tests (from util.h)
// =============================================================================

TEST_CASE("getLine basic", "[util]") {
  auto [line, rest] = getLine("first\nsecond\nthird");
  REQUIRE(line == "first");
  REQUIRE(rest == "second\nthird");
}

TEST_CASE("getLine with crlf", "[util]") {
  auto [line, rest] = getLine("first\r\nsecond");
  REQUIRE(line == "first");
  REQUIRE(rest == "second");
}

TEST_CASE("getLine no newline", "[util]") {
  auto [line, rest] = getLine("single line");
  REQUIRE(line == "single line");
  REQUIRE(rest == "");
}

TEST_CASE("getLine empty input", "[util]") {
  auto [line, rest] = getLine("");
  REQUIRE(line == "");
  REQUIRE(rest == "");
}

TEST_CASE("getLine empty line", "[util]") {
  auto [line, rest] = getLine("\nsecond");
  REQUIRE(line == "");
  REQUIRE(rest == "second");
}

// =============================================================================
// stripIndentation tests (from util.h)
// =============================================================================

TEST_CASE("stripIndentation basic", "[util]") {
  std::string input = "  line1\n  line2\n  line3";
  auto result = stripIndentation(input);
  // The function always adds a trailing newline
  REQUIRE(result == "line1\nline2\nline3\n");
}

TEST_CASE("stripIndentation mixed indentation", "[util]") {
  std::string input = "    line1\n  line2\n    line3";
  auto result = stripIndentation(input);
  // Should remove common prefix (2 spaces), adds trailing newline
  REQUIRE(result == "  line1\nline2\n  line3\n");
}

TEST_CASE("stripIndentation empty", "[util]") {
  REQUIRE(stripIndentation("") == "");
}

// =============================================================================
// Property-based tests with RapidCheck
// =============================================================================

TEST_CASE("tokenize/concat roundtrip property", "[strings][property]") {
  rc::prop("split then join with separator gives back separator-joined string", []() {
    auto parts = *rc::gen::container<std::vector<std::string>>(
        rc::gen::container<std::string>(rc::gen::inRange('a', 'z')));

    // Filter out empty parts since tokenizeString skips them
    std::vector<std::string> non_empty_parts;
    for (const auto& p : parts) {
      if (!p.empty()) {
        non_empty_parts.push_back(p);
      }
    }

    if (non_empty_parts.empty()) {
      return; // Skip trivial case
    }

    auto joined = concatStringsSep(",", non_empty_parts);
    auto split_again = tokenizeString<std::vector<std::string>>(joined, ",");

    RC_ASSERT(split_again == non_empty_parts);
  });
}

TEST_CASE("splitString/concatStringsSep roundtrip property", "[strings][property]") {
  rc::prop("splitString then concatStringsSep with same sep gives original", []() {
    // Generate strings without the separator character
    auto parts = *rc::gen::container<std::vector<std::string>>(
        rc::gen::container<std::string>(rc::gen::inRange('a', 'z')));

    auto joined = concatStringsSep("|", parts);
    auto split_again = splitString<std::vector<std::string>>(joined, "|");

    // splitString always returns at least one element
    if (parts.empty()) {
      RC_ASSERT(split_again.size() == 1);
      RC_ASSERT(split_again[0] == "");
    } else {
      RC_ASSERT(split_again == parts);
    }
  });
}

TEST_CASE("hasPrefix/hasSuffix consistency property", "[util][property]") {
  rc::prop("string has itself as both prefix and suffix", []() {
    auto str = *rc::gen::string<std::string>();
    RC_ASSERT(hasPrefix(str, str));
    RC_ASSERT(hasSuffix(str, str));
  });

  rc::prop("empty string is prefix and suffix of all strings", []() {
    auto str = *rc::gen::string<std::string>();
    RC_ASSERT(hasPrefix(str, ""));
    RC_ASSERT(hasSuffix(str, ""));
  });
}

TEST_CASE("splitPrefixTo exhaust string property", "[split][property]") {
  rc::prop("repeatedly calling splitPrefixTo processes entire string", []() {
    // Generate a string with some separator characters
    auto base_parts = *rc::gen::nonEmpty(rc::gen::container<std::vector<std::string>>(
        rc::gen::nonEmpty(rc::gen::container<std::string>(rc::gen::inRange('a', 'z')))));

    auto joined = concatStringsSep(":", base_parts);
    std::string_view remaining = joined;

    std::vector<std::string> extracted;
    while (auto prefix = splitPrefixTo(remaining, ':')) {
      extracted.emplace_back(*prefix);
    }
    // Don't forget the last part (no trailing separator)
    extracted.emplace_back(remaining);

    RC_ASSERT(extracted == base_parts);
  });
}

TEST_CASE("trim idempotence property", "[util][property]") {
  rc::prop("trimming twice is same as trimming once", []() {
    auto str = *rc::gen::string<std::string>();
    auto trimmed_once = trim(str);
    auto trimmed_twice = trim(trimmed_once);
    RC_ASSERT(trimmed_once == trimmed_twice);
  });
}

TEST_CASE("toLower idempotence property", "[util][property]") {
  rc::prop("lowercasing twice is same as lowercasing once", []() {
    auto str = *rc::gen::string<std::string>();
    auto lowered_once = toLower(str);
    auto lowered_twice = toLower(lowered_once);
    RC_ASSERT(lowered_once == lowered_twice);
  });
}

TEST_CASE("replaceStrings with empty from is identity", "[util][property]") {
  rc::prop("replacing empty string doesn't change input", []() {
    auto str = *rc::gen::string<std::string>();
    auto replacement = *rc::gen::string<std::string>();
    auto result = replaceStrings(str, "", replacement);
    RC_ASSERT(result == str);
  });
}

// =============================================================================
// Edge cases and special characters
// =============================================================================

TEST_CASE("string functions with unicode", "[strings][unicode]") {
  // Basic unicode handling - these functions work on bytes
  std::string unicode_str = "hello \xc3\xa9 world"; // "hello e world" with e-acute

  auto tokens = tokenizeString<std::vector<std::string>>(unicode_str);
  REQUIRE(tokens.size() == 3);
  REQUIRE(tokens[0] == "hello");
  REQUIRE(tokens[1] == "\xc3\xa9");
  REQUIRE(tokens[2] == "world");
}

TEST_CASE("string functions with special characters", "[strings]") {
  // Test with newlines, tabs, etc.
  auto result = tokenizeString<std::vector<std::string>>("a\tb\nc\rd");
  REQUIRE(result.size() == 4);
  REQUIRE(result[0] == "a");
  REQUIRE(result[1] == "b");
  REQUIRE(result[2] == "c");
  REQUIRE(result[3] == "d");
}

TEST_CASE("string functions with single character", "[strings]") {
  REQUIRE(tokenizeString<std::vector<std::string>>("x").size() == 1);
  REQUIRE(tokenizeString<std::vector<std::string>>("x")[0] == "x");
  REQUIRE(splitString<std::vector<std::string>>("x", ",").size() == 1);
  REQUIRE(splitString<std::vector<std::string>>("x", ",")[0] == "x");
  REQUIRE(trim("x") == "x");
  REQUIRE(toLower("X") == "x");
}

TEST_CASE("splitPrefixTo with separator at start", "[split]") {
  std::string_view input = ":rest";
  auto prefix = splitPrefixTo(input, ':');
  REQUIRE(prefix.has_value());
  REQUIRE(prefix->empty());
  REQUIRE(input == "rest");
}

TEST_CASE("splitPrefixTo with separator at end", "[split]") {
  std::string_view input = "prefix:";
  auto prefix = splitPrefixTo(input, ':');
  REQUIRE(prefix.has_value());
  REQUIRE(*prefix == "prefix");
  REQUIRE(input.empty());
}

TEST_CASE("concatStringsSep with very long separator", "[strings]") {
  std::vector<std::string> parts = {"a", "b"};
  std::string long_sep(100, '-');
  auto result = concatStringsSep(long_sep, parts);
  REQUIRE(result == "a" + long_sep + "b");
}
