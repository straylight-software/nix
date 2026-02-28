// straylight // nix // util // tests
//
// Unit tests for URL parsing

// Catch2 must be included before rapidcheck/catch.h
#include <catch2/catch_test_macros.hpp>

// RapidCheck for property-based testing
#include <string>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include "nix/util/url.h"

using nix::BadURL;
using nix::decode_query;
using nix::encode_query;
using nix::fix_git_url;
using nix::is_valid_scheme_name;
using nix::parse_url;
using nix::parse_url_relative;
using nix::parse_url_scheme;
using nix::parsed_url_t;
using nix::percent_decode;
using nix::percent_encode;
using nix::render_url_path_ensure_legal;
using nix::string_map_t;
using nix::verbatim_url_t;

// ─────────────────────────────────────────────────────────────────────────────
// parseURL basic tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parse_url simple https url", "[url][parse]") {
  auto url = parse_url("https://example.com/path/to/resource");

  REQUIRE(url.scheme() == "https");
  REQUIRE(url.authority().has_value());
  REQUIRE(url.authority()->host() == "example.com");
  REQUIRE(url.authority()->port() == std::nullopt);
  REQUIRE(url.authority()->user() == std::nullopt);
  REQUIRE(url.path() == std::vector<std::string>{"", "path", "to", "resource"});
  REQUIRE(url.query().empty());
  REQUIRE(url.fragment().empty());
}

TEST_CASE("parse_url with port number", "[url][parse]") {
  auto url = parse_url("http://localhost:8080/api");

  REQUIRE(url.scheme() == "http");
  REQUIRE(url.authority().has_value());
  REQUIRE(url.authority()->host() == "localhost");
  REQUIRE(url.authority()->port() == 8080);
  REQUIRE(url.path() == std::vector<std::string>{"", "api"});
}

TEST_CASE("parse_url with query parameters", "[url][parse]") {
  auto url = parse_url("https://example.com/search?key=value&foo=bar");

  REQUIRE(url.scheme() == "https");
  REQUIRE(url.authority()->host() == "example.com");
  REQUIRE(url.path() == std::vector<std::string>{"", "search"});
  REQUIRE(url.query().size() == 2);
  REQUIRE(url.query().at("key") == "value");
  REQUIRE(url.query().at("foo") == "bar");
}

TEST_CASE("parse_url with fragment", "[url][parse]") {
  auto url = parse_url("https://example.com/page#section1");

  REQUIRE(url.scheme() == "https");
  REQUIRE(url.authority()->host() == "example.com");
  REQUIRE(url.path() == std::vector<std::string>{"", "page"});
  REQUIRE(url.fragment() == "section1");
}

TEST_CASE("parse_url with userinfo", "[url][parse]") {
  auto url = parse_url("https://user@example.com/path");

  REQUIRE(url.scheme() == "https");
  REQUIRE(url.authority().has_value());
  REQUIRE(url.authority()->user() == "user");
  REQUIRE(url.authority()->host() == "example.com");
}

TEST_CASE("parse_url with userinfo and password", "[url][parse]") {
  auto url = parse_url("https://user:pass@example.com/path");

  REQUIRE(url.scheme() == "https");
  REQUIRE(url.authority().has_value());
  REQUIRE(url.authority()->user() == "user");
  REQUIRE(url.authority()->password() == "pass");
  REQUIRE(url.authority()->host() == "example.com");
}

TEST_CASE("parse_url file scheme with empty authority", "[url][parse]") {
  auto url = parse_url("file:///home/user/file.txt");

  REQUIRE(url.scheme() == "file");
  REQUIRE(url.authority().has_value());
  REQUIRE(url.authority()->host().empty());
  REQUIRE(url.path() == std::vector<std::string>{"", "home", "user", "file.txt"});
}

TEST_CASE("parse_url scheme without authority", "[url][parse]") {
  auto url = parse_url("tel:+1-555-123-4567");

  REQUIRE(url.scheme() == "tel");
  REQUIRE_FALSE(url.authority().has_value());
  REQUIRE(url.path() == std::vector<std::string>{"+1-555-123-4567"});
}

