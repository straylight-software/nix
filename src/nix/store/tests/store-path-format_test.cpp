// straylight // nix // store // tests
//
// Store path format compatibility tests - executable specification for Nix store paths
//
// These tests verify that straylight-nix uses the same store path format as upstream nix.
// Store path format compatibility is CRITICAL for interoperability with existing stores.
//
// Background:
//   - Store paths follow the format: /nix/store/<hash>-<name>
//   - Hash is exactly 32 characters in Nix's custom base32 (omits e, o, u, t)
//   - Name has strict restrictions on allowed characters and length
//   - .drv extension indicates derivation files
//   - Content-addressed paths use specific prefix formats
//
// Reference: https://nixos.org/manual/nix/stable/store/store-path

#include <cstddef>
#include <regex>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "nix/store/content-address.h"
#include "nix/store/path-regex.h"
#include "nix/store/path.h"
#include "nix/store/store-dir-config.h"
#include "nix/util/base-nix-32.h"
#include "nix/util/hash.h"

namespace {

// =============================================================================
// Test constants - upstream nix compatibility
// =============================================================================

// The standard Nix store directory
constexpr std::string_view k_default_store_dir = "/nix/store";

// Store path hash is 160 bits encoded in base32 = 32 characters
constexpr size_t k_store_path_hash_len = 32;

// Maximum name length (from upstream nix)
constexpr size_t k_max_name_len = 211;

// Nix base32 alphabet (omits e, o, u, t to avoid confusing with 0, 1, etc.)
constexpr std::string_view k_nix_base32_chars = "0123456789abcdfghijklmnpqrsvwxyz";

// Characters NOT allowed in base32 (the omitted ones)
constexpr std::string_view k_invalid_base32_chars = "eout";

// =============================================================================
// Test helpers
// =============================================================================

bool is_valid_base32_char(char c) {
  return k_nix_base32_chars.find(c) != std::string_view::npos;
}

bool is_valid_name_char(char c) {
  // From path.cpp: alphanumeric plus + - . _ ? =
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '+' ||
         c == '-' || c == '.' || c == '_' || c == '?' || c == '=';
}

std::string make_valid_hash(char fill = 'a') {
  // Create a valid 32-character base32 hash
  return std::string(k_store_path_hash_len, fill);
}

} // namespace

// =============================================================================
// Store path regex pattern tests
// =============================================================================

TEST_CASE("Store path regex matches upstream format", "[store][path][format][compatibility]") {
  SECTION("nameRegexStr matches valid names") {
    std::string pattern{nix::nameRegexStr};
    std::regex name_re{pattern};

    // Basic valid names
    REQUIRE(std::regex_match(std::string{"hello"}, name_re));
    REQUIRE(std::regex_match(std::string{"hello-world"}, name_re));
    REQUIRE(std::regex_match(std::string{"hello_world"}, name_re));
    REQUIRE(std::regex_match(std::string{"hello.world"}, name_re));
    REQUIRE(std::regex_match(std::string{"hello+world"}, name_re));
    REQUIRE(std::regex_match(std::string{"hello123"}, name_re));
    REQUIRE(std::regex_match(std::string{"123hello"}, name_re));
    REQUIRE(std::regex_match(std::string{"HELLO"}, name_re));
    REQUIRE(std::regex_match(std::string{"Hello-World_123"}, name_re));
    REQUIRE(std::regex_match(std::string{"foo?bar"}, name_re));
    REQUIRE(std::regex_match(std::string{"foo=bar"}, name_re));
  }

  SECTION("nameRegexStr rejects invalid names") {
    std::string pattern{nix::nameRegexStr};
    std::regex name_re{pattern};

    // Single dot and double dot are rejected
    REQUIRE_FALSE(std::regex_match(std::string{"."}, name_re));
    REQUIRE_FALSE(std::regex_match(std::string{".."}, name_re));

    // Dot followed by dash is rejected (matches (?!\.\.?(-|$)))
    REQUIRE_FALSE(std::regex_match(std::string{".-foo"}, name_re));
    REQUIRE_FALSE(std::regex_match(std::string{"..-foo"}, name_re));

    // Empty string is rejected
    REQUIRE_FALSE(std::regex_match(std::string{""}, name_re));
  }

  SECTION("Full store path pattern matches format") {
    // Full regex for store paths: /nix/store/[hash]-[name]
    std::string full_pattern = std::string{k_default_store_dir} + "/[0-9a-df-np-sv-z]{32}-" +
                               std::string{nix::nameRegexStr};
    std::regex full_re{full_pattern};

    REQUIRE(std::regex_match(std::string{"/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa0-hello"},
                             full_re));
    REQUIRE(std::regex_match(std::string{"/nix/store/00000000000000000000000000000000-foo-1.0"},
                             full_re));
    REQUIRE(std::regex_match(std::string{"/nix/store/abcdfghijklmnpqrsvwxyz0123456789-test.drv"},
                             full_re));
  }
}

