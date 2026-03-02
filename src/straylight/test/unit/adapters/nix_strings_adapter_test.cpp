// straylight::nix::primitives::adapters::tests
//
// Tests for the Nix-compatible string adapter.
// Verifies that adapter functions behave identically to Nix originals.

#include <catch2/catch_test_macros.hpp>
// Catch2 must be included before rapidcheck/catch.h

// RapidCheck for property-based testing
#include <list>
#include <set>
#include <string>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include <straylight/nix/adapters/nix_strings_adapter.h>

// ─────────────────────────────────────────────────────────────────────────────
// hasPrefix / hasSuffix tests (matching nix::hasPrefix, nix::hasSuffix)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("hasPrefix basic", "[adapter][util]") {
  using straylight::nix::adapters::hasPrefix;
  REQUIRE(hasPrefix("hello world", "hello"));
  REQUIRE_FALSE(hasPrefix("hello world", "world"));
  REQUIRE(hasPrefix("hello", "hello"));
  REQUIRE(hasPrefix("hello", ""));
  REQUIRE_FALSE(hasPrefix("", "hello"));
  REQUIRE(hasPrefix("", ""));
}

TEST_CASE("hasSuffix basic", "[adapter][util]") {
  using straylight::nix::adapters::hasSuffix;
  REQUIRE(hasSuffix("hello world", "world"));
  REQUIRE_FALSE(hasSuffix("hello world", "hello"));
  REQUIRE(hasSuffix("hello", "hello"));
  REQUIRE(hasSuffix("hello", ""));
  REQUIRE_FALSE(hasSuffix("", "hello"));
  REQUIRE(hasSuffix("", ""));
}

TEST_CASE("hasPrefix/hasSuffix nix store paths", "[adapter][util]") {
  using straylight::nix::adapters::hasPrefix;
  using straylight::nix::adapters::hasSuffix;
  // Common Nix use case
  REQUIRE(hasPrefix("/nix/store/abc123-package", "/nix/store/"));
  REQUIRE(hasSuffix("/nix/store/abc123-package.drv", ".drv"));
  REQUIRE(hasSuffix("/nix/store/abc123-package.tar.gz", ".tar.gz"));
}

// ─────────────────────────────────────────────────────────────────────────────
// trim / chomp tests (matching nix::trim, nix::chomp)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("trim whitespace", "[adapter][util]") {
  using straylight::nix::adapters::trim;
  REQUIRE(trim("  hello  ") == "hello");
  REQUIRE(trim("\t\nhello\r\n") == "hello");
  REQUIRE(trim("hello") == "hello");
  REQUIRE(trim("   ") == "");
  REQUIRE(trim("") == "");
}

TEST_CASE("trim custom whitespace", "[adapter][util]") {
  using straylight::nix::adapters::trim;
  REQUIRE(trim("xxhelloxx", "x") == "hello");
  REQUIRE(trim("abchelloabc", "abc") == "hello");
}

TEST_CASE("chomp removes trailing whitespace only", "[adapter][util]") {
  using straylight::nix::adapters::chomp;
  REQUIRE(chomp("hello  ") == "hello");
  REQUIRE(chomp("  hello  ") == "  hello");
  REQUIRE(chomp("hello\n") == "hello");
  REQUIRE(chomp("hello") == "hello");
  REQUIRE(chomp("   ") == "");
}

// ─────────────────────────────────────────────────────────────────────────────
// replaceStrings tests (matching nix::replaceStrings)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("replaceStrings basic", "[adapter][util]") {
  using straylight::nix::adapters::replaceStrings;
  REQUIRE(replaceStrings("hello world", "world", "universe") == "hello universe");
  REQUIRE(replaceStrings("aaa", "a", "b") == "bbb");
  REQUIRE(replaceStrings("hello", "x", "y") == "hello");
  REQUIRE(replaceStrings("", "a", "b") == "");
}

TEST_CASE("replaceStrings multiple occurrences", "[adapter][util]") {
  using straylight::nix::adapters::replaceStrings;
  REQUIRE(replaceStrings("foo bar foo baz foo", "foo", "qux") == "qux bar qux baz qux");
}

TEST_CASE("replaceStrings empty from string", "[adapter][util]") {
  using straylight::nix::adapters::replaceStrings;
  // Nix behavior: empty from returns original string unchanged
  REQUIRE(replaceStrings("hello", "", "x") == "hello");
}

