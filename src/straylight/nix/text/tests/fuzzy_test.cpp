// straylight::nix::text tests
//
// Tests for SIMD-optimized fuzzy string matching.

// Catch2 MUST be included before rapidcheck/catch.h for v3 compatibility
#include <set>
#include <string>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include <catch2/catch_test_macros.hpp>

#include "../fuzzy.h"
namespace fuzzy = straylight::nix::text;

// ─────────────────────────────────────────────────────────────────────────────
// Levenshtein Distance Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("levenshtein basic", "[fuzzy][levenshtein]") {
  // Identical strings
  REQUIRE(fuzzy::levenshtein("hello", "hello") == 0);
  REQUIRE(fuzzy::levenshtein("", "") == 0);

  // Single edit operations
  REQUIRE(fuzzy::levenshtein("hello", "hallo") == 1);  // substitution
  REQUIRE(fuzzy::levenshtein("hello", "hell") == 1);   // deletion
  REQUIRE(fuzzy::levenshtein("hello", "helloo") == 1); // insertion

  // Multiple edits
  REQUIRE(fuzzy::levenshtein("kitten", "sitting") == 3);
  REQUIRE(fuzzy::levenshtein("saturday", "sunday") == 3);

  // Empty string comparisons
  REQUIRE(fuzzy::levenshtein("", "hello") == 5);
  REQUIRE(fuzzy::levenshtein("hello", "") == 5);
}

TEST_CASE("levenshtein with max cutoff", "[fuzzy][levenshtein]") {
  // Within cutoff
  REQUIRE(fuzzy::levenshtein("hello", "hallo", 2) == 1);
  REQUIRE(fuzzy::levenshtein("hello", "hallo", 1) == 1);

  // Exceeds cutoff - returns max + 1
  REQUIRE(fuzzy::levenshtein("hello", "world", 2) == 3);
  REQUIRE(fuzzy::levenshtein("abc", "xyz", 1) == 2);
}

TEST_CASE("levenshtein normalized", "[fuzzy][levenshtein]") {
  // Identical = 1.0
  REQUIRE(fuzzy::levenshtein_normalized("hello", "hello") == 1.0);

  // Different lengths
  double sim = fuzzy::levenshtein_normalized("hello", "hallo");
  REQUIRE(sim > 0.7);
  REQUIRE(sim < 1.0);

  // Completely different
  double diff = fuzzy::levenshtein_normalized("abc", "xyz");
  REQUIRE(diff < 0.5);
}

// ─────────────────────────────────────────────────────────────────────────────
// Fuzzy Ratio Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ratio basic", "[fuzzy][ratio]") {
  // Identical = 100.0
  REQUIRE(fuzzy::ratio("hello", "hello") == 100.0);

  // Similar strings
  double r = fuzzy::ratio("hello", "hallo");
  REQUIRE(r > 70.0);
  REQUIRE(r < 100.0);

  // Different strings
  double diff = fuzzy::ratio("abc", "xyz");
  REQUIRE(diff < 50.0);
}

TEST_CASE("partial_ratio", "[fuzzy][ratio]") {
  // Substring matching
  double r = fuzzy::partial_ratio("hello", "hello world");
  REQUIRE(r == 100.0); // "hello" is contained exactly

  double r2 = fuzzy::partial_ratio("world", "hello world");
  REQUIRE(r2 == 100.0);
}

TEST_CASE("token_sort_ratio", "[fuzzy][ratio]") {
  // Word order doesn't matter
  double r = fuzzy::token_sort_ratio("hello world", "world hello");
  REQUIRE(r == 100.0);

  double r2 = fuzzy::token_sort_ratio("a b c", "c b a");
  REQUIRE(r2 == 100.0);
}

