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

using namespace nix;

// ─────────────────────────────────────────────────────────────────────────────
// parseURL basic tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parse_url simple https url", "[url][parse]") {
  auto url = parseURL("https://example.com/path/to/resource");

  REQUIRE(url.scheme == "https");
  REQUIRE(url.authority.has_value());
  REQUIRE(url.authority->host == "example.com");
  REQUIRE(url.authority->port == std::nullopt);
  REQUIRE(url.authority->user == std::nullopt);
  REQUIRE(url.path == std::vector<std::string>{"", "path", "to", "resource"});
  REQUIRE(url.query.empty());
  REQUIRE(url.fragment.empty());
}

TEST_CASE("parse_url with port number", "[url][parse]") {
  auto url = parseURL("http://localhost:8080/api");

  REQUIRE(url.scheme == "http");
  REQUIRE(url.authority.has_value());
  REQUIRE(url.authority->host == "localhost");
  REQUIRE(url.authority->port == 8080);
  REQUIRE(url.path == std::vector<std::string>{"", "api"});
}

TEST_CASE("parse_url with query parameters", "[url][parse]") {
  auto url = parseURL("https://example.com/search?key=value&foo=bar");

  REQUIRE(url.scheme == "https");
  REQUIRE(url.authority->host == "example.com");
  REQUIRE(url.path == std::vector<std::string>{"", "search"});
  REQUIRE(url.query.size() == 2);
  REQUIRE(url.query.at("key") == "value");
  REQUIRE(url.query.at("foo") == "bar");
}

TEST_CASE("parse_url with fragment", "[url][parse]") {
  auto url = parseURL("https://example.com/page#section1");

  REQUIRE(url.scheme == "https");
  REQUIRE(url.authority->host == "example.com");
  REQUIRE(url.path == std::vector<std::string>{"", "page"});
  REQUIRE(url.fragment == "section1");
}

TEST_CASE("parse_url with userinfo", "[url][parse]") {
  auto url = parseURL("https://user@example.com/path");

  REQUIRE(url.scheme == "https");
  REQUIRE(url.authority.has_value());
  REQUIRE(url.authority->user == "user");
  REQUIRE(url.authority->host == "example.com");
}

TEST_CASE("parse_url with userinfo and password", "[url][parse]") {
  auto url = parseURL("https://user:pass@example.com/path");

  REQUIRE(url.scheme == "https");
  REQUIRE(url.authority.has_value());
  REQUIRE(url.authority->user == "user");
  REQUIRE(url.authority->password == "pass");
  REQUIRE(url.authority->host == "example.com");
}

TEST_CASE("parse_url file scheme with empty authority", "[url][parse]") {
  auto url = parseURL("file:///home/user/file.txt");

  REQUIRE(url.scheme == "file");
  REQUIRE(url.authority.has_value());
  REQUIRE(url.authority->host.empty());
  REQUIRE(url.path == std::vector<std::string>{"", "home", "user", "file.txt"});
}

TEST_CASE("parse_url scheme without authority", "[url][parse]") {
  auto url = parseURL("tel:+1-555-123-4567");

  REQUIRE(url.scheme == "tel");
  REQUIRE_FALSE(url.authority.has_value());
  REQUIRE(url.path == std::vector<std::string>{"+1-555-123-4567"});
}

