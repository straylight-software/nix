// straylight // nix // store // tests
//
// Hash format compatibility tests - executable specification for Nix hash formats
//
// These tests verify that straylight-nix uses the same hash formats as upstream nix.
// Hash format compatibility is CRITICAL for store path computation and NAR verification.
//
// Background:
//   - Nix uses multiple hash algorithms (MD5, SHA1, SHA256, SHA512)
//   - Nix uses multiple hash encodings (base16, nix32, base64, SRI)
//   - The "nix32" encoding is a custom base32 alphabet (omits E, O, U, T)
//   - SRI (Subresource Integrity) format is "<algo>-<base64 hash>"
//   - Content addresses use format "fixed:r:sha256:..."
//   - Hash lengths must be exact for each algorithm
//
// Reference: https://github.com/NixOS/nix/blob/master/src/libutil/hash.hh

#include <array>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "nix/store/content-address.h"
#include "nix/util/base-n.h"
#include "nix/util/base-nix-32.h"
#include "nix/util/hash.h"

using nix::base16;
using nix::base64;
using nix::base_nix32_t;
using nix::content_address_method_t;
using nix::content_address_t;
using nix::hash_algorithm_t;
using nix::hash_format_t;
using nix::hash_sizes;
using nix::hash_string;
using nix::hash_t;
using nix::parse_hash_algo;
using nix::parse_hash_format;
using nix::print_hash_algo;
using nix::print_hash_format;
using nix::regular_hash_size;
using nix::render_content_address;

// =============================================================================
// Hash algorithm support tests
// =============================================================================

TEST_CASE("All hash algorithms are supported", "[store][hash][compatibility]") {
  SECTION("MD5 is supported") {
    auto algo = parse_hash_algo("md5");
    REQUIRE(algo == hash_algorithm_t::md5);
    REQUIRE(print_hash_algo(algo) == "md5");
    REQUIRE(regular_hash_size(algo) == 16);
  }

  SECTION("SHA1 is supported") {
    auto algo = parse_hash_algo("sha1");
    REQUIRE(algo == hash_algorithm_t::sha1);
    REQUIRE(print_hash_algo(algo) == "sha1");
    REQUIRE(regular_hash_size(algo) == 20);
  }

  SECTION("SHA256 is supported") {
    auto algo = parse_hash_algo("sha256");
    REQUIRE(algo == hash_algorithm_t::sha256);
    REQUIRE(print_hash_algo(algo) == "sha256");
    REQUIRE(regular_hash_size(algo) == 32);
  }

  SECTION("SHA512 is supported") {
    auto algo = parse_hash_algo("sha512");
    REQUIRE(algo == hash_algorithm_t::sha512);
    REQUIRE(print_hash_algo(algo) == "sha512");
    REQUIRE(regular_hash_size(algo) == 64);
  }

  SECTION("hash_algorithm_t enum values are distinct") {
    REQUIRE(hash_algorithm_t::md5 != hash_algorithm_t::sha1);
    REQUIRE(hash_algorithm_t::sha1 != hash_algorithm_t::sha256);
    REQUIRE(hash_algorithm_t::sha256 != hash_algorithm_t::sha512);
  }
}

// =============================================================================
// Hash format support tests
// =============================================================================

TEST_CASE("All hash formats are supported", "[store][hash][compatibility]") {
  SECTION("base16 format is supported") {
    auto format = parse_hash_format("base16");
    REQUIRE(format == hash_format_t::base16);
    REQUIRE(print_hash_format(format) == "base16");
  }

  SECTION("base32 (nix32) format is supported") {
    auto format = parse_hash_format("base32");
    REQUIRE(format == hash_format_t::nix32);
    // "base32" is a deprecated alias, canonical name is "nix32"
    REQUIRE(print_hash_format(format) == "nix32");
  }

  SECTION("nix32 format is an alias") {
    auto format = parse_hash_format("nix32");
    REQUIRE(format == hash_format_t::nix32);
  }

  SECTION("base64 format is supported") {
    auto format = parse_hash_format("base64");
    REQUIRE(format == hash_format_t::base64);
    REQUIRE(print_hash_format(format) == "base64");
  }

  SECTION("sri format is supported") {
    auto format = parse_hash_format("sri");
    REQUIRE(format == hash_format_t::sri);
    REQUIRE(print_hash_format(format) == "sri");
  }
}

// =============================================================================
// Hash parsing tests
// =============================================================================

