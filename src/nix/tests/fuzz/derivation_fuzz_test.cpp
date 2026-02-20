// straylight // nix // tests // fuzz
//
// Derivation Parsing Fuzz Tests
//
// Fuzz ATerm derivation format parsing, which has complex escape handling,
// recursive parsing, and an assert(false) in a switch statement default case.
//
// Key attack surfaces:
// - parse_derivation() - Main ATerm parser
// - parse_string() - C-style string with escape sequences
// - parse_derivation_output() - Output specification parsing
// - parse_derived_path_map_node() - Recursive input parsing
//
// Known vulnerability:
// - derivations.cpp:399 has assert(false) in switch default case

#include <string>
#include <vector>

#include "nix/store/derivations.h"
#include "nix/store/store-dir-config.h"
#include "nix/tests/property.h"
#include "nix/util/error.h"
#include "nix/util/experimental-features.h"

using namespace nix;

// =============================================================================
// Helper: create a store_dir_config_t for testing
// =============================================================================

namespace {

store_dir_config_t make_store_config() {
  return store_dir_config_t{"/nix/store"};
}

// Minimal valid derivation for reference
const std::string minimal_derivation =
    R"(Derive([("out","","","")],[],[],"x86_64-linux","/bin/sh",["-c","touch $out"],[("out","")]))";

} // namespace

// =============================================================================
// Fuzz parse_derivation with arbitrary strings
// =============================================================================