TEST_CASE("parse_url trailing slash semantics", "[url][parse]") {
  SECTION("without trailing slash") {
    auto url = parseURL("https://example.com/bar");
    REQUIRE(url.path == std::vector<std::string>{"", "bar"});
  }

  SECTION("with trailing slash") {
    auto url = parseURL("https://example.com/bar/");
    REQUIRE(url.path == std::vector<std::string>{"", "bar", ""});
  }

  SECTION("multiple trailing slashes") {
    auto url = parseURL("https://example.com//bar///");
    REQUIRE(url.path == std::vector<std::string>{"", "", "bar", "", "", ""});
  }

  SECTION("root path only") {
    auto url = parseURL("https://example.com/");
    REQUIRE(url.path == std::vector<std::string>{"", ""});
  }

  SECTION("no path") {
    auto url = parseURL("https://example.com");
    REQUIRE(url.path == std::vector<std::string>{""});
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// IPv6 address tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parse_url ipv6 address", "[url][parse][ipv6]") {
  auto url = parseURL("http://[::1]:8080/path");

  REQUIRE(url.scheme == "http");
  REQUIRE(url.authority.has_value());
  REQUIRE(url.authority->host == "::1");
  REQUIRE(url.authority->hostType == parsed_url_t::authority_t::host_type_t::IPv6);
  REQUIRE(url.authority->port == 8080);
}

TEST_CASE("parse_url ipv6 full address", "[url][parse][ipv6]") {
  auto url = parseURL("http://[2001:db8:85a3::8a2e:370:7334]/");

  REQUIRE(url.authority.has_value());
  REQUIRE(url.authority->host == "2001:db8:85a3::8a2e:370:7334");
  REQUIRE(url.authority->hostType == parsed_url_t::authority_t::host_type_t::IPv6);
}

// ─────────────────────────────────────────────────────────────────────────────
// Percent encoding tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("percent_decode basic", "[url][encoding]") {
  REQUIRE(percentDecode("hello%20world") == "hello world");
  REQUIRE(percentDecode("foo%2Fbar") == "foo/bar");
  REQUIRE(percentDecode("%41%42%43") == "ABC");
  // Note: invalid percent encoding (like "no%encoding" where %en is not valid hex)
  // throws an exception, so we don't test that case here
}

TEST_CASE("percent_decode empty string", "[url][encoding]") {
  REQUIRE(percentDecode("").empty());
}

TEST_CASE("percent_encode basic", "[url][encoding]") {
  REQUIRE(percentEncode("hello world") == "hello%20world");
  REQUIRE(percentEncode("foo/bar") == "foo%2Fbar");
  // Unreserved characters should not be encoded
  REQUIRE(percentEncode("abc123") == "abc123");
  REQUIRE(percentEncode("a-b_c.d~e") == "a-b_c.d~e");
}

TEST_CASE("percent_encode with keep characters", "[url][encoding]") {
  REQUIRE(percentEncode("foo/bar", "/") == "foo/bar");
  REQUIRE(percentEncode("a:b@c", ":@") == "a:b@c");
}

TEST_CASE("parse_url percent encoded path segments", "[url][parse][encoding]") {
  // Path with encoded slash should preserve the slash in the segment
  auto url = parseURL("https://example.com/foo/bar%2Fbaz/quux");

  REQUIRE(url.path.size() == 4);
  REQUIRE(url.path[0] == "");
  REQUIRE(url.path[1] == "foo");
  REQUIRE(url.path[2] == "bar/baz"); // decoded %2F becomes /
  REQUIRE(url.path[3] == "quux");
}

TEST_CASE("parse_url percent encoded query", "[url][parse][encoding]") {
  auto url = parseURL("https://example.com?key=hello%20world");

  REQUIRE(url.query.at("key") == "hello world");
}

// ─────────────────────────────────────────────────────────────────────────────
// URL serialization tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parsed_url to_string simple", "[url][serialize]") {
  auto url = parseURL("https://example.com/path");
  REQUIRE(url.to_string() == "https://example.com/path");
}

TEST_CASE("parsed_url to_string with all components", "[url][serialize]") {
  auto url = parseURL("https://user:pass@example.com:8080/path?key=value#frag");

  auto serialized = url.to_string();
  REQUIRE(serialized.find("https://") == 0);
  REQUIRE(serialized.contains("user:pass@"));
  REQUIRE(serialized.contains("example.com:8080"));
  REQUIRE(serialized.contains("/path"));
  REQUIRE(serialized.contains("?key=value"));
  REQUIRE(serialized.contains("#frag"));
}

TEST_CASE("parsed_url render_path", "[url][serialize]") {
  auto url = parseURL("https://example.com/foo/bar/baz");

  REQUIRE(url.renderPath(false) == "/foo/bar/baz");
  REQUIRE(url.renderPath(true) == "/foo/bar/baz");
}

