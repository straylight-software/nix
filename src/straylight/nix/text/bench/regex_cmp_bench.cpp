// straylight::nix::text::bench::regex_cmp_bench
//
// Comprehensive comparison benchmarks: straylight regex.h (RE2) vs std::regex
//
// This benchmark compares:
//   - Compilation speed: RE2 vs std::regex
//   - Full match: RE2 vs std::regex_match (builtins.match semantics)
//   - Partial match: RE2 vs std::regex_search
//   - Find all: RE2 vs std::cregex_iterator (builtins.split semantics)
//
// Expected speedups (RE2 vs std::regex):
//   - Compilation: 3-10x faster
//   - Full match: 5-100x faster (depends on pattern complexity)
//   - Partial match: 10-100x faster (no backtracking)
//   - Complex patterns: 50-1000x+ faster (std::regex can be exponential)
//
// Why RE2 is faster than std::regex:
//   - RE2 uses DFA/NFA hybrid (guaranteed linear time)
//   - std::regex uses backtracking (potentially exponential)
//   - RE2 has optimized SIMD scanning for literals
//   - RE2 patterns are compiled to efficient bytecode

#define ANKERL_NANOBENCH_IMPLEMENT
#include <cstdint>
#include <iostream>
#include <random>
#include <regex>
#include <string>
#include <vector>

#include <nanobench.h>

// Straylight primitives
#include "../regex.h"

namespace regex = straylight::nix::text;

