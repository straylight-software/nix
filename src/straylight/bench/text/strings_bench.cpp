// straylight::nix::text::bench::strings_bench
//
// Microbenchmarks comparing string operations with realistic nix workloads:
//   - std::string_view (baseline)
//   - stringzilla (SIMD-accelerated)
//
// Build with -march=znver5 for AVX-512 support on Zen 5
//
// Uses ankerl::nanobench for high-quality microbenchmarking

#define ANKERL_NANOBENCH_IMPLEMENT
#include <algorithm>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <nanobench.h>
#include <straylight/nix/text/strings.h>

namespace sz = straylight::nix::text;

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
      "2izjh733byjickw9719nnkckxd63ghpm", "83bjslv61zdf6n2j40357y2dnra30p9f",
  };
  static const char* packages[] = {
      "gcc-15.2.0",        "clang-wrapper-19.1.7",  "boost-1.87.0",
      "ada-3.4.1",         "rapidcheck-0-unstable", "nanobench-4.3.11",
      "stringzilla-4.5.1", "glibc-2.40-66",         "openssl-3.3.2",
      "python3-3.12.8",    "rustc-1.83.0",          "nodejs-22.12.0",
      "nix-2.25.0",        "nixpkgs-unstable",      "home-manager-24.11",
  };
  static const char* suffixes[] = {
      "",
      "/lib",
      "/lib/x86_64-linux-gnu",
      "/include",
      "/include/stringzilla",
      "/bin",
      "/share/doc",
      "/lib/libfoo.so.1.2.3",
      "/lib/pkgconfig/foo.pc",
  };

  std::mt19937 rng(42);
  std::uniform_int_distribution<std::size_t> hash_dist(0, 7);
  std::uniform_int_distribution<std::size_t> pkg_dist(0, 14);
  std::uniform_int_distribution<std::size_t> suffix_dist(0, 8);

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

// Realistic flake references (github:owner/repo, path:./local, etc.)
std::vector<std::string> generate_flake_refs(std::size_t count) {
  static const char* refs[] = {
      "github:NixOS/nixpkgs/nixos-unstable",
      "github:nix-community/home-manager/master",
      "github:numtide/flake-utils",
      "github:hercules-ci/flake-parts",
      "path:/home/user/src/my-project",
      "path:./.",
      "git+https://github.com/owner/repo.git?ref=main&rev=abc123",
      "git+ssh://git@github.com/owner/private-repo.git",
      "tarball+https://github.com/owner/repo/archive/refs/tags/v1.0.0.tar.gz",
      "file+https://example.com/some-file.tar.gz",
      "indirect:nixpkgs",
      "indirect:home-manager",
  };

  std::mt19937 rng(123);
  std::uniform_int_distribution<std::size_t> dist(0, 11);

  std::vector<std::string> result;
  result.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    result.push_back(refs[dist(rng)]);
  }
  return result;
}

// Binary cache info content (nix-cache-info format)
std::string generate_cache_info() {
  return "StoreDir: /nix/store\n"
         "WantMassQuery: 1\n"
         "Priority: 40\n"
         "CompressionType: zstd\n"
         "AvailableCompressors: xz zstd\n";
}

// Derivation output lines (from .drv files or daemon communication)
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

// SSH options string (NIX_SSHOPTS format)
std::string generate_ssh_opts() {
  return "-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "
         "-o ConnectTimeout=30 -i /home/user/.ssh/id_ed25519 "
         "-o ProxyCommand=\"ssh -W %h:%p bastion.example.com\"";
}

// Environment variable content (PATH-like)
std::string generate_path_env() {
  return "/nix/store/abc123-gcc-15.2.0/bin:"
         "/nix/store/def456-coreutils-9.5/bin:"
         "/nix/store/ghi789-bash-5.2/bin:"
         "/nix/store/jkl012-findutils-4.9/bin:"
         "/nix/store/mno345-gnugrep-3.11/bin:"
         "/nix/store/pqr678-gawk-5.3/bin:"
         "/nix/store/stu901-gnused-4.9/bin:"
         "/home/user/.local/bin:"
         "/usr/local/bin:"
         "/usr/bin:"
         "/bin";
}