TEST_CASE("parse_url trailing slash semantics", "[url][parse]") {
  SECTION("without trailing slash") {
    auto url = parse_url("https://example.com/bar");
    REQUIRE(url.path() == std::vector<std::string>{"", "bar"});
  }

  SECTION("with trailing slash") {
    auto url = parse_url("https://example.com/bar/");
    REQUIRE(url.path() == std::vector<std::string>{"", "bar", ""});
  }

  SECTION("multiple trailing slashes") {
    auto url = parse_url("https://example.com//bar///");
    REQUIRE(url.path() == std::vector<std::string>{"", "", "bar", "", "", ""});
  }

  SECTION("root path only") {
    auto url = parse_url("https://example.com/");
    REQUIRE(url.path() == std::vector<std::string>{"", ""});
  }

  SECTION("no path") {
    auto url = parse_url("https://example.com");
    REQUIRE(url.path() == std::vector<std::string>{""});
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// IPv6 address tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parse_url ipv6 address", "[url][parse][ipv6]") {
  auto url = parse_url("http://[::1]:8080/path");

  REQUIRE(url.scheme() == "http");
  REQUIRE(url.authority().has_value());
  REQUIRE(url.authority()->host() == "::1");
  REQUIRE(url.authority()->host_type() == parsed_url_t::authority_t::host_type_t::ipv6);
  REQUIRE(url.authority()->port() == 8080);
}

TEST_CASE("parse_url ipv6 full address", "[url][parse][ipv6]") {
  auto url = parse_url("http://[2001:db8:85a3::8a2e:370:7334]/");

  REQUIRE(url.authority().has_value());
  REQUIRE(url.authority()->host() == "2001:db8:85a3::8a2e:370:7334");
  REQUIRE(url.authority()->host_type() == parsed_url_t::authority_t::host_type_t::ipv6);
}

// ─────────────────────────────────────────────────────────────────────────────
// Percent encoding tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("percent_decode basic", "[url][encoding]") {
  REQUIRE(percent_decode("hello%20world") == "hello world");
  REQUIRE(percent_decode("foo%2Fbar") == "foo/bar");
  REQUIRE(percent_decode("%41%42%43") == "ABC");
  // Note: invalid percent encoding (like "no%encoding" where %en is not valid hex)
  // throws an exception, so we don't test that case here
}

TEST_CASE("percent_decode empty string", "[url][encoding]") {
  REQUIRE(percent_decode("").empty());
}

TEST_CASE("percent_encode basic", "[url][encoding]") {
  REQUIRE(percent_encode("hello world") == "hello%20world");
  REQUIRE(percent_encode("foo/bar") == "foo%2Fbar");
  // Unreserved characters should not be encoded
  REQUIRE(percent_encode("abc123") == "abc123");
  REQUIRE(percent_encode("a-b_c.d~e") == "a-b_c.d~e");
}

TEST_CASE("percent_encode with keep characters", "[url][encoding]") {
  REQUIRE(percent_encode("foo/bar", "/") == "foo/bar");
  REQUIRE(percent_encode("a:b@c", ":@") == "a:b@c");
}

TEST_CASE("parse_url percent encoded path segments", "[url][parse][encoding]") {
  // Path with encoded slash should preserve the slash in the segment
  auto url = parse_url("https://example.com/foo/bar%2Fbaz/quux");

  REQUIRE(url.path().size() == 4);
  REQUIRE(url.path()[0] == "");
  REQUIRE(url.path()[1] == "foo");
  REQUIRE(url.path()[2] == "bar/baz"); // decoded %2F becomes /
  REQUIRE(url.path()[3] == "quux");
}

TEST_CASE("parse_url percent encoded query", "[url][parse][encoding]") {
  auto url = parse_url("https://example.com?key=hello%20world");

  REQUIRE(url.query().at("key") == "hello world");
}

