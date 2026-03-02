// straylight::nix::text::bench::strings_cmp_bench
//
// Comprehensive comparison benchmarks: straylight strings.h vs nix/util/strings.h
//
// This benchmark compares:
//   - straylight::split_to_strings vs nix::split_string<vector>
//   - straylight::tokenize vs nix::tokenize_string<vector>
//   - straylight::join vs nix::concat_strings_sep
//   - straylight::trim vs manual std::string operations
//
// Expected speedups (based on SIMD acceleration):
//   - split: 2-5x faster (stringzilla SIMD find)
//   - tokenize: 3-6x faster (byteset SIMD scanning)
//   - join: 1.5-2x faster (pre-computed size reservation)
//   - trim: 2-4x faster (SIMD find_first_not_of/find_last_not_of)

#define ANKERL_NANOBENCH_IMPLEMENT
#include <algorithm>
#include <cstring>
#include <iostream>
#include <list>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nanobench.h>

// Straylight primitives
#include "../strings.h"

namespace sz = straylight::nix::text;

// ─────────────────────────────────────────────────────────────────────────────
// Nix-compatible baseline implementations (from nix/util/strings.h)
// These are faithful reimplementations of the nix string functions
// ─────────────────────────────────────────────────────────────────────────────

namespace nix_impl {

// nix::tokenize_string<std::vector<std::string>>
// Splits on any character in separators, drops empty strings
template <class C>
C tokenize_string(std::string_view s, std::string_view separators = " \t\n\r") {
  C result;
  std::string::size_type pos = s.find_first_not_of(separators, 0);
  while (pos != std::string_view::npos) {
    std::string::size_type end = s.find_first_of(separators, pos + 1);
    if (end == std::string_view::npos) {
      end = s.size();
    }
    result.emplace(result.end(), s.substr(pos, end - pos));
    pos = s.find_first_not_of(separators, end);
  }
  return result;
}

// nix::split_string<std::vector<std::string>>
// Splits on delimiter, preserves empty strings
template <class C>
C split_string(std::string_view s, std::string_view separators) {
  C result;
  if (s.empty()) {
    result.emplace(result.end(), "");
    return result;
  }

  std::size_t pos = 0;
  while (pos <= s.size()) {
    std::size_t end = s.find_first_of(separators, pos);
    if (end == std::string_view::npos) {
      result.emplace(result.end(), s.substr(pos));
      break;
    }
    result.emplace(result.end(), s.substr(pos, end - pos));
    pos = end + 1;
  }
  return result;
}

// nix::concat_strings_sep
template <class C>
std::string concat_strings_sep(std::string_view sep, const C& ss) {
  size_t size = 0;
  bool tail = false;
  for (const auto& s : ss) {
    if (tail) {
      size += sep.size();
    }
    size += s.size();
    tail = true;
  }
  std::string s;
  s.reserve(size);
  tail = false;
  for (const auto& i : ss) {
    if (tail) {
      s += sep;
    }
    s += i;
    tail = true;
  }
  return s;
}

// Basic trim implementation (common in nix codebase)
std::string_view trim_left(std::string_view s, std::string_view chars = " \t\n\r") {
  auto pos = s.find_first_not_of(chars);
  if (pos == std::string_view::npos) {
    return {};
  }
  return s.substr(pos);
}

std::string_view trim_right(std::string_view s, std::string_view chars = " \t\n\r") {
  auto pos = s.find_last_not_of(chars);
  if (pos == std::string_view::npos) {
    return {};
  }
  return s.substr(0, pos + 1);
}

std::string_view trim(std::string_view s, std::string_view chars = " \t\n\r") {
  return trim_right(trim_left(s, chars), chars);
}

} // namespace nix_impl

// ─────────────────────────────────────────────────────────────────────────────
// Realistic nix workload data generators
// ─────────────────────────────────────────────────────────────────────────────

