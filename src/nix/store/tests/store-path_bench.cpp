// straylight // nix // store // tests
//
// Store path benchmarks - measure performance of core store path operations
//
// These benchmarks measure real-world performance of:
//   1. Parsing store paths from strings
//   2. Validating store path hashes
//   3. Store path to string conversion
//   4. Store path comparison and sorting
//   5. Hash part extraction
//
// Run with: buck2 test //src/nix/store/tests:store-path_bench

#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include "nix/store/path.h"
#include "nix/util/hash.h"

namespace {

// =============================================================================
// Nix base32 alphabet (omits e, o, u, t)
// =============================================================================

constexpr std::string_view k_nix_base32_chars = "0123456789abcdfghijklmnpqrsvwxyz";

// Store path hash length (from nix::store_path_t::HashLen)
constexpr size_t k_hash_len = 32;

// =============================================================================
// Test data generation
// =============================================================================

// Generate a random valid base32 hash (32 characters)
std::string generate_random_hash(std::mt19937& rng) {
  std::string hash;
  hash.reserve(k_hash_len);
  std::uniform_int_distribution<size_t> dist(0, k_nix_base32_chars.size() - 1);
  for (size_t i = 0; i < k_hash_len; ++i) {
    hash.push_back(k_nix_base32_chars[dist(rng)]);
  }
  return hash;
}

// Generate realistic package names like actual Nix packages
std::string generate_package_name(std::mt19937& rng, int /* idx */) {
  static const std::vector<std::string> prefixes = {
      "gcc",      "glibc", "openssl", "python3",   "nodejs",   "rust",    "cmake",
      "ninja",    "llvm",  "clang",   "boost",     "zlib",     "curl",    "git",
      "vim",      "emacs", "bash",    "coreutils", "binutils", "gawk",    "grep",
      "sed",      "tar",   "gzip",    "bzip2",     "xz",       "zstd",    "sqlite",
      "postgres", "redis", "nginx",   "httpd",     "nix",      "nixpkgs", "home-manager"};

  static const std::vector<std::string> versions = {
      "1.0.0",  "2.0.0",  "3.0.0",  "1.2.3",  "2.3.4",  "3.4.5",  "10.0.0", "11.0.0", "12.0.0",
      "13.0.0", "14.0.0", "15.0.0", "3.11.0", "3.12.0", "18.0.0", "20.0.0", "1.79.0", "1.80.0",
      "1.81.0", "3.26.0", "1.11.0", "17.0.0", "1.83.0", "1.3.0",  "8.2.0",  "2.44.0", "9.0.0",
      "29.1.0", "5.2.0",  "9.4.0",  "6.2.0",  "4.4.0",  "2.16.0", "3.5.0",  "1.12.0", "1.22.0",
      "1.5.0",  "3.45.0", "16.0.0", "7.4.0",  "1.26.0", "2.38.0"};

  std::uniform_int_distribution<size_t> prefix_dist(0, prefixes.size() - 1);
  std::uniform_int_distribution<size_t> version_dist(0, versions.size() - 1);

  return prefixes[prefix_dist(rng)] + "-" + versions[version_dist(rng)];
}

// Generate a vector of realistic store path base names
std::vector<std::string> generate_store_path_strings(size_t count, uint32_t seed = 42) {
  std::mt19937 rng(seed);
  std::vector<std::string> paths;
  paths.reserve(count);

  for (size_t i = 0; i < count; ++i) {
    std::string hash = generate_random_hash(rng);
    std::string name = generate_package_name(rng, static_cast<int>(i));
    paths.push_back(hash + "-" + name);
  }

  return paths;
}

// Generate full store paths (with /nix/store/ prefix)
std::vector<std::string> generate_full_store_paths(size_t count, uint32_t seed = 42) {
  auto base_names = generate_store_path_strings(count, seed);
  std::vector<std::string> full_paths;
  full_paths.reserve(count);

  for (const auto& base : base_names) {
    full_paths.push_back("/nix/store/" + base);
  }

  return full_paths;
}

// Generate derivation paths (with .drv suffix)
std::vector<std::string> generate_derivation_paths(size_t count, uint32_t seed = 42) {
  std::mt19937 rng(seed);
  std::vector<std::string> paths;
  paths.reserve(count);

  for (size_t i = 0; i < count; ++i) {
    std::string hash = generate_random_hash(rng);
    std::string name = generate_package_name(rng, static_cast<int>(i));
    paths.push_back(hash + "-" + name + ".drv");
  }

  return paths;
}

// Pre-parse store paths for benchmarks that need existing objects
std::vector<nix::store_path_t> generate_store_paths(size_t count, uint32_t seed = 42) {
  auto strings = generate_store_path_strings(count, seed);
  std::vector<nix::store_path_t> paths;
  paths.reserve(count);

  for (const auto& s : strings) {
    paths.emplace_back(s);
  }

  return paths;
}

} // namespace