// ─────────────────────────────────────────────────────────────────────────────
// URL serialization tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parsed_url to_string simple", "[url][serialize]") {
  auto url = parse_url("https://example.com/path");
  REQUIRE(url.to_string() == "https://example.com/path");
}

TEST_CASE("parsed_url to_string with all components", "[url][serialize]") {
  auto url = parse_url("https://user:pass@example.com:8080/path?key=value#frag");

  auto serialized = url.to_string();
  REQUIRE(serialized.find("https://") == 0);
  REQUIRE(serialized.contains("user:pass@"));
  REQUIRE(serialized.contains("example.com:8080"));
  REQUIRE(serialized.contains("/path"));
  REQUIRE(serialized.contains("?key=value"));
  REQUIRE(serialized.contains("#frag"));
}

TEST_CASE("parsed_url render_path", "[url][serialize]") {
  auto url = parse_url("https://example.com/foo/bar/baz");

  REQUIRE(url.render_path(false) == "/foo/bar/baz");
  REQUIRE(url.render_path(true) == "/foo/bar/baz");
}

TEST_CASE("parsed_url render_path with special characters", "[url][serialize]") {
  parsed_url_t url;
  url.set_scheme("https");
  parsed_url_t::authority_t auth;
  auth.set_host_type(parsed_url_t::authority_t::host_type_t::name);
  auth.set_host("example.com");
  auth.set_user(std::nullopt);
  auth.set_password(std::nullopt);
  auth.set_port(std::nullopt);
  url.set_authority(auth);
  url.set_path({"", "foo", "bar baz", "quux"});

  REQUIRE(url.render_path(false) == "/foo/bar baz/quux");
  REQUIRE(url.render_path(true) == "/foo/bar%20baz/quux");
}

TEST_CASE("parsed_url render_path preserves RFC 3986 sub-delims", "[url][serialize][s3]") {
  // This test verifies that RFC 3986 sub-delims are NOT percent-encoded in paths.
  // This is critical for AWS S3 signing - see https://github.com/NixOS/nix/issues/15315
  // Paths like "realisations/sha256:...!dist.drv" must keep "!" unencoded to avoid
  // SignatureDoesNotMatch errors when using curl's CURLOPT_AWS_SIGV4.
  parsed_url_t url;
  url.set_scheme("https");
  parsed_url_t::authority_t auth;
  auth.set_host_type(parsed_url_t::authority_t::host_type_t::name);
  auth.set_host("s3.example.com");
  auth.set_user(std::nullopt);
  auth.set_password(std::nullopt);
  auth.set_port(std::nullopt);
  url.set_authority(auth);

  // Test realisation path with "!" character
  url.set_path({"", "bucket", "realisations", "sha256:abcdef!dist.drv"});

  // "!" is a sub-delim per RFC 3986 and should NOT be encoded
  REQUIRE(url.render_path(false) == "/bucket/realisations/sha256:abcdef!dist.drv");
  REQUIRE(url.render_path(true) == "/bucket/realisations/sha256:abcdef!dist.drv");

  // Full URL should also preserve the "!"
  auto full_url = url.to_string();
  REQUIRE(full_url.find("!dist.drv") != std::string::npos);
  REQUIRE(full_url.find("%21") == std::string::npos); // Should NOT contain encoded "!"
}

TEST_CASE("parsed_url render_path all sub-delims unencoded", "[url][serialize]") {
  // All RFC 3986 sub-delims: ! $ & ' ( ) * + , ; =
  // These should all be preserved unencoded in paths
  parsed_url_t url;
  url.set_scheme("https");
  parsed_url_t::authority_t auth;
  auth.set_host_type(parsed_url_t::authority_t::host_type_t::name);
  auth.set_host("example.com");
  auth.set_user(std::nullopt);
  auth.set_password(std::nullopt);
  auth.set_port(std::nullopt);
  url.set_authority(auth);

  url.set_path({"", "test", "!$&'()*+,;="});

  auto encoded = url.render_path(true);
  // All sub-delims should be preserved
  REQUIRE(encoded == "/test/!$&'()*+,;=");
}

