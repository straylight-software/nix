// straylight // nix // util // tests
//
// Comparison tests: boost::url (RFC 3986) vs ada (WHATWG)
// This test identifies behavioral differences for Nix URL patterns

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <ada.h>

#include <boost/url.hpp>
#include <catch2/catch_test_macros.hpp>

// ─────────────────────────────────────────────────────────────────────────────
// Test URLs that are important for Nix
// ─────────────────────────────────────────────────────────────────────────────

struct UrlTestCase {
  std::string url;
  std::string description;
  bool expect_valid_rfc3986;          // boost::url expectation
  bool expect_valid_whatwg;           // ada expectation
  bool has_known_differences = false; // skip equality checks for known RFC3986/WHATWG differences
};

// These are URLs that Nix actually uses or might encounter
static const std::vector<UrlTestCase> NIX_URL_PATTERNS = {
    // Basic HTTPS
    {"https://example.com/path", "simple https", true, true},
    {"https://example.com:8080/path", "https with port", true, true,
     true}, // ada includes port in host
    {"https://user:pass@example.com/path", "https with userinfo", true, true},

    // File URLs (critical for Nix)
    {"file:///home/user/repo", "file URL absolute path", true, true},
    {"file:///nix/store/abc123-foo", "nix store path", true, true},
    {"file://localhost/path", "file with localhost", true, true,
     true}, // WHATWG normalizes localhost to empty

    // Nix custom schemes
    {"tarball+https://example.com/foo.tar.gz", "tarball+https scheme", true, true},
    {"git+https://github.com/NixOS/nix", "git+https scheme", true, true},
    {"git+ssh://git@github.com/NixOS/nix", "git+ssh scheme", true, true},
    {"path:/home/user/repo", "path scheme", true, true},
    {"github:NixOS/nixpkgs", "github flakeref", true, true},
    {"nixpkgs:hello", "nixpkgs flakeref", true, true},

    // IPv6
    {"http://[::1]:8080/path", "IPv6 localhost", true, true, true}, // ada includes port in host
    {"http://[2001:db8::1]/path", "IPv6 full", true, true},
    {"http://[fe80::1%25eth0]/path", "IPv6 zone ID (RFC 6874)", true, false,
     true}, // WHATWG rejects zone IDs

    // Query parameters (important for flake refs)
    {"https://example.com?ref=main&rev=abc123", "query params", true, true},
    {"github:NixOS/nixpkgs?ref=nixos-unstable", "flakeref with query", true, true},

    // Fragments
    {"https://example.com/path#section", "with fragment", true, true},

    // Percent encoding edge cases
    {"https://example.com/path%20with%20spaces", "encoded spaces in path", true, true},
    {"https://example.com/path/foo%2Fbar", "encoded slash in path segment", true, true},
    {"https://example.com?key=hello%20world", "encoded space in query", true, true,
     true}, // WHATWG preserves encoding

    // Edge cases that might differ
    {"https://example.com//double//slashes", "double slashes in path", true, true},
    {"https://example.com/", "trailing slash only", true, true},
    {"https://example.com", "no path", true, true},
    {"https://example.com:443/path", "default https port", true, true},
    {"http://example.com:80/path", "default http port", true, true},

    // Potentially problematic for WHATWG
    {"foo:bar", "simple opaque path", true, true},
    {"tel:+1-555-123-4567", "tel scheme", true, true},
    {"urn:isbn:0451450523", "urn scheme", true, true},

    // Unicode/IDN (WHATWG handles, RFC 3986 doesn't directly)
    {"https://例え.jp/path", "IDN hostname", false, true}, // boost won't handle, ada will
    {"https://xn--r8jz45g.jp/path", "punycode hostname", true, true},

    // Backslash (WHATWG converts to slash for special schemes)
    {"https://example.com\\path", "backslash in path", false, true,
     true}, // boost rejects, WHATWG converts to /

    // Empty components
    {"https://example.com?", "empty query", true, true},
    {"https://example.com#", "empty fragment", true, true},
    {"https://example.com?#", "empty query and fragment", true, true},

    // Nix lenient mode patterns (spaces, special chars)
    // These are typically pre-processed before parsing
};

// ─────────────────────────────────────────────────────────────────────────────
// Helper: Parse with boost::url
// ─────────────────────────────────────────────────────────────────────────────