// =============================================================================
// Benchmark: Parse store paths from strings
// =============================================================================

TEST_CASE("Store path parsing benchmark", "[benchmark][store-path]") {
  auto path_strings = generate_store_path_strings(10000);

  BENCHMARK("parse 10k paths") {
    std::vector<nix::store_path_t> paths;
    paths.reserve(10000);
    for (const auto& s : path_strings) {
      paths.emplace_back(s);
    }
    return paths.size();
  };

  BENCHMARK("parse 1k paths") {
    std::vector<nix::store_path_t> paths;
    paths.reserve(1000);
    for (size_t i = 0; i < 1000; ++i) {
      paths.emplace_back(path_strings[i]);
    }
    return paths.size();
  };

  // Benchmark single path parsing (amortized)
  BENCHMARK("parse single path (amortized over 1k)") {
    for (size_t i = 0; i < 1000; ++i) {
      nix::store_path_t path(path_strings[i]);
      (void)path;
    }
  };
}

// =============================================================================
// Benchmark: Validate store path hashes
// =============================================================================

TEST_CASE("Store path hash validation benchmark", "[benchmark][store-path]") {
  // Valid paths - should pass validation
  auto valid_strings = generate_store_path_strings(10000);

  // Invalid paths with bad hash characters (e, o, u, t)
  std::vector<std::string> invalid_strings;
  invalid_strings.reserve(1000);
  for (int i = 0; i < 1000; ++i) {
    // Replace first character with invalid base32 char
    std::string s = valid_strings[i];
    s[0] = 'e'; // 'e' is not in nix base32
    invalid_strings.push_back(s);
  }

  BENCHMARK("validate 10k valid paths") {
    size_t valid_count = 0;
    for (const auto& s : valid_strings) {
      try {
        nix::store_path_t path(s);
        ++valid_count;
      } catch (...) {
      }
    }
    return valid_count;
  };

  BENCHMARK("validate 1k invalid paths (expected failures)") {
    size_t invalid_count = 0;
    for (const auto& s : invalid_strings) {
      try {
        nix::store_path_t path(s);
      } catch (...) {
        ++invalid_count;
      }
    }
    return invalid_count;
  };
}

// =============================================================================
// Benchmark: Store path to string conversion
// =============================================================================

TEST_CASE("Store path to string benchmark", "[benchmark][store-path]") {
  auto paths = generate_store_paths(10000);

  BENCHMARK("to_string 10k paths") {
    size_t total_len = 0;
    for (const auto& path : paths) {
      auto s = path.to_string();
      total_len += s.size();
    }
    return total_len;
  };

  BENCHMARK("to_string 1k paths") {
    size_t total_len = 0;
    for (size_t i = 0; i < 1000; ++i) {
      auto s = paths[i].to_string();
      total_len += s.size();
    }
    return total_len;
  };

  // Extract name and hash parts
  BENCHMARK("extract name 10k paths") {
    size_t total_len = 0;
    for (const auto& path : paths) {
      auto name = path.name();
      total_len += name.size();
    }
    return total_len;
  };

  BENCHMARK("extract hash_part 10k paths") {
    size_t total_len = 0;
    for (const auto& path : paths) {
      auto hash = path.hash_part();
      total_len += hash.size();
    }
    return total_len;
  };
}

// =============================================================================
// Benchmark: Store path comparison and sorting
// =============================================================================

TEST_CASE("Store path comparison benchmark", "[benchmark][store-path]") {
  auto paths = generate_store_paths(10000);

  // Shuffle for realistic comparison patterns
  std::mt19937 rng(42);
  auto shuffled = paths;
  std::shuffle(shuffled.begin(), shuffled.end(), rng);

  BENCHMARK("compare 10k path pairs (equality)") {
    size_t equal_count = 0;
    for (size_t i = 0; i < 10000; ++i) {
      if (paths[i] == shuffled[i]) {
        ++equal_count;
      }
    }
    return equal_count;
  };

  BENCHMARK("compare 10k path pairs (ordering)") {
    size_t less_count = 0;
    for (size_t i = 0; i < 10000; ++i) {
      if (paths[i] < shuffled[i]) {
        ++less_count;
      }
    }
    return less_count;
  };

  BENCHMARK("sort 10k paths") {
    auto to_sort = shuffled;
    std::sort(to_sort.begin(), to_sort.end());
    return to_sort.size();
  };

  BENCHMARK("sort 1k paths") {
    std::vector<nix::store_path_t> to_sort(shuffled.begin(), shuffled.begin() + 1000);
    std::sort(to_sort.begin(), to_sort.end());
    return to_sort.size();
  };
}

