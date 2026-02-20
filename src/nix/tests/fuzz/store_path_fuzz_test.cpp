// straylight // nix // tests // fuzz
//
// Store Path Parsing Fuzz Tests
//
// Fuzz store path, derivation, and content address parsing.

#include <string>

#include "nix/store/content-address.h"
#include "nix/store/derived-path.h"
#include "nix/store/outputs-spec.h"
#include "nix/store/path.h"
#include "nix/store/store-dir-config.h"
#include "nix/tests/property.h"
#include "nix/util/error.h"

using namespace nix;

// =============================================================================
// Helper: create a store_dir_config_t for testing
// =============================================================================

namespace {

store_dir_config_t make_store_config() {
  return store_dir_config_t{"/nix/store"};
}

} // namespace

// =============================================================================
// Fuzz store_path_t constructor
// =============================================================================

TEST_CASE("fuzz: store_path_t handles arbitrary base names", "[fuzz][store][path]") {
  rc::prop("store_path_t constructor never crashes", []() {
    auto input = *rc::gen::arbitrary<std::string>();
    try {
      [[maybe_unused]] store_path_t path(input);
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz parseStorePath
// =============================================================================

TEST_CASE("fuzz: parseStorePath handles arbitrary input", "[fuzz][store][path]") {
  rc::prop("parseStorePath never crashes", []() {
    auto input = *rc::gen::arbitrary<std::string>();
    auto config = make_store_config();
    try {
      [[maybe_unused]] auto path = config.parseStorePath(input);
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// BUG: parseStorePath("") triggers assertion instead of exception
// This test documents the bug - it should throw, not assert.
// See: src/nix/util/file-system.cpp:111 - canon_path asserts path != ""
// =============================================================================

TEST_CASE("bug: parseStorePath empty string triggers assertion",
          "[fuzz][store][path][bug][!mayfail]") {
  // KNOWN BUG: This triggers assert(path != "") in canon_path
  // The test is marked [!mayfail] because it crashes the process
  auto config = make_store_config();

  // These inputs all trigger the assertion failure:
  std::vector<std::string> malicious_inputs = {
      "", // empty string
  };

  for (const auto& input : malicious_inputs) {
    INFO("Input: \"" << input << "\"");
    // This SHOULD throw an exception, but instead triggers abort()
    REQUIRE_THROWS_AS(config.parseStorePath(input), base_error_t);
  }
}

// =============================================================================
// Fuzz maybeParseStorePath
// =============================================================================

TEST_CASE("fuzz: maybeParseStorePath handles arbitrary input", "[fuzz][store][path]") {
  rc::prop("maybeParseStorePath never crashes", []() {
    auto input = *rc::gen::arbitrary<std::string>();
    auto config = make_store_config();
    try {
      [[maybe_unused]] auto path = config.maybeParseStorePath(input);
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz content_address_t::parse
// =============================================================================

TEST_CASE("fuzz: content_address_t::parse handles arbitrary input", "[fuzz][store][ca]") {
  rc::prop("content_address_t::parse never crashes", []() {
    auto input = *rc::gen::arbitrary<std::string>();
    try {
      [[maybe_unused]] auto ca = content_address_t::parse(input);
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz content_address_t::parseOpt
// =============================================================================

TEST_CASE("fuzz: content_address_t::parseOpt handles arbitrary input", "[fuzz][store][ca]") {
  rc::prop("content_address_t::parseOpt never crashes", []() {
    auto input = *rc::gen::arbitrary<std::string>();
    try {
      [[maybe_unused]] auto ca = content_address_t::parseOpt(input);
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz content_address_method_t::parse
// =============================================================================

TEST_CASE("fuzz: content_address_method_t::parse handles arbitrary input", "[fuzz][store][ca]") {
  rc::prop("content_address_method_t::parse never crashes", []() {
    auto input = *rc::gen::arbitrary<std::string>();
    try {
      [[maybe_unused]] auto method = content_address_method_t::parse(input);
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz OutputsSpec::parse
// =============================================================================

TEST_CASE("fuzz: OutputsSpec::parse handles arbitrary input", "[fuzz][store][outputs]") {
  rc::prop("OutputsSpec::parse never crashes", []() {
    auto input = *rc::gen::arbitrary<std::string>();
    try {
      [[maybe_unused]] auto spec = OutputsSpec::parse(input);
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz ExtendedOutputsSpec::parse
// =============================================================================

TEST_CASE("fuzz: ExtendedOutputsSpec::parse handles arbitrary input", "[fuzz][store][outputs]") {
  rc::prop("ExtendedOutputsSpec::parse never crashes", []() {
    auto input = *rc::gen::arbitrary<std::string>();
    try {
      [[maybe_unused]] auto spec = ExtendedOutputsSpec::parse(input);
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Structured fuzzing: store path-like strings
// =============================================================================

TEST_CASE("fuzz: parseStorePath with structured paths", "[fuzz][store][path]") {
  rc::prop("structured store path fuzzing", []() {
    auto store_dir = *rc::gen::element<std::string>("/nix/store", "/store", "/", "", "/nix/store/");

    // Generate a hash-like string (32 chars for base32)
    auto hash_chars = *rc::gen::container<std::string>(
        32, rc::gen::element<char>('0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c',
                                   'd', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'p', 'q', 'r',
                                   's', 'v', 'w', 'x', 'y', 'z'));

    auto name = *rc::gen::arbitrary<std::string>();

    std::string path = store_dir + "/" + hash_chars + "-" + name;

    auto config = make_store_config();
    try {
      [[maybe_unused]] auto parsed = config.parseStorePath(path);
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz DerivedPathOpaque::parse
// =============================================================================

TEST_CASE("fuzz: DerivedPathOpaque::parse handles arbitrary input", "[fuzz][store][derived]") {
  rc::prop("DerivedPathOpaque::parse never crashes", []() {
    auto input = *rc::gen::arbitrary<std::string>();
    auto config = make_store_config();
    try {
      [[maybe_unused]] auto path = DerivedPathOpaque::parse(config, input);
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz derived_path_t::parse
// =============================================================================

TEST_CASE("fuzz: derived_path_t::parse handles arbitrary input", "[fuzz][store][derived]") {
  rc::prop("derived_path_t::parse never crashes", []() {
    auto input = *rc::gen::arbitrary<std::string>();
    auto config = make_store_config();
    try {
      [[maybe_unused]] auto path = derived_path_t::parse(config, input);
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz SingleDerivedPath::parse
// =============================================================================

TEST_CASE("fuzz: SingleDerivedPath::parse handles arbitrary input", "[fuzz][store][derived]") {
  rc::prop("SingleDerivedPath::parse never crashes", []() {
    auto input = *rc::gen::arbitrary<std::string>();
    auto config = make_store_config();
    try {
      [[maybe_unused]] auto path = SingleDerivedPath::parse(config, input);
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Structured fuzzing: content address strings
// =============================================================================

TEST_CASE("fuzz: content_address_t::parse with structured CA strings", "[fuzz][store][ca]") {
  rc::prop("structured CA fuzzing", []() {
    auto method = *rc::gen::element<std::string>("text", "fixed", "recursive", "nar", "flat", "git",
                                                 "", "invalid", "TEXT", "FIXED");

    auto algo = *rc::gen::element<std::string>("md5", "sha1", "sha256", "sha512", "blake3", "",
                                               "invalid", "SHA256");

    auto hash_data = *rc::gen::arbitrary<std::string>();

    auto sep = *rc::gen::element<std::string>(":", "-", "=", "", " ");

    std::string ca = method + sep + algo + sep + hash_data;

    try {
      [[maybe_unused]] auto parsed = content_address_t::parse(ca);
    } catch (const base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}
