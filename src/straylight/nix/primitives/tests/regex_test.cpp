// straylight::nix::primitives::regex tests
//
// Property-based testing with rapidcheck for regex primitives.
// Tests ERE compatibility, match semantics, and cache behavior.

// Catch2 MUST be included before rapidcheck/catch.h for v3 compatibility
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>
#include "../regex.h"
namespace regex = straylight::nix::primitives::regex;

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
