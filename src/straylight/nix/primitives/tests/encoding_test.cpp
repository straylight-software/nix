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
      b = static_cast<uint8_t>((i * 7) + 13); // deterministic pseudo-random
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

// ─────────────────────────────────────────────────────────────────────────────
// Heavy metal roundtrip property tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("encoding roundtrip heavy metal", "[encoding][property][roundtrip]") {
  rc::prop("base16 roundtrip preserves all byte values 0-255", []() {
    // Explicitly test all byte values
    std::vector<uint8_t> all_bytes(256);
    for (int i = 0; i < 256; ++i) {
      all_bytes[static_cast<std::size_t>(i)] = static_cast<uint8_t>(i);
    }

    auto encoded = encoding::base16::encode(all_bytes);
    auto decoded = encoding::base16::decode(encoded);

    RC_ASSERT(decoded == all_bytes);
  });

  rc::prop("base64 roundtrip preserves all byte values 0-255", []() {
    std::vector<uint8_t> all_bytes(256);
    for (int i = 0; i < 256; ++i) {
      all_bytes[static_cast<std::size_t>(i)] = static_cast<uint8_t>(i);
    }

    auto encoded = encoding::base64::encode(all_bytes);
    auto decoded = encoding::base64::decode(encoded);

    RC_ASSERT(decoded == all_bytes);
  });

  rc::prop("nix32 roundtrip preserves all byte values 0-255", []() {
    std::vector<uint8_t> all_bytes(256);
    for (int i = 0; i < 256; ++i) {
      all_bytes[static_cast<std::size_t>(i)] = static_cast<uint8_t>(i);
    }

    auto encoded = encoding::nix32::encode(all_bytes);
    auto decoded = encoding::nix32::decode(encoded);

    RC_ASSERT(decoded == all_bytes);
  });

  rc::prop("base16 roundtrip with random length", []() {
    auto len = *rc::gen::inRange<std::size_t>(0, 1000);
    auto data = *rc::gen::container<std::vector<uint8_t>>(len, rc::gen::arbitrary<uint8_t>());

    auto encoded = encoding::base16::encode(data);
    auto decoded = encoding::base16::decode(encoded);

    RC_ASSERT(decoded == data);
  });

  rc::prop("base64 roundtrip with random length", []() {
    auto len = *rc::gen::inRange<std::size_t>(0, 1000);
    auto data = *rc::gen::container<std::vector<uint8_t>>(len, rc::gen::arbitrary<uint8_t>());

    auto encoded = encoding::base64::encode(data);
    auto decoded = encoding::base64::decode(encoded);

    RC_ASSERT(decoded == data);
  });

  rc::prop("nix32 roundtrip with random length", []() {
    auto len = *rc::gen::inRange<std::size_t>(0, 1000);
    auto data = *rc::gen::container<std::vector<uint8_t>>(len, rc::gen::arbitrary<uint8_t>());

    auto encoded = encoding::nix32::encode(data);
    auto decoded = encoding::nix32::decode(encoded);

    RC_ASSERT(decoded == data);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Encoded length property tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("encoded length properties", "[encoding][property][length]") {
  rc::prop("base16 encoded length is always 2x input", []() {
    auto len = *rc::gen::inRange<std::size_t>(0, 500);
    auto data = *rc::gen::container<std::vector<uint8_t>>(len, rc::gen::arbitrary<uint8_t>());

    auto encoded = encoding::base16::encode(data);

    RC_ASSERT(encoded.size() == data.size() * 2);
    RC_ASSERT(encoded.size() == encoding::base16::encoded_length(data.size()));
  });

  rc::prop("base64 encoded length matches formula (ceil(n/3)*4)", []() {
    auto len = *rc::gen::inRange<std::size_t>(0, 500);
    auto data = *rc::gen::container<std::vector<uint8_t>>(len, rc::gen::arbitrary<uint8_t>());

    auto encoded = encoding::base64::encode(data);

    RC_ASSERT(encoded.size() == encoding::base64::encoded_length(data.size()));
    if (!data.empty()) {
      RC_ASSERT(encoded.size() % 4 == 0);
    }
  });

  rc::prop("nix32 encoded length matches formula", []() {
    auto len = *rc::gen::inRange<std::size_t>(0, 500);
    auto data = *rc::gen::container<std::vector<uint8_t>>(len, rc::gen::arbitrary<uint8_t>());

    auto encoded = encoding::nix32::encode(data);

    RC_ASSERT(encoded.size() == encoding::nix32::encoded_length(data.size()));
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Malformed input handling
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("base16 malformed input handling", "[encoding][property][malformed]") {
  rc::prop("base16 decode rejects odd-length strings", []() {
    auto odd_len = *rc::gen::inRange<std::size_t>(1, 100);
    if (odd_len % 2 == 0)
      odd_len += 1;

    // Generate valid hex chars but odd length
    std::string hex;
    for (std::size_t i = 0; i < odd_len; ++i) {
      hex += encoding::base16::alphabet[i % 16];
    }

    bool threw = false;
    try {
      encoding::base16::decode(hex);
    } catch (const encoding::decode_error&) {
      threw = true;
    }
    RC_ASSERT(threw);
    RC_ASSERT(!encoding::base16::is_valid(hex));
  });

  rc::prop("base16 decode rejects invalid characters", []() {
    // Generate a string with at least one invalid char
    auto valid_hex = *rc::gen::nonEmpty(rc::gen::container<std::string>(rc::gen::element(
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f')));
    // Ensure even length
    if (valid_hex.size() % 2 != 0)
      valid_hex += '0';

    // Insert an invalid character
    auto pos = *rc::gen::inRange<std::size_t>(0, valid_hex.size());
    auto invalid_char = *rc::gen::suchThat<char>(
        rc::gen::arbitrary<char>(), [](char c) { return !encoding::base16::is_valid_char(c); });

    std::string invalid_hex = valid_hex;
    invalid_hex[pos] = invalid_char;

    bool threw = false;
    try {
      encoding::base16::decode(invalid_hex);
    } catch (const encoding::decode_error&) {
      threw = true;
    }
    RC_ASSERT(threw);
    RC_ASSERT(!encoding::base16::is_valid(invalid_hex));
  });
}

TEST_CASE("base64 malformed input handling", "[encoding][property][malformed]") {
  rc::prop("base64 decode rejects invalid characters", []() {
    // Generate valid base64
    auto data =
        *rc::gen::nonEmpty(rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>()));
    auto valid_b64 = encoding::base64::encode(data);

    // Insert an invalid character (not in alphabet, not =, not newline)
    auto pos = *rc::gen::inRange<std::size_t>(0, valid_b64.size());
    auto invalid_char = *rc::gen::suchThat<char>(
        rc::gen::arbitrary<char>(), [](char c) { return !encoding::base64::is_valid_char(c); });

    std::string invalid_b64 = valid_b64;
    invalid_b64[pos] = invalid_char;

    bool threw = false;
    try {
      encoding::base64::decode(invalid_b64);
    } catch (const encoding::decode_error&) {
      threw = true;
    }
    RC_ASSERT(threw);
  });
}

TEST_CASE("nix32 malformed input handling", "[encoding][property][malformed]") {
  rc::prop("nix32 decode rejects forbidden characters (e, o, t, u)", []() {
    // Generate valid nix32
    auto data =
        *rc::gen::nonEmpty(rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>()));
    auto valid_nix32 = encoding::nix32::encode(data);

    // Insert a forbidden character
    auto pos = *rc::gen::inRange<std::size_t>(0, valid_nix32.size());
    auto forbidden_char = *rc::gen::element('e', 'o', 't', 'u');

    std::string invalid_nix32 = valid_nix32;
    invalid_nix32[pos] = forbidden_char;

    bool threw = false;
    try {
      encoding::nix32::decode(invalid_nix32);
    } catch (const encoding::decode_error&) {
      threw = true;
    }
    RC_ASSERT(threw);
    RC_ASSERT(!encoding::nix32::is_valid(invalid_nix32));
  });

  rc::prop("nix32 decode rejects uppercase", []() {
    // Generate valid nix32
    auto data =
        *rc::gen::nonEmpty(rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>()));
    auto valid_nix32 = encoding::nix32::encode(data);

    // Find a lowercase letter and uppercase it
    bool has_alpha = false;
    std::size_t alpha_pos = 0;
    for (std::size_t i = 0; i < valid_nix32.size(); ++i) {
      if (valid_nix32[i] >= 'a' && valid_nix32[i] <= 'z') {
        has_alpha = true;
        alpha_pos = i;
        break;
      }
    }

    if (!has_alpha)
      return; // Skip if no letters

    std::string invalid_nix32 = valid_nix32;
    invalid_nix32[alpha_pos] = static_cast<char>(invalid_nix32[alpha_pos] - 'a' + 'A');

    bool threw = false;
    try {
      encoding::nix32::decode(invalid_nix32);
    } catch (const encoding::decode_error&) {
      threw = true;
    }
    RC_ASSERT(threw);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Character alphabet property tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("encoding alphabet properties", "[encoding][property][alphabet]") {
  rc::prop("base16 encode uses only hex chars", []() {
    auto data = *rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>());
    auto encoded = encoding::base16::encode(data);

    for (char c : encoded) {
      RC_ASSERT(encoding::base16::is_valid_char(c));
      RC_ASSERT((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
  });

  rc::prop("base64 encode uses only base64 chars", []() {
    auto data = *rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>());
    auto encoded = encoding::base64::encode(data);

    for (char c : encoded) {
      RC_ASSERT(encoding::base64::is_valid_char(c));
    }
  });

  rc::prop("nix32 encode never produces e, o, t, u", []() {
    auto data = *rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>());
    auto encoded = encoding::nix32::encode(data);

    for (char c : encoded) {
      RC_ASSERT(c != 'e');
      RC_ASSERT(c != 'o');
      RC_ASSERT(c != 't');
      RC_ASSERT(c != 'u');
      RC_ASSERT(encoding::nix32::is_valid_char(c));
    }
  });

  rc::prop("nix32 alphabet covers exactly 32 characters", []() {
    RC_ASSERT(encoding::nix32::alphabet.size() == 32);

    // All chars are unique
    std::set<char> chars(encoding::nix32::alphabet.begin(), encoding::nix32::alphabet.end());
    RC_ASSERT(chars.size() == 32);

    // None of the forbidden chars
    RC_ASSERT(chars.find('e') == chars.end());
    RC_ASSERT(chars.find('o') == chars.end());
    RC_ASSERT(chars.find('t') == chars.end());
    RC_ASSERT(chars.find('u') == chars.end());
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// decode_to property tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("decode_to properties", "[encoding][property][decode_to]") {
  rc::prop("base16 decode_to produces same result as decode", []() {
    auto data = *rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>());
    auto encoded = encoding::base16::encode(data);

    std::vector<uint8_t> decoded_vec = encoding::base16::decode(encoded);
    std::vector<uint8_t> decoded_to(data.size());
    encoding::base16::decode_to(encoded, decoded_to);

    RC_ASSERT(decoded_vec == decoded_to);
  });

  rc::prop("base64 decode_to produces same result as decode", []() {
    auto data = *rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>());
    auto encoded = encoding::base64::encode(data);

    std::vector<uint8_t> decoded_vec = encoding::base64::decode(encoded);
    std::vector<uint8_t> decoded_to(data.size());
    encoding::base64::decode_to(encoded, decoded_to);

    RC_ASSERT(decoded_vec == decoded_to);
  });

  rc::prop("nix32 decode_to produces same result as decode", []() {
    auto data = *rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>());
    auto encoded = encoding::nix32::encode(data);

    std::vector<uint8_t> decoded_vec = encoding::nix32::decode(encoded);
    std::vector<uint8_t> decoded_to(data.size());
    encoding::nix32::decode_to(encoded, decoded_to);

    RC_ASSERT(decoded_vec == decoded_to);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Specific hash size tests (for Nix store paths)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("encoding with hash-sized inputs", "[encoding][property][hash]") {
  rc::prop("SHA-256 hash (32 bytes) roundtrips correctly", []() {
    // Generate random 32-byte hash
    std::vector<uint8_t> hash(32);
    for (auto& b : hash) {
      b = *rc::gen::arbitrary<uint8_t>();
    }

    // Base16: 64 chars
    auto hex = encoding::base16::encode(hash);
    RC_ASSERT(hex.size() == 64);
    RC_ASSERT(encoding::base16::decode(hex) == hash);

    // Base64: 44 chars (with padding)
    auto b64 = encoding::base64::encode(hash);
    RC_ASSERT(b64.size() == 44);
    RC_ASSERT(encoding::base64::decode(b64) == hash);

    // Nix32: 52 chars
    auto nix = encoding::nix32::encode(hash);
    RC_ASSERT(nix.size() == 52);
    RC_ASSERT(encoding::nix32::decode(nix) == hash);
  });

  rc::prop("SHA-512 hash (64 bytes) roundtrips correctly", []() {
    // Generate random 64-byte hash
    std::vector<uint8_t> hash(64);
    for (auto& b : hash) {
      b = *rc::gen::arbitrary<uint8_t>();
    }

    // Base16: 128 chars
    auto hex = encoding::base16::encode(hash);
    RC_ASSERT(hex.size() == 128);
    RC_ASSERT(encoding::base16::decode(hex) == hash);

    // Base64: 88 chars (with padding)
    auto b64 = encoding::base64::encode(hash);
    RC_ASSERT(b64.size() == 88);
    RC_ASSERT(encoding::base64::decode(b64) == hash);

    // Nix32: 103 chars
    auto nix = encoding::nix32::encode(hash);
    RC_ASSERT(nix.size() == 103);
    RC_ASSERT(encoding::nix32::decode(nix) == hash);
  });

  rc::prop("MD5 hash (16 bytes) roundtrips correctly", []() {
    // Generate random 16-byte hash
    std::vector<uint8_t> hash(16);
    for (auto& b : hash) {
      b = *rc::gen::arbitrary<uint8_t>();
    }

    // Base16: 32 chars
    auto hex = encoding::base16::encode(hash);
    RC_ASSERT(hex.size() == 32);
    RC_ASSERT(encoding::base16::decode(hex) == hash);

    // Base64: 24 chars (with padding)
    auto b64 = encoding::base64::encode(hash);
    RC_ASSERT(b64.size() == 24);
    RC_ASSERT(encoding::base64::decode(b64) == hash);

    // Nix32: 26 chars
    auto nix = encoding::nix32::encode(hash);
    RC_ASSERT(nix.size() == 26);
    RC_ASSERT(encoding::nix32::decode(nix) == hash);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Fuzz tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("encoding fuzz tests", "[encoding][fuzz]") {
  rc::prop("base16 decode never crashes on arbitrary input", []() {
    auto s = *rc::gen::container<std::string>(rc::gen::arbitrary<char>());

    // Should not crash, may throw decode_error
    try {
      [[maybe_unused]] auto result = encoding::base16::decode(s);
    } catch (const encoding::decode_error&) {
      // Expected for invalid input
    }
  });

  rc::prop("base64 decode never crashes on arbitrary input", []() {
    auto s = *rc::gen::container<std::string>(rc::gen::arbitrary<char>());

    try {
      [[maybe_unused]] auto result = encoding::base64::decode(s);
    } catch (const encoding::decode_error&) {
      // Expected for invalid input
    }
  });

  rc::prop("nix32 decode never crashes on arbitrary input", []() {
    auto s = *rc::gen::container<std::string>(rc::gen::arbitrary<char>());

    try {
      [[maybe_unused]] auto result = encoding::nix32::decode(s);
    } catch (const encoding::decode_error&) {
      // Expected for invalid input
    }
  });

  rc::prop("is_valid never crashes on arbitrary input", []() {
    auto s = *rc::gen::container<std::string>(rc::gen::arbitrary<char>());

    // Should not crash
    [[maybe_unused]] auto b16 = encoding::base16::is_valid(s);
    [[maybe_unused]] auto b64 = encoding::base64::is_valid(s);
    [[maybe_unused]] auto n32 = encoding::nix32::is_valid(s);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Case sensitivity tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("base16 case insensitivity", "[encoding][property][case]") {
  rc::prop("base16 decode is case-insensitive", []() {
    auto data = *rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>());
    auto lower = encoding::base16::encode(data);

    // Convert to uppercase
    std::string upper = lower;
    for (char& c : upper) {
      if (c >= 'a' && c <= 'f') {
        c = static_cast<char>(c - 'a' + 'A');
      }
    }

    RC_ASSERT(encoding::base16::decode(lower) == data);
    RC_ASSERT(encoding::base16::decode(upper) == data);

    // Mixed case
    std::string mixed = lower;
    for (std::size_t i = 0; i < mixed.size(); i += 2) {
      if (mixed[i] >= 'a' && mixed[i] <= 'f') {
        mixed[i] = static_cast<char>(mixed[i] - 'a' + 'A');
      }
    }
    RC_ASSERT(encoding::base16::decode(mixed) == data);
  });
}

#include <set>

TEST_CASE("nix32 is case-sensitive (lowercase only)", "[encoding][property][case]") {
  rc::prop("nix32 encode always produces lowercase", []() {
    auto data = *rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>());
    auto encoded = encoding::nix32::encode(data);

    for (char c : encoded) {
      RC_ASSERT(c < 'A' || c > 'Z');
    }
  });
}