// Git LFS pointer file content
std::string generate_git_lfs_content() {
  std::string content;
  for (int i = 0; i < 50; ++i) {
    content += "oid sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
    content += "size 12345678\n";
    content += "\n";
  }
  return content;
}

// Mercurial status output
std::string generate_hg_status() {
  std::string content;
  for (int i = 0; i < 100; ++i) {
    content += "M src/file" + std::to_string(i) + ".cpp\n";
    content += "A tests/test" + std::to_string(i) + ".cpp\n";
    content += "? build/output" + std::to_string(i) + ".o\n";
  }
  return content;
}

} // namespace workloads

// ─────────────────────────────────────────────────────────────────────────────
// std::string_view baseline implementations
// ─────────────────────────────────────────────────────────────────────────────

namespace std_impl {

std::size_t find(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle);
}

bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool starts_with(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

std::vector<std::string> split(std::string_view s, std::string_view delimiter) {
  std::vector<std::string> result;
  std::size_t pos = 0;
  std::size_t prev = 0;
  while ((pos = s.find(delimiter, prev)) != std::string_view::npos) {
    result.emplace_back(s.substr(prev, pos - prev));
    prev = pos + delimiter.size();
  }
  result.emplace_back(s.substr(prev));
  return result;
}

std::vector<std::string> tokenize(std::string_view s, std::string_view separators) {
  std::vector<std::string> result;
  auto pos = s.find_first_not_of(separators, 0);
  while (pos != std::string_view::npos) {
    auto end_pos = s.find_first_of(separators, pos + 1);
    if (end_pos == std::string_view::npos) {
      end_pos = s.size();
    }
    result.emplace_back(s.substr(pos, end_pos - pos));
    pos = s.find_first_not_of(separators, end_pos);
  }
  return result;
}

std::string replace_all(std::string_view s, std::string_view from, std::string_view to) {
  std::string result{s};
  if (from.empty()) {
    return result;
  }
  std::size_t pos = 0;
  while ((pos = result.find(from, pos)) != std::string::npos) {
    result.replace(pos, from.size(), to);
    pos += to.size();
  }
  return result;
}

} // namespace std_impl

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Path splitting (splitString<vector>(path, "/"))
// This is one of the most common operations in nix
// ─────────────────────────────────────────────────────────────────────────────