// =============================================================================
// Hash format tests (32 characters base32)
// =============================================================================

TEST_CASE("Store path hash is exactly 32 characters base32",
          "[store][path][format][compatibility]") {
  SECTION("HashLen constant is 32") {
    REQUIRE(nix::store_path_t::HashLen == 32);
    REQUIRE(nix::store_path_t::HashLen == k_store_path_hash_len);
  }

  SECTION("Nix base32 alphabet has 32 characters") {
    REQUIRE(nix::base_nix32_t::characters.size() == 32);
  }

  SECTION("Nix base32 omits e, o, u, t") {
    // These letters are omitted to avoid confusion with digits
    auto& chars = nix::base_nix32_t::characters;
    std::string alphabet(chars.begin(), chars.end());

    REQUIRE(alphabet.find('e') == std::string::npos);
    REQUIRE(alphabet.find('o') == std::string::npos);
    REQUIRE(alphabet.find('u') == std::string::npos);
    REQUIRE(alphabet.find('t') == std::string::npos);

    // But lowercase versions of other letters are present
    REQUIRE(alphabet.find('a') != std::string::npos);
    REQUIRE(alphabet.find('b') != std::string::npos);
    REQUIRE(alphabet.find('c') != std::string::npos);
    REQUIRE(alphabet.find('d') != std::string::npos);
    REQUIRE(alphabet.find('f') != std::string::npos);
  }

  SECTION("Invalid base32 characters are rejected in hash part") {
    // Paths with e, o, u, t in hash should be rejected
    REQUIRE_THROWS_AS(nix::store_path_t("eaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa0-test"),
                      nix::BadStorePath);
    REQUIRE_THROWS_AS(nix::store_path_t("aoaaaaaaaaaaaaaaaaaaaaaaaaaaaaa0-test"),
                      nix::BadStorePath);
    REQUIRE_THROWS_AS(nix::store_path_t("auaaaaaaaaaaaaaaaaaaaaaaaaaaaaa0-test"),
                      nix::BadStorePath);
    REQUIRE_THROWS_AS(nix::store_path_t("ataaaaaaaaaaaaaaaaaaaaaaaaaaaaa0-test"),
                      nix::BadStorePath);
  }

  SECTION("Valid base32 hash is accepted") {
    // All valid base32 characters
    REQUIRE_NOTHROW(nix::store_path_t("00000000000000000000000000000000-test"));
    REQUIRE_NOTHROW(nix::store_path_t("abcdfghijklmnpqrsvwxyz0123456789-test"));
    REQUIRE_NOTHROW(nix::store_path_t("99999999999999999999999999999999-test"));
  }

  SECTION("Hash part extraction works correctly") {
    nix::store_path_t path("abcdfghijklmnpqrsvwxyz0123456789-hello");
    REQUIRE(path.hash_part() == "abcdfghijklmnpqrsvwxyz0123456789");
    REQUIRE(path.hash_part().length() == 32);
  }
}

// =============================================================================
// Name format restriction tests
// =============================================================================

