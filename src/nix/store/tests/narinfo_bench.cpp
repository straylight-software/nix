// straylight // nix // store // tests
//
// NAR Info Performance Benchmarks
//
// Benchmarks critical path operations for binary cache fetches:
//   1. Parse narinfo from string (cache.nixos.org format)
//   2. Parse narinfo with multiple signatures
//   3. Parse narinfo with many references (100+ deps)
//   4. Serialize narinfo to string
//   5. Signature verification
//
// Every binary cache fetch parses narinfo - this is critical path performance.

#include <random>
#include <string>
#include <vector>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include "nix/store/nar-info.h"
#include "nix/store/store-dir-config.h"
#include "nix/util/signature/local-keys.h"

namespace {

// =============================================================================
// Test Data Generators - Realistic cache.nixos.org format
// =============================================================================

// Valid 32-character base32 hash (nix store path hash)
constexpr const char* HASH_CHARS = "0123456789abcdfghijklmnpqrsvwxyz";

// Generate a random 32-char nix32 hash
std::string generate_nix32_hash(std::mt19937& rng) {
  std::string hash(32, '\0');
  std::uniform_int_distribution<> dist(0, 31);
  for (int i = 0; i < 32; ++i) {
    hash[i] = HASH_CHARS[dist(rng)];
  }
  return hash;
}

// Generate a valid SHA256 hash in hex format (64 chars)
std::string generate_sha256_hex(std::mt19937& rng) {
  static const char* HEX = "0123456789abcdef";
  std::string hash(64, '\0');
  std::uniform_int_distribution<> dist(0, 15);
  for (int i = 0; i < 64; ++i) {
    hash[i] = HEX[dist(rng)];
  }
  return hash;
}

// Generate a base64 signature (86 chars for ed25519)
std::string generate_base64_sig(std::mt19937& rng) {
  static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string sig(86, '\0');
  std::uniform_int_distribution<> dist(0, 63);
  for (int i = 0; i < 86; ++i) {
    sig[i] = B64[dist(rng)];
  }
  return sig;
}

nix::store_dir_config_t make_store_config() {
  static const std::string store_dir = "/nix/store";
  return nix::store_dir_config_t{store_dir};
}

// Generate realistic cache.nixos.org style narinfo
// Typical package: hello, curl, openssl, etc.
std::string generate_typical_narinfo(std::mt19937& rng) {
  auto hash = generate_nix32_hash(rng);
  auto nar_hash = generate_sha256_hex(rng);
  auto file_hash = generate_sha256_hex(rng);
  auto dep1_hash = generate_nix32_hash(rng);
  auto dep2_hash = generate_nix32_hash(rng);
  auto drv_hash = generate_nix32_hash(rng);
  auto sig = generate_base64_sig(rng);

  std::string result;
  result += "store_path_t: /nix/store/" + hash + "-hello-2.12.1\n";
  result += "URL: nar/" + generate_sha256_hex(rng).substr(0, 32) + ".nar.xz\n";
  result += "Compression: xz\n";
  result += "FileHash: sha256:" + file_hash + "\n";
  result += "FileSize: 45678\n";
  result += "NarHash: sha256:" + nar_hash + "\n";
  result += "NarSize: 123456\n";
  result += "References: " + dep1_hash + "-glibc-2.38 " + dep2_hash + "-gcc-libs-13.2.0\n";
  result += "Deriver: " + drv_hash + "-hello-2.12.1.drv\n";
  result += "Sig: cache.nixos.org-1:" + sig + "\n";
  return result;
}

// Generate narinfo with multiple signatures (multi-cache scenario)
std::string generate_multi_sig_narinfo(std::mt19937& rng, int sig_count) {
  auto hash = generate_nix32_hash(rng);
  auto nar_hash = generate_sha256_hex(rng);
  auto file_hash = generate_sha256_hex(rng);

  std::string result;
  result += "store_path_t: /nix/store/" + hash + "-multi-sig-pkg-1.0\n";
  result += "URL: nar/" + generate_sha256_hex(rng).substr(0, 32) + ".nar.zstd\n";
  result += "Compression: zstd\n";
  result += "FileHash: sha256:" + file_hash + "\n";
  result += "FileSize: 98765\n";
  result += "NarHash: sha256:" + nar_hash + "\n";
  result += "NarSize: 234567\n";
  result += "References: \n";

  // Add multiple signatures
  for (int i = 0; i < sig_count; ++i) {
    result +=
        "Sig: cache" + std::to_string(i) + ".example.org-1:" + generate_base64_sig(rng) + "\n";
  }
  return result;
}

// Generate narinfo with many references (heavy dependency package like chromium)
std::string generate_many_refs_narinfo(std::mt19937& rng, int ref_count) {
  auto hash = generate_nix32_hash(rng);
  auto nar_hash = generate_sha256_hex(rng);
  auto file_hash = generate_sha256_hex(rng);

  std::string result;
  result += "store_path_t: /nix/store/" + hash + "-chromium-120.0.6099.129\n";
  result += "URL: nar/" + generate_sha256_hex(rng).substr(0, 32) + ".nar.xz\n";
  result += "Compression: xz\n";
  result += "FileHash: sha256:" + file_hash + "\n";
  result += "FileSize: 567890123\n";
  result += "NarHash: sha256:" + nar_hash + "\n";
  result += "NarSize: 1234567890\n";
  result += "References:";

  // Add many references
  for (int i = 0; i < ref_count; ++i) {
    result += " " + generate_nix32_hash(rng) + "-dep" + std::to_string(i);
  }
  result += "\n";
  result += "Deriver: " + generate_nix32_hash(rng) + "-chromium-120.0.6099.129.drv\n";
  result += "Sig: cache.nixos.org-1:" + generate_base64_sig(rng) + "\n";
  return result;
}

// Generate a minimal narinfo (fastest possible parse)
std::string generate_minimal_narinfo(std::mt19937& rng) {
  auto hash = generate_nix32_hash(rng);
  auto nar_hash = generate_sha256_hex(rng);

  std::string result;
  result += "store_path_t: /nix/store/" + hash + "-minimal-1.0\n";
  result += "URL: nar/min.nar\n";
  result += "NarHash: sha256:" + nar_hash + "\n";
  result += "NarSize: 1024\n";
  return result;
}

// Build a nar_info_t programmatically for serialization benchmarks
nix::nar_info_t build_narinfo_for_serialization(const nix::store_dir_config_t& config,
                                                std::mt19937& rng, int ref_count, int sig_count) {
  auto hash = generate_nix32_hash(rng);
  auto path = config.parseStorePath("/nix/store/" + hash + "-bench-pkg-1.0");
  auto nar_hash = nix::Hash::parse_any_prefixed("sha256:" + generate_sha256_hex(rng));
  auto file_hash = nix::Hash::parse_any_prefixed("sha256:" + generate_sha256_hex(rng));

  nix::nar_info_t info(config, std::move(path), nar_hash);
  info.url = "nar/" + generate_sha256_hex(rng).substr(0, 32) + ".nar.xz";
  info.compression = "xz";
  info.fileHash = file_hash;
  info.file_size = 123456;
  info.nar_size = 987654;

  for (int i = 0; i < ref_count; ++i) {
    info.references.insert(
        nix::store_path_t(generate_nix32_hash(rng) + "-ref" + std::to_string(i)));
  }

  for (int i = 0; i < sig_count; ++i) {
    info.sigs.insert("key" + std::to_string(i) + ":" + generate_base64_sig(rng));
  }

  return info;
}

} // namespace

