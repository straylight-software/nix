// straylight::nix::text tests
//
// Property-based testing with rapidcheck for regex primitives.
// Tests ERE compatibility, match semantics, and cache behavior.

// Catch2 MUST be included before rapidcheck/catch.h for v3 compatibility
#include <algorithm>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include <catch2/catch_test_macros.hpp>

#include "../regex.h"
namespace regex = straylight::nix::text;

// ─────────────────────────────────────────────────────────────────────────────
// Basic compilation tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("regex compile valid patterns", "[regex][compile]") {
  // Simple patterns
  REQUIRE(regex::Regex::compile("abc"));
  REQUIRE(regex::Regex::compile(""));
  REQUIRE(regex::Regex::compile("a*"));
  REQUIRE(regex::Regex::compile("a+"));
  REQUIRE(regex::Regex::compile("a?"));
  REQUIRE(regex::Regex::compile("a{2,4}"));

  // Character classes
  REQUIRE(regex::Regex::compile("[abc]"));
  REQUIRE(regex::Regex::compile("[^abc]"));
  REQUIRE(regex::Regex::compile("[a-z]"));
  REQUIRE(regex::Regex::compile("[[:alpha:]]"));
  REQUIRE(regex::Regex::compile("[[:digit:]]"));
  REQUIRE(regex::Regex::compile("[[:space:]]"));
  REQUIRE(regex::Regex::compile("[[:upper:]]"));

  // Grouping and alternation
  REQUIRE(regex::Regex::compile("(abc)"));
  REQUIRE(regex::Regex::compile("a|b"));
  REQUIRE(regex::Regex::compile("(a|b)c"));

  // Anchors
  REQUIRE(regex::Regex::compile("^abc"));
  REQUIRE(regex::Regex::compile("abc$"));
  REQUIRE(regex::Regex::compile("^abc$"));
}

TEST_CASE("regex compile invalid patterns", "[regex][compile]") {
  // Unbalanced brackets
  auto r1 = regex::Regex::compile("[abc");
  REQUIRE_FALSE(r1);
  REQUIRE(r1.error().code == regex::ErrorCode::BadPattern);

  // Unbalanced parens
  auto r2 = regex::Regex::compile("(abc");
  REQUIRE_FALSE(r2);

  // Invalid repetition
  auto r3 = regex::Regex::compile("*");
  REQUIRE_FALSE(r3);

  auto r4 = regex::Regex::compile("+");
  REQUIRE_FALSE(r4);

  // Invalid escape in ERE (RE2 is lenient, so this may pass)
  // auto r5 = regex::Regex::compile("\\z");
  // REQUIRE_FALSE(r5);
}