// ─────────────────────────────────────────────────────────────────────────────
// URL scheme validation tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("is_valid_scheme_name", "[url][scheme]") {
  REQUIRE(is_valid_scheme_name("http"));
  REQUIRE(is_valid_scheme_name("https"));
  REQUIRE(is_valid_scheme_name("ftp"));
  REQUIRE(is_valid_scheme_name("file"));
  REQUIRE(is_valid_scheme_name("git+https"));
  REQUIRE(is_valid_scheme_name("custom-scheme"));
  REQUIRE(is_valid_scheme_name("scheme123"));

  // Must start with letter
  REQUIRE_FALSE(is_valid_scheme_name("123scheme"));
  REQUIRE_FALSE(is_valid_scheme_name(""));
  REQUIRE_FALSE(is_valid_scheme_name(":invalid"));
}

TEST_CASE("parse_url_scheme basic", "[url][scheme]") {
  SECTION("simple scheme") {
    auto parsed = parse_url_scheme("http");
    REQUIRE_FALSE(parsed.application().has_value());
    REQUIRE(parsed.transport() == "http");
  }

  SECTION("compound scheme") {
    auto parsed = parse_url_scheme("git+https");
    REQUIRE(parsed.application().has_value());
    REQUIRE(*parsed.application() == "git");
    REQUIRE(parsed.transport() == "https");
  }

  SECTION("tarball scheme") {
    auto parsed = parse_url_scheme("tarball+file");
    REQUIRE(parsed.application().has_value());
    REQUIRE(*parsed.application() == "tarball");
    REQUIRE(parsed.transport() == "file");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Query encoding/decoding tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("decode_query basic", "[url][query]") {
  auto query = decode_query("foo=bar&baz=quux");

  REQUIRE(query.size() == 2);
  REQUIRE(query.at("foo") == "bar");
  REQUIRE(query.at("baz") == "quux");
}

TEST_CASE("decode_query with encoded values", "[url][query]") {
  auto query = decode_query("key=hello%20world");

  REQUIRE(query.at("key") == "hello world");
}

TEST_CASE("decode_query empty", "[url][query]") {
  auto query = decode_query("");
  REQUIRE(query.empty());
}

TEST_CASE("encode_query basic", "[url][query]") {
  string_map_t query;
  query["foo"] = "bar";
  query["baz"] = "quux";

  auto encoded = encode_query(query);
  // Order may vary, so check both possible orderings
  REQUIRE((encoded == "baz=quux&foo=bar" || encoded == "foo=bar&baz=quux"));
}

TEST_CASE("encode_query with special characters", "[url][query]") {
  string_map_t query;
  query["key"] = "hello world";

  auto encoded = encode_query(query);
  REQUIRE(encoded == "key=hello%20world");
}

// ─────────────────────────────────────────────────────────────────────────────
// Relative URL resolution tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parse_url_relative simple path", "[url][relative]") {
  auto base = parse_url("https://example.com/foo/bar");
  auto resolved = parse_url_relative("baz", base);

  REQUIRE(resolved.scheme() == "https");
  REQUIRE(resolved.authority()->host() == "example.com");
  REQUIRE(resolved.path() == std::vector<std::string>{"", "foo", "baz"});
}

TEST_CASE("parse_url_relative absolute path", "[url][relative]") {
  auto base = parse_url("https://example.com/foo/bar");
  auto resolved = parse_url_relative("/absolute/path", base);

  REQUIRE(resolved.scheme() == "https");
  REQUIRE(resolved.authority()->host() == "example.com");
  REQUIRE(resolved.path() == std::vector<std::string>{"", "absolute", "path"});
}

TEST_CASE("parse_url_relative parent directory", "[url][relative]") {
  auto base = parse_url("https://example.com/foo/bar/baz");
  auto resolved = parse_url_relative("../quux", base);

  REQUIRE(resolved.path() == std::vector<std::string>{"", "foo", "quux"});
}

TEST_CASE("parse_url_relative with trailing slash in base", "[url][relative]") {
  auto base = parse_url("https://example.com/foo/bar/");
  auto resolved = parse_url_relative("baz", base);

  // With trailing slash, relative path is appended to directory
  REQUIRE(resolved.path() == std::vector<std::string>{"", "foo", "bar", "baz"});
}

// ─────────────────────────────────────────────────────────────────────────────
// URL canonicalisation tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parsed_url canonicalise dot segments", "[url][canonicalise]") {
  auto url = parse_url("https://example.com/foo/./bar/../baz");
  auto canonical = url.canonicalise();

  REQUIRE(canonical.path() == std::vector<std::string>{"", "foo", "baz"});
}

