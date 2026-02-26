// straylight // nix // util // tests
//
// Property-based and fuzz tests for hash utilities

// Catch2 must be included before rapidcheck/catch.h
#include <catch2/catch_test_macros.hpp>

// RapidCheck for property-based testing
#include <array>
#include <cstring>
#include <set>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include "nix/util/experimental-features.h"
#include "nix/util/hash.h"


// =============================================================================
// Helper to enable BLAKE3 for tests
// =============================================================================

namespace {

experimental_feature_settings_t make_blake3_enabled_settings() {
  experimental_feature_settings_t settings;
  settings.set("experimental-features", "blake3-hashes");
  return settings;
}

const experimental_feature_settings_t blake3_settings = make_blake3_enabled_settings();

} // namespace

// =============================================================================
// regularHashSize tests
// =============================================================================

TEST_CASE("regular_hash_size returns correct sizes", "[hash][size]") {
  REQUIRE(regular_hash_size(hash_algorithm_t::MD5) == 16);
  REQUIRE(regular_hash_size(hash_algorithm_t::SHA1) == 20);
  REQUIRE(regular_hash_size(hash_algorithm_t::SHA256) == 32);
  REQUIRE(regular_hash_size(hash_algorithm_t::SHA512) == 64);
  REQUIRE(regular_hash_size(hash_algorithm_t::BLAKE3) == 32);
}

// =============================================================================
// Hash constructor tests
// =============================================================================

TEST_CASE("hash constructor creates zero-filled hash", "[hash][constructor]") {
  Hash sha256(hash_algorithm_t::SHA256);
  REQUIRE(sha256.hash_size() == 32);
  REQUIRE(sha256.algo() == hash_algorithm_t::SHA256);
  // Check all bytes are zero
  bool all_zero = true;
  for (size_t i = 0; i < sha256.hash_size(); ++i) {
    if (sha256.hash()[i] != 0) { // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
      all_zero = false;
      break;
    }
  }
  REQUIRE(all_zero);
}

TEST_CASE("hash constructor for all algorithms", "[hash][constructor]") {
  Hash md5(hash_algorithm_t::MD5);
  REQUIRE(md5.hash_size() == 16);
  REQUIRE(md5.algo() == hash_algorithm_t::MD5);

  Hash sha1(hash_algorithm_t::SHA1);
  REQUIRE(sha1.hash_size() == 20);
  REQUIRE(sha1.algo() == hash_algorithm_t::SHA1);

  Hash sha256(hash_algorithm_t::SHA256);
  REQUIRE(sha256.hash_size() == 32);
  REQUIRE(sha256.algo() == hash_algorithm_t::SHA256);

  Hash sha512(hash_algorithm_t::SHA512);
  REQUIRE(sha512.hash_size() == 64);
  REQUIRE(sha512.algo() == hash_algorithm_t::SHA512);

  Hash blake3(hash_algorithm_t::BLAKE3, blake3_settings);
  REQUIRE(blake3.hash_size() == 32);
  REQUIRE(blake3.algo() == hash_algorithm_t::BLAKE3);
}

// =============================================================================
// Hash equality and comparison tests
// =============================================================================

TEST_CASE("hash equality same hash", "[hash][equality]") {
  auto h1 = hash_string(hash_algorithm_t::SHA256, "hello");
  auto h2 = hash_string(hash_algorithm_t::SHA256, "hello");
  REQUIRE(h1 == h2);
}

TEST_CASE("hash equality different content", "[hash][equality]") {
  auto h1 = hash_string(hash_algorithm_t::SHA256, "hello");
  auto h2 = hash_string(hash_algorithm_t::SHA256, "world");
  REQUIRE(h1 != h2);
}

TEST_CASE("hash equality different algorithms same content", "[hash][equality]") {
  auto h1 = hash_string(hash_algorithm_t::SHA256, "hello");
  auto h2 = hash_string(hash_algorithm_t::SHA512, "hello");
  // Different sizes, so not equal
  REQUIRE(h1 != h2);
}

TEST_CASE("hash comparison ordering", "[hash][comparison]") {
  Hash h1(hash_algorithm_t::SHA256);
  Hash h2(hash_algorithm_t::SHA256);
  h1.hash()[0] = 0x00;
  h2.hash()[0] = 0x01;
  REQUIRE(h1 < h2);
  REQUIRE(h2 > h1);
}

TEST_CASE("hash comparison same hash", "[hash][comparison]") {
  auto h1 = hash_string(hash_algorithm_t::SHA256, "test");
  auto h2 = hash_string(hash_algorithm_t::SHA256, "test");
  REQUIRE((h1 <=> h2) == std::strong_ordering::equivalent);
}

// =============================================================================
// hashString tests
// =============================================================================