namespace workloads {

// Realistic nix store paths (like those seen in /nix/store)
std::vector<std::string> generate_store_paths(std::size_t count) {
  static const char* hashes[] = {
      "3b4p38fk6hgwaf2p6jcci5k93b9cnxc8", "x5lnx4qwiy1kdlhbnavwqglnlncby1z6",
      "16g9kcg05l7rpllkqh549mmybsrpqcj1", "ckryz91bin3c15r6r77k9qy40zhz400l",
      "mjf8jlq9grydcdvyw6hb063x5c34g5gf", "17bmbbcf71q6x8v50q0gbc4k3ya8fg1c",
  };
  static const char* packages[] = {
      "gcc-15.2.0",    "clang-wrapper-19.1.7", "boost-1.87.0",   "ada-3.4.1",  "nanobench-4.3.11",
      "glibc-2.40-66", "openssl-3.3.2",        "python3-3.12.8", "nix-2.25.0", "home-manager-24.11",
  };
  static const char* suffixes[] = {
      "", "/lib", "/lib/x86_64-linux-gnu", "/include", "/bin", "/share/doc", "/lib/libfoo.so.1.2.3",
  };

  std::mt19937 rng(42);
  std::uniform_int_distribution<std::size_t> hash_dist(0, 5);
  std::uniform_int_distribution<std::size_t> pkg_dist(0, 9);
  std::uniform_int_distribution<std::size_t> suffix_dist(0, 6);

  std::vector<std::string> paths;
  paths.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    std::string path = "/nix/store/";
    path += hashes[hash_dist(rng)];
    path += "-";
    path += packages[pkg_dist(rng)];
    path += suffixes[suffix_dist(rng)];
    paths.push_back(std::move(path));
  }
  return paths;
}

// Environment variable content (PATH-like, colon-separated)
std::string generate_path_env(std::size_t components) {
  std::string result;
  for (std::size_t i = 0; i < components; ++i) {
    if (i > 0) {
      result += ":";
    }
    result += "/nix/store/abc" + std::to_string(i) + "-pkg-" + std::to_string(i) + "/bin";
  }
  return result;
}

// Multi-line content for tokenization
std::string generate_multiline(std::size_t lines) {
  std::string result;
  for (std::size_t i = 0; i < lines; ++i) {
    result += "line " + std::to_string(i) + ": some content here\n";
  }
  return result;
}

// Whitespace-heavy content for trimming
std::vector<std::string> generate_trimable_strings(std::size_t count) {
  std::vector<std::string> result;
  result.reserve(count);
  std::mt19937 rng(123);
  std::uniform_int_distribution<int> leading(0, 10);
  std::uniform_int_distribution<int> trailing(0, 10);

  for (std::size_t i = 0; i < count; ++i) {
    std::string s;
    int l = leading(rng);
    int t = trailing(rng);
    for (int j = 0; j < l; ++j) {
      s += "  \t";
    }
    s += "content-" + std::to_string(i);
    for (int j = 0; j < t; ++j) {
      s += " \n\t";
    }
    result.push_back(std::move(s));
  }
  return result;
}

// SSH options string (NIX_SSHOPTS format) - whitespace tokenization
std::string generate_ssh_opts() {
  return "-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "
         "-o ConnectTimeout=30 -i /home/user/.ssh/id_ed25519 "
         "-o ProxyCommand=\"ssh -W %h:%p bastion.example.com\"";
}

// Tab-delimited derivation outputs
std::vector<std::string> generate_drv_outputs(std::size_t count) {
  std::vector<std::string> outputs;
  outputs.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    std::string line = "out\t/nix/store/abc";
    line += std::to_string(i);
    line += "-package-";
    line += std::to_string(i);
    line += ".0.0\tsha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    outputs.push_back(std::move(line));
  }
  return outputs;
}

} // namespace workloads

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: split_string comparison
// nix::split_string<vector> vs straylight::split_to_strings
// ─────────────────────────────────────────────────────────────────────────────