// ─────────────────────────────────────────────────────────────────────────────
// fixGitURL tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("fix_git_url scp style", "[url][git]") {
  auto url = fix_git_url("git@github.com:NixOS/nix");

  REQUIRE(url.scheme() == "ssh");
  REQUIRE(url.authority().has_value());
  REQUIRE(url.authority()->user() == "git");
  REQUIRE(url.authority()->host() == "github.com");
}

TEST_CASE("fix_git_url strips git+ prefix", "[url][git]") {
  auto url = fix_git_url("git+https://github.com/NixOS/nix");

  REQUIRE(url.scheme() == "https");
  REQUIRE(url.authority()->host() == "github.com");
}

TEST_CASE("fix_git_url local path", "[url][git]") {
  auto url = fix_git_url("/home/user/repo");

  REQUIRE(url.scheme() == "file");
}

TEST_CASE("fix_git_url scp style without user", "[url][git]") {
  // This is the format used in .gitmodules for SSH submodules without a username
  // e.g., "moserv.lan.home.arpa:/storage/src/nix-secrets.git"
  // Note: The path starts with // because the original has an absolute path /path/...
  // which becomes ssh://host//path/... when converted
  auto url = fix_git_url("server.example.com:/path/to/repo.git");

  REQUIRE(url.scheme() == "ssh");
  REQUIRE(url.authority().has_value());
  REQUIRE_FALSE(url.authority()->user().has_value());
  REQUIRE(url.authority()->host() == "server.example.com");
  // Path has leading double-slash since original was an absolute path
  REQUIRE(url.render_path() == "//path/to/repo.git");
}

TEST_CASE("fix_git_url scp style without user relative path", "[url][git]") {
  // SCP-style URL without leading slash (relative path on remote)
  auto url = fix_git_url("github.com:org/repo");

  REQUIRE(url.scheme() == "ssh");
  REQUIRE(url.authority().has_value());
  REQUIRE_FALSE(url.authority()->user().has_value());
  REQUIRE(url.authority()->host() == "github.com");
  REQUIRE(url.render_path() == "/org/repo");
}

TEST_CASE("fix_git_url relative path produces valid URL", "[url][git]") {
  // Regression test for NixOS/nix#14867
  // Relative paths should not have an authority, since that would violate
  // RFC 3986: when authority is present, path must be empty or start with '/'
  auto url = fix_git_url("relative/path");

  REQUIRE(url.scheme() == "file");
  REQUIRE_FALSE(url.authority().has_value());
  REQUIRE(url.render_path() == "relative/path");
  // Most importantly: to_string() should not crash
  REQUIRE_NOTHROW(url.to_string());
}

TEST_CASE("fix_git_url absolute path has authority", "[url][git]") {
  // Absolute paths should have an authority (empty) to produce file:///path
  auto url = fix_git_url("/absolute/path");

  REQUIRE(url.scheme() == "file");
  REQUIRE(url.authority().has_value());
  REQUIRE(url.render_path() == "/absolute/path");
  REQUIRE_NOTHROW(url.to_string());
}

// ─────────────────────────────────────────────────────────────────────────────
// VerbatimURL tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("verbatim_url from string", "[url][verbatim]") {
  verbatim_url_t url(std::string{"https://example.com/path"});

  REQUIRE(url.to_string() == "https://example.com/path");
  REQUIRE(url.scheme() == "https");
}

