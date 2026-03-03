// straylight::nix::primitives::tests
//
// Heavy metal tests for hash primitives.
// Unit tests and property-based tests.

#include <catch2/catch_test_macros.hpp>
// Catch2 must be included before rapidcheck/catch.h

// RapidCheck for property-based testing
#include <string>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include <straylight/nix/crypto/hash.h>

namespace crypto = straylight::nix::crypto;

// ─────────────────────────────────────────────────────────────────────────────
// Generators for property-based tests
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Generate arbitrary byte strings
rc::Gen<std::string> bytes_gen() {
  return rc::gen::container<std::string>(rc::gen::arbitrary<char>());
}

// Generate non-empty byte strings
rc::Gen<std::string> nonempty_bytes_gen() {
  return rc::gen::nonEmpty(bytes_gen());
}

// Generate all algorithm types
rc::Gen<crypto::Algorithm> algo_gen() {
  return rc::gen::element(crypto::Algorithm::MD5, crypto::Algorithm::SHA1,
                          crypto::Algorithm::SHA256, crypto::Algorithm::SHA512,
                          crypto::Algorithm::BLAKE3);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Algorithm metadata tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("hash_size returns correct sizes", "[hash][algo]") {
  REQUIRE(crypto::hash_size(crypto::Algorithm::MD5) == 16);
  REQUIRE(crypto::hash_size(crypto::Algorithm::SHA1) == 20);
  REQUIRE(crypto::hash_size(crypto::Algorithm::SHA256) == 32);
  REQUIRE(crypto::hash_size(crypto::Algorithm::SHA512) == 64);
  REQUIRE(crypto::hash_size(crypto::Algorithm::BLAKE3) == 32);
}

TEST_CASE("algorithm_name returns correct names", "[hash][algo]") {
  REQUIRE(crypto::algorithm_name(crypto::Algorithm::MD5) == "md5");
  REQUIRE(crypto::algorithm_name(crypto::Algorithm::SHA1) == "sha1");
  REQUIRE(crypto::algorithm_name(crypto::Algorithm::SHA256) == "sha256");
  REQUIRE(crypto::algorithm_name(crypto::Algorithm::SHA512) == "sha512");
  REQUIRE(crypto::algorithm_name(crypto::Algorithm::BLAKE3) == "blake3");
}

// ─────────────────────────────────────────────────────────────────────────────
// Known test vectors
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SHA256 test vectors", "[hash][sha256]") {
  // Empty string
  auto h1 = crypto::sha256("");
  REQUIRE(h1.to_hex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  // "hello"
  auto h2 = crypto::sha256("hello");
  REQUIRE(h2.to_hex() == "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");

  // "hello world"
  auto h3 = crypto::sha256("hello world");
  REQUIRE(h3.to_hex() == "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
}

TEST_CASE("SHA512 test vectors", "[hash][sha512]") {
  // Empty string
  auto h1 = crypto::sha512("");
  REQUIRE(
      h1.to_hex() ==
      "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d28"
      "77eec2f63b931bd47417a81a538327af927da3e");
}

TEST_CASE("SHA1 test vectors", "[hash][sha1]") {
  // Empty string
  auto h1 = crypto::sha1("");
  REQUIRE(h1.to_hex() == "da39a3ee5e6b4b0d3255bfef95601890afd80709");

  // "hello"
  auto h2 = crypto::sha1("hello");
  REQUIRE(h2.to_hex() == "aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d");
}

TEST_CASE("MD5 test vectors", "[hash][md5]") {
  // Empty string
  auto h1 = crypto::md5("");
  REQUIRE(h1.to_hex() == "d41d8cd98f00b204e9800998ecf8427e");

  // "hello"
  auto h2 = crypto::md5("hello");
  REQUIRE(h2.to_hex() == "5d41402abc4b2a76b9719d911017c592");
}

TEST_CASE("BLAKE3 test vectors", "[hash][blake3]") {
  // Empty string
  auto h1 = crypto::blake3("");
  REQUIRE(h1.to_hex() == "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");

  // "hello"
  auto h2 = crypto::blake3("hello");
  REQUIRE(h2.to_hex() == "ea8f163db38682925e4491c5e58d4bb3506ef8c14eb78a86e908c5624a67200f");
}

// ─────────────────────────────────────────────────────────────────────────────
// Encoding tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("hex encoding roundtrip", "[hash][encoding]") {
  auto h1 = crypto::sha256("test");
  auto hex = h1.to_hex();
  auto h2 = crypto::Hash::from_hex(crypto::Algorithm::SHA256, hex);

  REQUIRE(h1 == h2);
}

TEST_CASE("base64 encoding roundtrip", "[hash][encoding]") {
  auto h1 = crypto::sha256("test");
  auto b64 = h1.to_base64();
  auto h2 = crypto::Hash::from_base64(crypto::Algorithm::SHA256, b64);

  REQUIRE(h1 == h2);
}

TEST_CASE("nix32 encoding roundtrip", "[hash][encoding]") {
  auto h1 = crypto::sha256("test");
  auto nix32 = h1.to_nix32();
  auto h2 = crypto::Hash::from_nix32(crypto::Algorithm::SHA256, nix32);

  REQUIRE(h1 == h2);
}

TEST_CASE("SRI format roundtrip", "[hash][encoding]") {
  auto h1 = crypto::sha256("test");
  auto sri = h1.to_sri();
  auto h2 = crypto::Hash::from_sri(sri);

  REQUIRE(h1 == h2);
  REQUIRE(sri.starts_with("sha256-"));
}

TEST_CASE("base64 encoding matches expected", "[hash][encoding]") {
  // SHA256 of "test" in base64
  auto h = crypto::sha256("test");
  REQUIRE(h.to_base64() == "n4bQgYhMfWWaL+qgxVrQFaO/TxsrC4Is0V1sFbDwCgg=");
}

// ─────────────────────────────────────────────────────────────────────────────
// Compression tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("hash compression", "[hash][compress]") {
  auto h = crypto::sha256("test");

  // Compress to 20 bytes (store path size)
  auto compressed = h.compress(20);
  REQUIRE(compressed.size() == 20);

  // Compressed hash should be deterministic
  auto compressed2 = h.compress(20);
  REQUIRE(compressed == compressed2);
}