// =============================================================================
// Benchmark: Parse typical narinfo (cache.nixos.org format)
// =============================================================================

TEST_CASE("narinfo benchmark: parse typical", "[benchmark][store][narinfo]") {
  std::mt19937 rng(42);
  auto config = make_store_config();

  // Pre-generate test data
  std::vector<std::string> narinfos;
  for (int i = 0; i < 100; ++i) {
    narinfos.push_back(generate_typical_narinfo(rng));
  }

  int idx = 0;

  BENCHMARK("parse typical narinfo (2 refs, 1 sig)") {
    auto& input = narinfos[idx++ % narinfos.size()];
    auto info = nix::nar_info_t(config, input, "bench.narinfo");
    return info.path.name();
  };
}

// =============================================================================
// Benchmark: Parse narinfo with multiple signatures
// =============================================================================

TEST_CASE("narinfo benchmark: parse multi-signature", "[benchmark][store][narinfo]") {
  std::mt19937 rng(42);
  auto config = make_store_config();

  // 3 signatures (common for multi-cache setups)
  std::vector<std::string> narinfos_3sig;
  for (int i = 0; i < 100; ++i) {
    narinfos_3sig.push_back(generate_multi_sig_narinfo(rng, 3));
  }

  // 10 signatures (extreme case)
  std::vector<std::string> narinfos_10sig;
  for (int i = 0; i < 100; ++i) {
    narinfos_10sig.push_back(generate_multi_sig_narinfo(rng, 10));
  }

  int idx = 0;

  BENCHMARK("parse narinfo (3 signatures)") {
    auto& input = narinfos_3sig[idx++ % narinfos_3sig.size()];
    auto info = nix::nar_info_t(config, input, "bench.narinfo");
    return info.sigs.size();
  };

  BENCHMARK("parse narinfo (10 signatures)") {
    auto& input = narinfos_10sig[idx++ % narinfos_10sig.size()];
    auto info = nix::nar_info_t(config, input, "bench.narinfo");
    return info.sigs.size();
  };
}

