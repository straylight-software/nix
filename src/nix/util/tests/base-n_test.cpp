// straylight // nix // util // tests
//
// Unit tests for base-n encoding/decoding (base16, base64)

// Catch2 must be included before rapidcheck/catch.h
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

// RapidCheck for property-based testing
#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include "nix/util/base-n.h"


// ─────────────────────────────────────────────────────────────────────────────
// Base16 tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("nix::base16::encode empty input", "[base16]") {
  std::vector<std::byte> empty;
  auto result = nix::base16::encode(std::span<const std::byte>(empty));
  REQUIRE(result.empty());
}

TEST_CASE("nix::base16::encode single byte", "[base16]") {
  std::vector<std::byte> input = {std::byte{0x00}};
  REQUIRE(nix::base16::encode(input) == "00");

  input = {std::byte{0xff}};
  REQUIRE(nix::base16::encode(input) == "ff");

  input = {std::byte{0xab}};
  REQUIRE(nix::base16::encode(input) == "ab");
}

TEST_CASE("nix::base16::encode multiple bytes", "[base16]") {
  std::vector<std::byte> input = {std::byte{0xde}, std::byte{0xad}, std::byte{0xbe},
                                  std::byte{0xef}};
  REQUIRE(nix::base16::encode(input) == "deadbeef");
}

TEST_CASE("nix::base16::decode empty input", "[base16]") {
  auto result = nix::base16::decode("");
  REQUIRE(result.empty());
}

TEST_CASE("nix::base16::decode single byte", "[base16]") {
  REQUIRE(nix::base16::decode("00") == std::string("\x00", 1));
  REQUIRE(nix::base16::decode("ff") == std::string("\xff", 1));
  REQUIRE(nix::base16::decode("FF") == std::string("\xff", 1)); // uppercase
  REQUIRE(nix::base16::decode("aB") == std::string("\xab", 1)); // mixed case
}

TEST_CASE("nix::base16::decode multiple bytes", "[base16]") {
  auto result = nix::base16::decode("deadbeef");
  REQUIRE(result.size() == 4);
  REQUIRE(static_cast<unsigned char>(result[0]) == 0xde);
  REQUIRE(static_cast<unsigned char>(result[1]) == 0xad);
  REQUIRE(static_cast<unsigned char>(result[2]) == 0xbe);
  REQUIRE(static_cast<unsigned char>(result[3]) == 0xef);
}

TEST_CASE("nix::base16::encodedLength", "[base16]") {
  REQUIRE(nix::base16::encoded_length(0) == 0);
  REQUIRE(nix::base16::encoded_length(1) == 2);
  REQUIRE(nix::base16::encoded_length(4) == 8);
  REQUIRE(nix::base16::encoded_length(32) == 64); // SHA256 hash size
}

// ─────────────────────────────────────────────────────────────────────────────
// Base64 tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("nix::base64::encode empty input", "[base64]") {
  std::vector<std::byte> empty;
  auto result = nix::base64::encode(std::span<const std::byte>(empty));
  REQUIRE(result.empty());
}

TEST_CASE("nix::base64::encode known values", "[base64]") {
  // "Man" -> "TWFu"
  std::vector<std::byte> input = {std::byte{'M'}, std::byte{'a'}, std::byte{'n'}};
  REQUIRE(nix::base64::encode(input) == "TWFu");

  // "Ma" -> "TWE=" (with padding)
  input = {std::byte{'M'}, std::byte{'a'}};
  REQUIRE(nix::base64::encode(input) == "TWE=");

  // "M" -> "TQ==" (with more padding)
  input = {std::byte{'M'}};
  REQUIRE(nix::base64::encode(input) == "TQ==");
}

TEST_CASE("nix::base64::decode empty input", "[base64]") {
  auto result = nix::base64::decode("");
  REQUIRE(result.empty());
}

TEST_CASE("nix::base64::decode known values", "[base64]") {
  REQUIRE(nix::base64::decode("TWFu") == "Man");
  REQUIRE(nix::base64::decode("TWE=") == "Ma");
  REQUIRE(nix::base64::decode("TQ==") == "M");
}

TEST_CASE("nix::base64::encodedLength", "[base64]") {
  REQUIRE(nix::base64::encoded_length(0) == 0);
  REQUIRE(nix::base64::encoded_length(1) == 4);
  REQUIRE(nix::base64::encoded_length(2) == 4);
  REQUIRE(nix::base64::encoded_length(3) == 4);
  REQUIRE(nix::base64::encoded_length(4) == 8);
  REQUIRE(nix::base64::encoded_length(32) == 44); // SHA256 hash
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-based tests with RapidCheck
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("base16 property tests", "[base16][property]") {
  rc::prop("roundtrip: decode(encode(x)) == x", []() {
    auto data = *rc::gen::arbitrary<std::vector<uint8_t>>();
    std::vector<std::byte> bytes;
    bytes.reserve(data.size());
    for (auto b : data) {
      bytes.push_back(std::byte{b});
    }

    auto encoded = nix::base16::encode(bytes);
    auto decoded = nix::base16::decode(encoded);

    RC_ASSERT(decoded.size() == bytes.size());
    for (size_t i = 0; i < bytes.size(); ++i) {
      RC_ASSERT(static_cast<uint8_t>(bytes[i]) == static_cast<uint8_t>(decoded[i]));
    }
  });

  rc::prop("encoded length matches prediction", []() {
    auto data = *rc::gen::arbitrary<std::vector<uint8_t>>();
    std::vector<std::byte> bytes;
    bytes.reserve(data.size());
    for (auto b : data) {
      bytes.push_back(std::byte{b});
    }

    auto encoded = nix::base16::encode(bytes);
    RC_ASSERT(encoded.size() == nix::base16::encoded_length(bytes.size()));
  });

  rc::prop("encode produces only hex chars", []() {
    auto data = *rc::gen::arbitrary<std::vector<uint8_t>>();
    std::vector<std::byte> bytes;
    bytes.reserve(data.size());
    for (auto b : data) {
      bytes.push_back(std::byte{b});
    }

    auto encoded = nix::base16::encode(bytes);
    for (char c : encoded) {
      RC_ASSERT((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
  });
}

TEST_CASE("base64 property tests", "[base64][property]") {
  rc::prop("roundtrip: decode(encode(x)) == x", []() {
    auto data = *rc::gen::arbitrary<std::vector<uint8_t>>();
    std::vector<std::byte> bytes;
    bytes.reserve(data.size());
    for (auto b : data) {
      bytes.push_back(std::byte{b});
    }

    auto encoded = nix::base64::encode(bytes);
    auto decoded = nix::base64::decode(encoded);

    RC_ASSERT(decoded.size() == bytes.size());
    for (size_t i = 0; i < bytes.size(); ++i) {
      RC_ASSERT(static_cast<uint8_t>(bytes[i]) == static_cast<uint8_t>(decoded[i]));
    }
  });
}