// =============================================================================
// Benchmark: Hash part extraction
// =============================================================================

TEST_CASE("Store path hash extraction benchmark", "[benchmark][store-path]") {
  auto paths = generate_store_paths(10000);

  BENCHMARK("extract hash_part 10k paths") {
    size_t total_len = 0;
    for (const auto& path : paths) {
      auto hash = path.hash_part();
      total_len += hash.size();
    }
    return total_len;
  };

  BENCHMARK("hash_part + name extraction 10k paths") {
    size_t total_len = 0;
    for (const auto& path : paths) {
      auto hash = path.hash_part();
      auto name = path.name();
      total_len += hash.size() + name.size();
    }
    return total_len;
  };

  // Benchmark hash computation using std::hash
  BENCHMARK("std::hash 10k paths") {
    size_t total_hash = 0;
    std::hash<nix::store_path_t> hasher;
    for (const auto& path : paths) {
      total_hash ^= hasher(path);
    }
    return total_hash;
  };
}

// =============================================================================
// Benchmark: Derivation path operations
// =============================================================================

TEST_CASE("Store path derivation operations benchmark", "[benchmark][store-path]") {
  auto drv_strings = generate_derivation_paths(10000);

  // Parse derivation paths
  std::vector<nix::store_path_t> drv_paths;
  drv_paths.reserve(10000);
  for (const auto& s : drv_strings) {
    drv_paths.emplace_back(s);
  }

  // Mix of derivations and regular paths
  auto regular_paths = generate_store_paths(5000, 123);
  std::vector<nix::store_path_t> mixed_paths;
  mixed_paths.reserve(10000);
  for (size_t i = 0; i < 5000; ++i) {
    mixed_paths.push_back(drv_paths[i]);
    mixed_paths.push_back(regular_paths[i]);
  }

  BENCHMARK("is_derivation 10k derivations") {
    size_t drv_count = 0;
    for (const auto& path : drv_paths) {
      if (path.is_derivation()) {
        ++drv_count;
      }
    }
    return drv_count;
  };

  BENCHMARK("is_derivation 10k mixed paths") {
    size_t drv_count = 0;
    for (const auto& path : mixed_paths) {
      if (path.is_derivation()) {
        ++drv_count;
      }
    }
    return drv_count;
  };
}

// =============================================================================
// Benchmark: Store path set operations
// =============================================================================

TEST_CASE("Store path set operations benchmark", "[benchmark][store-path]") {
  auto paths = generate_store_paths(10000);

  BENCHMARK("insert 10k paths into set") {
    nix::store_path_set_t path_set;
    for (const auto& path : paths) {
      path_set.insert(path);
    }
    return path_set.size();
  };

  BENCHMARK("insert 1k paths into set") {
    nix::store_path_set_t path_set;
    for (size_t i = 0; i < 1000; ++i) {
      path_set.insert(paths[i]);
    }
    return path_set.size();
  };

  // Pre-populate set for lookup benchmarks
  nix::store_path_set_t populated_set(paths.begin(), paths.end());

  // Generate paths not in set for miss benchmarks
  auto other_paths = generate_store_paths(1000, 999);

  BENCHMARK("lookup 10k paths in set (hits)") {
    size_t found_count = 0;
    for (const auto& path : paths) {
      if (populated_set.count(path) > 0) {
        ++found_count;
      }
    }
    return found_count;
  };

  BENCHMARK("lookup 1k paths in set (misses)") {
    size_t found_count = 0;
    for (const auto& path : other_paths) {
      if (populated_set.count(path) > 0) {
        ++found_count;
      }
    }
    return found_count;
  };
}

// =============================================================================
// Benchmark: Random path generation
// =============================================================================

TEST_CASE("Store path random generation benchmark", "[benchmark][store-path]") {
  BENCHMARK("generate 1k random paths") {
    std::vector<nix::store_path_t> paths;
    paths.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
      paths.push_back(nix::store_path_t::random("test-package-" + std::to_string(i)));
    }
    return paths.size();
  };

  BENCHMARK("generate 100 random paths") {
    std::vector<nix::store_path_t> paths;
    paths.reserve(100);
    for (int i = 0; i < 100; ++i) {
      paths.push_back(nix::store_path_t::random("pkg"));
    }
    return paths.size();
  };
}