namespace {

// Generate random alphanumeric string
std::string generate_random_string(std::size_t size, uint64_t seed = 42) {
  static const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<std::size_t> dist(0, sizeof(charset) - 2);
  std::string result;
  result.reserve(size);
  for (std::size_t i = 0; i < size; ++i) {
    result += charset[dist(rng)];
  }
  return result;
}

// Generate realistic nix store paths
std::vector<std::string> generate_store_paths(std::size_t count) {
  std::vector<std::string> paths;
  paths.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    paths.push_back("/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-hello-" + std::to_string(i) +
                    ".10.1");
  }
  return paths;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Test patterns (realistic nix usage)
// ─────────────────────────────────────────────────────────────────────────────

// Simple literal pattern (common in package name matching)
constexpr const char* pat_literal = "nixpkgs";

// Character class patterns (POSIX classes used in Nix)
constexpr const char* pat_posix_space = "[[:space:]]+([[:upper:]]+)[[:space:]]+";
constexpr const char* pat_posix_alpha = "([[:alpha:]]+)([[:digit:]]*)";

// Store path pattern (most common in nix)
constexpr const char* pat_store_path = "/nix/store/([a-z0-9]{32})-(.*)";

// Version extraction (common in derivations)
constexpr const char* pat_version = "([a-zA-Z0-9_-]+)-([0-9][0-9.]*[a-z0-9]*)";

// Flake ref pattern
constexpr const char* pat_flakeref = "([a-zA-Z][a-zA-Z0-9+.-]*)://.*";

// Git ref pattern (branch/tag names)
constexpr const char* pat_git_ref = "refs/(heads|tags)/([a-zA-Z0-9._-]+)";

// Email-like pattern (for git commits)
constexpr const char* pat_email = "([a-zA-Z0-9._%+-]+)@([a-zA-Z0-9.-]+)\\.([a-zA-Z]{2,})";

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Compilation speed
// ─────────────────────────────────────────────────────────────────────────────

void bench_compilation(ankerl::nanobench::Bench& b) {
  // Literal pattern
  b.run("std::regex/compile/literal", [] {
    std::regex re(pat_literal, std::regex::extended);
    ankerl::nanobench::doNotOptimizeAway(re);
  });

  b.run("RE2/compile/literal", [] {
    auto re = regex::Regex::compile(pat_literal);
    ankerl::nanobench::doNotOptimizeAway(re);
  });

  // POSIX class pattern
  b.run("std::regex/compile/posix_class", [] {
    std::regex re(pat_posix_alpha, std::regex::extended);
    ankerl::nanobench::doNotOptimizeAway(re);
  });

  b.run("RE2/compile/posix_class", [] {
    auto re = regex::Regex::compile(pat_posix_alpha);
    ankerl::nanobench::doNotOptimizeAway(re);
  });

  // Store path pattern
  b.run("std::regex/compile/store_path", [] {
    std::regex re(pat_store_path, std::regex::extended);
    ankerl::nanobench::doNotOptimizeAway(re);
  });

  b.run("RE2/compile/store_path", [] {
    auto re = regex::Regex::compile(pat_store_path);
    ankerl::nanobench::doNotOptimizeAway(re);
  });

  // Version pattern
  b.run("std::regex/compile/version", [] {
    std::regex re(pat_version, std::regex::extended);
    ankerl::nanobench::doNotOptimizeAway(re);
  });

  b.run("RE2/compile/version", [] {
    auto re = regex::Regex::compile(pat_version);
    ankerl::nanobench::doNotOptimizeAway(re);
  });

  // Expected: RE2 3-10x faster compilation
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Full match (builtins.match semantics)
// ─────────────────────────────────────────────────────────────────────────────

void bench_full_match(ankerl::nanobench::Bench& b) {
  // Pre-compile patterns
  auto re_literal = regex::Regex::compile(pat_literal).value();
  auto re_store_path = regex::Regex::compile(pat_store_path).value();
  auto re_version = regex::Regex::compile(pat_version).value();
  auto re_posix_alpha = regex::Regex::compile(pat_posix_alpha).value();

  std::regex std_literal(pat_literal, std::regex::extended);
  std::regex std_store_path(pat_store_path, std::regex::extended);
  std::regex std_version(pat_version, std::regex::extended);
  std::regex std_posix_alpha(pat_posix_alpha, std::regex::extended);

  // Test strings
  const std::string str_literal = "nixpkgs";
  const std::string str_literal_no = "nixos";
  const std::string str_store = "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-hello-2.10";
  const std::string str_version = "hello-2.10.1";
  const std::string str_alpha = "hello123";

  // Literal match (success)
  b.run("std::regex/full_match/literal/yes", [&] {
    std::cmatch m;
    bool result = std::regex_match(str_literal.c_str(), m, std_literal);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("RE2/full_match/literal/yes", [&] {
    auto m = re_literal.full_match(str_literal);
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  // Literal match (failure)
  b.run("std::regex/full_match/literal/no", [&] {
    std::cmatch m;
    bool result = std::regex_match(str_literal_no.c_str(), m, std_literal);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("RE2/full_match/literal/no", [&] {
    auto m = re_literal.full_match(str_literal_no);
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  // Store path match
  b.run("std::regex/full_match/store_path", [&] {
    std::cmatch m;
    bool result = std::regex_match(str_store.c_str(), m, std_store_path);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("RE2/full_match/store_path", [&] {
    auto m = re_store_path.full_match(str_store);
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  // Version match
  b.run("std::regex/full_match/version", [&] {
    std::cmatch m;
    bool result = std::regex_match(str_version.c_str(), m, std_version);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("RE2/full_match/version", [&] {
    auto m = re_version.full_match(str_version);
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  // POSIX alpha match
  b.run("std::regex/full_match/posix_alpha", [&] {
    std::cmatch m;
    bool result = std::regex_match(str_alpha.c_str(), m, std_posix_alpha);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("RE2/full_match/posix_alpha", [&] {
    auto m = re_posix_alpha.full_match(str_alpha);
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  // Expected: RE2 5-50x faster
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Partial match (search)
// ─────────────────────────────────────────────────────────────────────────────

void bench_partial_match(ankerl::nanobench::Bench& b) {
  auto re_literal = regex::Regex::compile(pat_literal).value();
  auto re_version = regex::Regex::compile(pat_version).value();

  std::regex std_literal(pat_literal, std::regex::extended);
  std::regex std_version(pat_version, std::regex::extended);

  const std::string text_short = "using nixpkgs for the build";
  const std::string text_medium =
      "This is a longer text that mentions nixpkgs somewhere in the middle "
      "and has more content after it to make the search more interesting.";
  const std::string text_long =
      generate_random_string(1000) + "nixpkgs" + generate_random_string(1000);

  // Short text
  b.run("std::regex/partial_match/short", [&] {
    std::cmatch m;
    bool result = std::regex_search(text_short.c_str(), m, std_literal);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("RE2/partial_match/short", [&] {
    auto m = re_literal.partial_match(text_short);
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  // Medium text
  b.run("std::regex/partial_match/medium", [&] {
    std::cmatch m;
    bool result = std::regex_search(text_medium.c_str(), m, std_literal);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("RE2/partial_match/medium", [&] {
    auto m = re_literal.partial_match(text_medium);
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  // Long text (2KB)
  b.run("std::regex/partial_match/long_2k", [&] {
    std::cmatch m;
    bool result = std::regex_search(text_long.c_str(), m, std_literal);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("RE2/partial_match/long_2k", [&] {
    auto m = re_literal.partial_match(text_long);
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  // Expected: RE2 10-100x faster on long strings
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Find all (builtins.split semantics)
// ─────────────────────────────────────────────────────────────────────────────

void bench_find_all(ankerl::nanobench::Bench& b) {
  const std::string str_split = "hello123world456test789abc012def345";

  auto re_digits = regex::Regex::compile("[0-9]+").value();
  std::regex std_digits("[0-9]+", std::regex::extended);

  b.run("std::regex/find_all/digits", [&] {
    std::vector<std::string> matches;
    auto begin =
        std::cregex_iterator(str_split.c_str(), str_split.c_str() + str_split.size(), std_digits);
    auto end = std::cregex_iterator();
    for (auto it = begin; it != end; ++it) {
      matches.push_back((*it)[0].str());
    }
    ankerl::nanobench::doNotOptimizeAway(matches);
  });

  b.run("RE2/find_all/digits", [&] {
    auto matches = re_digits.find_all(str_split);
    ankerl::nanobench::doNotOptimizeAway(matches);
  });

  // Larger input
  std::string large_input;
  for (int i = 0; i < 100; ++i) {
    large_input += "word" + std::to_string(i) + " ";
  }

  auto re_word = regex::Regex::compile("word([0-9]+)").value();
  std::regex std_word("word([0-9]+)", std::regex::extended);

  b.run("std::regex/find_all/word_100", [&] {
    std::vector<std::string> matches;
    auto begin = std::cregex_iterator(large_input.c_str(), large_input.c_str() + large_input.size(),
                                      std_word);
    auto end = std::cregex_iterator();
    for (auto it = begin; it != end; ++it) {
      matches.push_back((*it)[0].str());
    }
    ankerl::nanobench::doNotOptimizeAway(matches);
  });

  b.run("RE2/find_all/word_100", [&] {
    auto matches = re_word.find_all(large_input);
    ankerl::nanobench::doNotOptimizeAway(matches);
  });

  // Expected: RE2 5-20x faster
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Test-only (no captures, fastest path)
// ─────────────────────────────────────────────────────────────────────────────

void bench_test_only(ankerl::nanobench::Bench& b) {
  auto re_literal = regex::Regex::compile(pat_literal).value();
  auto re_store_path = regex::Regex::compile(pat_store_path).value();

  std::regex std_literal(pat_literal, std::regex::extended);
  std::regex std_store_path(pat_store_path, std::regex::extended);

  const std::string str_literal = "nixpkgs";
  const std::string str_store = "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-hello-2.10";

  // Test full match (no captures)
  b.run("std::regex/test/full", [&] {
    bool result = std::regex_match(str_literal.c_str(), std_literal);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("RE2/test/full", [&] {
    bool result = re_literal.test(str_literal);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // Test partial match (no captures)
  b.run("std::regex/test/partial", [&] {
    bool result = std::regex_search("prefix nixpkgs suffix", std_literal);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("RE2/test/partial", [&] {
    bool result = re_literal.test_partial("prefix nixpkgs suffix");
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // Expected: RE2 10-50x faster
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Batch matching (realistic usage)
// ─────────────────────────────────────────────────────────────────────────────

void bench_batch_match(ankerl::nanobench::Bench& b) {
  auto store_paths = generate_store_paths(1000);

  auto re_store = regex::Regex::compile(pat_store_path).value();
  std::regex std_store(pat_store_path, std::regex::extended);

  b.run("std::regex/batch/store_paths_1000", [&] {
    std::size_t matches = 0;
    for (const auto& path : store_paths) {
      std::cmatch m;
      if (std::regex_match(path.c_str(), m, std_store)) {
        ++matches;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(matches);
  });

  b.run("RE2/batch/store_paths_1000", [&] {
    std::size_t matches = 0;
    for (const auto& path : store_paths) {
      auto m = re_store.full_match(path);
      if (m) {
        ++matches;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(matches);
  });

  // Expected: RE2 20-100x faster in aggregate
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Cache performance
// ─────────────────────────────────────────────────────────────────────────────

void bench_cache(ankerl::nanobench::Bench& b) {
  regex::Cache cache;

  // Pre-populate
  cache.get(pat_literal);
  cache.get(pat_store_path);
  cache.get(pat_version);

  b.run("RE2/cache/hit", [&] {
    const auto& re = cache.get(pat_literal);
    ankerl::nanobench::doNotOptimizeAway(&re);
  });

  b.run("RE2/cache/miss", [&] {
    regex::Cache fresh_cache;
    const auto& re = fresh_cache.get(pat_literal);
    ankerl::nanobench::doNotOptimizeAway(&re);
  });

  // Compare to std::regex construction (no caching)
  b.run("std::regex/no_cache/construct", [] {
    std::regex re(pat_literal, std::regex::extended);
    ankerl::nanobench::doNotOptimizeAway(re);
  });

  // Expected: cache hit ~100x faster than std::regex construction
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
  std::cout << "Regex Comparison: std::regex vs straylight regex.h (RE2)\n";
  std::cout << "==========================================================\n";
  std::cout << "\n";
  std::cout << "Expected speedups (RE2 vs std::regex):\n";
  std::cout << "  - Compilation: 3-10x\n";
  std::cout << "  - Full match: 5-100x\n";
  std::cout << "  - Partial match: 10-100x (no backtracking)\n";
  std::cout << "  - Find all: 5-20x\n";
  std::cout << "  - Batch operations: 20-100x\n";
  std::cout << "\n";
  std::cout << "RE2 advantages:\n";
  std::cout << "  - Guaranteed linear time (no ReDoS)\n";
  std::cout << "  - SIMD-accelerated literal scanning\n";
  std::cout << "  - Thread-safe pattern sharing\n";
  std::cout << "\n";

  ankerl::nanobench::Bench b;
  b.title("Regex Comparison").warmup(100).minEpochIterations(1000).unit("op");
  b.relative(true);

  std::cout << "=== Compilation Speed ===\n";
  bench_compilation(b);

  std::cout << "\n=== Full Match (builtins.match) ===\n";
  bench_full_match(b);

  std::cout << "\n=== Partial Match (search) ===\n";
  bench_partial_match(b);

  std::cout << "\n=== Find All (builtins.split) ===\n";
  bench_find_all(b);

  std::cout << "\n=== Test Only (no captures) ===\n";
  bench_test_only(b);

  std::cout << "\n=== Batch Matching ===\n";
  bench_batch_match(b);

  std::cout << "\n=== Cache Performance ===\n";
  bench_cache(b);

  return 0;
}