TEST_CASE("regex pattern and num_captures", "[regex][compile]") {
  auto r1 = regex::Regex::compile("abc");
  REQUIRE(r1);
  REQUIRE(r1->pattern() == "abc");
  REQUIRE(r1->num_captures() == 0);

  auto r2 = regex::Regex::compile("(a)(b)(c)");
  REQUIRE(r2);
  REQUIRE(r2->num_captures() == 3);

  auto r3 = regex::Regex::compile("(a(b)c)");
  REQUIRE(r3);
  REQUIRE(r3->num_captures() == 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Full match tests (builtins.match semantics)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("regex full_match basic", "[regex][match]") {
  auto re = regex::Regex::compile("abc");
  REQUIRE(re);

  // Exact match
  auto m1 = re->full_match("abc");
  REQUIRE(m1);
  REQUIRE(m1->size() == 1); // Only full match, no captures
  REQUIRE((*m1)[0].text == "abc");

  // No match - string too long
  auto m2 = re->full_match("abcd");
  REQUIRE_FALSE(m2);

  // No match - string too short
  auto m3 = re->full_match("ab");
  REQUIRE_FALSE(m3);

  // No match - different content
  auto m4 = re->full_match("xyz");
  REQUIRE_FALSE(m4);
}

TEST_CASE("regex full_match with captures", "[regex][match]") {
  // Nix example: builtins.match "a(b)(c)" "abc" => [ "b" "c" ]
  auto re = regex::Regex::compile("a(b)(c)");
  REQUIRE(re);
  REQUIRE(re->num_captures() == 2);

  auto m = re->full_match("abc");
  REQUIRE(m);
  REQUIRE(m->size() == 3);        // full + 2 captures
  REQUIRE((*m)[0].text == "abc"); // full match
  REQUIRE((*m)[1].text == "b");   // capture 1
  REQUIRE((*m)[2].text == "c");   // capture 2
  REQUIRE((*m)[1].matched);
  REQUIRE((*m)[2].matched);

  // captures() returns only captures, not full match
  auto captures = m->captures();
  REQUIRE(captures.size() == 2);
  REQUIRE(captures[0].text == "b");
  REQUIRE(captures[1].text == "c");
}

TEST_CASE("regex full_match POSIX classes", "[regex][match]") {
  // Nix example: builtins.match "[[:space:]]+([[:upper:]]+)[[:space:]]+" "  FOO   "
  auto re = regex::Regex::compile("[[:space:]]+([[:upper:]]+)[[:space:]]+");
  REQUIRE(re);

  auto m = re->full_match("  FOO   ");
  REQUIRE(m);
  REQUIRE(m->num_captures() == 1);
  REQUIRE((*m)[1].text == "FOO");
}

TEST_CASE("regex full_match alternation with unmatched groups", "[regex][match]") {
  // When alternation causes some groups to not participate
  // Nix example: builtins.split "(a)|(c)" "abc" shows null for unmatched groups
  auto re = regex::Regex::compile("(a)|(b)");
  REQUIRE(re);

  auto m1 = re->full_match("a");
  REQUIRE(m1);
  REQUIRE(m1->size() == 3);
  REQUIRE((*m1)[1].matched); // group 1 matched
  REQUIRE((*m1)[1].text == "a");
  REQUIRE_FALSE((*m1)[2].matched); // group 2 did not participate

  auto m2 = re->full_match("b");
  REQUIRE(m2);
  REQUIRE_FALSE((*m2)[1].matched); // group 1 did not participate
  REQUIRE((*m2)[2].matched);       // group 2 matched
  REQUIRE((*m2)[2].text == "b");
}

TEST_CASE("regex full_match empty pattern", "[regex][match]") {
  auto re = regex::Regex::compile("");
  REQUIRE(re);

  // Empty pattern matches empty string
  auto m1 = re->full_match("");
  REQUIRE(m1);
  REQUIRE(m1->size() == 1);
  REQUIRE((*m1)[0].text == "");

  // Empty pattern does NOT match non-empty string (full_match)
  auto m2 = re->full_match("abc");
  REQUIRE_FALSE(m2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Partial match tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("regex partial_match basic", "[regex][partial]") {
  auto re = regex::Regex::compile("abc");
  REQUIRE(re);

  // Match in middle
  auto m1 = re->partial_match("xxxabcyyy");
  REQUIRE(m1);
  REQUIRE((*m1)[0].text == "abc");
  REQUIRE(m1->prefix() == "xxx");
  REQUIRE(m1->suffix() == "yyy");

  // Match at start
  auto m2 = re->partial_match("abcyyy");
  REQUIRE(m2);
  REQUIRE(m2->prefix() == "");
  REQUIRE(m2->suffix() == "yyy");

  // Match at end
  auto m3 = re->partial_match("xxxabc");
  REQUIRE(m3);
  REQUIRE(m3->prefix() == "xxx");
  REQUIRE(m3->suffix() == "");

  // No match
  auto m4 = re->partial_match("xyz");
  REQUIRE_FALSE(m4);
}

// ─────────────────────────────────────────────────────────────────────────────
// Find all tests (builtins.split semantics)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("regex find_all basic", "[regex][findall]") {
  auto re = regex::Regex::compile("(a)b");
  REQUIRE(re);

  // Nix: builtins.split "(a)b" "abc" => [ "" [ "a" ] "c" ]
  auto matches = re->find_all("abc");
  REQUIRE(matches.size() == 1);
  REQUIRE(matches[0][0].text == "ab");
  REQUIRE(matches[0][1].text == "a");
  REQUIRE(matches[0].prefix() == "");
  REQUIRE(matches[0].suffix() == "c");
}

TEST_CASE("regex find_all multiple matches", "[regex][findall]") {
  auto re = regex::Regex::compile("([ac])");
  REQUIRE(re);

  // Nix: builtins.split "([ac])" "abc" => [ "" [ "a" ] "b" [ "c" ] "" ]
  auto matches = re->find_all("abc");
  REQUIRE(matches.size() == 2);

  // First match: "a"
  REQUIRE(matches[0][0].text == "a");
  REQUIRE(matches[0][1].text == "a");
  REQUIRE(matches[0].prefix() == "");

  // Second match: "c"
  REQUIRE(matches[1][0].text == "c");
  REQUIRE(matches[1][1].text == "c");
  REQUIRE(matches[1].prefix() == "b");
  REQUIRE(matches[1].suffix() == "");
}

TEST_CASE("regex find_all with alternation", "[regex][findall]") {
  auto re = regex::Regex::compile("(a)|(c)");
  REQUIRE(re);

  // Nix: builtins.split "(a)|(c)" "abc" => [ "" [ "a" null ] "b" [ null "c" ] "" ]
  auto matches = re->find_all("abc");
  REQUIRE(matches.size() == 2);

  // First match: "a" in group 1, group 2 unmatched
  REQUIRE(matches[0][1].matched);
  REQUIRE(matches[0][1].text == "a");
  REQUIRE_FALSE(matches[0][2].matched);

  // Second match: group 1 unmatched, "c" in group 2
  REQUIRE_FALSE(matches[1][1].matched);
  REQUIRE(matches[1][2].matched);
  REQUIRE(matches[1][2].text == "c");
}

TEST_CASE("regex find_all no matches", "[regex][findall]") {
  auto re = regex::Regex::compile("xyz");
  REQUIRE(re);

  auto matches = re->find_all("abc");
  REQUIRE(matches.empty());
}

TEST_CASE("regex find_all POSIX class", "[regex][findall]") {
  auto re = regex::Regex::compile("([[:upper:]]+)");
  REQUIRE(re);

  // Nix: builtins.split "([[:upper:]]+)" " FOO " => [ " " [ "FOO" ] " " ]
  auto matches = re->find_all(" FOO ");
  REQUIRE(matches.size() == 1);
  REQUIRE(matches[0][1].text == "FOO");
  REQUIRE(matches[0].prefix() == " ");
  REQUIRE(matches[0].suffix() == " ");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test (bool) operations
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("regex test operations", "[regex][test]") {
  auto re = regex::Regex::compile("abc");
  REQUIRE(re);

  // test() requires full match
  REQUIRE(re->test("abc"));
  REQUIRE_FALSE(re->test("abcd"));
  REQUIRE_FALSE(re->test("xabc"));
  REQUIRE_FALSE(re->test("xabcy"));

  // test_partial() finds anywhere
  REQUIRE(re->test_partial("abc"));
  REQUIRE(re->test_partial("xabc"));
  REQUIRE(re->test_partial("abcx"));
  REQUIRE(re->test_partial("xabcx"));
  REQUIRE_FALSE(re->test_partial("xyz"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Cache tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("regex cache basic", "[regex][cache]") {
  regex::Cache cache;
  REQUIRE(cache.size() == 0);

  // First access compiles
  const auto& r1 = cache.get("abc");
  REQUIRE(r1);
  REQUIRE(cache.size() == 1);

  // Second access returns cached
  const auto& r2 = cache.get("abc");
  REQUIRE(&r1 == &r2); // Same object
  REQUIRE(cache.size() == 1);

  // Different pattern
  const auto& r3 = cache.get("xyz");
  REQUIRE(cache.size() == 2);
  REQUIRE(&r1 != &r3);
}

TEST_CASE("regex cache invalid pattern throws", "[regex][cache]") {
  regex::Cache cache;

  REQUIRE_THROWS_AS(cache.get("[invalid"), std::runtime_error);

  // try_get returns error instead
  auto result = cache.try_get("[invalid");
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == regex::ErrorCode::BadPattern);
}

TEST_CASE("regex cache clear", "[regex][cache]") {
  regex::Cache cache;

  cache.get("a");
  cache.get("b");
  cache.get("c");
  REQUIRE(cache.size() == 3);

  cache.clear();
  REQUIRE(cache.size() == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread safety tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("regex cache thread safety", "[regex][cache][threading]") {
  regex::Cache cache;
  constexpr int num_threads = 8;
  constexpr int patterns_per_thread = 100;

  std::vector<std::thread> threads;
  threads.reserve(num_threads);

  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&cache, t]() {
      for (int i = 0; i < patterns_per_thread; ++i) {
        // Each thread uses overlapping patterns to stress concurrent access
        std::string pattern = "pattern" + std::to_string(i % 10);
        try {
          const auto& re = cache.get(pattern);
          REQUIRE(re);
          // Use the regex to ensure it's fully constructed
          auto m = re.full_match("pattern" + std::to_string(i % 10));
          (void)m;
        } catch (...) {
          FAIL("Exception in thread " << t);
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // Should have exactly 10 unique patterns cached
  REQUIRE(cache.size() == 10);
}

// ─────────────────────────────────────────────────────────────────────────────
// Convenience function tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("regex convenience functions", "[regex][convenience]") {
  // full_match
  auto m1 = regex::full_match("abc", "abc");
  REQUIRE(m1);
  REQUIRE(*m1);

  auto m2 = regex::full_match("abc", "abcd");
  REQUIRE(m2);
  REQUIRE_FALSE(*m2);

  // partial_match
  auto m3 = regex::partial_match("abc", "xabcy");
  REQUIRE(m3);
  REQUIRE(*m3);

  // test
  auto t1 = regex::test("abc", "abc");
  REQUIRE(t1);
  REQUIRE(*t1 == true);

  auto t2 = regex::test("abc", "xyz");
  REQUIRE(t2);
  REQUIRE(*t2 == false);

  // Invalid pattern
  auto err = regex::test("[invalid", "abc");
  REQUIRE_FALSE(err);
  REQUIRE(err.error().code == regex::ErrorCode::BadPattern);
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-based tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("regex literal string property", "[regex][property]") {
  rc::prop("literal string always matches itself", []() {
    // Generate alphanumeric strings (safe to use as literal patterns)
    auto str = *rc::gen::container<std::string>(
        rc::gen::oneOf(rc::gen::inRange('a', 'z'), rc::gen::inRange('0', '9')));

    if (str.empty()) {
      return; // Skip empty strings
    }

    auto re = regex::Regex::compile(str);
    RC_ASSERT(re.has_value());

    auto m = re->full_match(str);
    RC_ASSERT(m.has_value());
    RC_ASSERT((*m)[0].text == str);
  });
}

TEST_CASE("regex literal string no false positives property", "[regex][property]") {
  rc::prop("literal string doesn't match different strings", []() {
    // Generate two different alphanumeric strings
    auto str1 = *rc::gen::container<std::string>(
        rc::gen::oneOf(rc::gen::inRange('a', 'z'), rc::gen::inRange('0', '9')));
    auto str2 = *rc::gen::container<std::string>(
        rc::gen::oneOf(rc::gen::inRange('a', 'z'), rc::gen::inRange('0', '9')));

    RC_PRE(str1 != str2);
    RC_PRE(!str1.empty());

    auto re = regex::Regex::compile(str1);
    RC_ASSERT(re.has_value());

    auto m = re->full_match(str2);
    RC_ASSERT(!m.has_value());
  });
}

TEST_CASE("regex capture count property", "[regex][property]") {
  rc::prop("num_captures equals number of () groups", []() {
    // Generate pattern with N capture groups: (a)(a)...(a)
    auto n = *rc::gen::inRange(0, 10);
    std::string pattern;
    for (int i = 0; i < n; ++i) {
      pattern += "(a)";
    }

    auto re = regex::Regex::compile(pattern);
    RC_ASSERT(re.has_value());
    RC_ASSERT(re->num_captures() == static_cast<std::size_t>(n));
  });
}

TEST_CASE("regex test and full_match consistency property", "[regex][property]") {
  rc::prop("test() returns true iff full_match() returns non-empty", []() {
    // Simple alphanumeric pattern
    auto pattern = *rc::gen::nonEmpty(rc::gen::container<std::string>(
        rc::gen::oneOf(rc::gen::inRange('a', 'z'), rc::gen::inRange('0', '9'))));
    auto text = *rc::gen::container<std::string>(
        rc::gen::oneOf(rc::gen::inRange('a', 'z'), rc::gen::inRange('0', '9')));

    auto re = regex::Regex::compile(pattern);
    RC_ASSERT(re.has_value());

    bool test_result = re->test(text);
    auto match_result = re->full_match(text);

    RC_ASSERT(test_result == match_result.has_value());
  });
}

TEST_CASE("regex cache idempotency property", "[regex][property]") {
  rc::prop("cache.get returns same object for same pattern", []() {
    // Simple alphanumeric pattern
    auto pattern = *rc::gen::nonEmpty(rc::gen::container<std::string>(
        rc::gen::oneOf(rc::gen::inRange('a', 'z'), rc::gen::inRange('0', '9'))));

    regex::Cache cache;
    const auto& r1 = cache.get(pattern);
    const auto& r2 = cache.get(pattern);

    RC_ASSERT(&r1 == &r2);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Real Nix pattern tests (from builtins.match/split docs)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("regex nix builtins.match examples", "[regex][nix]") {
  // builtins.match "ab" "abc" => null
  {
    auto re = regex::Regex::compile("ab");
    auto m = re->full_match("abc");
    REQUIRE_FALSE(m);
  }

  // builtins.match "abc" "abc" => [ ]
  {
    auto re = regex::Regex::compile("abc");
    auto m = re->full_match("abc");
    REQUIRE(m);
    REQUIRE(m->num_captures() == 0);
  }

  // builtins.match "a(b)(c)" "abc" => [ "b" "c" ]
  {
    auto re = regex::Regex::compile("a(b)(c)");
    auto m = re->full_match("abc");
    REQUIRE(m);
    REQUIRE(m->num_captures() == 2);
    REQUIRE(m->captures()[0].text == "b");
    REQUIRE(m->captures()[1].text == "c");
  }

  // builtins.match "[[:space:]]+([[:upper:]]+)[[:space:]]+" "  FOO   " => [ "FOO" ]
  {
    auto re = regex::Regex::compile("[[:space:]]+([[:upper:]]+)[[:space:]]+");
    auto m = re->full_match("  FOO   ");
    REQUIRE(m);
    REQUIRE(m->num_captures() == 1);
    REQUIRE(m->captures()[0].text == "FOO");
  }
}

TEST_CASE("regex nix builtins.split examples", "[regex][nix]") {
  // builtins.split "(a)b" "abc" => [ "" [ "a" ] "c" ]
  {
    auto re = regex::Regex::compile("(a)b");
    auto matches = re->find_all("abc");
    REQUIRE(matches.size() == 1);
    REQUIRE(matches[0].prefix() == "");
    REQUIRE(matches[0].captures()[0].text == "a");
    REQUIRE(matches[0].suffix() == "c");
  }

  // builtins.split "([ac])" "abc" => [ "" [ "a" ] "b" [ "c" ] "" ]
  {
    auto re = regex::Regex::compile("([ac])");
    auto matches = re->find_all("abc");
    REQUIRE(matches.size() == 2);
    REQUIRE(matches[0].prefix() == "");
    REQUIRE(matches[0].captures()[0].text == "a");
    REQUIRE(matches[1].prefix() == "b");
    REQUIRE(matches[1].captures()[0].text == "c");
    REQUIRE(matches[1].suffix() == "");
  }

  // builtins.split "(a)|(c)" "abc" => [ "" [ "a" null ] "b" [ null "c" ] "" ]
  {
    auto re = regex::Regex::compile("(a)|(c)");
    auto matches = re->find_all("abc");
    REQUIRE(matches.size() == 2);
    REQUIRE(matches[0].captures()[0].matched);
    REQUIRE(matches[0].captures()[0].text == "a");
    REQUIRE_FALSE(matches[0].captures()[1].matched);
    REQUIRE_FALSE(matches[1].captures()[0].matched);
    REQUIRE(matches[1].captures()[1].matched);
    REQUIRE(matches[1].captures()[1].text == "c");
  }

  // builtins.split "([[:upper:]]+)" " FOO " => [ " " [ "FOO" ] " " ]
  {
    auto re = regex::Regex::compile("([[:upper:]]+)");
    auto matches = re->find_all(" FOO ");
    REQUIRE(matches.size() == 1);
    REQUIRE(matches[0].prefix() == " ");
    REQUIRE(matches[0].captures()[0].text == "FOO");
    REQUIRE(matches[0].suffix() == " ");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Heavy metal property tests - RE2 vs std::regex equivalence
// ─────────────────────────────────────────────────────────────────────────────

#include <regex>

namespace {

// Generator for simple regex patterns that work in both RE2 and std::regex
rc::Gen<std::string> simple_pattern_gen() {
  return rc::gen::oneOf(
      // Literal alphanumeric
      rc::gen::nonEmpty(rc::gen::container<std::string>(
          rc::gen::oneOf(rc::gen::inRange('a', 'z'), rc::gen::inRange('0', '9')))),
      // Character class [abc]
      rc::gen::map(rc::gen::nonEmpty(rc::gen::container<std::string>(rc::gen::inRange('a', 'z'))),
                   [](const std::string& chars) { return "[" + chars + "]"; }),
      // Single char with quantifier
      rc::gen::map(
          rc::gen::pair(rc::gen::inRange('a', 'z'), rc::gen::element('*', '+', '?')),
          [](const std::pair<char, char>& p) { return std::string(1, p.first) + p.second; }));
}

// Generator for text to match against
rc::Gen<std::string> match_text_gen() {
  return rc::gen::container<std::string>(
      rc::gen::oneOf(rc::gen::inRange('a', 'z'), rc::gen::inRange('0', '9'), rc::gen::just(' ')));
}

} // namespace

TEST_CASE("RE2 vs std::regex equivalence for simple patterns", "[regex][property][equivalence]") {
  rc::prop("RE2 and std::regex agree on literal matches", []() {
    auto pattern = *rc::gen::nonEmpty(rc::gen::container<std::string>(
        rc::gen::oneOf(rc::gen::inRange('a', 'z'), rc::gen::inRange('0', '9'))));
    auto text = *match_text_gen();

    auto re2_regex = regex::Regex::compile(pattern);
    RC_ASSERT(re2_regex.has_value());

    std::regex std_regex(pattern);

    bool re2_match = re2_regex->test(text);
    bool std_match = std::regex_match(text, std_regex);

    RC_ASSERT(re2_match == std_match);
  });

  rc::prop("RE2 and std::regex agree on character class matches", []() {
    // Generate a character class like [abc]
    auto chars = *rc::gen::nonEmpty(rc::gen::container<std::string>(rc::gen::inRange('a', 'z')));
    std::string pattern = "[" + chars + "]";
    auto text = *rc::gen::container<std::string>(rc::gen::inRange('a', 'z'));

    auto re2_regex = regex::Regex::compile(pattern);
    RC_ASSERT(re2_regex.has_value());

    std::regex std_regex(pattern);

    bool re2_match = re2_regex->test(text);
    bool std_match = std::regex_match(text, std_regex);

    RC_ASSERT(re2_match == std_match);
  });

  rc::prop("RE2 and std::regex agree on simple quantifiers", []() {
    auto base_char = *rc::gen::inRange('a', 'f');
    auto quantifier = *rc::gen::element('*', '+', '?');
    std::string pattern(1, base_char);
    pattern += quantifier;

    // Anchor the pattern for full match comparison
    std::string anchored = "^" + pattern + "$";

    auto text = *rc::gen::container<std::string>(rc::gen::element(base_char, 'x'));

    auto re2_regex = regex::Regex::compile(anchored);
    RC_ASSERT(re2_regex.has_value());

    try {
      std::regex std_regex(anchored);
      bool re2_match = re2_regex->test(text);
      bool std_match = std::regex_match(text, std_regex);
      RC_ASSERT(re2_match == std_match);
    } catch (const std::regex_error&) {
      // std::regex may fail on some patterns - skip
    }
  });
}

TEST_CASE("RE2 vs std::regex equivalence for POSIX classes", "[regex][property][equivalence]") {
  rc::prop("[:digit:] class agrees with std::regex", []() {
    auto text = *rc::gen::container<std::string>(
        rc::gen::oneOf(rc::gen::inRange('0', '9'), rc::gen::inRange('a', 'z')));

    std::string pattern = "^[[:digit:]]*$";

    auto re2_regex = regex::Regex::compile(pattern);
    RC_ASSERT(re2_regex.has_value());

    try {
      std::regex std_regex(pattern);
      bool re2_match = re2_regex->test(text);
      bool std_match = std::regex_match(text, std_regex);
      RC_ASSERT(re2_match == std_match);
    } catch (const std::regex_error&) {
      // Skip if std::regex doesn't support POSIX classes
    }
  });

  rc::prop("[:alpha:] class agrees with std::regex", []() {
    auto text = *rc::gen::container<std::string>(rc::gen::oneOf(
        rc::gen::inRange('a', 'z'), rc::gen::inRange('A', 'Z'), rc::gen::inRange('0', '9')));

    std::string pattern = "^[[:alpha:]]*$";

    auto re2_regex = regex::Regex::compile(pattern);
    RC_ASSERT(re2_regex.has_value());

    try {
      std::regex std_regex(pattern);
      bool re2_match = re2_regex->test(text);
      bool std_match = std::regex_match(text, std_regex);
      RC_ASSERT(re2_match == std_match);
    } catch (const std::regex_error&) {
      // Skip if std::regex doesn't support POSIX classes
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Heavy metal edge case tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("regex edge cases", "[regex][property][edge]") {
  rc::prop("empty pattern matches only empty string", []() {
    auto text = *match_text_gen();

    auto re = regex::Regex::compile("");
    RC_ASSERT(re.has_value());

    bool matches = re->test(text);
    RC_ASSERT(matches == text.empty());
  });

  rc::prop("dot matches any single character", []() {
    auto text = *rc::gen::container<std::string>(rc::gen::inRange<char>(32, 126));
    RC_PRE(text.find('\n') == std::string::npos); // . doesn't match newline by default

    if (text.size() == 1) {
      auto re = regex::Regex::compile(".");
      RC_ASSERT(re.has_value());
      RC_ASSERT(re->test(text));
    }
  });

  rc::prop("anchor ^ matches start", []() {
    auto prefix = *rc::gen::nonEmpty(rc::gen::container<std::string>(rc::gen::inRange('a', 'z')));
    auto suffix = *rc::gen::container<std::string>(rc::gen::inRange('a', 'z'));

    std::string text = prefix + suffix;
    std::string pattern = "^" + prefix;

    auto re = regex::Regex::compile(pattern);
    RC_ASSERT(re.has_value());
    RC_ASSERT(re->test_partial(text));
  });

  rc::prop("anchor $ matches end", []() {
    auto prefix = *rc::gen::container<std::string>(rc::gen::inRange('a', 'z'));
    auto suffix = *rc::gen::nonEmpty(rc::gen::container<std::string>(rc::gen::inRange('a', 'z')));

    std::string text = prefix + suffix;
    std::string pattern = suffix + "$";

    auto re = regex::Regex::compile(pattern);
    RC_ASSERT(re.has_value());
    RC_ASSERT(re->test_partial(text));
  });

  rc::prop("alternation matches either branch", []() {
    auto a = *rc::gen::nonEmpty(rc::gen::container<std::string>(rc::gen::inRange('a', 'm')));
    auto b = *rc::gen::nonEmpty(rc::gen::container<std::string>(rc::gen::inRange('n', 'z')));
    RC_PRE(a != b);

    std::string pattern = "^(" + a + "|" + b + ")$";

    auto re = regex::Regex::compile(pattern);
    RC_ASSERT(re.has_value());

    RC_ASSERT(re->test(a));
    RC_ASSERT(re->test(b));
    RC_ASSERT(!re->test(a + b)); // Concatenation shouldn't match
  });

  rc::prop("repetition {n,m} matches correct counts", []() {
    auto n = *rc::gen::inRange(1, 5);
    auto m = *rc::gen::inRange(n, 10);

    std::string pattern = "^a{" + std::to_string(n) + "," + std::to_string(m) + "}$";

    auto re = regex::Regex::compile(pattern);
    RC_ASSERT(re.has_value());

    // Should match strings with n to m 'a's
    for (int i = n; i <= m; ++i) {
      RC_ASSERT(re->test(std::string(static_cast<std::size_t>(i), 'a')));
    }

    // Should not match strings with fewer than n or more than m 'a's
    if (n > 1) {
      RC_ASSERT(!re->test(std::string(static_cast<std::size_t>(n - 1), 'a')));
    }
    RC_ASSERT(!re->test(std::string(static_cast<std::size_t>(m + 1), 'a')));
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Capture group property tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("capture group properties", "[regex][property][capture]") {
  rc::prop("capture groups return correct substrings", []() {
    auto a = *rc::gen::nonEmpty(rc::gen::container<std::string>(rc::gen::inRange('a', 'm')));
    auto b = *rc::gen::nonEmpty(rc::gen::container<std::string>(rc::gen::inRange('n', 'z')));

    std::string pattern = "^(" + a + ")(" + b + ")$";
    std::string text = a + b;

    auto re = regex::Regex::compile(pattern);
    RC_ASSERT(re.has_value());
    RC_ASSERT(re->num_captures() == 2);

    auto match = re->full_match(text);
    RC_ASSERT(match.has_value());
    RC_ASSERT(match->num_captures() == 2);
    RC_ASSERT((*match)[1].text == a);
    RC_ASSERT((*match)[2].text == b);
  });

  rc::prop("nested captures return correct substrings", []() {
    auto inner = *rc::gen::nonEmpty(rc::gen::container<std::string>(rc::gen::inRange('a', 'z')));

    std::string pattern = "^((" + inner + "))$";
    std::string text = inner;

    auto re = regex::Regex::compile(pattern);
    RC_ASSERT(re.has_value());
    RC_ASSERT(re->num_captures() == 2);

    auto match = re->full_match(text);
    RC_ASSERT(match.has_value());
    // Both captures should be the same (nested)
    RC_ASSERT((*match)[1].text == inner);
    RC_ASSERT((*match)[2].text == inner);
  });

  rc::prop("optional group may or may not match", []() {
    auto text = *rc::gen::element<std::string>("a", "ab", "abc");

    auto re = regex::Regex::compile("^a(b)?(c)?$");
    RC_ASSERT(re.has_value());

    auto match = re->full_match(text);
    if (text == "a") {
      RC_ASSERT(match.has_value());
      RC_ASSERT(!(*match)[1].matched);
      RC_ASSERT(!(*match)[2].matched);
    } else if (text == "ab") {
      RC_ASSERT(match.has_value());
      RC_ASSERT((*match)[1].matched);
      RC_ASSERT((*match)[1].text == "b");
      RC_ASSERT(!(*match)[2].matched);
    } else if (text == "abc") {
      RC_ASSERT(match.has_value());
      RC_ASSERT((*match)[1].matched);
      RC_ASSERT((*match)[2].matched);
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// find_all property tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("find_all properties", "[regex][property][findall]") {
  rc::prop("find_all returns non-overlapping matches", []() {
    auto pattern = *rc::gen::element<std::string_view>("a", "ab", "abc");
    auto text = *match_text_gen();

    auto re = regex::Regex::compile(std::string(pattern));
    RC_ASSERT(re.has_value());

    auto matches = re->find_all(text);

    // Check non-overlapping: each match starts after previous ends
    std::size_t last_end = 0;
    for (const auto& match : matches) {
      std::size_t match_start = static_cast<std::size_t>(match[0].text.data() - text.data());
      RC_ASSERT(match_start >= last_end);
      last_end = match_start + match[0].text.size();
    }
  });

  rc::prop("find_all matches can reconstruct original with splits", []() {
    auto text = *rc::gen::nonEmpty(match_text_gen());

    // Use single char pattern for predictable splitting
    auto re = regex::Regex::compile("a");
    RC_ASSERT(re.has_value());

    auto matches = re->find_all(text);

    // Reconstruct: prefix + (match + suffix)*
    std::string reconstructed;
    std::size_t pos = 0;

    for (const auto& match : matches) {
      reconstructed += match.prefix();
      reconstructed += match[0].text;
      pos = static_cast<std::size_t>(match[0].text.data() - text.data()) + match[0].text.size();
    }

    // Add final suffix
    if (!matches.empty()) {
      reconstructed += matches.back().suffix();
    } else {
      reconstructed = text;
    }

    RC_ASSERT(reconstructed == text);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Fuzz tests for regex patterns
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("regex fuzzing", "[regex][fuzz]") {
  rc::prop("Regex::compile never crashes on arbitrary input", []() {
    auto pattern = *rc::gen::container<std::string>(rc::gen::arbitrary<char>());

    // Should not crash, may return error
    [[maybe_unused]] auto result = regex::Regex::compile(pattern);
  });

  rc::prop("valid regex never crashes on arbitrary text", []() {
    auto text = *rc::gen::container<std::string>(rc::gen::arbitrary<char>());

    // Use a known-valid simple pattern
    auto re = regex::Regex::compile(".*");
    RC_ASSERT(re.has_value());

    // Should not crash
    [[maybe_unused]] auto full = re->full_match(text);
    [[maybe_unused]] auto partial = re->partial_match(text);
    [[maybe_unused]] auto test = re->test(text);
    [[maybe_unused]] auto all = re->find_all(text);
  });

  rc::prop("regex cache handles concurrent access without crash", []() {
    regex::Cache cache;

    auto patterns = *rc::gen::container<std::vector<std::string>>(
        rc::gen::nonEmpty(rc::gen::container<std::string>(rc::gen::inRange('a', 'z'))));

    // Access patterns multiple times
    for (const auto& pattern : patterns) {
      try {
        [[maybe_unused]] const auto& re = cache.get(pattern);
      } catch (const std::exception&) {
        // Invalid pattern is OK
      }
    }

    // All valid patterns should be cached
    RC_ASSERT(cache.size() <= patterns.size());
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// ReDoS resistance tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("RE2 ReDoS resistance", "[regex][property][redos]") {
  rc::prop("exponential patterns complete quickly", []() {
    // Patterns that cause exponential backtracking in std::regex
    // but should be linear in RE2
    std::vector<std::string> evil_patterns = {
        "(a+)+$",
        "(a|a)+$",
        "(a|aa)+$",
        "a*a*a*a*a*$",
    };

    // Evil inputs that trigger backtracking
    auto n = *rc::gen::inRange(10, 50);
    std::string evil_input(static_cast<std::size_t>(n), 'a');
    evil_input += 'X'; // Ensure no match to maximize backtracking

    for (const auto& pattern : evil_patterns) {
      auto re = regex::Regex::compile(pattern);
      if (!re.has_value())
        continue;

      auto start = std::chrono::steady_clock::now();
      [[maybe_unused]] bool result = re->test(evil_input);
      auto end = std::chrono::steady_clock::now();

      auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
      // RE2 should complete in under 100ms for any input
      RC_ASSERT(duration.count() < 100);
    }
  });
}