void bench_path_splitting(ankerl::nanobench::Bench& b) {
  auto paths = workloads::generate_store_paths(1000);

  b.run("std/split_paths/1000", [&] {
    for (const auto& path : paths) {
      auto parts = std_impl::split(path, "/");
      ankerl::nanobench::doNotOptimizeAway(parts);
    }
  });

  b.run("sz/split_paths/1000", [&] {
    for (const auto& path : paths) {
      auto parts = sz::split_to_strings(path, "/");
      ankerl::nanobench::doNotOptimizeAway(parts);
    }
  });

  // Zero-copy version (views only)
  b.run("sz/split_paths_views/1000", [&] {
    for (const auto& path : paths) {
      auto parts = sz::split_to_views(path, "/");
      ankerl::nanobench::doNotOptimizeAway(parts);
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Line tokenization (tokenizeString(content, "\n"))
// Common for parsing multi-line responses
// ─────────────────────────────────────────────────────────────────────────────

void bench_line_tokenization(ankerl::nanobench::Bench& b) {
  auto cache_info = workloads::generate_cache_info();
  auto hg_status = workloads::generate_hg_status();
  auto lfs_content = workloads::generate_git_lfs_content();

  // Small content (cache info ~100 bytes)
  b.run("std/tokenize_lines/small", [&] {
    auto lines = std_impl::tokenize(cache_info, "\n");
    ankerl::nanobench::doNotOptimizeAway(lines);
  });

  b.run("sz/tokenize_lines/small", [&] {
    auto lines = sz::tokenize(cache_info, "\n");
    ankerl::nanobench::doNotOptimizeAway(lines);
  });

  // Medium content (hg status ~5KB, 300 lines)
  b.run("std/tokenize_lines/medium", [&] {
    auto lines = std_impl::tokenize(hg_status, "\n");
    ankerl::nanobench::doNotOptimizeAway(lines);
  });

  b.run("sz/tokenize_lines/medium", [&] {
    auto lines = sz::tokenize(hg_status, "\n");
    ankerl::nanobench::doNotOptimizeAway(lines);
  });

  // Large content (git lfs ~8KB, 150 lines)
  b.run("std/tokenize_lines/large", [&] {
    auto lines = std_impl::tokenize(lfs_content, "\n");
    ankerl::nanobench::doNotOptimizeAway(lines);
  });

  b.run("sz/tokenize_lines/large", [&] {
    auto lines = sz::tokenize(lfs_content, "\n");
    ankerl::nanobench::doNotOptimizeAway(lines);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Whitespace tokenization (tokenizeString(s) with default seps)
// Common for parsing command lines and env vars
// ─────────────────────────────────────────────────────────────────────────────

void bench_whitespace_tokenization(ankerl::nanobench::Bench& b) {
  auto ssh_opts = workloads::generate_ssh_opts();

  b.run("std/tokenize_ws/ssh_opts", [&] {
    auto tokens = std_impl::tokenize(ssh_opts, " \t\n\r");
    ankerl::nanobench::doNotOptimizeAway(tokens);
  });

  b.run("sz/tokenize_ws/ssh_opts", [&] {
    auto tokens = sz::tokenize(ssh_opts, " \t\n\r");
    ankerl::nanobench::doNotOptimizeAway(tokens);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Colon-separated splitting (PATH env var)
// ─────────────────────────────────────────────────────────────────────────────

void bench_path_env_splitting(ankerl::nanobench::Bench& b) {
  auto path_env = workloads::generate_path_env();

  b.run("std/split_path_env", [&] {
    auto parts = std_impl::split(path_env, ":");
    ankerl::nanobench::doNotOptimizeAway(parts);
  });

  b.run("sz/split_path_env", [&] {
    auto parts = sz::split_to_strings(path_env, ":");
    ankerl::nanobench::doNotOptimizeAway(parts);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Tab-delimited parsing (eval-cache, drv outputs)
// ─────────────────────────────────────────────────────────────────────────────

void bench_tab_delimited(ankerl::nanobench::Bench& b) {
  auto drv_outputs = workloads::generate_drv_outputs(100);

  b.run("std/split_tab/100", [&] {
    for (const auto& line : drv_outputs) {
      auto parts = std_impl::split(line, "\t");
      ankerl::nanobench::doNotOptimizeAway(parts);
    }
  });

  b.run("sz/split_tab/100", [&] {
    for (const auto& line : drv_outputs) {
      auto parts = sz::split_to_strings(line, "\t");
      ankerl::nanobench::doNotOptimizeAway(parts);
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Prefix/suffix checks (hasPrefix, hasSuffix)
// Very common for path and scheme checking
// ─────────────────────────────────────────────────────────────────────────────

void bench_prefix_suffix(ankerl::nanobench::Bench& b) {
  auto paths = workloads::generate_store_paths(1000);
  auto flake_refs = workloads::generate_flake_refs(1000);

  // Check /nix/store/ prefix
  b.run("std/starts_with/nix_store", [&] {
    std::size_t count = 0;
    for (const auto& path : paths) {
      if (std_impl::starts_with(path, "/nix/store/")) {
        ++count;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(count);
  });

  b.run("sz/starts_with/nix_store", [&] {
    std::size_t count = 0;
    for (const auto& path : paths) {
      if (sz::starts_with(path, "/nix/store/")) {
        ++count;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(count);
  });

  // Check .drv suffix
  b.run("std/ends_with/drv", [&] {
    std::size_t count = 0;
    for (const auto& path : paths) {
      if (std_impl::ends_with(path, ".drv")) {
        ++count;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(count);
  });

  b.run("sz/ends_with/drv", [&] {
    std::size_t count = 0;
    for (const auto& path : paths) {
      if (sz::ends_with(path, ".drv")) {
        ++count;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(count);
  });

  // Check github: prefix in flake refs
  b.run("std/starts_with/github", [&] {
    std::size_t count = 0;
    for (const auto& ref : flake_refs) {
      if (std_impl::starts_with(ref, "github:")) {
        ++count;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(count);
  });

  b.run("sz/starts_with/github", [&] {
    std::size_t count = 0;
    for (const auto& ref : flake_refs) {
      if (sz::starts_with(ref, "github:")) {
        ++count;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(count);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Contains/find operations
// ─────────────────────────────────────────────────────────────────────────────

void bench_contains(ankerl::nanobench::Bench& b) {
  auto paths = workloads::generate_store_paths(1000);
  auto hg_status = workloads::generate_hg_status();

  // Search for hash in paths
  b.run("std/contains/hash_in_paths", [&] {
    std::size_t count = 0;
    for (const auto& path : paths) {
      if (std_impl::contains(path, "3b4p38fk")) {
        ++count;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(count);
  });

  b.run("sz/contains/hash_in_paths", [&] {
    std::size_t count = 0;
    for (const auto& path : paths) {
      if (sz::contains(path, "3b4p38fk")) {
        ++count;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(count);
  });

  // Search in larger content
  b.run("std/find/in_hg_status", [&] {
    auto pos = std_impl::find(hg_status, "test50.cpp");
    ankerl::nanobench::doNotOptimizeAway(pos);
  });

  b.run("sz/find/in_hg_status", [&] {
    auto pos = sz::find(hg_status, "test50.cpp");
    ankerl::nanobench::doNotOptimizeAway(pos);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: String replacement
// ─────────────────────────────────────────────────────────────────────────────

void bench_replace(ankerl::nanobench::Bench& b) {
  auto hg_status = workloads::generate_hg_status();

  // Replace .cpp with .hpp
  b.run("std/replace/cpp_to_hpp", [&] {
    auto result = std_impl::replace_all(hg_status, ".cpp", ".hpp");
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("sz/replace/cpp_to_hpp", [&] {
    auto result = sz::replace_all(hg_status, ".cpp", ".hpp");
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // Replace newlines (common normalization)
  b.run("std/replace/crlf_to_lf", [&] {
    // First add some \r\n
    std::string content = std_impl::replace_all(hg_status, "\n", "\r\n");
    auto result = std_impl::replace_all(content, "\r\n", "\n");
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("sz/replace/crlf_to_lf", [&] {
    std::string content = sz::replace_all(hg_status, "\n", "\r\n");
    auto result = sz::replace_all(content, "\r\n", "\n");
    ankerl::nanobench::doNotOptimizeAway(result);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
  std::cout << "String Operations Benchmark: std vs stringzilla\n";
  std::cout << "================================================\n";
  std::cout << "Realistic nix workloads\n";
#ifdef __AVX512F__
  std::cout << "AVX-512: ENABLED\n";
#else
  std::cout << "AVX-512: disabled (use -march=znver5 to enable)\n";
#endif
#ifdef __AVX2__
  std::cout << "AVX2: ENABLED\n";
#endif
  std::cout << "\n";

  ankerl::nanobench::Bench b;
  b.title("Nix String Operations");
  b.warmup(100);
  b.minEpochIterations(100);
  b.relative(true);

  std::cout << "=== Path Splitting (splitString(path, \"/\")) ===\n";
  bench_path_splitting(b);

  std::cout << "\n=== Line Tokenization (tokenizeString(content, \"\\n\")) ===\n";
  bench_line_tokenization(b);

  std::cout << "\n=== Whitespace Tokenization (tokenizeString(s)) ===\n";
  bench_whitespace_tokenization(b);

  std::cout << "\n=== PATH Env Splitting (split(PATH, \":\")) ===\n";
  bench_path_env_splitting(b);

  std::cout << "\n=== Tab-Delimited Parsing (eval-cache, drv outputs) ===\n";
  bench_tab_delimited(b);

  std::cout << "\n=== Prefix/Suffix Checks (hasPrefix, hasSuffix) ===\n";
  bench_prefix_suffix(b);

  std::cout << "\n=== Contains/Find Operations ===\n";
  bench_contains(b);

  std::cout << "\n=== String Replacement ===\n";
  bench_replace(b);

  return 0;
}