TEST_CASE("parsed_url render_path with special characters", "[url][serialize]") {
  parsed_url_t url;
  url.scheme = "https";
  url.authority = parsed_url_t::authority_t{.hostType = parsed_url_t::authority_t::host_type_t::Name,
                                       .host = "example.com",
                                       .user = std::nullopt,
                                       .password = std::nullopt,
                                       .port = std::nullopt};
  url.path = {"", "foo", "bar baz", "quux"};

  REQUIRE(url.renderPath(false) == "/foo/bar baz/quux");
  REQUIRE(url.renderPath(true) == "/foo/bar%20baz/quux");
}

// ─────────────────────────────────────────────────────────────────────────────
// URL scheme validation tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("is_valid_scheme_name", "[url][scheme]") {
  REQUIRE(isValidSchemeName("http"));
  REQUIRE(isValidSchemeName("https"));
  REQUIRE(isValidSchemeName("ftp"));
  REQUIRE(isValidSchemeName("file"));
  REQUIRE(isValidSchemeName("git+https"));
  REQUIRE(isValidSchemeName("custom-scheme"));
  REQUIRE(isValidSchemeName("scheme123"));

  // Must start with letter
  REQUIRE_FALSE(isValidSchemeName("123scheme"));
  REQUIRE_FALSE(isValidSchemeName(""));
  REQUIRE_FALSE(isValidSchemeName(":invalid"));
}

TEST_CASE("parse_url_scheme basic", "[url][scheme]") {
  SECTION("simple scheme") {
    auto parsed = parseUrlScheme("http");
    REQUIRE_FALSE(parsed.application.has_value());
    REQUIRE(parsed.transport == "http");
  }

  SECTION("compound scheme") {
    auto parsed = parseUrlScheme("git+https");
    REQUIRE(parsed.application.has_value());
    REQUIRE(*parsed.application == "git");
    REQUIRE(parsed.transport == "https");
  }

  SECTION("tarball scheme") {
    auto parsed = parseUrlScheme("tarball+file");
    REQUIRE(parsed.application.has_value());
    REQUIRE(*parsed.application == "tarball");
    REQUIRE(parsed.transport == "file");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Query encoding/decoding tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("decode_query basic", "[url][query]") {
  auto query = decodeQuery("foo=bar&baz=quux");

  REQUIRE(query.size() == 2);
  REQUIRE(query.at("foo") == "bar");
  REQUIRE(query.at("baz") == "quux");
}

TEST_CASE("decode_query with encoded values", "[url][query]") {
  auto query = decodeQuery("key=hello%20world");

  REQUIRE(query.at("key") == "hello world");
}

TEST_CASE("decode_query empty", "[url][query]") {
  auto query = decodeQuery("");
  REQUIRE(query.empty());
}

TEST_CASE("encode_query basic", "[url][query]") {
  string_map_t query;
  query["foo"] = "bar";
  query["baz"] = "quux";

  auto encoded = encodeQuery(query);
  // Order may vary, so check both possible orderings
  REQUIRE((encoded == "baz=quux&foo=bar" || encoded == "foo=bar&baz=quux"));
}

TEST_CASE("encode_query with special characters", "[url][query]") {
  string_map_t query;
  query["key"] = "hello world";

  auto encoded = encodeQuery(query);
  REQUIRE(encoded == "key=hello%20world");
}

// ─────────────────────────────────────────────────────────────────────────────
// Relative URL resolution tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parse_url_relative simple path", "[url][relative]") {
  auto base = parseURL("https://example.com/foo/bar");
  auto resolved = parseURLRelative("baz", base);

  REQUIRE(resolved.scheme == "https");
  REQUIRE(resolved.authority->host == "example.com");
  REQUIRE(resolved.path == std::vector<std::string>{"", "foo", "baz"});
}

TEST_CASE("parse_url_relative absolute path", "[url][relative]") {
  auto base = parseURL("https://example.com/foo/bar");
  auto resolved = parseURLRelative("/absolute/path", base);

  REQUIRE(resolved.scheme == "https");
  REQUIRE(resolved.authority->host == "example.com");
  REQUIRE(resolved.path == std::vector<std::string>{"", "absolute", "path"});
}

TEST_CASE("parse_url_relative parent directory", "[url][relative]") {
  auto base = parseURL("https://example.com/foo/bar/baz");
  auto resolved = parseURLRelative("../quux", base);

  REQUIRE(resolved.path == std::vector<std::string>{"", "foo", "quux"});
}

TEST_CASE("parse_url_relative with trailing slash in base", "[url][relative]") {
  auto base = parseURL("https://example.com/foo/bar/");
  auto resolved = parseURLRelative("baz", base);

  // With trailing slash, relative path is appended to directory
  REQUIRE(resolved.path == std::vector<std::string>{"", "foo", "bar", "baz"});
}

// ─────────────────────────────────────────────────────────────────────────────
// URL canonicalisation tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parsed_url canonicalise dot segments", "[url][canonicalise]") {
  auto url = parseURL("https://example.com/foo/./bar/../baz");
  auto canonical = url.canonicalise();

  REQUIRE(canonical.path == std::vector<std::string>{"", "foo", "baz"});
}

