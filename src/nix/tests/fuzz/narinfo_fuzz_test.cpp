// straylight // nix // tests // fuzz
//
// NAR Info Parsing Fuzz Tests
//
// Fuzz .narinfo file format parsing used by binary caches.
// This is a key attack surface for remote code execution via binary caches.
//
// FIXED BUGS:
// 1. Missing bounds checking before string operations - FIXED in nar-info.cpp
// 2. Dangling reference in test's make_store_config() - FIXED in this file
//
// The parser now properly validates input and throws exceptions on malformed
// input instead of crashing.

#include <string>

#include "nix/store/nar-info.h"
#include "nix/store/store-dir-config.h"
#include "nix/tests/property.h"
#include "nix/util/error.h"

namespace {

nix::store_dir_config_t make_store_config() {
  // IMPORTANT: store_dir_config_t holds a reference, so we need a static string
  // to avoid dangling reference UB
  static const std::string store_dir = "/nix/store";
  return nix::store_dir_config_t{store_dir};
}

} // namespace

// =============================================================================
// Malformed input handling - these should throw proper exceptions
// =============================================================================

TEST_CASE("narinfo: missing newline throws proper error", "[fuzz][narinfo]") {
  auto config = make_store_config();

  // Input without trailing newline - should throw, not crash
  auto malicious = ::std::string{"store_path_t: /nix/store/test"};

  INFO("Input: \"" << malicious << "\"");
  REQUIRE_THROWS_AS(nix::nar_info_t(config, malicious, "test.narinfo"), nix::base_error_t);
}

TEST_CASE("narinfo: CR without LF throws proper error", "[fuzz][narinfo]") {
  auto config = make_store_config();

  // Input with \r but no \n - should throw, not crash
  auto malicious = ::std::string{"store_path_t: /nix/store/test\r"};

  INFO("Input with CR but no LF");
  REQUIRE_THROWS_AS(nix::nar_info_t(config, malicious, "test.narinfo"), nix::base_error_t);
}

TEST_CASE("narinfo: missing space after colon throws proper error", "[fuzz][narinfo]") {
  auto config = make_store_config();

  // Input with no space after colon
  auto malicious = ::std::string{"store_path_t:/nix/store/test\n"};

  INFO("Input without space after colon");
  REQUIRE_THROWS_AS(nix::nar_info_t(config, malicious, "test.narinfo"), nix::base_error_t);
}

TEST_CASE("narinfo: truncated after colon throws proper error", "[fuzz][narinfo]") {
  auto config = make_store_config();

  // Input truncated right after colon
  auto malicious = ::std::string{"store_path_t:"};

  INFO("Input truncated after colon");
  REQUIRE_THROWS_AS(nix::nar_info_t(config, malicious, "test.narinfo"), nix::base_error_t);
}

// =============================================================================
// Valid input parsing test
// =============================================================================

TEST_CASE("narinfo: valid input parses correctly", "[fuzz][narinfo]") {
  auto config = make_store_config();

  auto input = ::std::string{
      "store_path_t: /nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-test\n"
      "URL: nar/test.nar\n"
      "NarHash: sha256:0000000000000000000000000000000000000000000000000000000000000000\n"
      "NarSize: 1234\n"};

  REQUIRE_NOTHROW(nix::nar_info_t(config, input, "test.narinfo"));
}

// =============================================================================
// CRLF handling tests
// =============================================================================

TEST_CASE("narinfo: CRLF line endings are handled", "[fuzz][narinfo]") {
  auto config = make_store_config();

  // Windows-style line endings
  auto input = ::std::string{
      "store_path_t: /nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-test\r\n"
      "URL: nar/test.nar\r\n"
      "NarHash: sha256:0000000000000000000000000000000000000000000000000000000000000000\r\n"
      "NarSize: 1234\r\n"};

  REQUIRE_NOTHROW(nix::nar_info_t(config, input, "test.narinfo"));
}

// =============================================================================
// Placeholder fuzz tests - can be enabled now that basic parsing is safe
// =============================================================================

TEST_CASE("fuzz: nar_info_t arbitrary input", "[fuzz][narinfo][.]") {
  SKIP("TODO: Implement arbitrary input fuzzing");
}

TEST_CASE("fuzz: nar_info_t structured fuzzing", "[fuzz][narinfo][.]") {
  SKIP("TODO: Implement structured fuzzing");
}

TEST_CASE("fuzz: nar_info_t size overflow", "[fuzz][narinfo][.]") {
  SKIP("TODO: Implement size overflow fuzzing");
}

TEST_CASE("fuzz: nar_info_t references field", "[fuzz][narinfo][.]") {
  SKIP("TODO: Implement references field fuzzing");
}