TEST_CASE("verbatim_url from parsed_url", "[url][verbatim]") {
  auto parsed = parse_url("https://example.com/path");
  verbatim_url_t url(parsed);

  REQUIRE(url.scheme() == "https");
  auto reparsed = url.parsed();
  REQUIRE(reparsed.authority()->host() == "example.com");
}

TEST_CASE("verbatim_url last_path_segment", "[url][verbatim]") {
  verbatim_url_t url(std::string{"https://example.com/path/to/file.txt"});

  auto segment = url.last_path_segment();
  REQUIRE(segment.has_value());
  REQUIRE(*segment == "file.txt");
}

TEST_CASE("verbatim_url last_path_segment with query", "[url][verbatim]") {
  verbatim_url_t url(std::string{"https://example.com/path/to/file.txt?query=value"});

  auto segment = url.last_path_segment();
  REQUIRE(segment.has_value());
  REQUIRE(*segment == "file.txt");
}

TEST_CASE("verbatim_url last_path_segment empty path", "[url][verbatim]") {
  verbatim_url_t url(std::string{"https://example.com/"});

  auto segment = url.last_path_segment();
  REQUIRE_FALSE(segment.has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Error handling tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parse_url invalid url throws", "[url][error]") {
  REQUIRE_THROWS_AS(parse_url("not a valid url"), BadURL);
  REQUIRE_THROWS_AS(parse_url("://missing-scheme"), BadURL);
  REQUIRE_THROWS_AS(parse_url(""), BadURL);
}

TEST_CASE("parse_url file with authority throws", "[url][error]") {
  // file:// URLs with non-empty host are invalid
  REQUIRE_THROWS_AS(parse_url("file://remotehost/path"), BadURL);
}

TEST_CASE("percent_decode invalid encoding throws", "[url][error][encoding]") {
  REQUIRE_THROWS_AS(percent_decode("%GG"), BadURL);
  REQUIRE_THROWS_AS(percent_decode("%"), BadURL);
  REQUIRE_THROWS_AS(percent_decode("%1"), BadURL);
}

TEST_CASE("render_url_path_ensure_legal with slash throws", "[url][error]") {
  std::vector<std::string> path = {"foo", "bar/baz", "quux"};
  REQUIRE_THROWS_AS(render_url_path_ensure_legal(path), BadURL);
}

TEST_CASE("render_url_path_ensure_legal with nul throws", "[url][error]") {
  std::vector<std::string> path = {"foo", std::string("bar\0baz", 7), "quux"};
  REQUIRE_THROWS_AS(render_url_path_ensure_legal(path), BadURL);
}

TEST_CASE("to_string throws with authority and non-absolute path", "[url][error]") {
  // RFC 3986: If authority is present, path must be empty or start with '/'
  // NixOS/nix#14867: hand-constructed URLs might violate this
  parsed_url_t url;
  url.set_scheme("file");
  url.set_authority(parsed_url_t::authority_t{});
  url.set_path({"relative", "path"}); // Doesn't start with empty string (/)
  REQUIRE_THROWS_AS(url.to_string(), nix::Error);
}

TEST_CASE("to_string throws with double-slash path and no authority", "[url][error]") {
  // RFC 3986: If no authority, path cannot start with '//'
  // Path ["", "", "foo"] would render as "//foo" which is ambiguous
  parsed_url_t url;
  url.set_scheme("scheme");
  url.set_authority(std::nullopt);
  url.set_path({"", "", "foo"}); // Starts with // (two empty strings)
  REQUIRE_THROWS_AS(url.to_string(), nix::Error);
}

// ─────────────────────────────────────────────────────────────────────────────
// Authority parsing tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("authority parse simple host", "[url][authority]") {
  auto auth = parsed_url_t::authority_t::parse("example.com");

  REQUIRE(auth.host() == "example.com");
  REQUIRE(auth.host_type() == parsed_url_t::authority_t::host_type_t::name);
  REQUIRE_FALSE(auth.port().has_value());
  REQUIRE_FALSE(auth.user().has_value());
}

