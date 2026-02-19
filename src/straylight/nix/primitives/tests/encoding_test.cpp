// straylight::nix::primitives::encoding tests
//
// Property-based testing with rapidcheck for encoding primitives.
// Tests base16, base64, and nix32 encoding/decoding roundtrips.

// Catch2 MUST be included before rapidcheck/catch.h for v3 compatibility
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include "../encoding.h"
namespace encoding = straylight::nix::primitives::encoding;

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

  // Case insensitive decode
  auto decoded_upper = encoding::base16::decode("DEADBEEF");
  REQUIRE(decoded_upper == std::vector<uint8_t>{0xde, 0xad, 0xbe, 0xef});

  auto decoded_mixed = encoding::base16::decode("DeAdBeEf");
  REQUIRE(decoded_mixed == std::vector<uint8_t>{0xde, 0xad, 0xbe, 0xef});
}

TEST_CASE("base16 decode errors", "[encoding][base16]") {
  // Odd length
  REQUIRE_THROWS_AS(encoding::base16::decode("abc"), encoding::decode_error);

  // Invalid characters
  REQUIRE_THROWS_AS(encoding::base16::decode("gg"), encoding::decode_error);
  REQUIRE_THROWS_AS(encoding::base16::decode("zz"), encoding::decode_error);
}

TEST_CASE("base16 is_valid", "[encoding][base16]") {
  REQUIRE(encoding::base16::is_valid(""));
  REQUIRE(encoding::base16::is_valid("00"));
  REQUIRE(encoding::base16::is_valid("deadbeef"));
  REQUIRE(encoding::base16::is_valid("DEADBEEF"));
  REQUIRE(encoding::base16::is_valid("DeAdBeEf"));

  REQUIRE_FALSE(encoding::base16::is_valid("a"));     // odd length
  REQUIRE_FALSE(encoding::base16::is_valid("gg"));    // invalid char
  REQUIRE_FALSE(encoding::base16::is_valid("xx"));    // invalid char
  REQUIRE_FALSE(encoding::base16::is_valid("ab cd")); // space
}

TEST_CASE("base16 roundtrip property", "[encoding][base16][property]") {
  rc::prop("decode(encode(data)) == data", []() {
    auto data = *rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>());
    auto encoded = encoding::base16::encode(data);
    auto decoded = encoding::base16::decode(encoded);
    RC_ASSERT(decoded == data);
  });
}

TEST_CASE("base16 encoded length property", "[encoding][base16][property]") {
  rc::prop("encoded length is always 2x input", []() {
    auto data = *rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>());
    auto encoded = encoding::base16::encode(data);
    RC_ASSERT(encoded.size() == data.size() * 2);
    RC_ASSERT(encoded.size() == encoding::base16::encoded_length(data.size()));
  });
}

