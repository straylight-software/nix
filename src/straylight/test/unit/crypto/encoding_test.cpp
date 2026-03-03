// straylight::nix::crypto tests
//
// Property-based testing with rapidcheck for encoding primitives.
// Tests base16, base64, and nix32 encoding/decoding roundtrips.

#include <catch2/catch_test_macros.hpp>
// Catch2 must be included before rapidcheck/catch.h for v3 compatibility

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>
#include <straylight/nix/crypto/encoding.h>
namespace encoding = straylight::nix::crypto;

// ─────────────────────────────────────────────────────────────────────────────
// Base16 (Hex) Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("base16 empty input", "[encoding][base16]") {
  std::vector<uint8_t> empty;
  REQUIRE(encoding::base16::encode(empty) == "");
  REQUIRE(encoding::base16::decode("") == empty);
}

TEST_CASE("base16 known values", "[encoding][base16]") {
  // Single byte
  std::vector<uint8_t> data1 = {0x00};
  REQUIRE(encoding::base16::encode(data1) == "00");

  std::vector<uint8_t> data2 = {0xff};
  REQUIRE(encoding::base16::encode(data2) == "ff");

  // Multiple bytes
  std::vector<uint8_t> data3 = {0xde, 0xad, 0xbe, 0xef};
  REQUIRE(encoding::base16::encode(data3) == "deadbeef");

  // SHA256 of empty string prefix
  std::vector<uint8_t> data4 = {0xe3, 0xb0, 0xc4, 0x42};
  REQUIRE(encoding::base16::encode(data4) == "e3b0c442");
}

TEST_CASE("base16 decode known values", "[encoding][base16]") {
  auto decoded = encoding::base16::decode("deadbeef");
  REQUIRE(decoded == std::vector<uint8_t>{0xde, 0xad, 0xbe, 0xef});

  // Case insensitive
  REQUIRE(encoding::base16::decode("DEADBEEF") == std::vector<uint8_t>{0xde, 0xad, 0xbe, 0xef});
  REQUIRE(encoding::base16::decode("DeAdBeEf") == std::vector<uint8_t>{0xde, 0xad, 0xbe, 0xef});
}

TEST_CASE("base16 roundtrip", "[encoding][base16]") {
  // Edge cases
  std::vector<uint8_t> all_zeros(32, 0);
  REQUIRE(encoding::base16::decode(encoding::base16::encode(all_zeros)) == all_zeros);

  std::vector<uint8_t> all_ff(32, 0xff);
  REQUIRE(encoding::base16::decode(encoding::base16::encode(all_ff)) == all_ff);
}

TEST_CASE("base16 property roundtrip", "[encoding][base16]") {
  rc::prop("encode/decode roundtrip", [](std::vector<uint8_t> data) {
    auto encoded = encoding::base16::encode(data);
    auto decoded = encoding::base16::decode(encoded);
    RC_ASSERT(decoded == data);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Base64 Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("base64 empty input", "[encoding][base64]") {
  std::vector<uint8_t> empty;
  REQUIRE(encoding::base64::encode(empty) == "");
  REQUIRE(encoding::base64::decode("") == empty);
}

TEST_CASE("base64 known values RFC 4648", "[encoding][base64]") {
  // RFC 4648 test vectors
  auto encode = [](std::string_view sv) {
    return encoding::base64::encode(std::vector<uint8_t>(sv.begin(), sv.end()));
  };

  REQUIRE(encode("") == "");
  REQUIRE(encode("f") == "Zg==");
  REQUIRE(encode("fo") == "Zm8=");
  REQUIRE(encode("foo") == "Zm9v");
  REQUIRE(encode("foob") == "Zm9vYg==");
  REQUIRE(encode("fooba") == "Zm9vYmE=");
  REQUIRE(encode("foobar") == "Zm9vYmFy");
}

TEST_CASE("base64 decode known values", "[encoding][base64]") {
  auto decoded = encoding::base64::decode("Zm9vYmFy");
  std::string result(decoded.begin(), decoded.end());
  REQUIRE(result == "foobar");
}

TEST_CASE("base64 roundtrip", "[encoding][base64]") {
  std::vector<uint8_t> binary = {0x00, 0x01, 0xfe, 0xff};
  REQUIRE(encoding::base64::decode(encoding::base64::encode(binary)) == binary);
}

TEST_CASE("base64 property roundtrip", "[encoding][base64]") {
  rc::prop("encode/decode roundtrip", [](std::vector<uint8_t> data) {
    auto encoded = encoding::base64::encode(data);
    auto decoded = encoding::base64::decode(encoded);
    RC_ASSERT(decoded == data);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Nix32 Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("nix32 empty input", "[encoding][nix32]") {
  std::vector<uint8_t> empty;
  REQUIRE(encoding::nix32::encode(empty) == "");
  REQUIRE(encoding::nix32::decode("") == empty);
}

TEST_CASE("nix32 known values", "[encoding][nix32]") {
  // SHA256 of empty string (commonly seen in Nix)
  std::vector<uint8_t> sha256_empty = {0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
                                       0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
                                       0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
                                       0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55};
  auto encoded = encoding::nix32::encode(sha256_empty);
  // Nix32 encoding produces 52 characters for SHA256
  REQUIRE(encoded.length() == 52);
}

TEST_CASE("nix32 roundtrip", "[encoding][nix32]") {
  // 20-byte truncated hash (store path)
  std::vector<uint8_t> store_hash(20);
  for (size_t i = 0; i < 20; ++i) {
    store_hash[i] = static_cast<uint8_t>(i * 13);
  }
  REQUIRE(encoding::nix32::decode(encoding::nix32::encode(store_hash)) == store_hash);
}

TEST_CASE("nix32 alphabet validation", "[encoding][nix32]") {
  // Nix32 uses: 0123456789abcdfghijklmnpqrsvwxyz (no e, o, t, u)
  auto encoded = encoding::nix32::encode(std::vector<uint8_t>{0xff, 0x00, 0xff, 0x00});
  // Should only contain valid nix32 characters
  for (char c : encoded) {
    REQUIRE(((c >= '0' && c <= '9') ||
             (c >= 'a' && c <= 'z' && c != 'e' && c != 'o' && c != 't' && c != 'u')));
  }
}

TEST_CASE("nix32 property roundtrip", "[encoding][nix32]") {
  rc::prop("encode/decode roundtrip", [](std::vector<uint8_t> data) {
    auto encoded = encoding::nix32::encode(data);
    auto decoded = encoding::nix32::decode(encoded);
    RC_ASSERT(decoded == data);
  });
}
