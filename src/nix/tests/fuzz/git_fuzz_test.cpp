// straylight // nix // tests // fuzz
//
// Git Object Parsing Fuzz Tests
//
// Fuzz git object parsing functions. These parse binary git objects
// (blobs and trees) for git content-addressed paths.
//
// Key attack surfaces:
// - parse_ls_remote_line() - Parse git ls-remote output with regex
// - decode_mode() - Convert raw_mode_t to Mode enum
//
// Known vulnerabilities in more complex functions (documented but not tested):
// - parse_blob(): assert(false) in switch default - git.cpp:102
// - parse(): assert(false) in switch default - git.cpp:178
// - dump_tree(): assert(!name2.empty()), assert(name2.back() == '/') - git.cpp:238-239
//
// These require file_system_object_sink_t and source_t which are harder to fuzz
// directly in property tests.

#include <optional>
#include <string>
#include <vector>

#include "nix/tests/property.h"
#include "nix/util/git.h"

using namespace nix::git;

// =============================================================================
// Fuzz: decode_mode with all possible uint32_t values
// =============================================================================

TEST_CASE("fuzz: decode_mode handles arbitrary raw_mode_t", "[fuzz][git]") {
  rc::prop("decode_mode never crashes", []() {
    auto mode = *rc::gen::arbitrary<raw_mode_t>();
    auto result = decode_mode(mode);
    // Either returns a valid Mode or nullopt - never crashes
    if (result) {
      RC_ASSERT(*result == Mode::directory_t || *result == Mode::regular ||
                *result == Mode::executable || *result == Mode::symlink);
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz: parse_ls_remote_line with arbitrary strings
// =============================================================================

TEST_CASE("fuzz: parse_ls_remote_line handles arbitrary input", "[fuzz][git]") {
  rc::prop("parse_ls_remote_line never crashes", []() {
    auto input = *rc::gen::arbitrary<std::string>();
    auto result = parse_ls_remote_line(input);
    // Either returns a valid ls_remote_ref_line_t or nullopt - never crashes
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz: parse_ls_remote_line with structured git ls-remote output
// =============================================================================

TEST_CASE("fuzz: parse_ls_remote_line with structured input", "[fuzz][git]") {
  rc::prop("structured ls-remote fuzzing", []() {
    auto is_symbolic = *rc::gen::arbitrary<bool>();
    auto target = *rc::gen::arbitrary<std::string>();
    auto reference = *rc::gen::arbitrary<std::string>();

    std::string line;
    if (is_symbolic) {
      line = "ref: " + target + "\t" + reference;
    } else {
      line = target + "\t" + reference;
    }

    auto result = parse_ls_remote_line(line);
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Test: parse_ls_remote_line with known valid inputs
// =============================================================================

TEST_CASE("parse_ls_remote_line: valid symbolic ref", "[git]") {
  std::string line = "ref: refs/heads/main\tHEAD";
  auto result = parse_ls_remote_line(line);

  REQUIRE(result.has_value());
  CHECK(result->kind == ls_remote_ref_line_t::Kind::symbolic);
  CHECK(result->target == "refs/heads/main");
  CHECK(result->reference == "HEAD");
}

TEST_CASE("parse_ls_remote_line: valid object ref", "[git]") {
  std::string line = "abc123def456\trefs/heads/main";
  auto result = parse_ls_remote_line(line);

  REQUIRE(result.has_value());
  CHECK(result->kind == ls_remote_ref_line_t::Kind::Object);
  CHECK(result->target == "abc123def456");
  CHECK(result->reference == "refs/heads/main");
}

TEST_CASE("parse_ls_remote_line: object ref without reference", "[git]") {
  std::string line = "abc123def456";
  auto result = parse_ls_remote_line(line);

  REQUIRE(result.has_value());
  CHECK(result->kind == ls_remote_ref_line_t::Kind::Object);
  CHECK(result->target == "abc123def456");
  CHECK(!result->reference.has_value());
}

TEST_CASE("parse_ls_remote_line: empty string", "[git]") {
  std::string line = "";
  auto result = parse_ls_remote_line(line);

  // Empty string should not match
  CHECK(!result.has_value());
}

// =============================================================================
// Fuzz: parse_ls_remote_line with malicious regex inputs
// These test potential ReDoS (regex denial of service) vulnerabilities
// =============================================================================

TEST_CASE("fuzz: parse_ls_remote_line ReDoS resistance", "[fuzz][git]") {
  std::vector<std::string> regex_attack_inputs = {
      // Long strings of whitespace
      std::string(10000, ' '),
      std::string(10000, '\t'),
      // Long strings that match partial patterns
      std::string(10000, 'a'),
      "ref: " + std::string(10000, 'a'),
      std::string(10000, 'a') + "\t" + std::string(10000, 'b'),
      // Alternating patterns
      std::string(5000, '\t'),
      // Long ref: prefix
      "ref: ref: ref: ref: ref: ref: ref: ref: ref: ref: " + std::string(1000, 'x'),
  };

  for (const auto& input : regex_attack_inputs) {
    INFO("Input length: " << input.size());
    // This should complete in reasonable time without hanging
    [[maybe_unused]] auto result = parse_ls_remote_line(input);
  }
  SUCCEED("No ReDoS vulnerability detected");
}

// =============================================================================
// Test: decode_mode with known mode values
// =============================================================================

TEST_CASE("decode_mode: valid modes", "[git]") {
  CHECK(decode_mode(040000) == Mode::directory_t);
  CHECK(decode_mode(0100644) == Mode::regular);
  CHECK(decode_mode(0100755) == Mode::executable);
  CHECK(decode_mode(0120000) == Mode::symlink);
}

TEST_CASE("decode_mode: invalid modes return nullopt", "[git]") {
  CHECK(!decode_mode(0).has_value());
  CHECK(!decode_mode(0777).has_value());
  CHECK(!decode_mode(0100000).has_value());
  CHECK(!decode_mode(0xFFFFFFFF).has_value());
}