// =============================================================================
// Benchmark: Parse narinfo with many references
// =============================================================================

TEST_CASE("narinfo benchmark: parse many references", "[benchmark][store][narinfo]") {
  std::mt19937 rng(42);
  auto config = make_store_config();

  // 50 references (moderate - many libraries)
  std::vector<std::string> narinfos_50ref;
  for (int i = 0; i < 50; ++i) {
    narinfos_50ref.push_back(generate_many_refs_narinfo(rng, 50));
  }

  // 100 references (chromium-like)
  std::vector<std::string> narinfos_100ref;
  for (int i = 0; i < 50; ++i) {
    narinfos_100ref.push_back(generate_many_refs_narinfo(rng, 100));
  }

  // 200 references (extreme case)
  std::vector<std::string> narinfos_200ref;
  for (int i = 0; i < 20; ++i) {
    narinfos_200ref.push_back(generate_many_refs_narinfo(rng, 200));
  }

  int idx = 0;

  BENCHMARK("parse narinfo (50 references)") {
    auto& input = narinfos_50ref[idx++ % narinfos_50ref.size()];
    auto info = nix::nar_info_t(config, input, "bench.narinfo");
    return info.references.size();
  };

  BENCHMARK("parse narinfo (100 references)") {
    auto& input = narinfos_100ref[idx++ % narinfos_100ref.size()];
    auto info = nix::nar_info_t(config, input, "bench.narinfo");
    return info.references.size();
  };

  BENCHMARK("parse narinfo (200 references)") {
    auto& input = narinfos_200ref[idx++ % narinfos_200ref.size()];
    auto info = nix::nar_info_t(config, input, "bench.narinfo");
    return info.references.size();
  };
}

// =============================================================================
// Benchmark: Serialize narinfo to string
// =============================================================================

TEST_CASE("narinfo benchmark: serialize", "[benchmark][store][narinfo]") {
  std::mt19937 rng(42);
  auto config = make_store_config();

  // Build narinfo objects for serialization
  auto info_small = build_narinfo_for_serialization(config, rng, 2, 1);
  auto info_medium = build_narinfo_for_serialization(config, rng, 20, 2);
  auto info_large = build_narinfo_for_serialization(config, rng, 100, 3);

  BENCHMARK("serialize narinfo (2 refs, 1 sig)") {
    auto output = info_small.to_string(config);
    return output.size();
  };

  BENCHMARK("serialize narinfo (20 refs, 2 sigs)") {
    auto output = info_medium.to_string(config);
    return output.size();
  };

  BENCHMARK("serialize narinfo (100 refs, 3 sigs)") {
    auto output = info_large.to_string(config);
    return output.size();
  };
}

// =============================================================================
// Benchmark: Signature verification
// =============================================================================

