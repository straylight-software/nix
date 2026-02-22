// straylight // nix // store // tests
//
// Hash operation benchmarks - measuring real-world performance characteristics
//
// These benchmarks measure hash operations that occur on EVERY store path access:
//   - Hash computation (SHA256 of NAR contents)
//   - Hash parsing (from narinfo files, derivations)
//   - Hash serialization (for display, storage, network transfer)
//   - Nix base32 encoding/decoding (store path computation)
//   - Content address parsing (from narinfo, database)
//
// Performance context:
//   - A typical `nix build` may access 100s-1000s of store paths
//   - Each store path requires hash parsing/serialization
//   - NAR hashing dominates for large packages (100MB+ NARs common)
//   - Parsing/encoding overhead matters for path-heavy operations
//
// Run with: buck2 test //src/nix/store/tests:hash_bench

#include <array>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include "nix/store/content-address.h"
#include "nix/util/base-n.h"
#include "nix/util/base-nix-32.h"
#include "nix/util/hash.h"

using namespace nix;

// =============================================================================
// Test data generators - realistic NAR file sizes
// =============================================================================

namespace {

/// Generate deterministic random data of specified size
/// Uses fixed seed for reproducible benchmarks
auto generate_random_data(size_t size) -> std::string {
  std::mt19937_64 rng(42); // NOLINT: magic number is fixed seed
  std::string data(size, '\0');

  auto* ptr = reinterpret_cast<uint64_t*>(data.data());
  const size_t num_words = size / sizeof(uint64_t);
  for (size_t i = 0; i < num_words; ++i) {
    ptr[i] = rng(); // NOLINT: pointer arithmetic
  }

  // Fill remaining bytes
  for (size_t i = num_words * sizeof(uint64_t); i < size; ++i) {
    data[i] = static_cast<char>(rng() & 0xFF);
  }

  return data;
}

/// Pre-computed test data at realistic NAR sizes
/// Sizes chosen based on real-world Nixpkgs packages:
///   - 1KB: tiny packages, derivation files
///   - 1MB: small packages, scripts
///   - 100MB: large packages (gcc, llvm, etc)
struct test_data_t {
  static constexpr size_t size_1kb = 1024;
  static constexpr size_t size_1mb = 1024 * 1024;
  static constexpr size_t size_100mb = 100 * 1024 * 1024;

  std::string data_1kb;
  std::string data_1mb;
  std::string data_100mb;

  test_data_t()
      : data_1kb(generate_random_data(size_1kb)),
        data_1mb(generate_random_data(size_1mb)),
        data_100mb(generate_random_data(size_100mb)) {}

  static auto instance() -> const test_data_t& {
    static const test_data_t data;
    return data;
  }
};

/// Pre-computed hash strings for parsing benchmarks
/// These match what we'd see in narinfo files, derivations
struct hash_strings_t {
  // SHA256 of empty string in various formats
  static constexpr std::string_view base16 =
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
  static constexpr std::string_view base16_prefixed =
      "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
  static constexpr std::string_view base64 = "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=";
  static constexpr std::string_view sri = "sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=";
  // nix32 encoding of SHA256 empty string
  static constexpr std::string_view nix32 = "0mdqa9w1p6cmli6976v4wi0sw9r4p5prkj7lzfd1877wk11c9c73";

  // Content address strings (from narinfo files)
  static constexpr std::string_view ca_text =
      "text:sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
  static constexpr std::string_view ca_fixed_flat =
      "fixed:sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
  static constexpr std::string_view ca_fixed_nar =
      "fixed:r:sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
};

} // namespace

// =============================================================================
// SHA256 hashing benchmarks - the most critical operation
// =============================================================================

TEST_CASE("SHA256 hash computation", "[benchmark][hash][sha256]") {
  const auto& data = test_data_t::instance();

  // Context: SHA256 is used for:
  //   - NAR content hashing
  //   - Store path computation
  //   - Fixed-output derivation verification

  BENCHMARK("sha256/1KB - derivation-sized") {
    return hash_string(hash_algorithm_t::sha256, data.data_1kb);
  };

  BENCHMARK("sha256/1MB - small package") {
    return hash_string(hash_algorithm_t::sha256, data.data_1mb);
  };

  BENCHMARK("sha256/100MB - large package (gcc, llvm)") {
    return hash_string(hash_algorithm_t::sha256, data.data_100mb);
  };
}