TEST_CASE("fuzz: parse_derivation handles arbitrary input", "[fuzz][derivation]") {
  rc::prop("parse_derivation never crashes on arbitrary input", []() {
    auto input = *rc::gen::arbitrary<std::string>();
    auto config = make_store_config();

    try {
      [[maybe_unused]] auto drv = parse_derivation(config, std::string(input), "test");
    } catch (const base_error_t&) {
      // Expected: FormatError, Error, etc.
    } catch (const std::exception&) {
      // Other exceptions are fine too
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz parse_derivation with Derive prefix
// =============================================================================

TEST_CASE("fuzz: parse_derivation with Derive prefix", "[fuzz][derivation]") {
  rc::prop("parse_derivation handles malformed Derive(...)", []() {
    auto body = *rc::gen::arbitrary<std::string>();
    std::string input = "Derive(" + body + ")";
    auto config = make_store_config();

    try {
      [[maybe_unused]] auto drv = parse_derivation(config, std::string(input), "test");
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz parse_derivation with DrvWithVersion prefix
// =============================================================================

TEST_CASE("fuzz: parse_derivation with DrvWithVersion prefix", "[fuzz][derivation]") {
  rc::prop("parse_derivation handles malformed DrvWithVersion(...)", []() {
    auto version = *rc::gen::arbitrary<std::string>();
    auto body = *rc::gen::arbitrary<std::string>();
    std::string input = "DrvWithVersion(\"" + version + "\"," + body + ")";
    auto config = make_store_config();

    try {
      [[maybe_unused]] auto drv = parse_derivation(config, std::string(input), "test");
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz: C-style string escape sequences
// =============================================================================

TEST_CASE("fuzz: parse_derivation string escape handling", "[fuzz][derivation]") {
  rc::prop("escape sequences don't crash parser", []() {
    // Generate strings with escape characters
    auto escape_char = *rc::gen::element<char>('n', 'r', 't', '\\', '"', '0', 'x', 'u');
    auto content = *rc::gen::arbitrary<std::string>();

    // Build string with embedded escapes
    std::string str;
    for (size_t i = 0; i < content.size(); ++i) {
      if (i % 3 == 0) {
        str += '\\';
        str += escape_char;
      }
      str += content[i];
    }

    std::string input = "Derive([(\"" + str +
                        "\",\"\",\"\",\"\")],[],[],"
                        "\"x86_64-linux\",\"/bin/sh\",[\"-c\",\"echo\"],[(\"out\",\"\")])";
    auto config = make_store_config();

    try {
      [[maybe_unused]] auto drv = parse_derivation(config, std::string(input), "test");
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz: unterminated strings
// =============================================================================

TEST_CASE("fuzz: parse_derivation unterminated strings", "[fuzz][derivation]") {
  std::vector<std::string> malicious_inputs = {
      "Derive([(\"unterminated",
      "Derive([(\"out\",\"",
      "Derive([(\"out\",\"\",\"",
      "Derive([(\"out\",\"\",\"\",\"",
      "Derive([(\"\\",
      "Derive([(\"\\\\\\",
      "Derive([(\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\""
      "\\\"\\\"",
  };

  auto config = make_store_config();

  for (const auto& input : malicious_inputs) {
    INFO("Input: " << input);
    try {
      [[maybe_unused]] auto drv = parse_derivation(config, std::string(input), "test");
    } catch (const base_error_t&) {
      // Expected
    } catch (const std::exception&) {
      // Also OK
    }
  }
  SUCCEED("No crashes on unterminated strings");
}

// =============================================================================
// Fuzz: deeply nested structures
// =============================================================================

TEST_CASE("fuzz: parse_derivation deeply nested input drvs", "[fuzz][derivation]") {
  rc::prop("deeply nested structures don't crash", []() {
    auto depth = *rc::gen::inRange<size_t>(1, 50);

    // Build nested input drv structure
    std::string nested;
    for (size_t i = 0; i < depth; ++i) {
      nested += "([\"out\",(";
    }
    nested += "[\"out\"]";
    for (size_t i = 0; i < depth; ++i) {
      nested += ")])";
    }

    std::string input = "DrvWithVersion(\"xp-dyn-drv\","
                        "[(\"out\",\"\",\"\",\"\")],"
                        "[(\"/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-foo.drv\"," +
                        nested +
                        ")],"
                        "[],"
                        "\"x86_64-linux\",\"/bin/sh\",[\"-c\",\"echo\"],[(\"out\",\"\")])";

    auto config = make_store_config();

    try {
      [[maybe_unused]] auto drv = parse_derivation(config, std::string(input), "test");
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz: invalid output types
// =============================================================================

TEST_CASE("fuzz: parse_derivation invalid output specifications", "[fuzz][derivation]") {
  rc::prop("invalid output specs don't crash", []() {
    auto hash_algo = *rc::gen::element<std::string>("sha256", "sha512", "sha1", "md5", "blake3", "",
                                                    "invalid", "SHA256", "r:sha256", "text:sha256",
                                                    "git:sha256", "flat:sha256", "nar:sha256");
    auto hash_value = *rc::gen::element<std::string>(
        "", "abc", "impure", "0000000000000000000000000000000000000000000000000000000000000000",
        "sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=", "invalid!");

    std::string input = "Derive([(\"out\",\"/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-out\","
                        "\"" +
                        hash_algo + "\",\"" + hash_value +
                        "\")],"
                        "[],"
                        "[],"
                        "\"x86_64-linux\",\"/bin/sh\",[\"-c\",\"echo\"],[(\"out\",\"\")])";

    auto config = make_store_config();

    try {
      [[maybe_unused]] auto drv = parse_derivation(config, std::string(input), "test");
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz: structured fuzzing with valid-ish structure
// =============================================================================

TEST_CASE("fuzz: parse_derivation structured fuzzing", "[fuzz][derivation]") {
  rc::prop("structured derivation fuzzing", []() {
    auto output_name = *rc::gen::arbitrary<std::string>();
    auto platform = *rc::gen::arbitrary<std::string>();
    auto builder = *rc::gen::arbitrary<std::string>();
    auto arg = *rc::gen::arbitrary<std::string>();
    auto env_key = *rc::gen::arbitrary<std::string>();
    auto env_val = *rc::gen::arbitrary<std::string>();

    // Simple escaping for strings
    auto escape = [](const std::string& s) {
      std::string result;
      for (char c : s) {
        if (c == '"')
          result += "\\\"";
        else if (c == '\\')
          result += "\\\\";
        else if (c == '\n')
          result += "\\n";
        else if (c == '\r')
          result += "\\r";
        else if (c == '\t')
          result += "\\t";
        else
          result += c;
      }
      return result;
    };

    std::string input = "Derive([(\"" + escape(output_name) +
                        "\",\"\",\"\",\"\")],"
                        "[],"
                        "[],"
                        "\"" +
                        escape(platform) + "\",\"" + escape(builder) + "\",[\"" + escape(arg) +
                        "\"],[(\"" + escape(env_key) + "\",\"" + escape(env_val) + "\")])";

    auto config = make_store_config();

    try {
      [[maybe_unused]] auto drv = parse_derivation(config, std::string(input), "test");
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz: binary data in strings
// =============================================================================

TEST_CASE("fuzz: parse_derivation with binary data", "[fuzz][derivation]") {
  rc::prop("binary data doesn't crash parser", []() {
    // Generate arbitrary bytes
    auto bytes = *rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>());

    std::string binary_str(bytes.begin(), bytes.end());

    // Embed in derivation - parser should reject or handle gracefully
    std::string input = "Derive([" + binary_str + "])";

    auto config = make_store_config();

    try {
      [[maybe_unused]] auto drv = parse_derivation(config, std::string(input), "test");
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz: extremely long strings
// =============================================================================

TEST_CASE("fuzz: parse_derivation with long strings", "[fuzz][derivation]") {
  rc::prop("long strings don't crash", []() {
    auto len = *rc::gen::inRange<size_t>(1000, 10000);
    auto ch = *rc::gen::element<char>('a', 'b', 'c', '\\', '"', '\n');

    std::string long_str(len, ch);

    std::string input = "Derive([(\"out\",\"\",\"\",\"\")],"
                        "[(\"/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-foo.drv\",[\"out\"])],"
                        "[\"/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-src\"],"
                        "\"x86_64-linux\",\"/bin/sh\",[\"-c\",\"" +
                        long_str +
                        "\"],"
                        "[(\"out\",\"\")])";

    auto config = make_store_config();

    try {
      [[maybe_unused]] auto drv = parse_derivation(config, std::string(input), "test");
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz: multiple backslashes before quote
// =============================================================================

TEST_CASE("fuzz: parse_derivation backslash quote sequences", "[fuzz][derivation]") {
  // Test edge cases in escape handling: odd/even backslash counts before quote
  std::vector<std::string> edge_cases = {
      R"(Derive([("\\","")]))",      // single escaped backslash
      R"(Derive([("\\\\","")]))",    // two escaped backslashes
      R"(Derive([("\\\"","")]))",    // backslash then escaped quote
      R"(Derive([("\\\\\"","")]))",  // two backslashes then escaped quote
      R"(Derive([("\\\\\\"","")]))", // three backslashes (1.5 escaped) then quote
      R"(Derive([("a\\b","")]))",    // escaped backslash in middle
      R"(Derive([("a\"b","")]))",    // escaped quote in middle
      R"(Derive([("a\\\"b","")]))",  // backslash then escaped quote in middle
  };

  auto config = make_store_config();

  for (const auto& input : edge_cases) {
    INFO("Input: " << input);
    try {
      [[maybe_unused]] auto drv = parse_derivation(config, std::string(input), "test");
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
  }
  SUCCEED("No crashes on escape sequences");
}

// =============================================================================
// Fuzz: is_derivation helper
// =============================================================================

TEST_CASE("fuzz: is_derivation handles arbitrary filenames", "[fuzz][derivation]") {
  rc::prop("is_derivation never crashes", []() {
    auto input = *rc::gen::arbitrary<std::string>();
    [[maybe_unused]] bool result = is_derivation(input);
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Test: valid minimal derivation parses successfully
// =============================================================================

TEST_CASE("parse_derivation: valid minimal derivation", "[derivation]") {
  auto config = make_store_config();

  // This is a valid minimal Derive() format
  std::string valid = "Derive([(\"out\",\"\",\"\",\"\")],[],[],"
                      "\"x86_64-linux\",\"/bin/sh\",[\"-c\",\"touch $out\"],[(\"out\",\"\")])";

  REQUIRE_NOTHROW(parse_derivation(config, std::string(valid), "test"));
}

// =============================================================================
// Test: DrvWithVersion requires experimental feature
// =============================================================================

TEST_CASE("parse_derivation: DrvWithVersion requires xp-dyn-drv", "[derivation]") {
  auto config = make_store_config();

  std::string drvWithVersion = "DrvWithVersion(\"xp-dyn-drv\","
                               "[(\"out\",\"\",\"\",\"\")],"
                               "[],"
                               "[],"
                               "\"x86_64-linux\",\"/bin/sh\",[\"-c\",\"echo\"],[(\"out\",\"\")])";

  // Without xp feature enabled, should throw
  experimental_feature_settings_t no_features;
  REQUIRE_THROWS_AS(parse_derivation(config, std::string(drvWithVersion), "test", no_features),
                    base_error_t);
}
