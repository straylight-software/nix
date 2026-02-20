// straylight // nix // tests // fuzz
//
// NixStringContextElem::parse() Fuzz Tests
//
// This tests the string context parsing code path that is exercised when
// reading from the eval cache. The eval cache stores context strings in
// SQLite and parses them on read via NixStringContextElem::parse().
//
// Attack vector: An attacker with write access to ~/.cache/nix/eval-cache-v6/
// can craft malicious SQLite files with arbitrary context strings. When nix
// reads the cache, these strings are parsed and may cause crashes.

#include <string>
#include <vector>

#include "nix/expr/value/context.h"
#include "nix/store/path.h"
#include "nix/store/store-dir-config.h"
#include "nix/tests/property.h"
#include "nix/util/error.h"

using namespace nix;

// =============================================================================
// Direct tests of NixStringContextElem::parse() attack surface
// =============================================================================

TEST_CASE("context parse: empty string throws BadNixStringContextElem", "[fuzz][context]") {
  // Empty string is explicitly checked at context.cpp:33-34
  REQUIRE_THROWS_AS(NixStringContextElem::parse(""), BadNixStringContextElem);
}

TEST_CASE("context parse: malformed opaque path throws BadStorePath", "[fuzz][context]") {
  // Opaque paths (no special prefix) are passed directly to store_path_t constructor
  // which throws BadStorePath for invalid paths

  std::vector<std::string> invalid_paths = {
      "short",                                         // Too short for store path
      "abc",                                           // Way too short
      "ffffffffffffffffffffffffffffffff",              // 32 chars but no dash
      "ffffffffffffffffffffffffffffffff-",             // 33 chars but empty name
      "/nix/store/ffffffffffffffffffffffffffffffff-x", // Full path (should be basename)
      "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee-name",         // Invalid base32 char 'e'
      "oooooooooooooooooooooooooooooooo-name",         // Invalid base32 char 'o'
      "uuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuu-name",         // Invalid base32 char 'u'
      "tttttttttttttttttttttttttttttttt-name",         // Invalid base32 char 't'
  };

  for (const auto& path : invalid_paths) {
    INFO("Testing invalid opaque path: " << path);
    REQUIRE_THROWS_AS(NixStringContextElem::parse(path), BadStorePath);
  }
}

TEST_CASE("context parse: malformed DrvDeep (=prefix) throws BadStorePath", "[fuzz][context]") {
  // DrvDeep paths start with '=' and the rest is passed to store_path_t

  std::vector<std::string> invalid_drv_deep = {
      "=",                                      // Empty after prefix
      "=short",                                 // Too short
      "=ffffffffffffffffffffffffffffffff",      // No dash
      "=ffffffffffffffffffffffffffffffff-",     // Empty name
      "=eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee-name", // Invalid base32
  };

  for (const auto& ctx : invalid_drv_deep) {
    INFO("Testing invalid DrvDeep: " << ctx);
    REQUIRE_THROWS_AS(NixStringContextElem::parse(ctx), BadStorePath);
  }
}

TEST_CASE("context parse: malformed Path (@prefix) throws BadStorePath", "[fuzz][context]") {
  // Path context starts with '@' and the rest is passed to store_path_t

  std::vector<std::string> invalid_path_ctx = {
      "@",                                      // Empty after prefix
      "@short",                                 // Too short
      "@ffffffffffffffffffffffffffffffff",      // No dash
      "@ffffffffffffffffffffffffffffffff-",     // Empty name
      "@eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee-name", // Invalid base32
  };

  for (const auto& ctx : invalid_path_ctx) {
    INFO("Testing invalid Path context: " << ctx);
    REQUIRE_THROWS_AS(NixStringContextElem::parse(ctx), BadStorePath);
  }
}

TEST_CASE("context parse: malformed Built (!prefix) throws", "[fuzz][context]") {
  // Built paths start with '!' and must have a second '!'
  // Format: !<output>!<drv_path>

  SECTION("missing second bang throws BadNixStringContextElem") {
    std::vector<std::string> missing_second_bang = {
        "!output",
        "!output/path",
        "!",
    };

    for (const auto& ctx : missing_second_bang) {
      INFO("Testing Built missing second !: " << ctx);
      REQUIRE_THROWS_AS(NixStringContextElem::parse(ctx), BadNixStringContextElem);
    }
  }

  SECTION("invalid drv path after second bang throws BadStorePath") {
    std::vector<std::string> invalid_drv_path = {
        "!output!short",
        "!output!",
        "!output!eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee-name",
    };

    for (const auto& ctx : invalid_drv_path) {
      INFO("Testing Built with invalid drv path: " << ctx);
      REQUIRE_THROWS_AS(NixStringContextElem::parse(ctx), BadStorePath);
    }
  }
}

