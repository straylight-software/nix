// straylight // nix // tests // fuzz
//
// URL Parsing Fuzz Tests
//
// Fuzz the URL parser with malformed and random input to find crashes,
// hangs, and assertion failures.

#include <string>

#include "nix/tests/property.h"
#include "nix/util/error.h"
#include "nix/util/url.h"


// =============================================================================
// Fuzz parse_url with random strings
// =============================================================================

TEST_CASE("fuzz: parse_url handles arbitrary input", "[fuzz][url]") {
  rc::prop("parse_url never crashes on arbitrary input", []() {
    auto input = *rc::gen::arbitrary<::std::string>();
    try {
      [[maybe_unused]] auto result = nix::parse_url(input);
    } catch (const nix::base_error_t&) {
      // Expected for malformed URLs
    } catch (const ::std::exception&) {
      // Other exceptions are acceptable
    }
    // If we get here without crashing, the test passes
    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: parse_url lenient mode handles arbitrary input", "[fuzz][url]") {
  rc::prop("parse_url lenient never crashes", []() {
    auto input = *rc::gen::arbitrary<::std::string>();
    try {
      [[maybe_unused]] auto result = nix::parse_url(input, true);
    } catch (const nix::base_error_t&) {
    } catch (const ::std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz percent_decode
// =============================================================================

TEST_CASE("fuzz: percent_decode handles arbitrary input", "[fuzz][url]") {
  rc::prop("percent_decode never crashes", []() {
    auto input = *rc::gen::arbitrary<::std::string>();
    try {
      [[maybe_unused]] auto result = nix::percent_decode(input);
    } catch (const nix::base_error_t&) {
    } catch (const ::std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz decode_query
// =============================================================================

TEST_CASE("fuzz: decode_query handles arbitrary input", "[fuzz][url]") {
  rc::prop("decode_query never crashes", []() {
    auto input = *rc::gen::arbitrary<::std::string>();
    try {
      [[maybe_unused]] auto result = nix::decode_query(input);
    } catch (const nix::base_error_t&) {
    } catch (const ::std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: decode_query lenient handles arbitrary input", "[fuzz][url]") {
  rc::prop("decode_query lenient never crashes", []() {
    auto input = *rc::gen::arbitrary<::std::string>();
    try {
      [[maybe_unused]] auto result = nix::decode_query(input, true);
    } catch (const nix::base_error_t&) {
    } catch (const ::std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz parsed_url_t::authority_t::parse
// =============================================================================

TEST_CASE("fuzz: authority_t::parse handles arbitrary input", "[fuzz][url]") {
  rc::prop("authority_t::parse never crashes", []() {
    auto input = *rc::gen::arbitrary<::std::string>();
    try {
      [[maybe_unused]] auto result = nix::parsed_url_t::authority_t::parse(input);
    } catch (const nix::base_error_t&) {
    } catch (const ::std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz fix_git_url
// =============================================================================

TEST_CASE("fuzz: fix_git_url handles arbitrary input", "[fuzz][url]") {
  rc::prop("fix_git_url never crashes", []() {
    auto input = *rc::gen::arbitrary<::std::string>();
    try {
      auto result = nix::fix_git_url(input);
      // NixOS/nix#14867: to_string() must not crash with assertion failure
      [[maybe_unused]] auto str = result.to_string();
    } catch (const nix::base_error_t&) {
    } catch (const ::std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Structured fuzzing: URLs with random components
// =============================================================================

TEST_CASE("fuzz: parse_url with structured random URLs", "[fuzz][url]") {
  rc::prop("structured URL fuzzing", []() {
    auto scheme =
        *rc::gen::element<::std::string>("http", "https", "file", "git", "ssh", "ftp", "");
    auto host = *rc::gen::arbitrary<::std::string>();
    auto port = *rc::gen::inRange(0, 65536);
    auto path = *rc::gen::arbitrary<::std::string>();
    auto query = *rc::gen::arbitrary<::std::string>();
    auto fragment = *rc::gen::arbitrary<::std::string>();

    auto url = scheme + "://" + host;
    if (port > 0 && port < 65536) {
      url += ":" + ::std::to_string(port);
    }
    url += "/" + path;
    if (!query.empty()) {
      url += "?" + query;
    }
    if (!fragment.empty()) {
      url += "#" + fragment;
    }

    try {
      [[maybe_unused]] auto result = nix::parse_url(url);
    } catch (const nix::base_error_t&) {
    } catch (const ::std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Edge case fuzzing: special characters
// =============================================================================

TEST_CASE("fuzz: parse_url with special characters", "[fuzz][url]") {
  rc::prop("URLs with special chars don't crash", []() {
    auto base = *rc::gen::element<::std::string>("http://example.com/", "https://[::1]/",
                                                 "file:///", "git+ssh://user@host/");

    auto suffix = *rc::gen::container<::std::string>(rc::gen::element<char>(
        '\0', '\n', '\r', '\t', ' ', '%', '&', '=', '?', '#', '/', '\\', ':', '@', '[', ']', '{',
        '}', '<', '>', '"', '\'', '|', '^', '`', '\x7f', '\xff'));

    try {
      [[maybe_unused]] auto result = nix::parse_url(base + suffix);
    } catch (const nix::base_error_t&) {
    } catch (const ::std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}