TEST_CASE("base16 all lowercase property", "[encoding][base16][property]") {
  rc::prop("encoded output is always lowercase", []() {
    auto data = *rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>());
    auto encoded = encoding::base16::encode(data);
    for (char c : encoded) {
      RC_ASSERT((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
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

TEST_CASE("base64 known values", "[encoding][base64]") {
  // RFC 4648 test vectors
  std::vector<uint8_t> data1 = {'f'};
  REQUIRE(encoding::base64::encode(data1) == "Zg==");

  std::vector<uint8_t> data2 = {'f', 'o'};
  REQUIRE(encoding::base64::encode(data2) == "Zm8=");

  std::vector<uint8_t> data3 = {'f', 'o', 'o'};
  REQUIRE(encoding::base64::encode(data3) == "Zm9v");

  std::vector<uint8_t> data4 = {'f', 'o', 'o', 'b'};
  REQUIRE(encoding::base64::encode(data4) == "Zm9vYg==");

  std::vector<uint8_t> data5 = {'f', 'o', 'o', 'b', 'a'};
  REQUIRE(encoding::base64::encode(data5) == "Zm9vYmE=");

  std::vector<uint8_t> data6 = {'f', 'o', 'o', 'b', 'a', 'r'};
  REQUIRE(encoding::base64::encode(data6) == "Zm9vYmFy");
}

TEST_CASE("base64 decode known values", "[encoding][base64]") {
  auto decoded = encoding::base64::decode("Zm9vYmFy");
  REQUIRE(decoded == std::vector<uint8_t>{'f', 'o', 'o', 'b', 'a', 'r'});

  // With padding
  auto decoded_pad1 = encoding::base64::decode("Zm9vYmE=");
  REQUIRE(decoded_pad1 == std::vector<uint8_t>{'f', 'o', 'o', 'b', 'a'});

  auto decoded_pad2 = encoding::base64::decode("Zm9vYg==");
  REQUIRE(decoded_pad2 == std::vector<uint8_t>{'f', 'o', 'o', 'b'});
}

TEST_CASE("base64 decode errors", "[encoding][base64]") {
  // Invalid character
  REQUIRE_THROWS_AS(encoding::base64::decode("!!!"), encoding::decode_error);
}

TEST_CASE("base64 is_valid", "[encoding][base64]") {
  REQUIRE(encoding::base64::is_valid(""));
  REQUIRE(encoding::base64::is_valid("Zm9v"));
  REQUIRE(encoding::base64::is_valid("Zm9vYg=="));
  REQUIRE(encoding::base64::is_valid("abc+def/ghi="));

  REQUIRE_FALSE(encoding::base64::is_valid("!!!"));
  REQUIRE_FALSE(encoding::base64::is_valid("abc$def"));
}

TEST_CASE("base64 roundtrip property", "[encoding][base64][property]") {
  rc::prop("decode(encode(data)) == data", []() {
    auto data = *rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>());
    auto encoded = encoding::base64::encode(data);
    auto decoded = encoding::base64::decode(encoded);
    RC_ASSERT(decoded == data);
  });
}

TEST_CASE("base64 encoded length property", "[encoding][base64][property]") {
  rc::prop("encoded length matches formula", []() {
    auto data = *rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>());
    auto encoded = encoding::base64::encode(data);
    RC_ASSERT(encoded.size() == encoding::base64::encoded_length(data.size()));

    // Multiple of 4
    RC_ASSERT(encoded.size() % 4 == 0 || data.empty());
  });
}

TEST_CASE("base64 valid characters property", "[encoding][base64][property]") {
  rc::prop("encoded output uses valid base64 chars", []() {
    auto data = *rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>());
    auto encoded = encoding::base64::encode(data);
    for (char c : encoded) {
      RC_ASSERT(encoding::base64::is_valid_char(c));
    }
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
  // Test with known store path hashes
  // SHA256 hash of empty string: e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
  std::vector<uint8_t> sha256_empty = {0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
                                       0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
                                       0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
                                       0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55};
  auto nix32_empty = encoding::nix32::encode(sha256_empty);
  // Verify it's 52 chars (256 bits / 5 = 51.2 -> 52)
  REQUIRE(nix32_empty.size() == 52);
  // Verify roundtrip
  auto decoded = encoding::nix32::decode(nix32_empty);
  REQUIRE(decoded == sha256_empty);
}

TEST_CASE("nix32 alphabet excludes e, o, t, u", "[encoding][nix32]") {
  // Generate many random inputs and verify no forbidden chars appear
  for (int i = 0; i < 100; ++i) {
    std::vector<uint8_t> data(32);
    for (auto& b : data) {
      b = static_cast<uint8_t>(i * 7 + 13); // deterministic pseudo-random
    }
    auto encoded = encoding::nix32::encode(data);
    for (char c : encoded) {
      REQUIRE(c != 'e');
      REQUIRE(c != 'o');
      REQUIRE(c != 't');
      REQUIRE(c != 'u');
    }
  }
}

TEST_CASE("nix32 lookup_reverse", "[encoding][nix32]") {
  // Valid chars
  REQUIRE(encoding::nix32::lookup_reverse('0') == 0);
  REQUIRE(encoding::nix32::lookup_reverse('9') == 9);
  REQUIRE(encoding::nix32::lookup_reverse('a') == 10);
  REQUIRE(encoding::nix32::lookup_reverse('z') == 31);

  // Invalid chars return 0xFF
  REQUIRE(encoding::nix32::lookup_reverse('e') == 0xFF);
  REQUIRE(encoding::nix32::lookup_reverse('o') == 0xFF);
  REQUIRE(encoding::nix32::lookup_reverse('t') == 0xFF);
  REQUIRE(encoding::nix32::lookup_reverse('u') == 0xFF);
  REQUIRE(encoding::nix32::lookup_reverse('A') == 0xFF); // uppercase
}

TEST_CASE("nix32 decode errors", "[encoding][nix32]") {
  // Invalid characters (e, o, t, u are forbidden)
  REQUIRE_THROWS_AS(encoding::nix32::decode("hello"), encoding::decode_error);
  REQUIRE_THROWS_AS(encoding::nix32::decode("world"), encoding::decode_error);
}

TEST_CASE("nix32 is_valid", "[encoding][nix32]") {
  REQUIRE(encoding::nix32::is_valid(""));
  REQUIRE(encoding::nix32::is_valid("0123456789"));
  REQUIRE(encoding::nix32::is_valid("abcdfghijklmnpqrsvwxyz"));

  // Forbidden chars
  REQUIRE_FALSE(encoding::nix32::is_valid("hello"));  // contains 'e' and 'o'
  REQUIRE_FALSE(encoding::nix32::is_valid("test"));   // contains 'e' and 't'
  REQUIRE_FALSE(encoding::nix32::is_valid("ubuntu")); // contains 'u' and 't'

  // Uppercase not allowed
  REQUIRE_FALSE(encoding::nix32::is_valid("ABC"));
}

TEST_CASE("nix32 roundtrip property", "[encoding][nix32][property]") {
  rc::prop("decode(encode(data)) == data", []() {
    auto data = *rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>());
    auto encoded = encoding::nix32::encode(data);
    auto decoded = encoding::nix32::decode(encoded);
    RC_ASSERT(decoded == data);
  });
}

TEST_CASE("nix32 encoded length property", "[encoding][nix32][property]") {
  rc::prop("encoded length matches formula", []() {
    auto data = *rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>());
    auto encoded = encoding::nix32::encode(data);
    RC_ASSERT(encoded.size() == encoding::nix32::encoded_length(data.size()));
  });
}

TEST_CASE("nix32 valid characters property", "[encoding][nix32][property]") {
  rc::prop("encoded output uses valid nix32 chars only", []() {
    auto data = *rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>());
    auto encoded = encoding::nix32::encode(data);
    for (char c : encoded) {
      RC_ASSERT(encoding::nix32::is_valid_char(c));
      // Explicitly check forbidden chars
      RC_ASSERT(c != 'e');
      RC_ASSERT(c != 'o');
      RC_ASSERT(c != 't');
      RC_ASSERT(c != 'u');
    }
  });
}

TEST_CASE("nix32 no uppercase property", "[encoding][nix32][property]") {
  rc::prop("encoded output is always lowercase", []() {
    auto data = *rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>());
    auto encoded = encoding::nix32::encode(data);
    for (char c : encoded) {
      RC_ASSERT(c < 'A' || c > 'Z');
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Cross-encoding Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("all encodings produce different outputs for same input", "[encoding]") {
  std::vector<uint8_t> data = {0xde, 0xad, 0xbe, 0xef};

  auto hex = encoding::base16::encode(data);
  auto b64 = encoding::base64::encode(data);
  auto nix = encoding::nix32::encode(data);

  REQUIRE(hex != b64);
  REQUIRE(hex != nix);
  REQUIRE(b64 != nix);

  // But all decode to same data
  REQUIRE(encoding::base16::decode(hex) == data);
  REQUIRE(encoding::base64::decode(b64) == data);
  REQUIRE(encoding::nix32::decode(nix) == data);
}

TEST_CASE("decoded_length calculations", "[encoding]") {
  // Base16: 2 chars per byte
  REQUIRE(encoding::base16::decoded_length(0) == 0);
  REQUIRE(encoding::base16::decoded_length(2) == 1);
  REQUIRE(encoding::base16::decoded_length(64) == 32);

  // Base64: 4 chars per 3 bytes
  REQUIRE(encoding::base64::decoded_length("") == 0);
  REQUIRE(encoding::base64::decoded_length("Zg==") == 1);
  REQUIRE(encoding::base64::decoded_length("Zm8=") == 2);
  REQUIRE(encoding::base64::decoded_length("Zm9v") == 3);

  // Nix32: 5 bits per char, 8 bits per byte
  REQUIRE(encoding::nix32::decoded_length(0) == 0);
  REQUIRE(encoding::nix32::decoded_length(52) == 32); // SHA256 hash
}
