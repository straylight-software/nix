// straylight::nix::primitives::regex benchmarks
//
// Benchmarks for RE2-based regex primitives using nanobench.
// Tests realistic workloads from Nix builtins.match/builtins.split operations.

#include <cstdint>
#include <random>
#include <regex>
#include <string>
#include <vector>

#include "../regex.h"

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

namespace regex = straylight::nix::primitives::regex;

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

} // namespace

int main() {
  ankerl::nanobench::Bench bench;
  bench.title("Regex Benchmarks").warmup(100).minEpochIterations(1000).unit("op");

  // ───────────────────────────────────────────────────────────────────────────
  // Test patterns from real Nix code
  // ───────────────────────────────────────────────────────────────────────────

  // Simple literal patterns (common in package name matching)
  const std::string pat_literal = "nixpkgs";

  // Character class patterns (POSIX classes used in Nix)
  const std::string pat_posix_space = "[[:space:]]+([[:upper:]]+)[[:space:]]+";
  const std::string pat_posix_alpha = "([[:alpha:]]+)([[:digit:]]*)";

  // Flake ref patterns (from flakeref.cpp)
  const std::string pat_flakeref = "([a-zA-Z][a-zA-Z0-9+.-]*)://.*";

  // Store path pattern (package name extraction)
  const std::string pat_store_path = "/nix/store/([a-z0-9]{32})-(.*)";

  // Version extraction (common in derivations)
  const std::string pat_version = "([a-zA-Z0-9_-]+)-([0-9][0-9.]*[a-z0-9]*)";

  // Pre-compile patterns (RE2)
  auto re_literal = regex::Regex::compile(pat_literal);
  auto re_posix_space = regex::Regex::compile(pat_posix_space);
  auto re_posix_alpha = regex::Regex::compile(pat_posix_alpha);
  auto re_flakeref = regex::Regex::compile(pat_flakeref);
  auto re_store_path = regex::Regex::compile(pat_store_path);
  auto re_version = regex::Regex::compile(pat_version);

  // Pre-compile patterns (std::regex for comparison)
  std::regex std_literal(pat_literal, std::regex::extended);
  std::regex std_posix_space(pat_posix_space, std::regex::extended);
  std::regex std_posix_alpha(pat_posix_alpha, std::regex::extended);
  std::regex std_flakeref(pat_flakeref, std::regex::extended);
  std::regex std_store_path(pat_store_path, std::regex::extended);
  std::regex std_version(pat_version, std::regex::extended);

  // Test strings
  const std::string str_literal_match = "nixpkgs";
  const std::string str_literal_nomatch = "nixos-config";
  const std::string str_posix_space = "  HELLO   ";
  const std::string str_posix_alpha = "hello123";
  const std::string str_flakeref = "github://NixOS/nixpkgs/master";
  const std::string str_store_path = "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-hello-2.10";
  const std::string str_version = "hello-2.10.1";
  const std::string str_long = generate_random_string(1000);

  // ───────────────────────────────────────────────────────────────────────────
  // Compilation benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("compile/literal (RE2)", [&] {
    auto re = regex::Regex::compile(pat_literal);
    ankerl::nanobench::doNotOptimizeAway(re);
  });

  bench.run("compile/literal (std::regex)", [&] {
    std::regex re(pat_literal, std::regex::extended);
    ankerl::nanobench::doNotOptimizeAway(re);
  });

  bench.run("compile/posix_class (RE2)", [&] {
    auto re = regex::Regex::compile(pat_posix_space);
    ankerl::nanobench::doNotOptimizeAway(re);
  });

  bench.run("compile/posix_class (std::regex)", [&] {
    std::regex re(pat_posix_space, std::regex::extended);
    ankerl::nanobench::doNotOptimizeAway(re);
  });

  bench.run("compile/store_path (RE2)", [&] {
    auto re = regex::Regex::compile(pat_store_path);
    ankerl::nanobench::doNotOptimizeAway(re);
  });

  bench.run("compile/store_path (std::regex)", [&] {
    std::regex re(pat_store_path, std::regex::extended);
    ankerl::nanobench::doNotOptimizeAway(re);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Full match benchmarks (builtins.match semantics)
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("full_match/literal/match (RE2)", [&] {
    auto m = re_literal->full_match(str_literal_match);
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  bench.run("full_match/literal/match (std::regex)", [&] {
    std::cmatch m;
    bool result = std::regex_match(str_literal_match.c_str(), m, std_literal);
    ankerl::nanobench::doNotOptimizeAway(result);
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  bench.run("full_match/literal/nomatch (RE2)", [&] {
    auto m = re_literal->full_match(str_literal_nomatch);
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  bench.run("full_match/literal/nomatch (std::regex)", [&] {
    std::cmatch m;
    bool result = std::regex_match(str_literal_nomatch.c_str(), m, std_literal);
    ankerl::nanobench::doNotOptimizeAway(result);
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  bench.run("full_match/posix_class (RE2)", [&] {
    auto m = re_posix_space->full_match(str_posix_space);
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  bench.run("full_match/posix_class (std::regex)", [&] {
    std::cmatch m;
    bool result = std::regex_match(str_posix_space.c_str(), m, std_posix_space);
    ankerl::nanobench::doNotOptimizeAway(result);
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  bench.run("full_match/store_path (RE2)", [&] {
    auto m = re_store_path->full_match(str_store_path);
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  bench.run("full_match/store_path (std::regex)", [&] {
    std::cmatch m;
    bool result = std::regex_match(str_store_path.c_str(), m, std_store_path);
    ankerl::nanobench::doNotOptimizeAway(result);
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  bench.run("full_match/version (RE2)", [&] {
    auto m = re_version->full_match(str_version);
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  bench.run("full_match/version (std::regex)", [&] {
    std::cmatch m;
    bool result = std::regex_match(str_version.c_str(), m, std_version);
    ankerl::nanobench::doNotOptimizeAway(result);
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Partial match benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("partial_match/literal (RE2)", [&] {
    auto m = re_literal->partial_match("prefix nixpkgs suffix");
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  bench.run("partial_match/literal (std::regex)", [&] {
    std::cmatch m;
    bool result = std::regex_search("prefix nixpkgs suffix", m, std_literal);
    ankerl::nanobench::doNotOptimizeAway(result);
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  bench.run("partial_match/long_string (RE2)", [&] {
    auto m = re_posix_alpha->partial_match(str_long);
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  bench.run("partial_match/long_string (std::regex)", [&] {
    std::cmatch m;
    bool result = std::regex_search(str_long.c_str(), m, std_posix_alpha);
    ankerl::nanobench::doNotOptimizeAway(result);
    ankerl::nanobench::doNotOptimizeAway(m);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Find all benchmarks (builtins.split semantics)
  // ───────────────────────────────────────────────────────────────────────────

  const std::string str_split = "hello123world456test789";

  bench.run("find_all/digits (RE2)", [&] {
    auto re = regex::Regex::compile("([0-9]+)");
    auto matches = re->find_all(str_split);
    ankerl::nanobench::doNotOptimizeAway(matches);
  });

  bench.run("find_all/digits (std::cregex_iterator)", [&] {
    std::regex re("[0-9]+", std::regex::extended);
    std::vector<std::string> matches;
    auto begin = std::cregex_iterator(str_split.c_str(), str_split.c_str() + str_split.size(), re);
    auto end = std::cregex_iterator();
    for (auto it = begin; it != end; ++it) {
      matches.push_back((*it)[0].str());
    }
    ankerl::nanobench::doNotOptimizeAway(matches);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Test-only benchmarks (no captures, fastest path)
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("test/literal (RE2)", [&] {
    bool result = re_literal->test(str_literal_match);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("test/literal (std::regex)", [&] {
    bool result = std::regex_match(str_literal_match.c_str(), std_literal);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("test_partial/literal (RE2)", [&] {
    bool result = re_literal->test_partial("prefix nixpkgs suffix");
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("test_partial/literal (std::regex_search)", [&] {
    bool result = std::regex_search("prefix nixpkgs suffix", std_literal);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Cache benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  regex::Cache cache;
  // Pre-populate cache
  cache.get(pat_literal);
  cache.get(pat_store_path);
  cache.get(pat_version);

  bench.run("cache/hit (RE2)", [&] {
    const auto& re = cache.get(pat_literal);
    ankerl::nanobench::doNotOptimizeAway(&re);
  });

  bench.run("cache/miss (RE2 compile)", [&] {
    regex::Cache fresh_cache;
    const auto& re = fresh_cache.get(pat_literal);
    ankerl::nanobench::doNotOptimizeAway(&re);
  });

  return 0;
}
