// straylight::nix::text benchmarks
//
// Benchmarks for rapidfuzz-based fuzzy string matching using nanobench.
// Tests realistic workloads from Nix (attribute suggestions, typo correction).

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <straylight/nix/text/fuzzy.h>

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

namespace fuzzy = straylight::nix::text;

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

// Simulate Nix attribute names (pkgs.*, lib.*, etc.)
std::vector<std::string> generate_nix_attrs(std::size_t count, uint64_t seed = 123) {
  static const char* prefixes[] = {"pkgs.", "lib.", "config.", "options.", "nixos."};
  static const char* names[] = {
      "hello",
      "gcc",
      "python3",
      "nodejs",
      "rustc",
      "cargo",
      "cmake",
      "ninja",
      "meson",
      "llvm",
      "clang",
      "boost",
      "openssl",
      "zlib",
      "curl",
      "git",
      "vim",
      "neovim",
      "emacs",
      "vscode",
      "firefox",
      "chromium",
      "thunderbird",
      "libreoffice",
      "gimp",
      "inkscape",
      "blender",
      "krita",
      "audacity",
      "obs",
      "stdenv",
      "mkShell",
      "mkDerivation",
      "fetchurl",
      "fetchgit",
      "buildInputs",
      "nativeBuildInputs",
      "propagatedBuildInputs",
      "meta",
      "description",
      "homepage",
      "license",
      "maintainers",
  };

  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<std::size_t> prefix_dist(0, std::size(prefixes) - 1);
  std::uniform_int_distribution<std::size_t> name_dist(0, std::size(names) - 1);

  std::vector<std::string> result;
  result.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    result.push_back(std::string(prefixes[prefix_dist(rng)]) + names[name_dist(rng)]);
  }
  return result;
}

// Naive Levenshtein implementation for comparison
std::size_t naive_levenshtein(std::string_view s1, std::string_view s2) {
  const std::size_t m = s1.size();
  const std::size_t n = s2.size();

  std::vector<std::vector<std::size_t>> dp(m + 1, std::vector<std::size_t>(n + 1));

  for (std::size_t i = 0; i <= m; ++i) {
    dp[i][0] = i;
  }
  for (std::size_t j = 0; j <= n; ++j) {
    dp[0][j] = j;
  }

  for (std::size_t i = 1; i <= m; ++i) {
    for (std::size_t j = 1; j <= n; ++j) {
      if (s1[i - 1] == s2[j - 1]) {
        dp[i][j] = dp[i - 1][j - 1];
      } else {
        dp[i][j] = 1 + std::min({dp[i - 1][j], dp[i][j - 1], dp[i - 1][j - 1]});
      }
    }
  }
  return dp[m][n];
}

} // namespace