TEST_CASE("Store path name restrictions match upstream", "[store][path][format][compatibility]") {
  SECTION("MaxPathLen constant is 211") {
    REQUIRE(nix::store_path_t::MaxPathLen == 211);
    REQUIRE(nix::store_path_t::MaxPathLen == k_max_name_len);
  }

  SECTION("Empty name is rejected") {
    REQUIRE_THROWS_AS(nix::store_path_t(make_valid_hash() + "-"), nix::BadStorePath);
  }

  SECTION("Name too long is rejected") {
    std::string long_name(k_max_name_len + 1, 'a');
    REQUIRE_THROWS_AS(nix::store_path_t(make_valid_hash() + "-" + long_name), nix::BadStorePath);
  }

  SECTION("Name at max length is accepted") {
    std::string max_name(k_max_name_len, 'a');
    REQUIRE_NOTHROW(nix::store_path_t(make_valid_hash() + "-" + max_name));
  }

  SECTION("Allowed characters are accepted") {
    // Alphanumeric
    REQUIRE_NOTHROW(nix::store_path_t(make_valid_hash() + "-abc123"));
    REQUIRE_NOTHROW(nix::store_path_t(make_valid_hash() + "-ABC123"));
    REQUIRE_NOTHROW(nix::store_path_t(make_valid_hash() + "-abcABC"));

    // Special allowed characters: + - . _ ? =
    REQUIRE_NOTHROW(nix::store_path_t(make_valid_hash() + "-a+b"));
    REQUIRE_NOTHROW(nix::store_path_t(make_valid_hash() + "-a-b"));
    REQUIRE_NOTHROW(nix::store_path_t(make_valid_hash() + "-a.b"));
    REQUIRE_NOTHROW(nix::store_path_t(make_valid_hash() + "-a_b"));
    REQUIRE_NOTHROW(nix::store_path_t(make_valid_hash() + "-a?b"));
    REQUIRE_NOTHROW(nix::store_path_t(make_valid_hash() + "-a=b"));
  }

  SECTION("Disallowed characters are rejected") {
    // Common illegal characters
    REQUIRE_THROWS_AS(nix::store_path_t(make_valid_hash() + "-a/b"), nix::BadStorePath);
    REQUIRE_THROWS_AS(nix::store_path_t(make_valid_hash() + "-a b"), nix::BadStorePath);
    REQUIRE_THROWS_AS(nix::store_path_t(make_valid_hash() + "-a*b"), nix::BadStorePath);
    REQUIRE_THROWS_AS(nix::store_path_t(make_valid_hash() + "-a@b"), nix::BadStorePath);
    REQUIRE_THROWS_AS(nix::store_path_t(make_valid_hash() + "-a#b"), nix::BadStorePath);
    REQUIRE_THROWS_AS(nix::store_path_t(make_valid_hash() + "-a$b"), nix::BadStorePath);
    REQUIRE_THROWS_AS(nix::store_path_t(make_valid_hash() + "-a%b"), nix::BadStorePath);
    REQUIRE_THROWS_AS(nix::store_path_t(make_valid_hash() + "-a!b"), nix::BadStorePath);
  }

  SECTION("Dot-prefixed names have restrictions") {
    // Single dot is rejected
    REQUIRE_THROWS_AS(nix::store_path_t(make_valid_hash() + "-."), nix::BadStorePath);

    // Double dot is rejected
    REQUIRE_THROWS_AS(nix::store_path_t(make_valid_hash() + "-.."), nix::BadStorePath);

    // Dot followed by dash is rejected
    REQUIRE_THROWS_AS(nix::store_path_t(make_valid_hash() + "-.-foo"), nix::BadStorePath);
    REQUIRE_THROWS_AS(nix::store_path_t(make_valid_hash() + "-..-foo"), nix::BadStorePath);

    // But dot followed by other characters is OK
    REQUIRE_NOTHROW(nix::store_path_t(make_valid_hash() + "-.foo"));
    REQUIRE_NOTHROW(nix::store_path_t(make_valid_hash() + "-..foo"));
    REQUIRE_NOTHROW(nix::store_path_t(make_valid_hash() + "-.hidden"));
  }

  SECTION("Name extraction works correctly") {
    nix::store_path_t path("00000000000000000000000000000000-hello-world");
    REQUIRE(path.name() == "hello-world");
  }
}

// =============================================================================
// .drv extension handling tests
// =============================================================================