TEST_CASE("context parse: valid context strings are accepted", "[fuzz][context]") {
  // Make sure valid inputs still work

  std::string valid_hash = "0123456789abcdfghijklmnpqrsvwxyz"; // Valid nix32 chars, 32 chars
  std::string valid_path = valid_hash + "-test-name";

  SECTION("opaque path") {
    auto elem = NixStringContextElem::parse(valid_path);
    auto* opaque = std::get_if<NixStringContextElem::opaque_t>(&elem.raw);
    REQUIRE(opaque != nullptr);
  }

  SECTION("DrvDeep path") {
    auto elem = NixStringContextElem::parse("=" + valid_path);
    auto* drv_deep = std::get_if<NixStringContextElem::DrvDeep>(&elem.raw);
    REQUIRE(drv_deep != nullptr);
  }

  SECTION("Path context") {
    auto elem = NixStringContextElem::parse("@" + valid_path);
    auto* path_ctx = std::get_if<NixStringContextElem::Path>(&elem.raw);
    REQUIRE(path_ctx != nullptr);
  }

  SECTION("Built path") {
    auto elem = NixStringContextElem::parse("!out!" + valid_path);
    auto* built = std::get_if<NixStringContextElem::Built>(&elem.raw);
    REQUIRE(built != nullptr);
    REQUIRE(built->output == "out");
  }
}

// =============================================================================
// Property tests for fuzzing context parsing
// =============================================================================

TEST_CASE("fuzz: arbitrary strings never crash NixStringContextElem::parse",
          "[fuzz][context][property]") {
  rc::prop("arbitrary strings should throw or succeed, never crash", []() {
    auto s = *rc::gen::arbitrary<std::string>();

    try {
      auto elem = NixStringContextElem::parse(s);
      // If it succeeded, the result should be valid
      auto str = elem.to_string();
      RC_ASSERT(!str.empty() || s.empty());
    } catch (const BadNixStringContextElem&) {
      // Expected for malformed context
    } catch (const BadStorePath&) {
      // Expected for invalid store paths
    } catch (const BadStorePathName&) {
      // Expected for invalid store path names
    } catch (const Error&) {
      // Other nix errors are acceptable
    }

    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: context strings with null bytes", "[fuzz][context][property]") {
  rc::prop("null bytes in context strings should not crash", []() {
    auto s = *rc::gen::arbitrary<std::string>();
    // Insert null bytes at random positions
    auto pos = *rc::gen::inRange<std::size_t>(0, s.size() + 1);
    s.insert(pos, 1, '\0');

    try {
      NixStringContextElem::parse(s);
    } catch (...) {
      // Any exception is fine
    }

    RC_SUCCEED("No crash with null bytes");
  });
}

TEST_CASE("fuzz: very long context strings", "[fuzz][context][property]") {
  rc::prop("very long strings should not crash", []() {
    auto len = *rc::gen::inRange(1000, 100000);
    auto c = *rc::gen::arbitrary<char>();
    std::string s(len, c);

    try {
      NixStringContextElem::parse(s);
    } catch (...) {
      // Any exception is fine
    }

    RC_SUCCEED("No crash with long strings");
  });
}

// =============================================================================
// Attack vector documentation: eval cache context field injection
// =============================================================================

TEST_CASE("doc: eval cache context injection attack vector", "[fuzz][context][doc]") {
  // This documents how an attacker could exploit context parsing via eval cache
  //
  // Attack steps:
  // 1. Attacker gains write access to ~/.cache/nix/eval-cache-v6/
  // 2. Attacker creates a malicious SQLite database with:
  //    - A valid-looking fingerprint hash as filename (*.sqlite)
  //    - An Attributes row with type=2 (String) and malicious context
  // 3. When a user runs `nix build` or similar command that uses the cache:
  //    - EvalCache loads the malicious database
  //    - AttrCursor::getStringWithContext() is called
  //    - attr_db_t::get_attr() reads the context field
  //    - tokenize_string splits by ";" and NixStringContextElem::parse() is called
  //    - Malicious context triggers exception or (if bug exists) crash
  //
  // Current status: All known inputs throw exceptions (safe), no crashes found.
  // The store_path_t constructor properly validates inputs and throws.

  SUCCEED("Attack vector documented - see code comments");
}