int main() {
  ankerl::nanobench::Bench bench;
  bench.title("Fuzzy Matching Benchmarks").warmup(100).minEpochIterations(1000).unit("op");

  // ───────────────────────────────────────────────────────────────────────────
  // Test strings
  // ───────────────────────────────────────────────────────────────────────────

  // Short strings (typo correction scenarios)
  const std::string short1 = "hello";
  const std::string short2 = "hallo"; // 1 edit

  // Medium strings (package names)
  const std::string med1 = "buildInputs";
  const std::string med2 = "buildInput"; // missing s

  // Longer strings (paths/descriptions)
  const std::string long1 = "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-hello-2.10";
  const std::string long2 = "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-hello-2.11";

  // Very long (stress test)
  const std::string vlong1 = generate_random_string(200, 1);
  const std::string vlong2 = generate_random_string(200, 2);

  // ───────────────────────────────────────────────────────────────────────────
  // Levenshtein distance benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("levenshtein/short/rapidfuzz", [&] {
    auto d = fuzzy::levenshtein(short1, short2);
    ankerl::nanobench::doNotOptimizeAway(d);
  });

  bench.run("levenshtein/short/naive", [&] {
    auto d = naive_levenshtein(short1, short2);
    ankerl::nanobench::doNotOptimizeAway(d);
  });

  bench.run("levenshtein/medium/rapidfuzz", [&] {
    auto d = fuzzy::levenshtein(med1, med2);
    ankerl::nanobench::doNotOptimizeAway(d);
  });

  bench.run("levenshtein/medium/naive", [&] {
    auto d = naive_levenshtein(med1, med2);
    ankerl::nanobench::doNotOptimizeAway(d);
  });

  bench.run("levenshtein/long/rapidfuzz", [&] {
    auto d = fuzzy::levenshtein(long1, long2);
    ankerl::nanobench::doNotOptimizeAway(d);
  });

  bench.run("levenshtein/long/naive", [&] {
    auto d = naive_levenshtein(long1, long2);
    ankerl::nanobench::doNotOptimizeAway(d);
  });

  bench.run("levenshtein/vlong/rapidfuzz", [&] {
    auto d = fuzzy::levenshtein(vlong1, vlong2);
    ankerl::nanobench::doNotOptimizeAway(d);
  });

  bench.run("levenshtein/vlong/naive", [&] {
    auto d = naive_levenshtein(vlong1, vlong2);
    ankerl::nanobench::doNotOptimizeAway(d);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Levenshtein with cutoff (early termination)
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("levenshtein_cutoff/short/max=1", [&] {
    auto d = fuzzy::levenshtein(short1, short2, 1);
    ankerl::nanobench::doNotOptimizeAway(d);
  });

  bench.run("levenshtein_cutoff/short/max=3", [&] {
    auto d = fuzzy::levenshtein(short1, short2, 3);
    ankerl::nanobench::doNotOptimizeAway(d);
  });

  bench.run("levenshtein_cutoff/long/max=3", [&] {
    auto d = fuzzy::levenshtein(long1, long2, 3);
    ankerl::nanobench::doNotOptimizeAway(d);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Normalized similarity
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("levenshtein_normalized/short", [&] {
    auto d = fuzzy::levenshtein_normalized(short1, short2);
    ankerl::nanobench::doNotOptimizeAway(d);
  });

  bench.run("levenshtein_normalized/long", [&] {
    auto d = fuzzy::levenshtein_normalized(long1, long2);
    ankerl::nanobench::doNotOptimizeAway(d);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Fuzzy ratio (fuzz.ratio)
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("ratio/short", [&] {
    auto r = fuzzy::ratio(short1, short2);
    ankerl::nanobench::doNotOptimizeAway(r);
  });

  bench.run("ratio/long", [&] {
    auto r = fuzzy::ratio(long1, long2);
    ankerl::nanobench::doNotOptimizeAway(r);
  });

  bench.run("partial_ratio/short", [&] {
    auto r = fuzzy::partial_ratio(short1, short2);
    ankerl::nanobench::doNotOptimizeAway(r);
  });

  bench.run("token_sort_ratio/short", [&] {
    auto r = fuzzy::token_sort_ratio("hello world", "world hello");
    ankerl::nanobench::doNotOptimizeAway(r);
  });

  bench.run("token_set_ratio/short", [&] {
    auto r = fuzzy::token_set_ratio("hello hello world", "world hello");
    ankerl::nanobench::doNotOptimizeAway(r);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Best matches (Nix attribute suggestion scenario)
  // ───────────────────────────────────────────────────────────────────────────

  auto attrs_100 = generate_nix_attrs(100);
  auto attrs_1000 = generate_nix_attrs(1000);
  auto attrs_10000 = generate_nix_attrs(10000);

  const std::string typo_query = "buildInput"; // missing 's'

  bench.run("best_matches/100_candidates", [&] {
    auto matches = fuzzy::best_matches(attrs_100, typo_query, 5, 3);
    ankerl::nanobench::doNotOptimizeAway(matches);
  });

  bench.run("best_matches/1000_candidates", [&] {
    auto matches = fuzzy::best_matches(attrs_1000, typo_query, 5, 3);
    ankerl::nanobench::doNotOptimizeAway(matches);
  });

  bench.run("best_matches/10000_candidates", [&] {
    auto matches = fuzzy::best_matches(attrs_10000, typo_query, 5, 3);
    ankerl::nanobench::doNotOptimizeAway(matches);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Best matches with ratio (similarity-based)
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("best_matches_ratio/100_candidates", [&] {
    auto matches = fuzzy::best_matches_ratio(attrs_100, typo_query, 5, 0.7);
    ankerl::nanobench::doNotOptimizeAway(matches);
  });

  bench.run("best_matches_ratio/1000_candidates", [&] {
    auto matches = fuzzy::best_matches_ratio(attrs_1000, typo_query, 5, 0.7);
    ankerl::nanobench::doNotOptimizeAway(matches);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // CachedQuery (batch operations)
  // ───────────────────────────────────────────────────────────────────────────

  fuzzy::CachedQuery cached_query(typo_query);

  bench.run("cached_query/distance", [&] {
    auto d = cached_query.distance("buildInputs");
    ankerl::nanobench::doNotOptimizeAway(d);
  });

  bench.run("cached_query/distance_cutoff", [&] {
    auto d = cached_query.distance("buildInputs", 3);
    ankerl::nanobench::doNotOptimizeAway(d);
  });

  bench.run("cached_query/ratio", [&] {
    auto r = cached_query.ratio("buildInputs");
    ankerl::nanobench::doNotOptimizeAway(r);
  });

  return 0;
}