TEST_CASE("replaceStrings empty to string", "[adapter][util]") {
  using straylight::nix::adapters::replaceStrings;
  REQUIRE(replaceStrings("hello", "l", "") == "heo");
}

// ─────────────────────────────────────────────────────────────────────────────
// tokenizeString tests (matching nix::tokenizeString)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("tokenizeString basic", "[adapter][strings]") {
  using straylight::nix::adapters::tokenizeString;
  auto result = tokenizeString<std::vector<std::string>>("hello world foo");

  REQUIRE(result.size() == 3);
  REQUIRE(result[0] == "hello");
  REQUIRE(result[1] == "world");
  REQUIRE(result[2] == "foo");
}

TEST_CASE("tokenizeString with custom separators", "[adapter][strings]") {
  using straylight::nix::adapters::tokenizeString;
  auto result = tokenizeString<std::vector<std::string>>("a,b:c;d", ",;:");

  REQUIRE(result.size() == 4);
  REQUIRE(result[0] == "a");
  REQUIRE(result[1] == "b");
  REQUIRE(result[2] == "c");
  REQUIRE(result[3] == "d");
}

TEST_CASE("tokenizeString filters empty strings", "[adapter][strings]") {
  using straylight::nix::adapters::tokenizeString;
  auto result = tokenizeString<std::vector<std::string>>("  hello  world  ");

  REQUIRE(result.size() == 2);
  REQUIRE(result[0] == "hello");
  REQUIRE(result[1] == "world");
}

TEST_CASE("tokenizeString to list", "[adapter][strings]") {
  using straylight::nix::adapters::tokenizeString;
  auto result = tokenizeString<std::list<std::string>>("a b c");

  REQUIRE(result.size() == 3);
  auto it = result.begin();
  REQUIRE(*it++ == "a");
  REQUIRE(*it++ == "b");
  REQUIRE(*it++ == "c");
}

TEST_CASE("tokenizeString to set", "[adapter][strings]") {
  using straylight::nix::adapters::tokenizeString;
  auto result = tokenizeString<std::set<std::string>>("a b c b a");

  // Sets deduplicate
  REQUIRE(result.size() == 3);
  REQUIRE(result.count("a") == 1);
  REQUIRE(result.count("b") == 1);
  REQUIRE(result.count("c") == 1);
}

TEST_CASE("tokenizeString empty string", "[adapter][strings]") {
  using straylight::nix::adapters::tokenizeString;
  auto result = tokenizeString<std::vector<std::string>>("");

  REQUIRE(result.empty());
}

TEST_CASE("tokenizeString only separators", "[adapter][strings]") {
  using straylight::nix::adapters::tokenizeString;
  auto result = tokenizeString<std::vector<std::string>>("   \t\n  ");

  REQUIRE(result.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// splitString tests (matching nix::splitString)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("splitString basic", "[adapter][strings]") {
  using straylight::nix::adapters::splitString;
  auto result = splitString<std::vector<std::string>>("a,b,c", ",");

  REQUIRE(result.size() == 3);
  REQUIRE(result[0] == "a");
  REQUIRE(result[1] == "b");
  REQUIRE(result[2] == "c");
}

TEST_CASE("splitString preserves empty strings", "[adapter][strings]") {
  using straylight::nix::adapters::splitString;
  auto result = splitString<std::vector<std::string>>("a,,b", ",");

  REQUIRE(result.size() == 3);
  REQUIRE(result[0] == "a");
  REQUIRE(result[1] == "");
  REQUIRE(result[2] == "b");
}

TEST_CASE("splitString leading/trailing separators", "[adapter][strings]") {
  using straylight::nix::adapters::splitString;
  auto result = splitString<std::vector<std::string>>(",a,b,", ",");

  REQUIRE(result.size() == 4);
  REQUIRE(result[0] == "");
  REQUIRE(result[1] == "a");
  REQUIRE(result[2] == "b");
  REQUIRE(result[3] == "");
}

TEST_CASE("splitString character set separators", "[adapter][strings]") {
  using straylight::nix::adapters::splitString;
  // Nix splitString treats separators as a character set
  auto result = splitString<std::vector<std::string>>("a:b;c", ":;");

  REQUIRE(result.size() == 3);
  REQUIRE(result[0] == "a");
  REQUIRE(result[1] == "b");
  REQUIRE(result[2] == "c");
}

TEST_CASE("splitString empty string", "[adapter][strings]") {
  using straylight::nix::adapters::splitString;
  auto result = splitString<std::vector<std::string>>("", ",");

  // Nix behavior: splitting empty string produces single empty string
  REQUIRE(result.size() == 1);
  REQUIRE(result[0] == "");
}

TEST_CASE("splitString to list", "[adapter][strings]") {
  using straylight::nix::adapters::splitString;
  auto result = splitString<std::list<std::string>>("a:b:c", ":");

  REQUIRE(result.size() == 3);
  auto it = result.begin();
  REQUIRE(*it++ == "a");
  REQUIRE(*it++ == "b");
  REQUIRE(*it++ == "c");
}

// ─────────────────────────────────────────────────────────────────────────────
// concatStringsSep tests (matching nix::concatStringsSep)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("concatStringsSep basic", "[adapter][strings]") {
  using straylight::nix::adapters::concatStringsSep;
  std::vector<std::string> parts = {"a", "b", "c"};
  REQUIRE(concatStringsSep(",", parts) == "a,b,c");
}