TEST_CASE("Derivation extension handling matches upstream",
          "[store][path][format][compatibility]") {
  SECTION("drvExtension constant is .drv") {
    REQUIRE(nix::drvExtension == ".drv");
  }

  SECTION("is_derivation() detects .drv suffix") {
    nix::store_path_t drv_path(make_valid_hash() + "-hello.drv");
    REQUIRE(drv_path.is_derivation());

    nix::store_path_t non_drv_path(make_valid_hash() + "-hello");
    REQUIRE_FALSE(non_drv_path.is_derivation());
  }

  SECTION("is_derivation() handles edge cases") {
    // .drv in the middle is not a derivation
    nix::store_path_t middle_drv(make_valid_hash() + "-hello.drv.bak");
    REQUIRE_FALSE(middle_drv.is_derivation());

    // Just .drv is a valid derivation name
    nix::store_path_t just_drv(make_valid_hash() + "-.drv");
    REQUIRE(just_drv.is_derivation());
  }

  SECTION("requireDerivation() throws for non-derivations") {
    nix::store_path_t non_drv(make_valid_hash() + "-hello");
    REQUIRE_THROWS(non_drv.requireDerivation());

    nix::store_path_t drv(make_valid_hash() + "-hello.drv");
    REQUIRE_NOTHROW(drv.requireDerivation());
  }
}

// =============================================================================
// Content-addressed path format tests
// =============================================================================

TEST_CASE("Content-addressed path formats match upstream", "[store][path][format][compatibility]") {
  SECTION("Text content address format") {
    // Format: text:sha256:<hash>
    auto ca = nix::content_address_t::parse(
        "text:sha256:0mdqa9w1p6cmli6976v4wi0sw9r4p5prkj7lzfd1877wk11c9c73");

    REQUIRE(ca.method.raw == nix::content_address_method_t::raw_t::Text);
    REQUIRE(ca.hash.algo() == nix::hash_algorithm_t::SHA256);
  }

  SECTION("Fixed flat content address format") {
    // Format: fixed:sha256:<hash> (no 'r:' prefix means flat)
    auto ca = nix::content_address_t::parse(
        "fixed:sha256:0mdqa9w1p6cmli6976v4wi0sw9r4p5prkj7lzfd1877wk11c9c73");

    REQUIRE(ca.method.raw == nix::content_address_method_t::raw_t::flat);
    REQUIRE(ca.hash.algo() == nix::hash_algorithm_t::SHA256);
  }

  SECTION("Fixed recursive (NAR) content address format") {
    // Format: fixed:r:sha256:<hash>
    auto ca = nix::content_address_t::parse(
        "fixed:r:sha256:0mdqa9w1p6cmli6976v4wi0sw9r4p5prkj7lzfd1877wk11c9c73");

    REQUIRE(ca.method.raw == nix::content_address_method_t::raw_t::nix_archive);
    REQUIRE(ca.hash.algo() == nix::hash_algorithm_t::SHA256);
  }

  SECTION("Content address render/parse roundtrip") {
    // Create a content address and verify roundtrip
    nix::Hash hash =
        nix::Hash::parse_any("sha256:0mdqa9w1p6cmli6976v4wi0sw9r4p5prkj7lzfd1877wk11c9c73",
                             nix::hash_algorithm_t::SHA256);
    nix::content_address_t ca{nix::content_address_method_t::raw_t::nix_archive, hash};

    std::string rendered = ca.render();
    auto parsed = nix::content_address_t::parse(rendered);

    REQUIRE(parsed.method == ca.method);
    REQUIRE(parsed.hash == ca.hash);
  }

  SECTION("Content address method names") {
    REQUIRE(nix::content_address_method_t{nix::content_address_method_t::raw_t::flat}.render() ==
            "flat");
    REQUIRE(
        nix::content_address_method_t{nix::content_address_method_t::raw_t::nix_archive}.render() ==
        "nar");
    REQUIRE(nix::content_address_method_t{nix::content_address_method_t::raw_t::Text}.render() ==
            "text");
  }
}

// =============================================================================
// Store path parsing and serialization roundtrip tests
// =============================================================================