TEST_CASE("token_set_ratio", "[fuzzy][ratio]") {
  // Duplicates don't matter
  double r = fuzzy::token_set_ratio("hello hello world", "hello world");
  REQUIRE(r == 100.0);

  // Order and duplicates don't matter
  double r2 = fuzzy::token_set_ratio("a b b c", "c a b");
  REQUIRE(r2 == 100.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Best Matches Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("best_matches basic", "[fuzzy][matches]") {
  std::vector<std::string> candidates = {"hello", "hallo", "world", "help", "hero", "hell"};

  auto matches = fuzzy::best_matches(candidates, "hello", 3, 2);

  REQUIRE(matches.size() == 3);
  REQUIRE(matches[0].distance == 0);
  REQUIRE(matches[0].text == "hello");
  REQUIRE(matches[1].distance == 1); // hallo or hell
}

TEST_CASE("best_matches with set", "[fuzzy][matches]") {
  std::set<std::string> candidates = {"nixpkgs", "nixos", "nix", "nixpkgs-unstable"};

  auto matches = fuzzy::best_matches(candidates, "nixpkg", 5, 3);

  REQUIRE(!matches.empty());
  // "nix" should be close (distance 3), "nixpkgs" should be closest (distance 1)
  REQUIRE(matches[0].text == "nixpkgs");
  REQUIRE(matches[0].distance == 1);
}

TEST_CASE("best_matches empty result", "[fuzzy][matches]") {
  std::vector<std::string> candidates = {"abc", "def", "ghi"};

  auto matches = fuzzy::best_matches(candidates, "xyz", 5, 1);

  // All distances > 1, so empty
  REQUIRE(matches.empty());
}

TEST_CASE("best_matches_ratio", "[fuzzy][matches]") {
  std::vector<std::string> candidates = {"hello", "hallo", "world", "help"};

  auto matches = fuzzy::best_matches_ratio(candidates, "hello", 3, 0.7);

  REQUIRE(!matches.empty());
  REQUIRE(matches[0].first == 1.0); // exact match score
  REQUIRE(matches[0].second == "hello");
}

// ─────────────────────────────────────────────────────────────────────────────
// CachedQuery Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("CachedQuery", "[fuzzy][cached]") {
  fuzzy::CachedQuery query("hello");

  REQUIRE(query.distance("hello") == 0);
  REQUIRE(query.distance("hallo") == 1);
  REQUIRE(query.distance("world", 2) == 3); // exceeds cutoff

  double r = query.ratio("hello");
  REQUIRE(r == 100.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-based Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("levenshtein identity property", "[fuzzy][property]") {
  rc::prop("distance(s, s) == 0", []() {
    auto s = *rc::gen::string<std::string>();
    RC_ASSERT(fuzzy::levenshtein(s, s) == 0);
  });
}

TEST_CASE("levenshtein symmetry property", "[fuzzy][property]") {
  rc::prop("distance(a, b) == distance(b, a)", []() {
    auto a = *rc::gen::string<std::string>();
    auto b = *rc::gen::string<std::string>();
    RC_ASSERT(fuzzy::levenshtein(a, b) == fuzzy::levenshtein(b, a));
  });
}

TEST_CASE("levenshtein triangle inequality property", "[fuzzy][property]") {
  rc::prop("distance(a, c) <= distance(a, b) + distance(b, c)", []() {
    auto a = *rc::gen::string<std::string>();
    auto b = *rc::gen::string<std::string>();
    auto c = *rc::gen::string<std::string>();

    auto ab = fuzzy::levenshtein(a, b);
    auto bc = fuzzy::levenshtein(b, c);
    auto ac = fuzzy::levenshtein(a, c);

    RC_ASSERT(ac <= ab + bc);
  });
}

TEST_CASE("levenshtein empty string property", "[fuzzy][property]") {
  rc::prop("distance('', s) == len(s)", []() {
    auto s = *rc::gen::string<std::string>();
    RC_ASSERT(fuzzy::levenshtein("", s) == s.size());
    RC_ASSERT(fuzzy::levenshtein(s, "") == s.size());
  });
}

TEST_CASE("ratio bounds property", "[fuzzy][property]") {
  rc::prop("ratio is between 0 and 100", []() {
    auto a = *rc::gen::string<std::string>();
    auto b = *rc::gen::string<std::string>();

    double r = fuzzy::ratio(a, b);
    RC_ASSERT(r >= 0.0);
    RC_ASSERT(r <= 100.0);
  });
}

TEST_CASE("best_matches sorted property", "[fuzzy][property]") {
  rc::prop("best_matches returns sorted results", []() {
    auto candidates = *rc::gen::container<std::vector<std::string>>(rc::gen::string<std::string>());
    auto query = *rc::gen::string<std::string>();

    auto matches = fuzzy::best_matches(candidates, query, 10, 5);

    // Check sorted by distance
    for (std::size_t i = 1; i < matches.size(); ++i) {
      RC_ASSERT(matches[i - 1].distance <= matches[i].distance);
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Nix-specific tests (suggestions use case)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("nix attribute suggestions", "[fuzzy][nix]") {
  // Simulating typos in nix attribute names
  std::vector<std::string> attrs = {"buildInputs",
                                    "nativeBuildInputs",
                                    "propagatedBuildInputs",
                                    "checkInputs",
                                    "installCheckInputs",
                                    "devShell",
                                    "packages",
                                    "apps",
                                    "overlays",
                                    "nixosModules",
                                    "nixosConfigurations"};

  // Typo: "buildInput" (missing 's')
  auto matches1 = fuzzy::best_matches(attrs, "buildInput", 3, 2);
  REQUIRE(!matches1.empty());
  REQUIRE(matches1[0].text == "buildInputs");
  REQUIRE(matches1[0].distance == 1);

  // Typo: "nativeBuildInput" (missing 's')
  auto matches2 = fuzzy::best_matches(attrs, "nativeBuildInput", 3, 2);
  REQUIRE(!matches2.empty());
  REQUIRE(matches2[0].text == "nativeBuildInputs");

  // Typo: "devshell" (case sensitivity)
  auto matches3 = fuzzy::best_matches(attrs, "devshell", 3, 2);
  REQUIRE(!matches3.empty());
  REQUIRE(matches3[0].text == "devShell");
}

TEST_CASE("nix package name suggestions", "[fuzzy][nix]") {
  std::vector<std::string> packages = {"hello",  "gcc",   "clang", "python3", "python311",
                                       "nodejs", "rustc", "cargo", "ripgrep"};

  // Looking for "python"
  auto matches = fuzzy::best_matches(packages, "python", 5, 3);
  REQUIRE(!matches.empty());
  // python3 has distance 1, python311 has distance 3
  REQUIRE(matches[0].text == "python3");
}