void bench_split_comparison(ankerl::nanobench::Bench& b) {
  auto paths = workloads::generate_store_paths(1000);
  auto path_env = workloads::generate_path_env(50);

  // Path splitting: "/nix/store/hash-pkg/lib" -> ["", "nix", "store", "hash-pkg", "lib"]
  b.run("nix/split_string/paths/1000", [&] {
    for (const auto& path : paths) {
      auto parts = nix_impl::split_string<std::vector<std::string>>(path, "/");
      ankerl::nanobench::doNotOptimizeAway(parts);
    }
  });

  b.run("straylight/split_to_strings/paths/1000", [&] {
    for (const auto& path : paths) {
      auto parts = sz::split_to_strings(path, "/");
      ankerl::nanobench::doNotOptimizeAway(parts);
    }
  });

  // Expected: straylight ~2-3x faster due to SIMD find

  // PATH env splitting (colon-separated, 50 components)
  b.run("nix/split_string/PATH/50", [&] {
    auto parts = nix_impl::split_string<std::vector<std::string>>(path_env, ":");
    ankerl::nanobench::doNotOptimizeAway(parts);
  });

  b.run("straylight/split_to_strings/PATH/50", [&] {
    auto parts = sz::split_to_strings(path_env, ":");
    ankerl::nanobench::doNotOptimizeAway(parts);
  });

  // Tab-delimited (drv outputs)
  auto drv_outputs = workloads::generate_drv_outputs(100);

  b.run("nix/split_string/tab/100", [&] {
    for (const auto& line : drv_outputs) {
      auto parts = nix_impl::split_string<std::vector<std::string>>(line, "\t");
      ankerl::nanobench::doNotOptimizeAway(parts);
    }
  });

  b.run("straylight/split_to_strings/tab/100", [&] {
    for (const auto& line : drv_outputs) {
      auto parts = sz::split_to_strings(line, "\t");
      ankerl::nanobench::doNotOptimizeAway(parts);
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: tokenize_string comparison
// nix::tokenize_string<vector> vs straylight::tokenize
// ─────────────────────────────────────────────────────────────────────────────

void bench_tokenize_comparison(ankerl::nanobench::Bench& b) {
  auto multiline_small = workloads::generate_multiline(10);
  auto multiline_medium = workloads::generate_multiline(100);
  auto multiline_large = workloads::generate_multiline(1000);
  auto ssh_opts = workloads::generate_ssh_opts();

  // Line tokenization (newline separator)
  b.run("nix/tokenize_string/lines/10", [&] {
    auto lines = nix_impl::tokenize_string<std::vector<std::string>>(multiline_small, "\n");
    ankerl::nanobench::doNotOptimizeAway(lines);
  });

  b.run("straylight/tokenize/lines/10", [&] {
    auto lines = sz::tokenize(multiline_small, "\n");
    ankerl::nanobench::doNotOptimizeAway(lines);
  });

  b.run("nix/tokenize_string/lines/100", [&] {
    auto lines = nix_impl::tokenize_string<std::vector<std::string>>(multiline_medium, "\n");
    ankerl::nanobench::doNotOptimizeAway(lines);
  });

  b.run("straylight/tokenize/lines/100", [&] {
    auto lines = sz::tokenize(multiline_medium, "\n");
    ankerl::nanobench::doNotOptimizeAway(lines);
  });

  b.run("nix/tokenize_string/lines/1000", [&] {
    auto lines = nix_impl::tokenize_string<std::vector<std::string>>(multiline_large, "\n");
    ankerl::nanobench::doNotOptimizeAway(lines);
  });

  b.run("straylight/tokenize/lines/1000", [&] {
    auto lines = sz::tokenize(multiline_large, "\n");
    ankerl::nanobench::doNotOptimizeAway(lines);
  });

  // Expected: straylight ~3-5x faster due to SIMD byteset scanning

  // Whitespace tokenization (SSH options)
  b.run("nix/tokenize_string/whitespace/ssh", [&] {
    auto tokens = nix_impl::tokenize_string<std::vector<std::string>>(ssh_opts, " \t\n\r");
    ankerl::nanobench::doNotOptimizeAway(tokens);
  });

  b.run("straylight/tokenize/whitespace/ssh", [&] {
    auto tokens = sz::tokenize(ssh_opts, " \t\n\r");
    ankerl::nanobench::doNotOptimizeAway(tokens);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: concat_strings_sep comparison
// nix::concat_strings_sep vs straylight::join
// ─────────────────────────────────────────────────────────────────────────────

void bench_join_comparison(ankerl::nanobench::Bench& b) {
  // Generate string lists of various sizes
  std::vector<std::string> small_list = {"one", "two", "three", "four", "five"};
  std::vector<std::string> medium_list;
  for (int i = 0; i < 50; ++i) {
    medium_list.push_back("element-" + std::to_string(i));
  }
  std::vector<std::string> large_list;
  for (int i = 0; i < 500; ++i) {
    large_list.push_back("/nix/store/hash" + std::to_string(i) + "-package-" + std::to_string(i));
  }

  // Small list
  b.run("nix/concat_strings_sep/small/5", [&] {
    auto result = nix_impl::concat_strings_sep(":", small_list);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("straylight/join/small/5", [&] {
    auto result = sz::join(":", small_list);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // Medium list
  b.run("nix/concat_strings_sep/medium/50", [&] {
    auto result = nix_impl::concat_strings_sep(":", medium_list);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("straylight/join/medium/50", [&] {
    auto result = sz::join(":", medium_list);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // Large list
  b.run("nix/concat_strings_sep/large/500", [&] {
    auto result = nix_impl::concat_strings_sep(":", large_list);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("straylight/join/large/500", [&] {
    auto result = sz::join(":", large_list);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // Expected: similar performance, both pre-compute size
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: trim comparison
// Manual trim vs straylight::trim (SIMD-accelerated)
// ─────────────────────────────────────────────────────────────────────────────

void bench_trim_comparison(ankerl::nanobench::Bench& b) {
  auto trimable = workloads::generate_trimable_strings(1000);

  b.run("nix/trim/1000", [&] {
    for (const auto& s : trimable) {
      auto result = nix_impl::trim(s);
      ankerl::nanobench::doNotOptimizeAway(result);
    }
  });

  b.run("straylight/trim/1000", [&] {
    for (const auto& s : trimable) {
      auto result = sz::trim(s);
      ankerl::nanobench::doNotOptimizeAway(result);
    }
  });

  // Expected: straylight ~2-3x faster due to SIMD find_first_not_of

  // Trim left only
  b.run("nix/trim_left/1000", [&] {
    for (const auto& s : trimable) {
      auto result = nix_impl::trim_left(s);
      ankerl::nanobench::doNotOptimizeAway(result);
    }
  });

  b.run("straylight/trim_left/1000", [&] {
    for (const auto& s : trimable) {
      auto result = sz::trim_left(s);
      ankerl::nanobench::doNotOptimizeAway(result);
    }
  });

  // Trim right only
  b.run("nix/trim_right/1000", [&] {
    for (const auto& s : trimable) {
      auto result = nix_impl::trim_right(s);
      ankerl::nanobench::doNotOptimizeAway(result);
    }
  });

  b.run("straylight/trim_right/1000", [&] {
    for (const auto& s : trimable) {
      auto result = sz::trim_right(s);
      ankerl::nanobench::doNotOptimizeAway(result);
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
  std::cout << "String Operations Comparison: nix/util/strings.h vs straylight strings.h\n";
  std::cout << "=========================================================================\n";
#ifdef __AVX512F__
  std::cout << "AVX-512: ENABLED\n";
#else
  std::cout << "AVX-512: disabled (use -march=znver5 to enable)\n";
#endif
#ifdef __AVX2__
  std::cout << "AVX2: ENABLED\n";
#endif
  std::cout << "\n";
  std::cout << "Expected speedups:\n";
  std::cout << "  - split: 2-5x (SIMD find)\n";
  std::cout << "  - tokenize: 3-6x (SIMD byteset scan)\n";
  std::cout << "  - join: ~1x (similar algorithm)\n";
  std::cout << "  - trim: 2-4x (SIMD find_first/last_not_of)\n";
  std::cout << "\n";

  ankerl::nanobench::Bench b;
  b.title("Nix vs Straylight String Operations");
  b.warmup(100);
  b.minEpochIterations(100);
  b.relative(true);

  std::cout << "=== Split String Comparison ===\n";
  bench_split_comparison(b);

  std::cout << "\n=== Tokenize String Comparison ===\n";
  bench_tokenize_comparison(b);

  std::cout << "\n=== Join/Concat Comparison ===\n";
  bench_join_comparison(b);

  std::cout << "\n=== Trim Comparison ===\n";
  bench_trim_comparison(b);

  return 0;
}