TEST_CASE("store_path_hash produces 20-byte hash", "[hash][compress]") {
  auto h = crypto::store_path_hash("test");
  REQUIRE(h.size() == 20);

  // Nix32 encoding of 20 bytes should be 32 characters
  auto nix32 = h.to_nix32();
  REQUIRE(nix32.size() == 32);
}

// ─────────────────────────────────────────────────────────────────────────────
// Streaming hasher tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("streaming hasher matches one-shot", "[hash][streaming]") {
  std::string data = "hello world this is a test";

  // One-shot
  auto h1 = crypto::sha256(data);

  // Streaming
  crypto::Hasher hasher(crypto::Algorithm::SHA256);
  hasher.update("hello ");
  hasher.update("world ");
  hasher.update("this is a test");
  auto h2 = hasher.finish();

  REQUIRE(h1 == h2);
}

TEST_CASE("streaming hasher current() doesn't finalize", "[hash][streaming]") {
  crypto::Hasher hasher(crypto::Algorithm::SHA256);
  hasher.update("hello");

  auto current = hasher.current();
  REQUIRE(current == crypto::sha256("hello"));

  // Can still update after current()
  hasher.update(" world");
  auto final_hash = hasher.finish();
  REQUIRE(final_hash == crypto::sha256("hello world"));
}