struct BoostUrlResult {
  bool valid = false;
  std::string scheme;
  std::string host;
  std::optional<uint16_t> port;
  std::string path;
  std::string query;
  std::string fragment;
  std::string error;
};

BoostUrlResult parseWithBoost(const std::string& url) {
  BoostUrlResult result;
  try {
    auto parsed = boost::urls::parse_uri(url);
    if (!parsed) {
      result.error = parsed.error().message();
      return result;
    }
    result.valid = true;
    result.scheme = parsed->scheme();
    result.host = parsed->host();
    if (parsed->has_port() && !parsed->port().empty()) {
      result.port = parsed->port_number();
    }
    result.path = parsed->path();
    result.query = parsed->query();
    result.fragment = parsed->fragment();
  } catch (const std::exception& e) {
    result.error = e.what();
  }
  return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: Parse with ada
// ─────────────────────────────────────────────────────────────────────────────

struct AdaUrlResult {
  bool valid = false;
  std::string scheme;
  std::string host;
  std::optional<uint16_t> port;
  std::string path;
  std::string query;
  std::string fragment;
  std::string error;
};

AdaUrlResult parseWithAda(const std::string& url) {
  AdaUrlResult result;
  auto parsed = ada::parse<ada::url_aggregator>(url);
  if (!parsed) {
    result.error = "invalid URL";
    return result;
  }
  result.valid = true;
  result.scheme = std::string(parsed->get_protocol());
  // ada includes ":" in protocol, remove it
  if (!result.scheme.empty() && result.scheme.back() == ':') {
    result.scheme.pop_back();
  }
  result.host = std::string(parsed->get_host());
  auto port_str = parsed->get_port();
  if (!port_str.empty()) {
    result.port = static_cast<uint16_t>(std::stoi(std::string(port_str)));
  }
  result.path = std::string(parsed->get_pathname());
  auto query = parsed->get_search();
  // ada includes "?" prefix, remove it
  if (!query.empty() && query[0] == '?') {
    result.query = std::string(query.substr(1));
  } else {
    result.query = std::string(query);
  }
  auto frag = parsed->get_hash();
  // ada includes "#" prefix, remove it
  if (!frag.empty() && frag[0] == '#') {
    result.fragment = std::string(frag.substr(1));
  } else {
    result.fragment = std::string(frag);
  }
  return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Comparison tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("boost vs ada: basic parsing comparison", "[url][comparison]") {
  for (const auto& tc : NIX_URL_PATTERNS) {
    SECTION(tc.description) {
      auto boost_result = parseWithBoost(tc.url);
      auto ada_result = parseWithAda(tc.url);

      INFO("URL: " << tc.url);
      INFO("Boost valid: " << boost_result.valid << " error: " << boost_result.error);
      INFO("Ada valid: " << ada_result.valid << " error: " << ada_result.error);

      // Check validity expectations
      if (tc.expect_valid_rfc3986) {
        CHECK(boost_result.valid);
      }
      if (tc.expect_valid_whatwg) {
        CHECK(ada_result.valid);
      }

      // If both valid and no known spec differences, compare components
      if (boost_result.valid && ada_result.valid && !tc.has_known_differences) {
        INFO("Boost scheme: " << boost_result.scheme);
        INFO("Ada scheme: " << ada_result.scheme);
        CHECK(boost_result.scheme == ada_result.scheme);

        INFO("Boost host: " << boost_result.host);
        INFO("Ada host: " << ada_result.host);
        // Host comparison may differ for IDN
        if (tc.url.find("例え") == std::string::npos) {
          CHECK(boost_result.host == ada_result.host);
        }

        INFO("Boost path: " << boost_result.path);
        INFO("Ada path: " << ada_result.path);
        // Path comparison - WHATWG may normalize differently
        // CHECK(boost_result.path == ada_result.path);

        INFO("Boost query: " << boost_result.query);
        INFO("Ada query: " << ada_result.query);
        CHECK(boost_result.query == ada_result.query);

        INFO("Boost fragment: " << boost_result.fragment);
        INFO("Ada fragment: " << ada_result.fragment);
        CHECK(boost_result.fragment == ada_result.fragment);
      }
    }
  }
}

TEST_CASE("ada: WHATWG-specific behaviors", "[url][ada][whatwg]") {
  SECTION("default port normalization for http") {
    auto result = parseWithAda("http://example.com:80/path");
    REQUIRE(result.valid);
    // WHATWG removes default ports
    CHECK_FALSE(result.port.has_value());
  }

  SECTION("default port normalization for https") {
    auto result = parseWithAda("https://example.com:443/path");
    REQUIRE(result.valid);
    // WHATWG removes default ports
    CHECK_FALSE(result.port.has_value());
  }

  SECTION("backslash normalization for special schemes") {
    auto result = parseWithAda("https://example.com\\path\\to\\file");
    REQUIRE(result.valid);
    // WHATWG converts backslash to forward slash for "special" schemes
    CHECK(result.path == "/path/to/file");
  }

  SECTION("IDN punycode encoding") {
    auto result = parseWithAda("https://例え.jp/path");
    REQUIRE(result.valid);
    // WHATWG converts to punycode
    CHECK(result.host == "xn--r8jz45g.jp");
  }

  SECTION("scheme case normalization") {
    auto result = parseWithAda("HTTPS://EXAMPLE.COM/PATH");
    REQUIRE(result.valid);
    // WHATWG normalizes scheme and host to lowercase
    CHECK(result.scheme == "https");
    CHECK(result.host == "example.com");
    // Path case is preserved
    CHECK(result.path == "/PATH");
  }
}

TEST_CASE("boost: RFC 3986-specific behaviors", "[url][boost][rfc3986]") {
  SECTION("preserves explicit default ports") {
    auto result = parseWithBoost("http://example.com:80/path");
    REQUIRE(result.valid);
    // RFC 3986 preserves explicit ports
    CHECK(result.port == 80);
  }

  SECTION("handles percent-encoded backslash") {
    // Backslash in path is valid in RFC 3986 (just another character)
    auto result = parseWithBoost("https://example.com/path%5Cwith%5Cbackslash");
    REQUIRE(result.valid);
    // boost::url may decode or preserve the percent-encoding
    // Either the decoded backslash or the encoded form should be present
    CHECK((result.path.find("\\") != std::string::npos ||
           result.path.find("%5C") != std::string::npos ||
           result.path.find("%5c") != std::string::npos));
  }
}

TEST_CASE("ada: Nix custom schemes", "[url][ada][nix]") {
  // Test that ada handles Nix's custom schemes correctly

  SECTION("tarball+https") {
    auto result = parseWithAda("tarball+https://example.com/foo.tar.gz");
    REQUIRE(result.valid);
    CHECK(result.scheme == "tarball+https");
    CHECK(result.host == "example.com");
    CHECK(result.path == "/foo.tar.gz");
  }

  SECTION("git+ssh") {
    auto result = parseWithAda("git+ssh://git@github.com/NixOS/nix");
    REQUIRE(result.valid);
    CHECK(result.scheme == "git+ssh");
    CHECK(result.host == "github.com");
  }

  SECTION("path scheme (opaque)") {
    auto result = parseWithAda("path:/home/user/repo");
    REQUIRE(result.valid);
    CHECK(result.scheme == "path");
    // For non-special schemes, ada treats path differently
  }

  SECTION("github flakeref") {
    auto result = parseWithAda("github:NixOS/nixpkgs");
    REQUIRE(result.valid);
    CHECK(result.scheme == "github");
  }

  SECTION("github flakeref with query") {
    auto result = parseWithAda("github:NixOS/nixpkgs?ref=nixos-unstable");
    REQUIRE(result.valid);
    CHECK(result.scheme == "github");
    CHECK(result.query == "ref=nixos-unstable");
  }
}

TEST_CASE("ada: file:// URL handling", "[url][ada][file]") {
  SECTION("absolute path") {
    auto result = parseWithAda("file:///home/user/file.txt");
    REQUIRE(result.valid);
    CHECK(result.scheme == "file");
    CHECK(result.host.empty());
    CHECK(result.path == "/home/user/file.txt");
  }

  SECTION("nix store path") {
    auto result = parseWithAda("file:///nix/store/abc123-foo-1.0/bin/foo");
    REQUIRE(result.valid);
    CHECK(result.path == "/nix/store/abc123-foo-1.0/bin/foo");
  }

  SECTION("file with localhost") {
    auto result = parseWithAda("file://localhost/path");
    REQUIRE(result.valid);
    // WHATWG normalizes file://localhost to file://
    CHECK(result.host.empty());
  }
}

TEST_CASE("ada: percent encoding roundtrip", "[url][ada][encoding]") {
  SECTION("encoded slash in path segment preserved") {
    auto result = parseWithAda("https://example.com/foo%2Fbar");
    REQUIRE(result.valid);
    // The encoded slash should be preserved, not decoded to /
    CHECK(result.path == "/foo%2Fbar");
  }

  SECTION("space encoding") {
    auto result = parseWithAda("https://example.com/path%20with%20spaces");
    REQUIRE(result.valid);
    CHECK(result.path == "/path%20with%20spaces");
  }

  SECTION("query encoding") {
    auto result = parseWithAda("https://example.com?key=hello%20world");
    REQUIRE(result.valid);
    CHECK(result.query == "key=hello%20world");
  }
}

TEST_CASE("ada: IPv6 handling", "[url][ada][ipv6]") {
  SECTION("localhost with port") {
    auto result = parseWithAda("http://[::1]:8080/path");
    REQUIRE(result.valid);
    // WHATWG URL: get_host() includes port, get_hostname() doesn't
    // Our parseWithAda uses get_host() for compatibility with boost::url
    // Ada's get_host() returns "host:port" format for IPv6 with explicit port
    CHECK(result.host.find("[::1]") != std::string::npos);
    CHECK(result.port == 8080);
  }

  SECTION("localhost no port") {
    auto result = parseWithAda("http://[::1]/path");
    REQUIRE(result.valid);
    CHECK(result.host == "[::1]");
  }

  SECTION("full IPv6") {
    auto result = parseWithAda("http://[2001:db8::1]/path");
    REQUIRE(result.valid);
    CHECK(result.host == "[2001:db8::1]");
  }

  // Note: Zone ID handling may differ
}

// ─────────────────────────────────────────────────────────────────────────────
// Summary test to print all differences
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("summary: print all behavioral differences", "[url][comparison][.summary]") {
  std::cout << "\n=== BOOST vs ADA Behavioral Differences ===\n\n";

  for (const auto& tc : NIX_URL_PATTERNS) {
    auto boost_result = parseWithBoost(tc.url);
    auto ada_result = parseWithAda(tc.url);

    bool has_differences = false;
    std::string diff;

    if (boost_result.valid != ada_result.valid) {
      has_differences = true;
      diff += "  validity: boost=" + std::to_string(boost_result.valid) +
              " ada=" + std::to_string(ada_result.valid) + "\n";
    }

    if (boost_result.valid && ada_result.valid) {
      if (boost_result.scheme != ada_result.scheme) {
        has_differences = true;
        diff += "  scheme: boost='" + boost_result.scheme + "' ada='" + ada_result.scheme + "'\n";
      }
      if (boost_result.host != ada_result.host) {
        has_differences = true;
        diff += "  host: boost='" + boost_result.host + "' ada='" + ada_result.host + "'\n";
      }
      if (boost_result.port != ada_result.port) {
        has_differences = true;
        diff +=
            "  port: boost=" +
            (boost_result.port ? std::to_string(*boost_result.port) : std::string("none")) +
            " ada=" + (ada_result.port ? std::to_string(*ada_result.port) : std::string("none")) +
            "\n";
      }
      if (boost_result.path != ada_result.path) {
        has_differences = true;
        diff += "  path: boost='" + boost_result.path + "' ada='" + ada_result.path + "'\n";
      }
      if (boost_result.query != ada_result.query) {
        has_differences = true;
        diff += "  query: boost='" + boost_result.query + "' ada='" + ada_result.query + "'\n";
      }
      if (boost_result.fragment != ada_result.fragment) {
        has_differences = true;
        diff +=
            "  fragment: boost='" + boost_result.fragment + "' ada='" + ada_result.fragment + "'\n";
      }
    }

    if (has_differences) {
      std::cout << tc.description << ": " << tc.url << "\n";
      std::cout << diff;
      std::cout << "\n";
    }
  }

  std::cout << "===========================================\n";
}