// =============================================================================
// Hash parsing benchmarks - happens on every narinfo/derivation read
// =============================================================================

TEST_CASE("Hash parsing from strings", "[benchmark][hash][parsing]") {
  // Context: Every narinfo file contains hash strings that must be parsed.
  // A binary cache fetch parses 1 narinfo per path.
  // Building nixpkgs may parse 10,000+ narinfo files.

  SECTION("parse base16 (hex) - most common in internal storage") {
    BENCHMARK("parse/base16/unprefixed") {
      return hash_t::parse_non_sri_unprefixed(hash_strings_t::base16, hash_algorithm_t::sha256);
    };

    BENCHMARK("parse/base16/prefixed") {
      return hash_t::parse_any_prefixed(hash_strings_t::base16_prefixed);
    };
  }

  SECTION("parse base32 (nix32) - used in store paths") {
    BENCHMARK("parse/nix32") {
      return hash_t::parse_non_sri_unprefixed(hash_strings_t::nix32, hash_algorithm_t::sha256);
    };
  }

  SECTION("parse base64 - compact representation") {
    BENCHMARK("parse/base64") {
      return hash_t::parse_explicit_format_unprefixed(
          hash_strings_t::base64, hash_algorithm_t::sha256, hash_format_t::base64);
    };
  }

  SECTION("parse SRI - used in flake.lock, fetchurl") {
    BENCHMARK("parse/sri") {
      return hash_t::parse_sri(hash_strings_t::sri);
    };

    BENCHMARK("parse/sri (via parse_any)") {
      return hash_t::parse_any(hash_strings_t::sri, std::nullopt);
    };
  }
}

// =============================================================================
// Hash serialization benchmarks - happens on every narinfo/db write
// =============================================================================

TEST_CASE("Hash serialization to strings", "[benchmark][hash][serialization]") {
  auto hash = hash_string(hash_algorithm_t::sha256, "benchmark test data");

  // Context: Serialization happens when:
  //   - Writing narinfo files to cache
  //   - Storing paths in SQLite database
  //   - Displaying paths to users
  //   - Network protocol messages

  SECTION("serialize without algorithm prefix") {
    BENCHMARK("serialize/base16") {
      return hash.to_string(hash_format_t::base16, false);
    };

    BENCHMARK("serialize/nix32") {
      return hash.to_string(hash_format_t::nix32, false);
    };

    BENCHMARK("serialize/base64") {
      return hash.to_string(hash_format_t::base64, false);
    };

    BENCHMARK("serialize/sri") {
      return hash.to_string(hash_format_t::sri, false);
    };
  }

  SECTION("serialize with algorithm prefix") {
    BENCHMARK("serialize/base16+algo") {
      return hash.to_string(hash_format_t::base16, true);
    };

    BENCHMARK("serialize/nix32+algo") {
      return hash.to_string(hash_format_t::nix32, true);
    };
  }
}

// =============================================================================
// Nix base32 encoding/decoding - critical for store path computation
// =============================================================================

TEST_CASE("Nix base32 encoding/decoding", "[benchmark][hash][nix32]") {
  // Context: Nix base32 (nix32) is used for:
  //   - Store path hash component (the 32-char hash in /nix/store/xxx-name)
  //   - Derivation output path computation
  //   - Every store path access involves nix32 operations

  // 32 bytes = SHA256 hash (most common case)
  std::array<std::byte, 32> sha256_bytes{};
  for (size_t i = 0; i < sha256_bytes.size(); ++i) {
    sha256_bytes[i] = static_cast<std::byte>(i);
  }

  BENCHMARK("nix32/encode/32bytes (SHA256)") {
    return base_nix32_t::encode(sha256_bytes);
  };

  auto nix32_str = base_nix32_t::encode(sha256_bytes);
  BENCHMARK("nix32/decode/52chars") {
    return base_nix32_t::decode(nix32_str);
  };

  // Compare with standard encodings
  BENCHMARK("base16/encode/32bytes") {
    return base16::encode(sha256_bytes);
  };

  auto hex_str = base16::encode(sha256_bytes);
  BENCHMARK("base16/decode/64chars") {
    return base16::decode(hex_str);
  };

  BENCHMARK("base64/encode/32bytes") {
    return base64::encode(sha256_bytes);
  };

  auto b64_str = base64::encode(sha256_bytes);
  BENCHMARK("base64/decode/44chars") {
    return base64::decode(b64_str);
  };
}