TEST_CASE("hash_string empty string", "[hash][string]") {
  // SHA256 of empty string is well-known
  auto hash = hash_string(hash_algorithm_t::SHA256, "");
  auto hex = hash.to_string(hash_format_t::base16, false);
  REQUIRE(hex == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("hash_string known values sha256", "[hash][string]") {
  // SHA256("hello") is well-known
  auto hash = hash_string(hash_algorithm_t::SHA256, "hello");
  auto hex = hash.to_string(hash_format_t::base16, false);
  REQUIRE(hex == "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
}

TEST_CASE("hash_string known values md5", "[hash][string]") {
  // MD5("hello") is well-known
  auto hash = hash_string(hash_algorithm_t::MD5, "hello");
  auto hex = hash.to_string(hash_format_t::base16, false);
  REQUIRE(hex == "5d41402abc4b2a76b9719d911017c592");
}

TEST_CASE("hash_string known values sha1", "[hash][string]") {
  // SHA1("hello") is well-known
  auto hash = hash_string(hash_algorithm_t::SHA1, "hello");
  auto hex = hash.to_string(hash_format_t::base16, false);
  REQUIRE(hex == "aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d");
}

TEST_CASE("hash_string known values sha512", "[hash][string]") {
  // SHA512("hello") first 32 hex chars
  auto hash = hash_string(hash_algorithm_t::SHA512, "hello");
  auto hex = hash.to_string(hash_format_t::base16, false);
  REQUIRE(hex.starts_with("9b71d224bd62f3785d96d46ad3ea3d73"));
  REQUIRE(hex.size() == 128); // 64 bytes * 2
}

TEST_CASE("hash_string blake3", "[hash][string][blake3]") {
  auto hash = hash_string(hash_algorithm_t::BLAKE3, "hello", blake3_settings);
  auto hex = hash.to_string(hash_format_t::base16, false);
  REQUIRE(hex.size() == 64); // 32 bytes * 2
  // BLAKE3("hello") is deterministic
  REQUIRE(hex == "ea8f163db38682925e4491c5e58d4bb3506ef8c14eb78a86e908c5624a67200f");
}

// =============================================================================
// to_string format tests
// =============================================================================

TEST_CASE("hash to_string base16", "[hash][format][base16]") {
  auto hash = hash_string(hash_algorithm_t::SHA256, "test");
  auto str = hash.to_string(hash_format_t::base16, false);
  REQUIRE(str.size() == 64);
  // All chars should be hex
  for (char c : str) {
    REQUIRE(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')));
  }
}

TEST_CASE("hash to_string base16 with algo", "[hash][format][base16]") {
  auto hash = hash_string(hash_algorithm_t::SHA256, "test");
  auto str = hash.to_string(hash_format_t::base16, true);
  REQUIRE(str.starts_with("sha256:"));
}

TEST_CASE("hash to_string nix32", "[hash][format][nix32]") {
  auto hash = hash_string(hash_algorithm_t::SHA256, "test");
  auto str = hash.to_string(hash_format_t::nix32, false);
  // Nix32 uses specific character set (omits E O U T)
  std::string valid_chars = "0123456789abcdfghijklmnpqrsvwxyz";
  for (char c : str) {
    REQUIRE(valid_chars.contains(c));
  }
}

TEST_CASE("hash to_string base64", "[hash][format][base64]") {
  auto hash = hash_string(hash_algorithm_t::SHA256, "test");
  auto str = hash.to_string(hash_format_t::base64, false);
  // Base64 encoding - SHA256 (32 bytes) encodes to 44 chars with padding
  REQUIRE(str.size() == 44);
}

TEST_CASE("hash to_string sri", "[hash][format][sri]") {
  auto hash = hash_string(hash_algorithm_t::SHA256, "test");
  auto str = hash.to_string(hash_format_t::sri, true);
  REQUIRE(str.starts_with("sha256-"));
  // Should end with base64 encoding
  REQUIRE(str.find('-') == 6); // "sha256" is 6 chars
}

// =============================================================================
// gitRev and gitShortRev tests
// =============================================================================

TEST_CASE("hash git_rev returns base16", "[hash][git]") {
  auto hash = hash_string(hash_algorithm_t::SHA1, "test");
  auto rev = hash.git_rev();
  REQUIRE(rev.size() == 40); // SHA1 is 20 bytes = 40 hex chars
}

TEST_CASE("hash git_short_rev returns first 7 chars", "[hash][git]") {
  auto hash = hash_string(hash_algorithm_t::SHA1, "test");
  auto full = hash.git_rev();
  auto short_rev = hash.git_short_rev();
  REQUIRE(short_rev.size() == 7);
  REQUIRE(full.starts_with(short_rev));
}

// =============================================================================
// Hash::random tests
// =============================================================================

TEST_CASE("hash random produces different hashes", "[hash][random]") {
  auto h1 = Hash::random(hash_algorithm_t::SHA256);
  auto h2 = Hash::random(hash_algorithm_t::SHA256);
  // Extremely unlikely to be equal
  REQUIRE(h1 != h2);
}

TEST_CASE("hash random has correct size", "[hash][random]") {
  auto h = Hash::random(hash_algorithm_t::SHA256);
  REQUIRE(h.hash_size() == 32);
  REQUIRE(h.algo() == hash_algorithm_t::SHA256);
}

// =============================================================================
// parseHashAlgo / printHashAlgo tests
// =============================================================================

TEST_CASE("parse_hash_algo valid algorithms", "[hash][parse][algo]") {
  REQUIRE(parse_hash_algo("md5") == hash_algorithm_t::MD5);
  REQUIRE(parse_hash_algo("sha1") == hash_algorithm_t::SHA1);
  REQUIRE(parse_hash_algo("sha256") == hash_algorithm_t::SHA256);
  REQUIRE(parse_hash_algo("sha512") == hash_algorithm_t::SHA512);
  REQUIRE(parse_hash_algo("blake3", blake3_settings) == hash_algorithm_t::BLAKE3);
}

TEST_CASE("parse_hash_algo invalid throws", "[hash][parse][algo]") {
  REQUIRE_THROWS(parse_hash_algo("invalid"));
  REQUIRE_THROWS(parse_hash_algo(""));
  REQUIRE_THROWS(parse_hash_algo("SHA256")); // case-sensitive
}

TEST_CASE("parse_hash_algo_opt valid algorithms", "[hash][parse][algo]") {
  REQUIRE(parse_hash_algo_opt("md5") == hash_algorithm_t::MD5);
  REQUIRE(parse_hash_algo_opt("sha1") == hash_algorithm_t::SHA1);
  REQUIRE(parse_hash_algo_opt("sha256") == hash_algorithm_t::SHA256);
  REQUIRE(parse_hash_algo_opt("sha512") == hash_algorithm_t::SHA512);
  REQUIRE(parse_hash_algo_opt("blake3", blake3_settings) == hash_algorithm_t::BLAKE3);
}

TEST_CASE("parse_hash_algo_opt invalid returns nullopt", "[hash][parse][algo]") {
  REQUIRE(!parse_hash_algo_opt("invalid").has_value());
  REQUIRE(!parse_hash_algo_opt("").has_value());
  REQUIRE(!parse_hash_algo_opt("SHA256").has_value());
}

TEST_CASE("print_hash_algo roundtrip", "[hash][format][algo]") {
  REQUIRE(print_hash_algo(hash_algorithm_t::MD5) == "md5");
  REQUIRE(print_hash_algo(hash_algorithm_t::SHA1) == "sha1");
  REQUIRE(print_hash_algo(hash_algorithm_t::SHA256) == "sha256");
  REQUIRE(print_hash_algo(hash_algorithm_t::SHA512) == "sha512");
  REQUIRE(print_hash_algo(hash_algorithm_t::BLAKE3) == "blake3");
}

// =============================================================================
// parseHashFormat / printHashFormat tests
// =============================================================================

TEST_CASE("parse_hash_format valid formats", "[hash][parse][format]") {
  REQUIRE(parse_hash_format("base16") == hash_format_t::base16);
  REQUIRE(parse_hash_format("nix32") == hash_format_t::nix32);
  REQUIRE(parse_hash_format("base64") == hash_format_t::base64);
  REQUIRE(parse_hash_format("sri") == hash_format_t::sri);
}

TEST_CASE("parse_hash_format base32 deprecated alias", "[hash][parse][format]") {
  // base32 is deprecated alias for nix32
  REQUIRE(parse_hash_format("base32") == hash_format_t::nix32);
}

TEST_CASE("parse_hash_format invalid throws", "[hash][parse][format]") {
  REQUIRE_THROWS(parse_hash_format("invalid"));
  REQUIRE_THROWS(parse_hash_format(""));
  REQUIRE_THROWS(parse_hash_format("BASE16"));
}

TEST_CASE("parse_hash_format_opt valid formats", "[hash][parse][format]") {
  REQUIRE(parse_hash_format_opt("base16") == hash_format_t::base16);
  REQUIRE(parse_hash_format_opt("nix32") == hash_format_t::nix32);
  REQUIRE(parse_hash_format_opt("base64") == hash_format_t::base64);
  REQUIRE(parse_hash_format_opt("sri") == hash_format_t::sri);
}

TEST_CASE("parse_hash_format_opt invalid returns nullopt", "[hash][parse][format]") {
  REQUIRE(!parse_hash_format_opt("invalid").has_value());
  REQUIRE(!parse_hash_format_opt("").has_value());
}

TEST_CASE("print_hash_format roundtrip", "[hash][format]") {
  REQUIRE(print_hash_format(hash_format_t::base16) == "base16");
  REQUIRE(print_hash_format(hash_format_t::nix32) == "nix32");
  REQUIRE(print_hash_format(hash_format_t::base64) == "base64");
  REQUIRE(print_hash_format(hash_format_t::sri) == "sri");
}

// =============================================================================
// Hash parsing tests
// =============================================================================

TEST_CASE("hash parse_any_prefixed base16", "[hash][parse]") {
  auto hash = Hash::parse_any_prefixed(
      "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  REQUIRE(hash.algo() == hash_algorithm_t::SHA256);
  REQUIRE(hash.hash_size() == 32);
}

TEST_CASE("hash parse_any_prefixed sri", "[hash][parse]") {
  // SHA256 of empty string in SRI format
  auto hash = Hash::parse_any_prefixed("sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=");
  REQUIRE(hash.algo() == hash_algorithm_t::SHA256);
  REQUIRE(hash.hash_size() == 32);
}

TEST_CASE("hash parse_any_prefixed requires prefix", "[hash][parse]") {
  // Should throw without prefix
  REQUIRE_THROWS_AS(
      Hash::parse_any_prefixed("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
      BadHash);
}

TEST_CASE("hash parse_any with optional algo", "[hash][parse]") {
  // Without prefix, algo provided
  auto hash = Hash::parse_any("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                              hash_algorithm_t::SHA256);
  REQUIRE(hash.algo() == hash_algorithm_t::SHA256);
}

TEST_CASE("hash parse_any with prefix overrides algo", "[hash][parse]") {
  // With prefix matching optional algo - should work
  auto hash =
      Hash::parse_any("sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                      hash_algorithm_t::SHA256);
  REQUIRE(hash.algo() == hash_algorithm_t::SHA256);
}

TEST_CASE("hash parse_any with conflicting algo throws", "[hash][parse]") {
  // With prefix not matching optional algo - should throw
  REQUIRE_THROWS_AS(
      Hash::parse_any("sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                      hash_algorithm_t::SHA512),
      BadHash);
}

TEST_CASE("hash parse_any no algo available throws", "[hash][parse]") {
  REQUIRE_THROWS_AS(
      Hash::parse_any("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                      std::nullopt),
      BadHash);
}

TEST_CASE("hash parse_any_returning_format base16", "[hash][parse]") {
  auto [hash, format] = Hash::parse_any_returning_format(
      "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca4959"
      "91b7852b855",
      std::nullopt);
  REQUIRE(hash.algo() == hash_algorithm_t::SHA256);
  REQUIRE(format == hash_format_t::base16);
}

TEST_CASE("hash parse_any_returning_format sri", "[hash][parse]") {
  auto [hash, format] = Hash::parse_any_returning_format(
      "sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=", std::nullopt);
  REQUIRE(hash.algo() == hash_algorithm_t::SHA256);
  REQUIRE(format == hash_format_t::sri);
}

TEST_CASE("hash parse_sri", "[hash][parse]") {
  auto hash = Hash::parse_sri("sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=");
  REQUIRE(hash.algo() == hash_algorithm_t::SHA256);
  REQUIRE(hash.hash_size() == 32);
}

TEST_CASE("hash parse_sri invalid format throws", "[hash][parse]") {
  // No hyphen separator
  REQUIRE_THROWS_AS(Hash::parse_sri("sha256:47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU="),
                    BadHash);
  // Invalid algo
  REQUIRE_THROWS_AS(Hash::parse_sri("invalid-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU="),
                    UsageError);
}

TEST_CASE("hash parse_non_sri_unprefixed base16", "[hash][parse]") {
  auto hash = Hash::parse_non_sri_unprefixed(
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", hash_algorithm_t::SHA256);
  REQUIRE(hash.algo() == hash_algorithm_t::SHA256);
}

TEST_CASE("hash parse_non_sri_unprefixed nix32", "[hash][parse]") {
  // SHA256 of empty string in nix32
  auto hash = Hash::parse_non_sri_unprefixed("0mdqa9w1p6cmli6976v4wi0sw9r4p5prkj7lzfd1877wk11c9c73",
                                             hash_algorithm_t::SHA256);
  REQUIRE(hash.algo() == hash_algorithm_t::SHA256);
}

TEST_CASE("hash parse_non_sri_unprefixed wrong length throws", "[hash][parse]") {
  REQUIRE_THROWS_AS(Hash::parse_non_sri_unprefixed("deadbeef", hash_algorithm_t::SHA256), BadHash);
}

TEST_CASE("hash parse_explicit_format_unprefixed", "[hash][parse]") {
  auto hash = Hash::parse_explicit_format_unprefixed(
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", hash_algorithm_t::SHA256,
      hash_format_t::base16);
  REQUIRE(hash.algo() == hash_algorithm_t::SHA256);
}

// =============================================================================
// newHashAllowEmpty tests
// =============================================================================

TEST_CASE("new_hash_allow_empty with hash string", "[hash][parse]") {
  auto hash = new_hash_allow_empty(
      "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", std::nullopt);
  REQUIRE(hash.algo() == hash_algorithm_t::SHA256);
}

TEST_CASE("new_hash_allow_empty with empty string and algo", "[hash][parse]") {
  auto hash = new_hash_allow_empty("", hash_algorithm_t::SHA256);
  // Should return zero-filled hash
  REQUIRE(hash.algo() == hash_algorithm_t::SHA256);
  REQUIRE(hash.hash_size() == 32);
}

TEST_CASE("new_hash_allow_empty with empty string no algo throws", "[hash][parse]") {
  REQUIRE_THROWS_AS(new_hash_allow_empty("", std::nullopt), BadHash);
}

// =============================================================================
// compressHash tests
// =============================================================================

TEST_CASE("compress_hash basic", "[hash][compress]") {
  auto hash = hash_string(hash_algorithm_t::SHA256, "test");
  auto compressed = compress_hash(hash, 20);
  REQUIRE(compressed.hash_size() == 20);
  REQUIRE(compressed.algo() == hash_algorithm_t::SHA256);
}

TEST_CASE("compress_hash to same size", "[hash][compress]") {
  auto hash = hash_string(hash_algorithm_t::SHA256, "test");
  auto compressed = compress_hash(hash, 32);
  REQUIRE(compressed.hash_size() == 32);
  // When compressing to same size, hash should be equal
  REQUIRE(std::memcmp(compressed.hash(), hash.hash(), 32) == 0);
}

TEST_CASE("compress_hash to smaller size", "[hash][compress]") {
  auto hash = hash_string(hash_algorithm_t::SHA512, "test");
  auto compressed = compress_hash(hash, 20);
  REQUIRE(compressed.hash_size() == 20);
}

TEST_CASE("compress_hash deterministic", "[hash][compress]") {
  auto hash = hash_string(hash_algorithm_t::SHA256, "test");
  auto c1 = compress_hash(hash, 16);
  auto c2 = compress_hash(hash, 16);
  REQUIRE(std::memcmp(c1.hash(), c2.hash(), 16) == 0);
}

// =============================================================================
// HashSink tests
// =============================================================================

TEST_CASE("hash_sink basic usage", "[hash][sink]") {
  hash_sink_t sink(hash_algorithm_t::SHA256);
  sink("hello");
  auto result = sink.finish();
  REQUIRE(result.hash.algo() == hash_algorithm_t::SHA256);
  REQUIRE(result.num_bytes_digested == 5);
}

TEST_CASE("hash_sink incremental same as whole", "[hash][sink]") {
  // Hash the whole string at once
  auto whole = hash_string(hash_algorithm_t::SHA256, "hello world");

  // Hash incrementally
  hash_sink_t sink(hash_algorithm_t::SHA256);
  sink("hello");
  sink(" ");
  sink("world");
  auto incremental = sink.finish();

  REQUIRE(whole == incremental.hash);
}

TEST_CASE("hash_sink current_hash", "[hash][sink]") {
  hash_sink_t sink(hash_algorithm_t::SHA256);
  sink("hello");
  auto mid = sink.current_hash();
  sink(" world");
  auto final_result = sink.finish();

  REQUIRE(mid.num_bytes_digested == 5);
  REQUIRE(final_result.num_bytes_digested == 11);
  REQUIRE(mid.hash != final_result.hash);
}

TEST_CASE("hash_sink empty input", "[hash][sink]") {
  hash_sink_t sink(hash_algorithm_t::SHA256);
  auto result = sink.finish();
  REQUIRE(result.num_bytes_digested == 0);

  // Should match hash of empty string
  auto empty_hash = hash_string(hash_algorithm_t::SHA256, "");
  REQUIRE(result.hash == empty_hash);
}

// NOTE: HashSink copy constructor is declared but not implemented in the current codebase,
// so we cannot test it. This would be a good addition to the implementation.

// =============================================================================
// hashAlgorithms and hashFormats sets
// =============================================================================

TEST_CASE("hash_algorithms set contains all algos", "[hash][sets]") {
  REQUIRE(hash_algorithms.count("md5") == 1);
  REQUIRE(hash_algorithms.count("sha1") == 1);
  REQUIRE(hash_algorithms.count("sha256") == 1);
  REQUIRE(hash_algorithms.count("sha512") == 1);
  REQUIRE(hash_algorithms.count("blake3") == 1);
  REQUIRE(hash_algorithms.size() == 5);
}

TEST_CASE("hash_formats set contains all formats", "[hash][sets]") {
  REQUIRE(hash_formats.count("base16") == 1);
  REQUIRE(hash_formats.count("nix32") == 1);
  REQUIRE(hash_formats.count("base64") == 1);
  REQUIRE(hash_formats.count("sri") == 1);
  REQUIRE(hash_formats.size() == 4);
}

// =============================================================================
// std::hash specialization tests
// =============================================================================

TEST_CASE("std_hash produces same hash for equal hashes", "[hash][stdhash]") {
  auto h1 = hash_string(hash_algorithm_t::SHA256, "test");
  auto h2 = hash_string(hash_algorithm_t::SHA256, "test");

  std::hash<Hash> hasher;
  REQUIRE(hasher(h1) == hasher(h2));
}

TEST_CASE("std_hash can be used in unordered_set", "[hash][stdhash]") {
  std::unordered_set<Hash> set;
  auto h1 = hash_string(hash_algorithm_t::SHA256, "one");
  auto h2 = hash_string(hash_algorithm_t::SHA256, "two");
  auto h3 = hash_string(hash_algorithm_t::SHA256, "one"); // same as h1

  set.insert(h1);
  set.insert(h2);
  set.insert(h3); // should be deduped

  REQUIRE(set.size() == 2);
  REQUIRE(set.count(h1) == 1);
  REQUIRE(set.count(h2) == 1);
}

// =============================================================================
// Edge case tests
// =============================================================================

TEST_CASE("hash of null bytes", "[hash][edge]") {
  std::string null_bytes("\x00\x00\x00", 3);
  auto hash = hash_string(hash_algorithm_t::SHA256, null_bytes);
  REQUIRE(hash.hash_size() == 32);
  // Should not be zero hash - use std::array instead of C-style array
  std::array<uint8_t, 32> zero_hash = {};
  REQUIRE_FALSE(std::memcmp(hash.hash(), zero_hash.data(), 32) == 0);
}

TEST_CASE("hash of very long string", "[hash][edge]") {
  std::string long_str(static_cast<size_t>(1024) * 1024, 'x'); // 1MB
  auto hash = hash_string(hash_algorithm_t::SHA256, long_str);
  REQUIRE(hash.hash_size() == 32);
}

TEST_CASE("hash of binary data", "[hash][edge]") {
  std::string binary;
  for (int i = 0; i < 256; ++i) {
    binary.push_back(static_cast<char>(i));
  }
  auto hash = hash_string(hash_algorithm_t::SHA256, binary);
  REQUIRE(hash.hash_size() == 32);
}

TEST_CASE("parse hash with trailing whitespace throws", "[hash][parse][edge]") {
  REQUIRE_THROWS(Hash::parse_any_prefixed(
      "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 "));
}

TEST_CASE("parse hash with leading whitespace throws", "[hash][parse][edge]") {
  REQUIRE_THROWS(Hash::parse_any_prefixed(
      " sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

// =============================================================================
// Malformed input tests (fuzz-like)
// =============================================================================

TEST_CASE("parse malformed base16 throws", "[hash][parse][malformed]") {
  // Invalid hex chars
  REQUIRE_THROWS(Hash::parse_any_prefixed(
      "sha256:gggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggg"));
  // Too short
  REQUIRE_THROWS(Hash::parse_any_prefixed("sha256:deadbeef"));
  // Too long
  REQUIRE_THROWS(Hash::parse_any_prefixed(
      "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855ff"));
}

TEST_CASE("parse malformed base64 throws", "[hash][parse][malformed]") {
  // Invalid base64 chars
  REQUIRE_THROWS(Hash::parse_sri("sha256-!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"));
  // Wrong length
  REQUIRE_THROWS(Hash::parse_sri("sha256-dGVzdA=="));
}

TEST_CASE("parse malformed nix32 throws", "[hash][parse][malformed]") {
  // Invalid chars (nix32 doesn't use e, o, u, t)
  auto hash_str = "sha256:" + std::string(52, 'e'); // 'e' is not valid in nix32
  REQUIRE_THROWS(Hash::parse_any_prefixed(hash_str));
}

TEST_CASE("parse empty algo throws", "[hash][parse][malformed]") {
  REQUIRE_THROWS(Hash::parse_any_prefixed(
      ":e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

TEST_CASE("parse unknown algo throws", "[hash][parse][malformed]") {
  REQUIRE_THROWS(Hash::parse_any_prefixed(
      "unknown:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

// =============================================================================
// Property-based tests with RapidCheck
// =============================================================================

TEST_CASE("hash property tests", "[hash][property]") {
  rc::prop("hashing same data produces same hash", []() {
    auto data = *rc::gen::arbitrary<std::string>();
    auto h1 = hash_string(hash_algorithm_t::SHA256, data);
    auto h2 = hash_string(hash_algorithm_t::SHA256, data);
    RC_ASSERT(h1 == h2);
  });

  rc::prop("different data usually produces different hash", []() {
    auto data1 = *rc::gen::nonEmpty<std::string>();
    auto data2 = *rc::gen::nonEmpty<std::string>();
    if (data1 == data2) {
      return;
    }
    auto h1 = hash_string(hash_algorithm_t::SHA256, data1);
    auto h2 = hash_string(hash_algorithm_t::SHA256, data2);
    RC_ASSERT(h1 != h2);
  });

  rc::prop("hash size is always correct for algorithm", []() {
    auto data = *rc::gen::arbitrary<std::string>();
    auto algo = *rc::gen::element(hash_algorithm_t::MD5, hash_algorithm_t::SHA1,
                                  hash_algorithm_t::SHA256, hash_algorithm_t::SHA512);
    auto hash = hash_string(algo, data);
    RC_ASSERT(hash.hash_size() == regular_hash_size(algo));
  });
}

TEST_CASE("hash format roundtrip property tests", "[hash][property][roundtrip]") {
  rc::prop("base16 encode/parse roundtrip", []() {
    auto data = *rc::gen::arbitrary<std::string>();
    auto original = hash_string(hash_algorithm_t::SHA256, data);
    auto encoded = original.to_string(hash_format_t::base16, false);
    auto parsed = Hash::parse_non_sri_unprefixed(encoded, hash_algorithm_t::SHA256);
    RC_ASSERT(original == parsed);
  });

  rc::prop("nix32 encode/parse roundtrip", []() {
    auto data = *rc::gen::arbitrary<std::string>();
    auto original = hash_string(hash_algorithm_t::SHA256, data);
    auto encoded = original.to_string(hash_format_t::nix32, false);
    auto parsed = Hash::parse_non_sri_unprefixed(encoded, hash_algorithm_t::SHA256);
    RC_ASSERT(original == parsed);
  });

  rc::prop("base64 encode/parse roundtrip", []() {
    auto data = *rc::gen::arbitrary<std::string>();
    auto original = hash_string(hash_algorithm_t::SHA256, data);
    auto encoded = original.to_string(hash_format_t::base64, false);
    auto parsed = Hash::parse_non_sri_unprefixed(encoded, hash_algorithm_t::SHA256);
    RC_ASSERT(original == parsed);
  });

  rc::prop("sri encode/parse roundtrip", []() {
    auto data = *rc::gen::arbitrary<std::string>();
    auto original = hash_string(hash_algorithm_t::SHA256, data);
    auto encoded = original.to_string(hash_format_t::sri, true);
    auto parsed = Hash::parse_sri(encoded);
    RC_ASSERT(original == parsed);
  });
}

TEST_CASE("hash prefixed parse roundtrip property tests", "[hash][property][roundtrip]") {
  rc::prop("prefixed base16 roundtrip", []() {
    auto data = *rc::gen::arbitrary<std::string>();
    auto algo = *rc::gen::element(hash_algorithm_t::MD5, hash_algorithm_t::SHA1,
                                  hash_algorithm_t::SHA256, hash_algorithm_t::SHA512);
    auto original = hash_string(algo, data);
    auto encoded = original.to_string(hash_format_t::base16, true);
    auto parsed = Hash::parse_any_prefixed(encoded);
    RC_ASSERT(original == parsed);
    RC_ASSERT(original.algo() == parsed.algo());
  });

  rc::prop("sri format roundtrip", []() {
    auto data = *rc::gen::arbitrary<std::string>();
    auto algo = *rc::gen::element(hash_algorithm_t::MD5, hash_algorithm_t::SHA1,
                                  hash_algorithm_t::SHA256, hash_algorithm_t::SHA512);
    auto original = hash_string(algo, data);
    auto encoded = original.to_string(hash_format_t::sri, true);
    auto parsed = Hash::parse_any_prefixed(encoded);
    RC_ASSERT(original == parsed);
    RC_ASSERT(original.algo() == parsed.algo());
  });
}

TEST_CASE("hash algo parse/print roundtrip property", "[hash][property][algo]") {
  rc::prop("parseHashAlgo(printHashAlgo(algo)) == algo", []() {
    auto algo = *rc::gen::element(hash_algorithm_t::MD5, hash_algorithm_t::SHA1,
                                  hash_algorithm_t::SHA256, hash_algorithm_t::SHA512);
    auto printed = print_hash_algo(algo);
    auto parsed = parse_hash_algo(std::string(printed));
    RC_ASSERT(parsed == algo);
  });
}

TEST_CASE("hash format parse/print roundtrip property", "[hash][property][format]") {
  rc::prop("parseHashFormat(printHashFormat(fmt)) == fmt", []() {
    auto fmt = *rc::gen::element(hash_format_t::base16, hash_format_t::nix32, hash_format_t::base64,
                                 hash_format_t::sri);
    auto printed = print_hash_format(fmt);
    auto parsed = parse_hash_format(std::string(printed));
    RC_ASSERT(parsed == fmt);
  });
}

TEST_CASE("hash incremental equals whole property", "[hash][property][sink]") {
  rc::prop("incremental hashing equals whole string hashing", []() {
    auto parts = *rc::gen::container<std::vector<std::string>>(rc::gen::arbitrary<std::string>());
    auto algo = *rc::gen::element(hash_algorithm_t::MD5, hash_algorithm_t::SHA1,
                                  hash_algorithm_t::SHA256, hash_algorithm_t::SHA512);

    // Concatenate all parts
    std::string whole;
    for (const auto& part : parts) {
      whole += part;
    }

    // Hash whole
    auto whole_hash = hash_string(algo, whole);

    // Hash incrementally
    hash_sink_t sink(algo);
    for (const auto& part : parts) {
      sink(part);
    }
    auto incremental_result = sink.finish();

    RC_ASSERT(whole_hash == incremental_result.hash);
    RC_ASSERT(incremental_result.num_bytes_digested == whole.size());
  });
}

TEST_CASE("compress_hash property tests", "[hash][property][compress]") {
  rc::prop("compressed hash has requested size", []() {
    auto data = *rc::gen::arbitrary<std::string>();
    auto hash = hash_string(hash_algorithm_t::SHA512, data);
    auto new_size = *rc::gen::inRange<unsigned int>(1, 64);
    auto compressed = compress_hash(hash, new_size);
    RC_ASSERT(compressed.hash_size() == new_size);
  });

  rc::prop("compress is deterministic", []() {
    auto data = *rc::gen::arbitrary<std::string>();
    auto hash = hash_string(hash_algorithm_t::SHA256, data);
    auto new_size = *rc::gen::inRange<unsigned int>(1, 32);
    auto c1 = compress_hash(hash, new_size);
    auto c2 = compress_hash(hash, new_size);
    for (unsigned int i = 0; i < new_size; ++i) {
      RC_ASSERT(c1.hash()[i] == c2.hash()[i]);
    }
  });
}

TEST_CASE("hash comparison property tests", "[hash][property][comparison]") {
  rc::prop("hash equality is reflexive", []() {
    auto data = *rc::gen::arbitrary<std::string>();
    auto hash = hash_string(hash_algorithm_t::SHA256, data);
    RC_ASSERT(hash == hash);
  });

  rc::prop("hash equality is symmetric", []() {
    auto data = *rc::gen::arbitrary<std::string>();
    auto h1 = hash_string(hash_algorithm_t::SHA256, data);
    auto h2 = hash_string(hash_algorithm_t::SHA256, data);
    RC_ASSERT((h1 == h2) == (h2 == h1));
  });

  rc::prop("hash comparison is consistent", []() {
    auto data1 = *rc::gen::arbitrary<std::string>();
    auto data2 = *rc::gen::arbitrary<std::string>();
    auto h1 = hash_string(hash_algorithm_t::SHA256, data1);
    auto h2 = hash_string(hash_algorithm_t::SHA256, data2);

    auto cmp = h1 <=> h2;
    if (cmp < 0) {
      RC_ASSERT(h1 < h2);
      RC_ASSERT(h2 > h1);
      RC_ASSERT(h1 != h2);
    } else if (cmp > 0) {
      RC_ASSERT(h1 > h2);
      RC_ASSERT(h2 < h1);
      RC_ASSERT(h1 != h2);
    } else {
      RC_ASSERT(h1 == h2);
    }
  });
}

TEST_CASE("hash encoding length property tests", "[hash][property][encoding]") {
  rc::prop("base16 encoding is twice hash size", []() {
    auto data = *rc::gen::arbitrary<std::string>();
    auto algo = *rc::gen::element(hash_algorithm_t::MD5, hash_algorithm_t::SHA1,
                                  hash_algorithm_t::SHA256, hash_algorithm_t::SHA512);
    auto hash = hash_string(algo, data);
    auto encoded = hash.to_string(hash_format_t::base16, false);
    RC_ASSERT(encoded.size() == hash.hash_size() * 2);
  });
}

// =============================================================================
// Fuzz-like tests for malformed inputs
// =============================================================================

TEST_CASE("fuzz parse_any_prefixed with random strings", "[hash][fuzz]") {
  rc::prop("random strings either parse or throw", []() {
    auto input = *rc::gen::arbitrary<std::string>();
    try {
      auto hash = Hash::parse_any_prefixed(input);
      // If it parsed, it should have valid properties
      RC_ASSERT(hash.hash_size() > 0);
      RC_ASSERT(hash.hash_size() <= Hash::max_hash_size);
    } catch (const BadHash& e) {
      // Expected for invalid inputs
      (void)e;
    } catch (const UsageError& e) {
      // Expected for unknown algorithms
      (void)e;
    } catch (const Error& e) {
      // Other nix errors are acceptable
      (void)e;
    }
  });
}

TEST_CASE("fuzz parse_sri with random strings", "[hash][fuzz]") {
  rc::prop("random strings either parse or throw SRI", []() {
    auto input = *rc::gen::arbitrary<std::string>();
    try {
      auto hash = Hash::parse_sri(input);
      RC_ASSERT(hash.hash_size() > 0);
    } catch (const BadHash& e) {
      // Expected
      (void)e;
    } catch (const UsageError& e) {
      // Expected for unknown algorithms
      (void)e;
    } catch (const Error& e) {
      // Other nix errors are acceptable
      (void)e;
    }
  });
}

TEST_CASE("fuzz parseHashAlgo with random strings", "[hash][fuzz]") {
  rc::prop("random strings either parse or throw algo", []() {
    auto input = *rc::gen::arbitrary<std::string>();
    auto result = parse_hash_algo_opt(input);
    if (result.has_value()) {
      auto algo = *result;
      RC_ASSERT(algo == hash_algorithm_t::MD5 || algo == hash_algorithm_t::SHA1 ||
                algo == hash_algorithm_t::SHA256 || algo == hash_algorithm_t::SHA512);
    }
  });
}

TEST_CASE("fuzz parseHashFormat with random strings", "[hash][fuzz]") {
  rc::prop("random strings either parse or return nullopt", []() {
    auto input = *rc::gen::arbitrary<std::string>();
    auto result = parse_hash_format_opt(input);
    if (result.has_value()) {
      auto fmt = *result;
      RC_ASSERT(fmt == hash_format_t::base16 || fmt == hash_format_t::nix32 ||
                fmt == hash_format_t::base64 || fmt == hash_format_t::sri);
    }
  });
}

TEST_CASE("fuzz hashString with binary data", "[hash][fuzz]") {
  rc::prop("hashString handles all binary data", []() {
    // Generate arbitrary binary data including null bytes
    auto data = *rc::gen::container<std::string>(rc::gen::inRange<char>(0, 127));
    auto algo = *rc::gen::element(hash_algorithm_t::MD5, hash_algorithm_t::SHA1,
                                  hash_algorithm_t::SHA256, hash_algorithm_t::SHA512);
    auto hash = hash_string(algo, data);

    // Should always succeed and produce valid hash
    RC_ASSERT(hash.hash_size() == regular_hash_size(algo));
    RC_ASSERT(hash.algo() == algo);

    // Should be able to serialize and parse back
    auto encoded = hash.to_string(hash_format_t::base16, true);
    auto parsed = Hash::parse_any_prefixed(encoded);
    RC_ASSERT(hash == parsed);
  });
}

// =============================================================================
// Buffer overflow prevention tests
// =============================================================================

TEST_CASE("hash buffer bounds check", "[hash][security]") {
  // Verify that hash data doesn't overflow maxHashSize
  for (auto algo : {hash_algorithm_t::MD5, hash_algorithm_t::SHA1, hash_algorithm_t::SHA256,
                    hash_algorithm_t::SHA512}) {
    auto hash = hash_string(algo, "test");
    REQUIRE(hash.hash_size() <= Hash::max_hash_size);
    // Verify the hash can be serialized without error (implicitly checks bounds)
    auto encoded = hash.to_string(hash_format_t::base16, false);
    REQUIRE(encoded.size() == hash.hash_size() * 2);
  }
}

TEST_CASE("compress_hash bounds check", "[hash][security]") {
  auto hash = hash_string(hash_algorithm_t::SHA256, "test");
  // Compress to various sizes, ensure no overflow
  for (unsigned int size = 1; size <= hash.hash_size(); ++size) {
    auto compressed = compress_hash(hash, size);
    REQUIRE(compressed.hash_size() == size);
    // Verify by serializing - this implicitly checks bounds access
    auto encoded = compressed.to_string(hash_format_t::base16, false);
    REQUIRE(encoded.size() == static_cast<size_t>(size) * 2);
  }
}

// =============================================================================
// BLAKE3 specific tests (with experimental feature enabled)
// =============================================================================

TEST_CASE("blake3 hash roundtrip", "[hash][blake3]") {
  auto hash = hash_string(hash_algorithm_t::BLAKE3, "hello world", blake3_settings);
  auto encoded = hash.to_string(hash_format_t::sri, true);
  auto parsed = Hash::parse_sri(encoded, blake3_settings);
  REQUIRE(hash == parsed);
}

// NOTE: BLAKE3 HashSink test is not possible without globally enabling experimental feature,
// since HashSink::finish() uses the default experimental settings internally.
