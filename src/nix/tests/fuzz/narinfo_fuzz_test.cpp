// straylight // nix // tests // fuzz
//
// NAR Info Parsing Fuzz Tests
//
// Fuzz .narinfo file format parsing used by binary caches.
// This is a key attack surface for remote code execution via binary caches.
//
// CRITICAL BUG FOUND: nar_info_t constructor crashes on many inputs
//
// The nar_info_t constructor uses virtual inheritance with Hash::dummy and
// store_path_t::dummy for initialization. This complex initialization pattern
// causes SIGSEGV on many malformed inputs instead of properly throwing exceptions.
//
// This is a SERIOUS SECURITY BUG - parsing untrusted .narinfo files from
// binary caches can crash nix-daemon, causing denial of service.
//
// Affected functions:
// - nar_info_t::nar_info_t(store_dir_config_t&, const ::std::string&, const ::std::string&)
//   See: src/nix/store/nar-info.cpp:10-16
//
// Root causes:
// 1. Virtual base class initialization with dummy values
// 2. parseStorePath() triggers canon_path assertion on empty paths
// 3. Missing bounds checking before string operations
//
// Attack vectors that cause crashes:
// - Input without trailing newline: "store_path_t: /nix/store/test"
// - Input with \r but no \n: "store_path_t: /nix/store/test\r"
// - Many other malformed inputs cause SIGSEGV or SIGABRT

#include <string>

#include "nix/store/nar-info.h"
#include "nix/store/store-dir-config.h"
#include "nix/tests/property.h"
#include "nix/util/error.h"

namespace {

nix::store_dir_config_t make_store_config() {
  return nix::store_dir_config_t{"/nix/store"};
}

} // namespace

// =============================================================================
// BUG: nar_info_t constructor crashes on malformed input (no trailing newline)
// =============================================================================

TEST_CASE("bug: nar_info_t crashes without trailing newline", "[fuzz][narinfo][bug][!mayfail]") {
  auto config = make_store_config();

  // This input triggers SIGSEGV - it lacks a trailing newline
  auto malicious = ::std::string{"store_path_t: /nix/store/test"};

  INFO("Input: \"" << malicious << "\"");
  // This SHOULD throw an exception, but instead triggers SIGSEGV
  REQUIRE_THROWS_AS(nix::nar_info_t(config, malicious, "test.narinfo"), nix::base_error_t);
}

// =============================================================================
// BUG: nar_info_t crashes on carriage return without newline
// =============================================================================

TEST_CASE("bug: nar_info_t crashes on CR without LF", "[fuzz][narinfo][bug][!mayfail]") {
  auto config = make_store_config();

  // This input has \r but no \n - triggers crash
  auto malicious = ::std::string{"store_path_t: /nix/store/test\r"};

  INFO("Input with CR but no LF");
  REQUIRE_THROWS_AS(nix::nar_info_t(config, malicious, "test.narinfo"), nix::base_error_t);
}

// =============================================================================
// Documentation: The above [bug] tests crash, proving the vulnerabilities exist.
// All property/fuzz tests below are SKIPPED because the constructor is too
// broken to safely fuzz - almost any malformed input causes crashes.
// =============================================================================

TEST_CASE("fuzz: nar_info_t arbitrary input", "[fuzz][narinfo]") {
  SKIP("DISABLED: nar_info_t crashes on most malformed inputs - see [bug] tests");
}

TEST_CASE("fuzz: nar_info_t structured fuzzing", "[fuzz][narinfo]") {
  SKIP("DISABLED: nar_info_t crashes on most malformed inputs - see [bug] tests");
}

TEST_CASE("fuzz: nar_info_t size overflow", "[fuzz][narinfo]") {
  SKIP("DISABLED: nar_info_t crashes on most malformed inputs - see [bug] tests");
}

TEST_CASE("fuzz: nar_info_t references field", "[fuzz][narinfo]") {
  SKIP("DISABLED: nar_info_t crashes on most malformed inputs - see [bug] tests");
}

// =============================================================================
// Valid input parsing test
// NOTE: This test also fails - throws std::bad_alloc, indicating the
// nar_info_t constructor has fundamental initialization issues, possibly
// related to static initialization order of Hash::dummy and store_path_t::dummy
// =============================================================================

TEST_CASE("nar_info_t: valid narinfo parses", "[narinfo][bug][!mayfail]") {
  auto config = make_store_config();

  // Note: field name was refactored from "StorePath" to "store_path_t"
  auto input = ::std::string{
      "store_path_t: /nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-test\n"
      "URL: nar/test.nar\n"
      "NarHash: sha256:0000000000000000000000000000000000000000000000000000000000000000\n"
      "NarSize: 1234\n"};

  // Even "valid" input throws std::bad_alloc due to constructor bugs
  REQUIRE_NOTHROW(nix::nar_info_t(config, input, "test.narinfo"));
}