// ─────────────────────────────────────────────────────────────────────────────
// fixGitURL tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("fix_git_url scp style", "[url][git]") {
  auto url = fixGitURL("git@github.com:NixOS/nix");

  REQUIRE(url.scheme == "ssh");
  REQUIRE(url.authority.has_value());
  REQUIRE(url.authority->user == "git");
  REQUIRE(url.authority->host == "github.com");
}

TEST_CASE("fix_git_url strips git+ prefix", "[url][git]") {
  auto url = fixGitURL("git+https://github.com/NixOS/nix");

  REQUIRE(url.scheme == "https");
  REQUIRE(url.authority->host == "github.com");
}

TEST_CASE("fix_git_url local path", "[url][git]") {
  auto url = fixGitURL("/home/user/repo");

  REQUIRE(url.scheme == "file");
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
  auto parsed = parseURL("https://example.com/path");
  verbatim_url_t url(parsed);

  REQUIRE(url.scheme() == "https");
  auto reparsed = url.parsed();
  REQUIRE(reparsed.authority->host == "example.com");
}

TEST_CASE("verbatim_url last_path_segment", "[url][verbatim]") {
  verbatim_url_t url(std::string{"https://example.com/path/to/file.txt"});

  auto segment = url.lastPathSegment();
  REQUIRE(segment.has_value());
  REQUIRE(*segment == "file.txt");
}

TEST_CASE("verbatim_url last_path_segment with query", "[url][verbatim]") {
  verbatim_url_t url(std::string{"https://example.com/path/to/file.txt?query=value"});

  auto segment = url.lastPathSegment();
  REQUIRE(segment.has_value());
  REQUIRE(*segment == "file.txt");
}

TEST_CASE("verbatim_url last_path_segment empty path", "[url][verbatim]") {
  verbatim_url_t url(std::string{"https://example.com/"});

  auto segment = url.lastPathSegment();
  REQUIRE_FALSE(segment.has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Error handling tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parse_url invalid url throws", "[url][error]") {
  REQUIRE_THROWS_AS(parseURL("not a valid url"), BadURL);
  REQUIRE_THROWS_AS(parseURL("://missing-scheme"), BadURL);
  REQUIRE_THROWS_AS(parseURL(""), BadURL);
}

TEST_CASE("parse_url file with authority throws", "[url][error]") {
  // file:// URLs with non-empty host are invalid
  REQUIRE_THROWS_AS(parseURL("file://remotehost/path"), BadURL);
}

TEST_CASE("percent_decode invalid encoding throws", "[url][error][encoding]") {
  REQUIRE_THROWS_AS(percentDecode("%GG"), BadURL);
  REQUIRE_THROWS_AS(percentDecode("%"), BadURL);
  REQUIRE_THROWS_AS(percentDecode("%1"), BadURL);
}

TEST_CASE("render_url_path_ensure_legal with slash throws", "[url][error]") {
  std::vector<std::string> path = {"foo", "bar/baz", "quux"};
  REQUIRE_THROWS_AS(renderUrlPathEnsureLegal(path), BadURL);
}

TEST_CASE("render_url_path_ensure_legal with nul throws", "[url][error]") {
  std::vector<std::string> path = {"foo", std::string("bar\0baz", 7), "quux"};
  REQUIRE_THROWS_AS(renderUrlPathEnsureLegal(path), BadURL);
}

// ─────────────────────────────────────────────────────────────────────────────
// Authority parsing tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("authority parse simple host", "[url][authority]") {
  auto auth = parsed_url_t::authority_t::parse("example.com");

  REQUIRE(auth.host == "example.com");
  REQUIRE(auth.hostType == parsed_url_t::authority_t::host_type_t::Name);
  REQUIRE_FALSE(auth.port.has_value());
  REQUIRE_FALSE(auth.user.has_value());
}