TEST_CASE("Hash parsing from strings", "[store][hash][compatibility]") {
  SECTION("Parse prefixed base16 hash") {
    // A known SHA256 hash of empty string
    auto hash = hash_t::parse_any_prefixed(
        "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(hash.algo() == hash_algorithm_t::sha256);
    REQUIRE(hash.hash_size() == 32);
  }

  SECTION("Parse unprefixed hash with known algorithm") {
    auto hash = hash_t::parse_non_sri_unprefixed(
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        hash_algorithm_t::sha256);
    REQUIRE(hash.algo() == hash_algorithm_t::sha256);
  }

  SECTION("Parse SRI format hash") {
    // SHA256 of empty string in SRI format
    auto hash = hash_t::parse_sri("sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=");
    REQUIRE(hash.algo() == hash_algorithm_t::sha256);
    REQUIRE(hash.hash_size() == 32);
  }

  SECTION("Parse colon-prefixed hash") {
    auto hash = hash_t::parse_any(
        "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", std::nullopt);
    REQUIRE(hash.algo() == hash_algorithm_t::sha256);
  }

  SECTION("Parse with explicit algorithm override") {
    auto hash =
        hash_t::parse_any("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                          hash_algorithm_t::sha256);
    REQUIRE(hash.algo() == hash_algorithm_t::sha256);
  }

  SECTION("parse_any_returning_format returns correct format") {
    auto [hash, format] = hash_t::parse_any_returning_format(
        "sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=", std::nullopt);
    REQUIRE(format == hash_format_t::sri);
    REQUIRE(hash.algo() == hash_algorithm_t::sha256);
  }
}

// =============================================================================
// Hash serialization tests
// =============================================================================

TEST_CASE("Hash serialization to strings", "[store][hash][compatibility]") {
  // Create a hash from known bytes
  auto hash = hash_string(hash_algorithm_t::sha256, "");

  SECTION("Serialize to base16") {
    auto str = hash.to_string(hash_format_t::base16, false);
    REQUIRE(str == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  }

  SECTION("Serialize to base16 with algo prefix") {
    auto str = hash.to_string(hash_format_t::base16, true);
    REQUIRE(str == "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  }

  SECTION("Serialize to nix32") {
    auto str = hash.to_string(hash_format_t::nix32, false);
    // Nix32 encoding of SHA256 empty string
    REQUIRE(str.length() == 52); // SHA256 nix32 is 52 chars
    // Verify it only contains valid nix32 characters
    for (char c : str) {
      bool valid = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z');
      // nix32 omits e, o, u, t
      valid = valid && c != 'e' && c != 'o' && c != 'u' && c != 't';
      REQUIRE(valid);
    }
  }

  SECTION("Serialize to base64") {
    auto str = hash.to_string(hash_format_t::base64, false);
    REQUIRE(str == "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=");
  }

  SECTION("Serialize to SRI format") {
    auto str = hash.to_string(hash_format_t::sri, false);
    REQUIRE(str == "sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=");
  }

  SECTION("SRI format always includes algorithm") {
    // Even with include_algo=false, SRI format includes the algorithm
    auto str = hash.to_string(hash_format_t::sri, false);
    REQUIRE(str.starts_with("sha256-"));
  }
}

// =============================================================================
// SRI format tests
// =============================================================================

TEST_CASE("SRI format (sha256-xxxxx)", "[store][hash][compatibility]") {
  SECTION("SRI format uses hyphen separator") {
    auto hash = hash_string(hash_algorithm_t::sha256, "test");
    auto sri = hash.to_string(hash_format_t::sri, false);
    REQUIRE(sri.find('-') != std::string::npos);
    REQUIRE(sri.starts_with("sha256-"));
  }

  SECTION("SRI format uses base64 encoding") {
    auto hash = hash_string(hash_algorithm_t::sha256, "");
    auto sri = hash.to_string(hash_format_t::sri, false);
    auto b64 = hash.to_string(hash_format_t::base64, false);
    REQUIRE(sri == "sha256-" + b64);
  }

  SECTION("Parse SHA256 SRI hash") {
    auto hash = hash_t::parse_sri("sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=");
    REQUIRE(hash.algo() == hash_algorithm_t::sha256);
  }

  SECTION("Parse SHA512 SRI hash") {
    // SHA512 of empty string
    auto hash = hash_t::parse_sri("sha512-z4PhNX7vuL3xVChQ1m2AB9Yg5AULVxXcg/"
                                  "SpIdNs6c5H0NE8XYXysP+DGNKHfuwvY7kxvUdBeoGlODJ6+SfaPg==");
    REQUIRE(hash.algo() == hash_algorithm_t::sha512);
    REQUIRE(hash.hash_size() == 64);
  }

  SECTION("Parse SHA1 SRI hash") {
    // SHA1 of empty string
    auto hash = hash_t::parse_sri("sha1-2jmj7l5rSw0yVb/vlWAYkK/YBwk=");
    REQUIRE(hash.algo() == hash_algorithm_t::sha1);
    REQUIRE(hash.hash_size() == 20);
  }

  SECTION("SRI roundtrip preserves hash") {
    auto original = hash_string(hash_algorithm_t::sha256, "hello world");
    auto sri = original.to_string(hash_format_t::sri, false);
    auto parsed = hash_t::parse_sri(sri);
    REQUIRE(original == parsed);
  }
}

// =============================================================================
// Nix32 encoding tests
// =============================================================================

TEST_CASE("Nix32 encoding (the weird base32 nix uses)", "[store][hash][compatibility]") {
  SECTION("Nix32 alphabet omits E, O, U, T") {
    // The nix32 alphabet is: 0123456789abcdfghijklmnpqrsvwxyz
    const auto& chars = base_nix32_t::characters;
    REQUIRE(chars.size() == 32);

    // Verify omitted characters
    bool has_e = false, has_o = false, has_u = false, has_t = false;
    for (char c : chars) {
      if (c == 'e')
        has_e = true;
      if (c == 'o')
        has_o = true;
      if (c == 'u')
        has_u = true;
      if (c == 't')
        has_t = true;
    }
    REQUIRE_FALSE(has_e);
    REQUIRE_FALSE(has_o);
    REQUIRE_FALSE(has_u);
    REQUIRE_FALSE(has_t);
  }

  SECTION("Nix32 alphabet starts with 0-9") {
    const auto& chars = base_nix32_t::characters;
    for (int i = 0; i < 10; i++) {
      REQUIRE(chars[i] == '0' + i);
    }
  }

  SECTION("Nix32 encoded length calculation") {
    // SHA256 is 32 bytes, should encode to 52 chars
    REQUIRE(base_nix32_t::encoded_length(32) == 52);
    // SHA512 is 64 bytes
    REQUIRE(base_nix32_t::encoded_length(64) == 103);
    // SHA1 is 20 bytes
    REQUIRE(base_nix32_t::encoded_length(20) == 32);
    // MD5 is 16 bytes
    REQUIRE(base_nix32_t::encoded_length(16) == 26);
  }

  SECTION("Nix32 encode/decode roundtrip") {
    std::string original = "hello, world!";
    std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(original.data()),
                                     original.size());
    auto encoded = base_nix32_t::encode(bytes);
    auto decoded = base_nix32_t::decode(encoded);
    REQUIRE(decoded == original);
  }

  SECTION("Hash nix32 roundtrip") {
    auto hash = hash_string(hash_algorithm_t::sha256, "test data");
    auto nix32_str = hash.to_string(hash_format_t::nix32, false);
    auto parsed = hash_t::parse_non_sri_unprefixed(nix32_str, hash_algorithm_t::sha256);
    REQUIRE(hash == parsed);
  }

  SECTION("Nix32 reverse lookup works") {
    // Test valid character lookup
    auto digit = base_nix32_t::lookup_reverse('a');
    REQUIRE(digit.has_value());
    REQUIRE(*digit == 10); // 'a' is at index 10 (after 0-9)

    // Test invalid character
    auto invalid = base_nix32_t::lookup_reverse('e'); // 'e' is omitted
    REQUIRE_FALSE(invalid.has_value());
  }
}

// =============================================================================
// Hash length validation tests
// =============================================================================

TEST_CASE("Hash length validation for each type", "[store][hash][compatibility]") {
  SECTION("MD5 hash length") {
    REQUIRE(hash_sizes::md5 == 16);
    REQUIRE(regular_hash_size(hash_algorithm_t::md5) == 16);

    auto hash = hash_string(hash_algorithm_t::md5, "");
    REQUIRE(hash.hash_size() == 16);

    // Base16 encoded MD5 is 32 chars
    auto b16 = hash.to_string(hash_format_t::base16, false);
    REQUIRE(b16.length() == 32);
  }

  SECTION("SHA1 hash length") {
    REQUIRE(hash_sizes::sha1 == 20);
    REQUIRE(regular_hash_size(hash_algorithm_t::sha1) == 20);

    auto hash = hash_string(hash_algorithm_t::sha1, "");
    REQUIRE(hash.hash_size() == 20);

    // Base16 encoded SHA1 is 40 chars
    auto b16 = hash.to_string(hash_format_t::base16, false);
    REQUIRE(b16.length() == 40);
  }

  SECTION("SHA256 hash length") {
    REQUIRE(hash_sizes::sha256 == 32);
    REQUIRE(regular_hash_size(hash_algorithm_t::sha256) == 32);

    auto hash = hash_string(hash_algorithm_t::sha256, "");
    REQUIRE(hash.hash_size() == 32);

    // Base16 encoded SHA256 is 64 chars
    auto b16 = hash.to_string(hash_format_t::base16, false);
    REQUIRE(b16.length() == 64);
  }

  SECTION("SHA512 hash length") {
    REQUIRE(hash_sizes::sha512 == 64);
    REQUIRE(regular_hash_size(hash_algorithm_t::sha512) == 64);

    auto hash = hash_string(hash_algorithm_t::sha512, "");
    REQUIRE(hash.hash_size() == 64);

    // Base16 encoded SHA512 is 128 chars
    auto b16 = hash.to_string(hash_format_t::base16, false);
    REQUIRE(b16.length() == 128);
  }

  SECTION("Maximum hash size constant") {
    REQUIRE(hash_t::max_hash_size >= 64); // At least SHA512
    REQUIRE(hash_t::max_hash_size == 64); // Currently exactly SHA512
  }
}

// =============================================================================
// Content address format tests
// =============================================================================

TEST_CASE("Content address format (fixed:r:sha256:...)", "[store][hash][compatibility]") {
  SECTION("Parse text content address") {
    auto ca = content_address_t::parse(
        "text:sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(ca.method.raw == content_address_method_t::raw_t::Text);
    REQUIRE(ca.hash.algo() == hash_algorithm_t::sha256);
  }

  SECTION("Parse fixed flat content address") {
    auto ca = content_address_t::parse(
        "fixed:sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(ca.method.raw == content_address_method_t::raw_t::flat);
    REQUIRE(ca.hash.algo() == hash_algorithm_t::sha256);
  }

  SECTION("Parse fixed recursive (NAR) content address") {
    // "r:" prefix indicates recursive (NAR) method
    auto ca = content_address_t::parse(
        "fixed:r:sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(ca.method.raw == content_address_method_t::raw_t::nix_archive);
    REQUIRE(ca.hash.algo() == hash_algorithm_t::sha256);
  }

  SECTION("Content address roundtrip") {
    auto hash = hash_string(hash_algorithm_t::sha256, "test");
    content_address_t ca{.method =
                             content_address_method_t{content_address_method_t::raw_t::nix_archive},
                         .hash = hash};
    auto rendered = ca.render();
    auto parsed = content_address_t::parse(rendered);
    REQUIRE(parsed.method == ca.method);
    REQUIRE(parsed.hash == ca.hash);
  }

  SECTION("Content address method render/parse roundtrip") {
    auto method = content_address_method_t{content_address_method_t::raw_t::nix_archive};
    auto rendered = method.render();
    auto parsed = content_address_method_t::parse(rendered);
    REQUIRE(parsed == method);
  }

  SECTION("All content address methods can be parsed") {
    REQUIRE(content_address_method_t::parse("text").raw == content_address_method_t::raw_t::Text);
    REQUIRE(content_address_method_t::parse("flat").raw == content_address_method_t::raw_t::flat);
    REQUIRE(content_address_method_t::parse("nar").raw ==
            content_address_method_t::raw_t::nix_archive);
  }

  SECTION("parseOpt returns nullopt for empty string") {
    auto ca = content_address_t::parseOpt("");
    REQUIRE_FALSE(ca.has_value());
  }

  SECTION("render_content_address handles nullopt") {
    auto rendered = render_content_address(std::nullopt);
    REQUIRE(rendered.empty());
  }
}

// =============================================================================
// Base16/Base64 encoding tests
// =============================================================================

TEST_CASE("Base16 encoding compatibility", "[store][hash][compatibility]") {
  SECTION("Base16 encode produces lowercase hex") {
    std::array<std::byte, 3> data = {std::byte{0x00}, std::byte{0xff}, std::byte{0x42}};
    auto encoded = base16::encode(data);
    REQUIRE(encoded == "00ff42");
  }

  SECTION("Base16 decode handles lowercase") {
    auto decoded = base16::decode("deadbeef");
    REQUIRE(decoded.size() == 4);
    REQUIRE(static_cast<unsigned char>(decoded[0]) == 0xde);
    REQUIRE(static_cast<unsigned char>(decoded[1]) == 0xad);
    REQUIRE(static_cast<unsigned char>(decoded[2]) == 0xbe);
    REQUIRE(static_cast<unsigned char>(decoded[3]) == 0xef);
  }

  SECTION("Base16 roundtrip") {
    std::string original = "hello world";
    std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(original.data()),
                                     original.size());
    auto encoded = base16::encode(bytes);
    auto decoded = base16::decode(encoded);
    REQUIRE(decoded == original);
  }

  SECTION("Base16 encoded length calculation") {
    REQUIRE(base16::encoded_length(0) == 0);
    REQUIRE(base16::encoded_length(1) == 2);
    REQUIRE(base16::encoded_length(32) == 64);
  }
}

TEST_CASE("Base64 encoding compatibility", "[store][hash][compatibility]") {
  SECTION("Base64 encode standard vectors") {
    std::string data = "";
    std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data.data()), data.size());
    REQUIRE(base64::encode(bytes) == "");

    data = "f";
    bytes = std::span(reinterpret_cast<const std::byte*>(data.data()), data.size());
    REQUIRE(base64::encode(bytes) == "Zg==");

    data = "fo";
    bytes = std::span(reinterpret_cast<const std::byte*>(data.data()), data.size());
    REQUIRE(base64::encode(bytes) == "Zm8=");

    data = "foo";
    bytes = std::span(reinterpret_cast<const std::byte*>(data.data()), data.size());
    REQUIRE(base64::encode(bytes) == "Zm9v");
  }

  SECTION("Base64 decode standard vectors") {
    REQUIRE(base64::decode("") == "");
    REQUIRE(base64::decode("Zg==") == "f");
    REQUIRE(base64::decode("Zm8=") == "fo");
    REQUIRE(base64::decode("Zm9v") == "foo");
  }

  SECTION("Base64 roundtrip") {
    std::string original = "The quick brown fox jumps over the lazy dog";
    std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(original.data()),
                                     original.size());
    auto encoded = base64::encode(bytes);
    auto decoded = base64::decode(encoded);
    REQUIRE(decoded == original);
  }

  SECTION("Base64 encoded length calculation") {
    // Base64 produces 4 chars for every 3 bytes, rounded up with padding
    REQUIRE(base64::encoded_length(0) == 0);
    REQUIRE(base64::encoded_length(1) == 4);
    REQUIRE(base64::encoded_length(2) == 4);
    REQUIRE(base64::encoded_length(3) == 4);
    REQUIRE(base64::encoded_length(4) == 8);
  }
}

// =============================================================================
// Hash comparison and equality tests
// =============================================================================

TEST_CASE("Hash comparison and equality", "[store][hash][compatibility]") {
  SECTION("Same hash equals itself") {
    auto hash1 = hash_string(hash_algorithm_t::sha256, "test");
    auto hash2 = hash_string(hash_algorithm_t::sha256, "test");
    REQUIRE(hash1 == hash2);
  }

  SECTION("Different content produces different hashes") {
    auto hash1 = hash_string(hash_algorithm_t::sha256, "test1");
    auto hash2 = hash_string(hash_algorithm_t::sha256, "test2");
    REQUIRE(hash1 != hash2);
  }

  SECTION("Same content with different algorithms produces different hashes") {
    auto sha256 = hash_string(hash_algorithm_t::sha256, "test");
    auto sha512 = hash_string(hash_algorithm_t::sha512, "test");
    REQUIRE(sha256 != sha512);
  }

  SECTION("Hash ordering is consistent") {
    auto hash1 = hash_string(hash_algorithm_t::sha256, "aaa");
    auto hash2 = hash_string(hash_algorithm_t::sha256, "bbb");
    // Just verify ordering is total
    REQUIRE((hash1 < hash2 || hash1 > hash2 || hash1 == hash2));
  }
}