TEST_CASE("narinfo benchmark: signature verification", "[benchmark][store][narinfo]") {
  std::mt19937 rng(42);
  auto config = make_store_config();

  // Generate a real signing key pair
  auto secret_key = nix::secret_key_t::generate("bench-key-1");
  auto public_key = secret_key.to_public_key();
  nix::public_keys_t trusted_keys;
  trusted_keys.emplace(public_key.name, public_key);

  // Build narinfo and sign it properly
  auto hash = generate_nix32_hash(rng);
  auto path = config.parseStorePath("/nix/store/" + hash + "-signed-pkg-1.0");
  auto nar_hash = nix::Hash::parse_any_prefixed("sha256:" + generate_sha256_hex(rng));
  auto file_hash = nix::Hash::parse_any_prefixed("sha256:" + generate_sha256_hex(rng));

  nix::nar_info_t signed_info(config, path, nar_hash);
  signed_info.url = "nar/test.nar.xz";
  signed_info.compression = "xz";
  signed_info.fileHash = file_hash;
  signed_info.file_size = 12345;
  signed_info.nar_size = 67890;
  signed_info.references.insert(nix::store_path_t(generate_nix32_hash(rng) + "-dep1"));

  // Sign the narinfo
  auto fingerprint = signed_info.fingerprint(config);
  auto signature = secret_key.sign_detached(fingerprint);
  signed_info.sigs.insert(signature);

  // Verify signature works
  REQUIRE(signed_info.checkSignatures(config, trusted_keys) >= 1);

  BENCHMARK("verify single signature") {
    auto count = signed_info.checkSignatures(config, trusted_keys);
    return count;
  };

  // Build narinfo with multiple trusted signatures
  auto secret_key2 = nix::secret_key_t::generate("bench-key-2");
  auto public_key2 = secret_key2.to_public_key();
  trusted_keys.emplace(public_key2.name, public_key2);

  auto secret_key3 = nix::secret_key_t::generate("bench-key-3");
  auto public_key3 = secret_key3.to_public_key();
  trusted_keys.emplace(public_key3.name, public_key3);

  nix::nar_info_t multi_signed_info(config, path, nar_hash);
  multi_signed_info.url = "nar/test.nar.xz";
  multi_signed_info.compression = "xz";
  multi_signed_info.fileHash = file_hash;
  multi_signed_info.file_size = 12345;
  multi_signed_info.nar_size = 67890;
  multi_signed_info.references.insert(nix::store_path_t(generate_nix32_hash(rng) + "-dep2"));

  // Sign with all three keys
  auto fp2 = multi_signed_info.fingerprint(config);
  multi_signed_info.sigs.insert(secret_key.sign_detached(fp2));
  multi_signed_info.sigs.insert(secret_key2.sign_detached(fp2));
  multi_signed_info.sigs.insert(secret_key3.sign_detached(fp2));

  REQUIRE(multi_signed_info.checkSignatures(config, trusted_keys) >= 3);

  BENCHMARK("verify 3 signatures") {
    auto count = multi_signed_info.checkSignatures(config, trusted_keys);
    return count;
  };
}

// =============================================================================
// Benchmark: Round-trip (parse + serialize)
// =============================================================================

TEST_CASE("narinfo benchmark: round-trip", "[benchmark][store][narinfo]") {
  std::mt19937 rng(42);
  auto config = make_store_config();

  // Pre-generate test data
  std::vector<std::string> narinfos;
  for (int i = 0; i < 100; ++i) {
    narinfos.push_back(generate_typical_narinfo(rng));
  }

  int idx = 0;

  BENCHMARK("round-trip: parse + serialize") {
    auto& input = narinfos[idx++ % narinfos.size()];
    auto info = nix::nar_info_t(config, input, "bench.narinfo");
    auto output = info.to_string(config);
    return output.size();
  };
}

// =============================================================================
// Benchmark: Minimal narinfo (best case)
// =============================================================================

TEST_CASE("narinfo benchmark: minimal", "[benchmark][store][narinfo]") {
  std::mt19937 rng(42);
  auto config = make_store_config();

  // Pre-generate minimal test data
  std::vector<std::string> narinfos;
  for (int i = 0; i < 100; ++i) {
    narinfos.push_back(generate_minimal_narinfo(rng));
  }

  int idx = 0;

  BENCHMARK("parse minimal narinfo (no refs, no sig)") {
    auto& input = narinfos[idx++ % narinfos.size()];
    auto info = nix::nar_info_t(config, input, "bench.narinfo");
    return info.path.name();
  };
}