TEST_CASE("authority parse with port", "[url][authority]") {
  auto auth = parsed_url_t::authority_t::parse("example.com:8080");

  REQUIRE(auth.host() == "example.com");
  REQUIRE(auth.port() == 8080);
}

TEST_CASE("authority parse with userinfo", "[url][authority]") {
  auto auth = parsed_url_t::authority_t::parse("user:pass@example.com");

  REQUIRE(auth.user() == "user");
  REQUIRE(auth.password() == "pass");
  REQUIRE(auth.host() == "example.com");
}

TEST_CASE("authority parse ipv4", "[url][authority]") {
  auto auth = parsed_url_t::authority_t::parse("192.168.1.1:80");

  REQUIRE(auth.host() == "192.168.1.1");
  REQUIRE(auth.host_type() == parsed_url_t::authority_t::host_type_t::ipv4);
  REQUIRE(auth.port() == 80);
}

TEST_CASE("authority parse ipv6", "[url][authority]") {
  auto auth = parsed_url_t::authority_t::parse("[::1]:8080");

  REQUIRE(auth.host() == "::1");
  REQUIRE(auth.host_type() == parsed_url_t::authority_t::host_type_t::ipv6);
  REQUIRE(auth.port() == 8080);
}

TEST_CASE("authority to_string roundtrip", "[url][authority]") {
  auto auth = parsed_url_t::authority_t::parse("user@example.com:8080");
  auto serialized = auth.to_string();

  REQUIRE(serialized == "user@example.com:8080");
}

// ─────────────────────────────────────────────────────────────────────────────
// Lenient parsing tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parse_url lenient mode with spaces in fragment", "[url][lenient]") {
  auto url = parse_url("https://example.com#hello world", true);

  REQUIRE(url.fragment() == "hello world");
}

TEST_CASE("parse_url lenient mode with spaces in query", "[url][lenient]") {
  auto url = parse_url("https://example.com?key=hello world", true);

  REQUIRE(url.query().at("key") == "hello world");
}

// ─────────────────────────────────────────────────────────────────────────────
// Path segments tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parsed_url path_segments skip empty", "[url][path]") {
  auto url = parse_url("https://example.com//foo///bar//");

  auto segments_all = url.path_segments(false);
  auto all_vec = std::vector<std::string>(segments_all.begin(), segments_all.end());
  REQUIRE(all_vec == std::vector<std::string>{"", "", "foo", "", "", "bar", "", ""});

  auto segments_non_empty = url.path_segments(true);
  auto non_empty_vec =
      std::vector<std::string>(segments_non_empty.begin(), segments_non_empty.end());
  REQUIRE(non_empty_vec == std::vector<std::string>{"foo", "bar"});
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-based tests with RapidCheck
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("percent encoding property tests", "[url][property][encoding]") {
  rc::prop("percent encode/decode roundtrip", []() {
    // Generate ASCII strings avoiding control characters
    auto input = *rc::gen::container<std::string>(rc::gen::inRange<char>(32, 127));

    auto encoded = percent_encode(input);
    auto decoded = percent_decode(encoded);

    RC_ASSERT(decoded == input);
  });

  rc::prop("percent encoding produces valid url characters", []() {
    auto input = *rc::gen::container<std::string>(rc::gen::inRange<char>(0, 127));

    auto encoded = percent_encode(input);

    // Encoded string should only contain unreserved chars or percent-encoded sequences
    for (size_t i = 0; i < encoded.size(); ++i) {
      char c = encoded[i];
      if (c == '%') {
        // Must be followed by two hex digits
        RC_ASSERT(i + 2 < encoded.size());
        RC_ASSERT(std::isxdigit(static_cast<unsigned char>(encoded[i + 1])));
        RC_ASSERT(std::isxdigit(static_cast<unsigned char>(encoded[i + 2])));
        i += 2;
      } else {
        // Must be unreserved character
        RC_ASSERT(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' ||
                  c == '~');
      }
    }
  });

  rc::prop("encoded length is at least original length", []() {
    auto input = *rc::gen::container<std::string>(rc::gen::inRange<char>(0, 127));

    auto encoded = percent_encode(input);

    RC_ASSERT(encoded.size() >= input.size());
  });
}