TEST_CASE("concatStringsSep single element", "[adapter][strings]") {
  using straylight::nix::adapters::concatStringsSep;
  std::vector<std::string> parts = {"hello"};
  REQUIRE(concatStringsSep(",", parts) == "hello");
}

TEST_CASE("concatStringsSep empty vector", "[adapter][strings]") {
  using straylight::nix::adapters::concatStringsSep;
  std::vector<std::string> parts = {};
  REQUIRE(concatStringsSep(",", parts) == "");
}

TEST_CASE("concatStringsSep with empty separator", "[adapter][strings]") {
  using straylight::nix::adapters::concatStringsSep;
  std::vector<std::string> parts = {"a", "b", "c"};
  REQUIRE(concatStringsSep("", parts) == "abc");
}

TEST_CASE("concatStringsSep with list", "[adapter][strings]") {
  using straylight::nix::adapters::concatStringsSep;
  std::list<std::string> parts = {"a", "b", "c"};
  REQUIRE(concatStringsSep(":", parts) == "a:b:c");
}

TEST_CASE("concatStringsSep with set", "[adapter][strings]") {
  using straylight::nix::adapters::concatStringsSep;
  std::set<std::string> parts = {"a", "b", "c"};
  // Set is ordered, so result is deterministic
  REQUIRE(concatStringsSep(",", parts) == "a,b,c");
}

TEST_CASE("concatStringsSep multi-char separator", "[adapter][strings]") {
  using straylight::nix::adapters::concatStringsSep;
  std::vector<std::string> parts = {"a", "b", "c"};
  REQUIRE(concatStringsSep(", ", parts) == "a, b, c");
}

// ─────────────────────────────────────────────────────────────────────────────
// concatMapStringsSep tests (matching nix::concatMapStringsSep)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("concatMapStringsSep basic", "[adapter][strings]") {
  using straylight::nix::adapters::concatMapStringsSep;
  std::vector<int> nums = {1, 2, 3};
  auto result = concatMapStringsSep(", ", nums, [](int n) { return std::to_string(n); });

  REQUIRE(result == "1, 2, 3");
}

TEST_CASE("concatMapStringsSep with transformation", "[adapter][strings]") {
  using straylight::nix::adapters::concatMapStringsSep;
  std::vector<std::string> words = {"hello", "world"};
  auto result = concatMapStringsSep("-", words, [](const std::string& s) {
    std::string upper = s;
    for (auto& c : upper) {
      c = static_cast<char>(std::toupper(c));
    }
    return upper;
  });

  REQUIRE(result == "HELLO-WORLD");
}

// ─────────────────────────────────────────────────────────────────────────────
// Zero-copy variants (not in original Nix API, but useful for performance)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("trim_view returns string_view", "[adapter][strings]") {
  using straylight::nix::adapters::trim_view;
  std::string original = "  hello  ";
  std::string_view trimmed = trim_view(original);

  REQUIRE(trimmed == "hello");
  // View should point into original string
  REQUIRE(trimmed.data() >= original.data());
  REQUIRE(trimmed.data() < original.data() + original.size());
}