TEST_CASE("Store path parsing and serialization roundtrips",
          "[store][path][format][compatibility]") {
  SECTION("to_string() returns original base name") {
    std::string original = "00000000000000000000000000000000-hello-world";
    nix::store_path_t path(original);
    REQUIRE(path.to_string() == original);
  }

  SECTION("Hash constructor produces valid base name") {
    nix::Hash hash = nix::Hash::parse_any("sha1:da39a3ee5e6b4b0d3255bfef95601890afd80709",
                                          nix::hash_algorithm_t::SHA1);
    nix::store_path_t path(hash, "test-pkg");

    // Base name should be hash + "-" + name
    std::string base_name = std::string(path.to_string());
    REQUIRE(base_name.length() > 32 + 1);
    REQUIRE(base_name[32] == '-');
    REQUIRE(path.name() == "test-pkg");
    REQUIRE(path.hash_part().length() == 32);
  }

  SECTION("Parsing extracts components correctly") {
    nix::store_path_t path("abcdfghijklmnpqrsvwxyz0123456789-foo-1.2.3");

    REQUIRE(path.hash_part() == "abcdfghijklmnpqrsvwxyz0123456789");
    REQUIRE(path.name() == "foo-1.2.3");
    REQUIRE(path.to_string() == "abcdfghijklmnpqrsvwxyz0123456789-foo-1.2.3");
  }

  SECTION("Equality comparison works") {
    nix::store_path_t path1("00000000000000000000000000000000-test");
    nix::store_path_t path2("00000000000000000000000000000000-test");
    nix::store_path_t path3("00000000000000000000000000000001-test");
    nix::store_path_t path4("00000000000000000000000000000000-other");

    REQUIRE(path1 == path2);
    REQUIRE(path1 != path3);
    REQUIRE(path1 != path4);
  }

  SECTION("Ordering comparison works") {
    nix::store_path_t path_a("00000000000000000000000000000000-aaa");
    nix::store_path_t path_b("00000000000000000000000000000000-bbb");
    nix::store_path_t path_c("00000000000000000000000000000001-aaa");

    REQUIRE(path_a < path_b);
    REQUIRE(path_a < path_c);
  }
}

// =============================================================================
// Invalid store path rejection tests
// =============================================================================

TEST_CASE("Invalid store paths are rejected", "[store][path][format][compatibility]") {
  SECTION("Too short base name is rejected") {
    // Hash must be at least 32 chars + 1 dash + at least 1 char name
    REQUIRE_THROWS_AS(nix::store_path_t("short"), nix::BadStorePath);
    REQUIRE_THROWS_AS(nix::store_path_t("0000000000000000000000000000000"),
                      nix::BadStorePath); // 31 chars
    REQUIRE_THROWS_AS(nix::store_path_t("00000000000000000000000000000000"),
                      nix::BadStorePath); // 32 chars, no dash
  }

  SECTION("Character at position 32 is ignored as separator") {
    // The store path format skips character at position 32 (the dash position)
    // This is by design - the dash is conventional but any character works
    nix::store_path_t path("00000000000000000000000000000000-hello");
    REQUIRE(path.hash_part() == "00000000000000000000000000000000");
    REQUIRE(path.name() == "hello");

    // Without a dash, the 33rd character is skipped and name starts at 34th
    nix::store_path_t no_dash("00000000000000000000000000000000Xhello");
    REQUIRE(no_dash.hash_part() == "00000000000000000000000000000000");
    REQUIRE(no_dash.name() == "hello"); // 'X' at position 32 is skipped
  }

  SECTION("Uppercase in hash is rejected") {
    // Base32 uses lowercase only
    REQUIRE_THROWS_AS(nix::store_path_t("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA-test"),
                      nix::BadStorePath);
    REQUIRE_THROWS_AS(nix::store_path_t("Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa0-test"),
                      nix::BadStorePath);
  }

  SECTION("Invalid content address strings are rejected") {
    // Malformed content address strings
    REQUIRE_THROWS(nix::content_address_t::parse("invalid"));
    REQUIRE_THROWS(nix::content_address_t::parse("fixed:invalid:sha256:abc"));
    REQUIRE_THROWS(nix::content_address_t::parse("unknown:sha256:abc"));
  }

  SECTION("Dummy path is available for testing") {
    // There's a static dummy path for placeholder use
    REQUIRE(nix::store_path_t::dummy.to_string() == "ffffffffffffffffffffffffffffffff-x");
    REQUIRE(nix::store_path_t::dummy.name() == "x");
  }

  SECTION("Random path generation works") {
    auto path1 = nix::store_path_t::random("test-random");
    auto path2 = nix::store_path_t::random("test-random");

    // Random paths should be valid
    REQUIRE(path1.name() == "test-random");
    REQUIRE(path2.name() == "test-random");

    // And should (almost certainly) be different
    REQUIRE(path1 != path2);
  }
}