// =============================================================================
// Content address parsing - happens on every narinfo read
// =============================================================================

TEST_CASE("Content address parsing", "[benchmark][hash][content-address]") {
  // Context: Content addresses appear in:
  //   - narinfo files (ca field)
  //   - SQLite database (ContentAddresses table)
  //   - Derivation outputs

  BENCHMARK("ca/parse/text") {
    return content_address_t::parse(hash_strings_t::ca_text);
  };

  BENCHMARK("ca/parse/fixed-flat") {
    return content_address_t::parse(hash_strings_t::ca_fixed_flat);
  };

  BENCHMARK("ca/parse/fixed-nar (most common)") {
    return content_address_t::parse(hash_strings_t::ca_fixed_nar);
  };

  // Rendering (for storage/display)
  auto ca = content_address_t::parse(hash_strings_t::ca_fixed_nar);
  BENCHMARK("ca/render") {
    return ca.render();
  };

  // Optional parsing (common in database reads)
  BENCHMARK("ca/parseOpt/valid") {
    return content_address_t::parseOpt(hash_strings_t::ca_fixed_nar);
  };

  BENCHMARK("ca/parseOpt/empty") {
    return content_address_t::parseOpt("");
  };
}

// =============================================================================
// Batch operation benchmarks - simulating real workloads
// =============================================================================

TEST_CASE("Batch hash operations", "[benchmark][hash][batch]") {
  // Context: Real operations process many hashes at once.
  // Measures amortized cost including any warmup/setup.

  constexpr size_t batch_size = 100;

  // Generate batch of hash strings (simulating parsing 100 narinfo files)
  std::vector<std::string> hash_strings;
  hash_strings.reserve(batch_size);
  for (size_t i = 0; i < batch_size; ++i) {
    auto h = hash_string(hash_algorithm_t::sha256, "package-" + std::to_string(i));
    hash_strings.push_back(h.to_string(hash_format_t::sri, false));
  }

  BENCHMARK("batch/parse-100-sri-hashes") {
    std::vector<hash_t> results;
    results.reserve(batch_size);
    for (const auto& s : hash_strings) {
      results.push_back(hash_t::parse_sri(s));
    }
    return results;
  };

  // Generate hashes for serialization batch
  std::vector<hash_t> hashes;
  hashes.reserve(batch_size);
  for (size_t i = 0; i < batch_size; ++i) {
    hashes.push_back(hash_string(hash_algorithm_t::sha256, "package-" + std::to_string(i)));
  }

  BENCHMARK("batch/serialize-100-to-nix32") {
    std::vector<std::string> results;
    results.reserve(batch_size);
    for (const auto& h : hashes) {
      results.push_back(h.to_string(hash_format_t::nix32, false));
    }
    return results;
  };
}

// =============================================================================
// Hash comparison/equality - used in cache lookups, deduplication
// =============================================================================

TEST_CASE("Hash comparison operations", "[benchmark][hash][comparison]") {
  auto hash1 = hash_string(hash_algorithm_t::sha256, "test data 1");
  auto hash2 = hash_string(hash_algorithm_t::sha256, "test data 2");
  auto hash3 = hash_string(hash_algorithm_t::sha256, "test data 1"); // same as hash1

  // Context: Hash comparison used in:
  //   - Cache lookup (checking if path exists)
  //   - Deduplication
  //   - Verification after download

  BENCHMARK("compare/equal") {
    return hash1 == hash3;
  };

  BENCHMARK("compare/not-equal") {
    return hash1 == hash2;
  };

  BENCHMARK("compare/ordering") {
    return hash1 < hash2;
  };

  // std::hash for use in unordered containers
  std::hash<hash_t> hasher;
  BENCHMARK("std::hash") {
    return hasher(hash1);
  };
}