TEST_CASE("trim_left_view", "[adapter][strings]") {
  using straylight::nix::adapters::trim_left_view;
  std::string original = "  hello";
  std::string_view trimmed = trim_left_view(original);

  REQUIRE(trimmed == "hello");
}

TEST_CASE("trim_right_view", "[adapter][strings]") {
  using straylight::nix::adapters::trim_right_view;
  std::string original = "hello  ";
  std::string_view trimmed = trim_right_view(original);

  REQUIRE(trimmed == "hello");
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-based tests
// ─────────────────────────────────────────────────────────────────────────────

namespace {

rc::Gen<std::string> printable_gen() {
  return rc::gen::container<std::string>(rc::gen::inRange<char>(32, 127));
}

rc::Gen<std::string> nonempty_printable_gen() {
  return rc::gen::nonEmpty(printable_gen());
}

} // namespace

TEST_CASE("hasPrefix/hasSuffix consistency property", "[adapter][property]") {
  using straylight::nix::adapters::hasPrefix;
  using straylight::nix::adapters::hasSuffix;
  rc::prop("string is both its own prefix and suffix", []() {
    auto str = *printable_gen();
    RC_ASSERT(straylight::nix::adapters::hasPrefix(str, str));
    RC_ASSERT(straylight::nix::adapters::hasSuffix(str, str));
  });

  rc::prop("empty string is prefix and suffix of everything", []() {
    auto str = *printable_gen();
    RC_ASSERT(straylight::nix::adapters::hasPrefix(str, ""));
    RC_ASSERT(straylight::nix::adapters::hasSuffix(str, ""));
  });
}

TEST_CASE("trim idempotence property", "[adapter][property]") {
  rc::prop("trimming twice is same as trimming once", []() {
    auto str = *printable_gen();
    auto trimmed_once = straylight::nix::adapters::trim(str);
    auto trimmed_twice = straylight::nix::adapters::trim(trimmed_once);
    RC_ASSERT(trimmed_once == trimmed_twice);
  });
}

TEST_CASE("replaceStrings with empty from is identity", "[adapter][property]") {
  rc::prop("empty pattern returns original string", []() {
    auto str = *printable_gen();
    auto replacement = *printable_gen();
    auto result = straylight::nix::adapters::replaceStrings(str, "", replacement);
    RC_ASSERT(result == str);
  });
}

TEST_CASE("tokenize/split difference property", "[adapter][property]") {
  rc::prop("tokenize result has no empty strings, split may have", []() {
    auto s = *printable_gen();
    auto sep_char = *rc::gen::element(' ', '\t', ',', ':');
    std::string seps(1, sep_char);

    auto tokenized = straylight::nix::adapters::tokenizeString<std::vector<std::string>>(s, seps);
    auto split = straylight::nix::adapters::splitString<std::vector<std::string>>(s, seps);

    // Tokenize never has empty strings
    for (const auto& t : tokenized) {
      RC_ASSERT(!t.empty());
    }

    // Split preserves structure: if we remove empty strings from split,
    // we should get tokenized
    std::vector<std::string> filtered_split;
    for (const auto& p : split) {
      if (!p.empty()) {
        filtered_split.push_back(p);
      }
    }
    RC_ASSERT(filtered_split == tokenized);
  });
}

TEST_CASE("concatStringsSep/split roundtrip", "[adapter][property]") {
  rc::prop("split(concat(parts, sep), sep) == parts when sep not in parts", []() {
    auto sep_char = *rc::gen::element(',', ':', ';', '|');
    std::string sep(1, sep_char);

    // Generate parts without separator character
    auto parts = *rc::gen::container<std::vector<std::string>>(
        rc::gen::container<std::string>(rc::gen::suchThat<char>(
            rc::gen::inRange<char>('a', 'z'), [sep_char](char c) { return c != sep_char; })));

    auto joined = straylight::nix::adapters::concatStringsSep(sep, parts);
    auto split_back = straylight::nix::adapters::splitString<std::vector<std::string>>(joined, sep);

    if (parts.empty()) {
      // concat of empty is "", split of "" is [""]
      RC_ASSERT(split_back.size() == 1);
      RC_ASSERT(split_back[0].empty());
    } else {
      RC_ASSERT(split_back == parts);
    }
  });
}