TEST_CASE("streaming hasher tracks bytes", "[hash][streaming]") {
  crypto::Hasher hasher(crypto::Algorithm::SHA256);
  REQUIRE(hasher.bytes_hashed() == 0);

  hasher.update("hello");
  REQUIRE(hasher.bytes_hashed() == 5);

  hasher.update(" world");
  REQUIRE(hasher.bytes_hashed() == 11);
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-based tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("hash determinism property", "[hash][property]") {
  rc::prop("same input produces same hash", []() {
    auto algo = *algo_gen();
    auto data = *bytes_gen();

    auto h1 = crypto::compute(algo, data);
    auto h2 = crypto::compute(algo, data);

    RC_ASSERT(h1 == h2);
  });
}

TEST_CASE("hash produces correct size", "[hash][property]") {
  rc::prop("hash size matches algorithm", []() {
    auto algo = *algo_gen();
    auto data = *bytes_gen();

    auto h = crypto::compute(algo, data);

    RC_ASSERT(h.size() == crypto::hash_size(algo));
    RC_ASSERT(h.algorithm() == algo);
  });
}

TEST_CASE("hex encoding roundtrip property", "[hash][property]") {
  rc::prop("from_hex(to_hex(h)) == h", []() {
    auto algo = *algo_gen();
    auto data = *bytes_gen();

    auto h1 = crypto::compute(algo, data);
    auto hex = h1.to_hex();
    auto h2 = crypto::Hash::from_hex(algo, hex);

    RC_ASSERT(h1 == h2);
    RC_ASSERT(hex.size() == h1.size() * 2);
  });
}

TEST_CASE("base64 encoding roundtrip property", "[hash][property]") {
  rc::prop("from_base64(to_base64(h)) == h", []() {
    auto algo = *algo_gen();
    auto data = *bytes_gen();

    auto h1 = crypto::compute(algo, data);
    auto b64 = h1.to_base64();
    auto h2 = crypto::Hash::from_base64(algo, b64);

    RC_ASSERT(h1 == h2);
  });
}

TEST_CASE("nix32 encoding roundtrip property", "[hash][property]") {
  rc::prop("from_nix32(to_nix32(h)) == h", []() {
    auto algo = *algo_gen();
    auto data = *bytes_gen();

    auto h1 = crypto::compute(algo, data);
    auto nix32 = h1.to_nix32();
    auto h2 = crypto::Hash::from_nix32(algo, nix32);

    RC_ASSERT(h1 == h2);
  });
}

TEST_CASE("SRI encoding roundtrip property", "[hash][property]") {
  rc::prop("from_sri(to_sri(h)) == h", []() {
    auto algo = *algo_gen();
    auto data = *bytes_gen();

    auto h1 = crypto::compute(algo, data);
    auto sri = h1.to_sri();
    auto h2 = crypto::Hash::from_sri(sri);

    RC_ASSERT(h1 == h2);
  });
}

TEST_CASE("streaming matches one-shot property", "[hash][property]") {
  rc::prop("streaming hasher produces same result as one-shot", []() {
    auto algo = *algo_gen();
    auto data = *bytes_gen();

    auto h1 = crypto::compute(algo, data);

    crypto::Hasher hasher(algo);
    hasher.update(data);
    auto h2 = hasher.finish();

    RC_ASSERT(h1 == h2);
  });
}

TEST_CASE("streaming chunked matches one-shot property", "[hash][property]") {
  rc::prop("chunked streaming produces same result", []() {
    auto algo = *algo_gen();
    auto data = *bytes_gen();

    auto h1 = crypto::compute(algo, data);

    // Split data into random chunks
    crypto::Hasher hasher(algo);
    std::size_t pos = 0;
    while (pos < data.size()) {
      std::size_t chunk_size = *rc::gen::inRange<std::size_t>(1, data.size() - pos + 1);
      hasher.update(std::string_view(data).substr(pos, chunk_size));
      pos += chunk_size;
    }
    auto h2 = hasher.finish();

    RC_ASSERT(h1 == h2);
  });
}

TEST_CASE("compression is deterministic property", "[hash][property]") {
  rc::prop("compress is deterministic", []() {
    auto data = *bytes_gen();
    auto new_size = *rc::gen::inRange<std::size_t>(1, crypto::sha256_size + 1);

    auto h = crypto::sha256(data);
    auto c1 = h.compress(new_size);
    auto c2 = h.compress(new_size);

    RC_ASSERT(c1 == c2);
    RC_ASSERT(c1.size() == new_size);
  });
}

TEST_CASE("different inputs produce different hashes property", "[hash][property]") {
  rc::prop("different inputs rarely collide", []() {
    auto algo = *algo_gen();
    auto data1 = *nonempty_bytes_gen();
    auto data2 = *nonempty_bytes_gen();

    RC_PRE(data1 != data2);

    auto h1 = crypto::compute(algo, data1);
    auto h2 = crypto::compute(algo, data2);

    RC_ASSERT(h1 != h2);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Comparison tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("hash comparison", "[hash][compare]") {
  auto h1 = crypto::sha256("a");
  auto h2 = crypto::sha256("b");
  auto h3 = crypto::sha256("a");

  REQUIRE(h1 == h3);
  REQUIRE(h1 != h2);

  // Same algorithm, different data should be comparable
  REQUIRE((h1 <=> h2) != 0);
  REQUIRE((h1 <=> h3) == 0);
}

TEST_CASE("is_zero check", "[hash][utility]") {
  crypto::Hash h1(crypto::Algorithm::SHA256); // Default constructed is zero
  REQUIRE(h1.is_zero());

  auto h2 = crypto::sha256("");
  REQUIRE_FALSE(h2.is_zero()); // SHA256 of empty string is not zero
}