TEST_CASE("url parse/serialize roundtrip property", "[url][property][roundtrip]") {
  rc::prop("parse then serialize produces parseable url", []() {
    // Generate valid URL components
    auto scheme = *rc::gen::element<std::string>("http", "https", "ftp");
    // Generate simple alphanumeric hostname
    auto host_data = *rc::gen::arbitrary<std::vector<uint8_t>>();

    // Build host from alphanumeric chars only
    std::string host;
    for (auto b : host_data) {
      char c = static_cast<char>('a' + (b % 26));
      host += c;
      if (host.size() >= 10) {
        break;
      }
    }
    if (host.empty()) {
      host = "example";
    }

    // Build a simple URL
    std::string url_string = scheme + "://" + host + "/";

    auto parsed = parse_url(url_string);
    auto serialized = parsed.to_string();

    // Should be able to parse the serialized result
    auto reparsed = parse_url(serialized);

    RC_ASSERT(reparsed.scheme() == parsed.scheme());
    RC_ASSERT(reparsed.authority()->host() == parsed.authority()->host());
  });
}

TEST_CASE("query encode/decode roundtrip property", "[url][property][query]") {
  rc::prop("query encode/decode roundtrip", []() {
    // Generate simple alphanumeric key-value pairs
    auto key_data = *rc::gen::arbitrary<std::vector<uint8_t>>();
    auto value_data = *rc::gen::arbitrary<std::vector<uint8_t>>();

    // Build key from alphanumeric chars only (need at least 1 char)
    std::string key;
    for (auto b : key_data) {
      char c = static_cast<char>('a' + (b % 26));
      key += c;
      if (key.size() >= 10) {
        break;
      }
    }
    if (key.empty()) {
      key = "key";
    }

    // Build value from alphanumeric chars
    std::string value;
    for (auto b : value_data) {
      char c = static_cast<char>('a' + (b % 26));
      value += c;
      if (value.size() >= 20) {
        break;
      }
    }

    string_map_t original;
    original[key] = value;

    auto encoded = encode_query(original);
    auto decoded = decode_query(encoded);

    RC_ASSERT(decoded.at(key) == value);
  });
}

TEST_CASE("scheme validation property", "[url][property][scheme]") {
  rc::prop("valid scheme names start with letter", []() {
    auto first_char = *rc::gen::inRange<char>('a', 'z' + 1);
    auto rest_data = *rc::gen::arbitrary<std::vector<uint8_t>>();

    // Build rest of scheme from valid chars
    std::string rest;
    // Valid scheme chars after first: a-z, 0-9, +, -, .
    constexpr std::string_view valid_chars = "abcdefghijklmnopqrstuvwxyz0123456789+-.";
    for (auto b : rest_data) {
      char c = valid_chars[b % valid_chars.size()];
      rest += c;
      if (rest.size() >= 10) {
        break;
      }
    }

    std::string scheme = std::string(1, first_char) + rest;

    RC_ASSERT(is_valid_scheme_name(scheme));
  });

  rc::prop("invalid scheme names start with non-letter", []() {
    auto first_char = *rc::gen::oneOf(rc::gen::inRange<char>('0', '9' + 1),
                                      rc::gen::element<char>('+', '-', '.', ':', '@', '!'));

    std::string scheme = std::string(1, first_char) + "abc";

    RC_ASSERT_FALSE(is_valid_scheme_name(scheme));
  });
}

TEST_CASE("url scheme parsing property", "[url][property][scheme]") {
  rc::prop("parsed scheme transport is suffix after +", []() {
    auto transport = *rc::gen::element<std::string>("http", "https", "file", "ssh");
    auto application = *rc::gen::element<std::string>("git", "tarball", "path");

    auto compound = application + "+" + transport;
    auto parsed = parse_url_scheme(compound);

    RC_ASSERT(parsed.application().has_value());
    RC_ASSERT(*parsed.application() == application);
    RC_ASSERT(parsed.transport() == transport);
  });
}