TEST_CASE("authority parse with port", "[url][authority]") {
  auto auth = parsed_url_t::authority_t::parse("example.com:8080");

  REQUIRE(auth.host == "example.com");
  REQUIRE(auth.port == 8080);
}

TEST_CASE("authority parse with userinfo", "[url][authority]") {
  auto auth = parsed_url_t::authority_t::parse("user:pass@example.com");

  REQUIRE(auth.user == "user");
  REQUIRE(auth.password == "pass");
  REQUIRE(auth.host == "example.com");
}

TEST_CASE("authority parse ipv4", "[url][authority]") {
  auto auth = parsed_url_t::authority_t::parse("192.168.1.1:80");

  REQUIRE(auth.host == "192.168.1.1");
  REQUIRE(auth.hostType == parsed_url_t::authority_t::host_type_t::IPv4);
  REQUIRE(auth.port == 80);
}

TEST_CASE("authority parse ipv6", "[url][authority]") {
  auto auth = parsed_url_t::authority_t::parse("[::1]:8080");

  REQUIRE(auth.host == "::1");
  REQUIRE(auth.hostType == parsed_url_t::authority_t::host_type_t::IPv6);
  REQUIRE(auth.port == 8080);
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
  auto url = parseURL("https://example.com#hello world", true);

  REQUIRE(url.fragment == "hello world");
}

TEST_CASE("parse_url lenient mode with spaces in query", "[url][lenient]") {
  auto url = parseURL("https://example.com?key=hello world", true);

  REQUIRE(url.query.at("key") == "hello world");
}

// ─────────────────────────────────────────────────────────────────────────────
// Path segments tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parsed_url path_segments skip empty", "[url][path]") {
  auto url = parseURL("https://example.com//foo///bar//");

  auto segments_all = url.pathSegments(false);
  auto all_vec = std::vector<std::string>(segments_all.begin(), segments_all.end());
  REQUIRE(all_vec == std::vector<std::string>{"", "", "foo", "", "", "bar", "", ""});

  auto segments_non_empty = url.pathSegments(true);
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

    auto encoded = percentEncode(input);
    auto decoded = percentDecode(encoded);

    RC_ASSERT(decoded == input);
  });

  rc::prop("percent encoding produces valid url characters", []() {
    auto input = *rc::gen::container<std::string>(rc::gen::inRange<char>(0, 127));

    auto encoded = percentEncode(input);

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

    auto encoded = percentEncode(input);

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

    auto parsed = parseURL(url_string);
    auto serialized = parsed.to_string();

    // Should be able to parse the serialized result
    auto reparsed = parseURL(serialized);

    RC_ASSERT(reparsed.scheme == parsed.scheme);
    RC_ASSERT(reparsed.authority->host == parsed.authority->host);
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

    auto encoded = encodeQuery(original);
    auto decoded = decodeQuery(encoded);

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

    RC_ASSERT(isValidSchemeName(scheme));
  });

  rc::prop("invalid scheme names start with non-letter", []() {
    auto first_char = *rc::gen::oneOf(rc::gen::inRange<char>('0', '9' + 1),
                                      rc::gen::element<char>('+', '-', '.', ':', '@', '!'));

    std::string scheme = std::string(1, first_char) + "abc";

    RC_ASSERT_FALSE(isValidSchemeName(scheme));
  });
}

TEST_CASE("url scheme parsing property", "[url][property][scheme]") {
  rc::prop("parsed scheme transport is suffix after +", []() {
    auto transport = *rc::gen::element<std::string>("http", "https", "file", "ssh");
    auto application = *rc::gen::element<std::string>("git", "tarball", "path");

    auto compound = application + "+" + transport;
    auto parsed = parseUrlScheme(compound);

    RC_ASSERT(parsed.application.has_value());
    RC_ASSERT(*parsed.application == application);
    RC_ASSERT(parsed.transport == transport);
  });
}
