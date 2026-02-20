// straylight // nix // tests // fuzz
//
// Hash Parsing Fuzz Tests
//
// Fuzz the hash parser with malformed and random input.

#include <string>

#include "nix/tests/property.h"
#include "nix/util/base-n.h"
#include "nix/util/error.h"
#include "nix/util/hash.h"


// =============================================================================
// Fuzz hash_t::parse_any_prefixed
// =============================================================================

TEST_CASE("fuzz: hash_t::parse_any_prefixed handles arbitrary input", "[fuzz][hash]") {
  rc::prop("parse_any_prefixed never crashes", []() {
    auto input = *rc::gen::arbitrary<::std::string>();
    try {
      [[maybe_unused]] auto result = nix::hash_t::parse_any_prefixed(input);
    } catch (const nix::base_error_t&) {
    } catch (const ::std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz hash_t::parse_any with optional algorithm
// =============================================================================

TEST_CASE("fuzz: hash_t::parse_any handles arbitrary input", "[fuzz][hash]") {
  rc::prop("parse_any with no algo never crashes", []() {
    auto input = *rc::gen::arbitrary<::std::string>();
    try {
      [[maybe_unused]] auto result = nix::hash_t::parse_any(input, ::std::nullopt);
    } catch (const nix::base_error_t&) {
    } catch (const ::std::exception&) {
    }
    RC_SUCCEED("No crash");
  });

  rc::prop("parse_any with sha256 never crashes", []() {
    auto input = *rc::gen::arbitrary<::std::string>();
    try {
      [[maybe_unused]] auto result = nix::hash_t::parse_any(input, nix::hash_algorithm_t::SHA256);
    } catch (const nix::base_error_t&) {
    } catch (const ::std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz hash_t::parse_sri
// =============================================================================

TEST_CASE("fuzz: hash_t::parse_sri handles arbitrary input", "[fuzz][hash]") {
  rc::prop("parse_sri never crashes", []() {
    auto input = *rc::gen::arbitrary<::std::string>();
    try {
      [[maybe_unused]] auto result = nix::hash_t::parse_sri(input);
    } catch (const nix::base_error_t&) {
    } catch (const ::std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz parse_hash_format
// =============================================================================

TEST_CASE("fuzz: parse_hash_format handles arbitrary input", "[fuzz][hash]") {
  rc::prop("parse_hash_format never crashes", []() {
    auto input = *rc::gen::arbitrary<::std::string>();
    try {
      [[maybe_unused]] auto result = nix::parse_hash_format(input);
    } catch (const nix::base_error_t&) {
    } catch (const ::std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz parse_hash_algo
// =============================================================================

TEST_CASE("fuzz: parse_hash_algo handles arbitrary input", "[fuzz][hash]") {
  rc::prop("parse_hash_algo never crashes", []() {
    auto input = *rc::gen::arbitrary<::std::string>();
    try {
      [[maybe_unused]] auto result = nix::parse_hash_algo(input);
    } catch (const nix::base_error_t&) {
    } catch (const ::std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Structured fuzzing: SRI-like strings
// =============================================================================

TEST_CASE("fuzz: parse_sri with structured SRI strings", "[fuzz][hash]") {
  rc::prop("structured SRI fuzzing", []() {
    auto algo = *rc::gen::element<::std::string>("md5", "sha1", "sha256", "sha512", "blake3", "MD5",
                                                 "SHA1", "SHA256", "SHA512", "BLAKE3", "",
                                                 "invalid", "sha", "sha2", "sha3");
    auto sep = *rc::gen::element<::std::string>("-", ":", "=", "", " ", "\t");
    auto hash_data = *rc::gen::arbitrary<::std::string>();

    auto sri = algo + sep + hash_data;

    try {
      [[maybe_unused]] auto result = nix::hash_t::parse_sri(sri);
    } catch (const nix::base_error_t&) {
    } catch (const ::std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz with base encoding edge cases
// =============================================================================

TEST_CASE("fuzz: hash parsing with base encoding variants", "[fuzz][hash]") {
  rc::prop("base16 hash strings", []() {
    // Generate hex-like strings
    auto hex_chars = *rc::gen::container<::std::string>(rc::gen::element<char>(
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f', 'A', 'B',
        'C', 'D', 'E', 'F', 'g', 'h', 'x', 'z', ' ', '\n'));

    try {
      [[maybe_unused]] auto result =
          nix::hash_t::parse_any(hex_chars, nix::hash_algorithm_t::SHA256);
    } catch (const nix::base_error_t&) {
    } catch (const ::std::exception&) {
    }
    RC_SUCCEED("No crash");
  });

  rc::prop("base64 hash strings", []() {
    // Generate base64-like strings
    auto b64_chars = *rc::gen::container<::std::string>(rc::gen::element<char>(
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R',
        'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j',
        'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1',
        '2', '3', '4', '5', '6', '7', '8', '9', '+', '/', '=', '-', '_'));

    try {
      [[maybe_unused]] auto result = nix::hash_t::parse_any_prefixed("sha256-" + b64_chars);
    } catch (const nix::base_error_t&) {
    } catch (const ::std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// BUG: base16::decode asserts on odd-length strings
// See: src/nix/util/base-n.cpp:39 - assert(s.size() % 2 == 0)
// This should throw an exception, not abort the process.
//
// NOTE: hash_t::parse_any validates length before calling base16::decode,
// so this bug is only reachable via direct calls to base16::decode.
// =============================================================================

TEST_CASE("bug: base16 decode odd length triggers assertion", "[fuzz][hash][bug][!mayfail]") {
  // KNOWN BUG: base16::decode asserts on odd-length hex strings
  // The test is marked [!mayfail] because it crashes the process

  // Odd-length hex strings that should throw, not assert:
  auto malicious_inputs = ::std::vector<::std::string>{
      "a",       // 1 char
      "abc",     // 3 chars
      "12345",   // 5 chars
      "deadbee", // 7 chars (missing final nibble)
  };

  for (const auto& input : malicious_inputs) {
    INFO("Input: \"" << input << "\" (length " << input.size() << ")");
    // Direct call to base16::decode triggers assertion
    // This SHOULD throw an exception, but instead triggers abort()
    REQUIRE_THROWS(nix::base16::decode(input));
  }
}
