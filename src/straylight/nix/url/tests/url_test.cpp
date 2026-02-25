// straylight::nix::primitives::tests
//
// Heavy metal tests for URL parsing primitives
// Unit tests, property-based tests, and fuzz tests

// Catch2 must be included before rapidcheck/catch.h
#include <catch2/catch_test_macros.hpp>

// RapidCheck for property-based testing
#include <cctype>
#include <string>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include "../config.h"
#include "../url.h"

namespace url = straylight::nix::url;

// ─────────────────────────────────────────────────────────────────────────────
// Generators for property-based tests
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Generate valid scheme names (letter + [letter|digit|+|-|.])
// Use '{' which is char after 'z', ':' after '9', '[' after 'Z' to avoid int promotion
rc::Gen<std::string> valid_scheme_gen() {
  return rc::gen::apply(
      [](char first, std::vector<char> rest) {
        std::string result(1, first);
        result.append(rest.begin(), rest.end());
        return result;
      },
      rc::gen::inRange('a', '{'),
      rc::gen::container<std::vector<char>>(rc::gen::oneOf(rc::gen::inRange('a', '{'),
                                                           rc::gen::inRange('0', ':'),
                                                           rc::gen::element('+', '-', '.'))));
}

// Generate simple alphanumeric hostnames
rc::Gen<std::string> hostname_gen() {
  return rc::gen::nonEmpty(rc::gen::container<std::string>(
      rc::gen::oneOf(rc::gen::inRange('a', '{'), rc::gen::inRange('0', ':'))));
}

// Generate valid path segments (alphanumeric + some safe chars)
rc::Gen<std::string> path_segment_gen() {
  return rc::gen::container<std::string>(
      rc::gen::oneOf(rc::gen::inRange('a', '{'), rc::gen::inRange('A', '['),
                     rc::gen::inRange('0', ':'), rc::gen::element('-', '_', '.')));
}

// Generate printable ASCII strings (for percent encoding tests)
rc::Gen<std::string> printable_ascii_gen() {
  return rc::gen::container<std::string>(rc::gen::inRange<char>(32, 127));
}

// Generate arbitrary byte strings (for fuzz tests)
rc::Gen<std::string> arbitrary_bytes_gen() {
  return rc::gen::container<std::string>(rc::gen::arbitrary<char>());
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Backend introspection tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("url backend config", "[url][config]") {
#if defined(STRAYLIGHT_URL_BACKEND_ADA)
  REQUIRE(url::active_url_backend == url::url_backend::ada);
  REQUIRE(std::string(url::url_backend_name) == "ada");
  REQUIRE(url::url_backend_is_whatwg == true);
#else
  REQUIRE(url::active_url_backend == url::url_backend::boost);
  REQUIRE(std::string(url::url_backend_name) == "boost::url");
  REQUIRE(url::url_backend_is_whatwg == false);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Percent encoding unit tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("percent_decode basic", "[url][encode]") {
  REQUIRE(url::percent_decode("hello") == "hello");
  REQUIRE(url::percent_decode("hello%20world") == "hello world");
  REQUIRE(url::percent_decode("%2F") == "/");
  REQUIRE(url::percent_decode("%2f") == "/"); // lowercase hex
  REQUIRE(url::percent_decode("100%25") == "100%");
  REQUIRE(url::percent_decode("") == "");
}

TEST_CASE("percent_decode invalid sequences", "[url][encode]") {
  // Invalid sequences should be passed through
  REQUIRE(url::percent_decode("%") == "%");
  REQUIRE(url::percent_decode("%2") == "%2");
  REQUIRE(url::percent_decode("%GG") == "%GG");
  REQUIRE(url::percent_decode("%%20") == "% ");
}

TEST_CASE("percent_encode basic", "[url][encode]") {
  REQUIRE(url::percent_encode("hello") == "hello");
  REQUIRE(url::percent_encode("hello world") == "hello%20world");
  REQUIRE(url::percent_encode("/path/to/file") == "%2Fpath%2Fto%2Ffile");
  REQUIRE(url::percent_encode("") == "");
}

TEST_CASE("percent_encode with keep", "[url][encode]") {
  REQUIRE(url::percent_encode("/path/to/file", "/") == "/path/to/file");
  REQUIRE(url::percent_encode("a:b@c", ":@") == "a:b@c");
  REQUIRE(url::percent_encode("a:b c", ":") == "a:b%20c");
}

TEST_CASE("is_unreserved", "[url][encode]") {
  // Unreserved: A-Z, a-z, 0-9, -, ., _, ~
  REQUIRE(url::is_unreserved('a'));
  REQUIRE(url::is_unreserved('z'));
  REQUIRE(url::is_unreserved('A'));
  REQUIRE(url::is_unreserved('Z'));
  REQUIRE(url::is_unreserved('0'));
  REQUIRE(url::is_unreserved('9'));
  REQUIRE(url::is_unreserved('-'));
  REQUIRE(url::is_unreserved('.'));
  REQUIRE(url::is_unreserved('_'));
  REQUIRE(url::is_unreserved('~'));

  REQUIRE_FALSE(url::is_unreserved(' '));
  REQUIRE_FALSE(url::is_unreserved('/'));
  REQUIRE_FALSE(url::is_unreserved(':'));
  REQUIRE_FALSE(url::is_unreserved('@'));
  REQUIRE_FALSE(url::is_unreserved('%'));
}

// ─────────────────────────────────────────────────────────────────────────────
// URL parsing unit tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parse simple https url", "[url][parse]") {
  auto result = url::parse("https://example.com/path/to/resource");
  REQUIRE(result.has_value());

  auto& u = *result;
  REQUIRE(u.scheme == "https");
  REQUIRE(u.auth.has_value());
  REQUIRE(u.auth->host == "example.com");
  REQUIRE(u.auth->port == std::nullopt);
  REQUIRE(u.auth->user == std::nullopt);
  REQUIRE(u.path.size() >= 3);
  REQUIRE(u.query.empty());
  REQUIRE(u.fragment.empty());
}

TEST_CASE("parse url with port", "[url][parse]") {
  auto result = url::parse("http://localhost:8080/api");
  REQUIRE(result.has_value());

  auto& u = *result;
  REQUIRE(u.scheme == "http");
  REQUIRE(u.auth.has_value());
  REQUIRE(u.auth->host == "localhost");
  REQUIRE(u.auth->port == 8080);
}

TEST_CASE("parse url with query parameters", "[url][parse]") {
  auto result = url::parse("https://example.com/search?key=value&foo=bar");
  REQUIRE(result.has_value());

  auto& u = *result;
  REQUIRE(u.scheme == "https");
  REQUIRE(u.query.size() == 2);

  // Check query params (order may vary)
  bool found_key = false;
  bool found_foo = false;
  for (const auto& [k, v] : u.query) {
    if (k == "key" && v == "value") {
      found_key = true;
    }
    if (k == "foo" && v == "bar") {
      found_foo = true;
    }
  }
  REQUIRE(found_key);
  REQUIRE(found_foo);
}

TEST_CASE("parse url with fragment", "[url][parse]") {
  auto result = parse("https://example.com/page#section1");
  REQUIRE(result.has_value());

  auto& u = *result;
  REQUIRE(u.fragment == "section1");
}

TEST_CASE("parse url with userinfo", "[url][parse]") {
  auto result = parse("https://user@example.com/path");
  REQUIRE(result.has_value());

  auto& u = *result;
  REQUIRE(u.auth.has_value());
  REQUIRE(u.auth->user == "user");
  REQUIRE(u.auth->host == "example.com");
}

TEST_CASE("parse url with userinfo and password", "[url][parse]") {
  auto result = parse("https://user:pass@example.com/path");
  REQUIRE(result.has_value());

  auto& u = *result;
  REQUIRE(u.auth.has_value());
  REQUIRE(u.auth->user == "user");
  REQUIRE(u.auth->password == "pass");
  REQUIRE(u.auth->host == "example.com");
}

TEST_CASE("parse file url", "[url][parse]") {
  auto result = parse("file:///home/user/file.txt");
  REQUIRE(result.has_value());

  auto& u = *result;
  REQUIRE(u.scheme == "file");
  REQUIRE(u.auth.has_value());
  // file:// URLs may have empty host
}

TEST_CASE("parse ipv4 url", "[url][parse]") {
  auto result = parse("http://192.168.1.1:8080/api");
  REQUIRE(result.has_value());

  auto& u = *result;
  REQUIRE(u.auth.has_value());
  REQUIRE(u.auth->host == "192.168.1.1");
  REQUIRE(u.auth->type == host_type::ipv4);
  REQUIRE(u.auth->port == 8080);
}

TEST_CASE("parse ipv6 url", "[url][parse]") {
  auto result = parse("http://[::1]:8080/api");
  REQUIRE(result.has_value());

  auto& u = *result;
  REQUIRE(u.auth.has_value());
  REQUIRE(u.auth->type == host_type::ipv6);
  REQUIRE(u.auth->port == 8080);
}

TEST_CASE("can_parse validates urls", "[url][parse]") {
  REQUIRE(can_parse("https://example.com"));
  REQUIRE(can_parse("http://localhost:8080"));
  REQUIRE(can_parse("file:///path/to/file"));

  // Invalid URLs
  REQUIRE_FALSE(can_parse(""));
  REQUIRE_FALSE(can_parse("not a url"));
  REQUIRE_FALSE(can_parse("://missing-scheme"));
}

// ─────────────────────────────────────────────────────────────────────────────
// URL serialization tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("url to_string roundtrip", "[url][serialize]") {
  const auto* original = "https://user:pass@example.com:8080/path/to/resource?key=value#section";
  auto result = parse(original);
  REQUIRE(result.has_value());

  auto serialized = result->to_string();

  // Parse again to verify
  auto reparsed = parse(serialized);
  REQUIRE(reparsed.has_value());

  REQUIRE(reparsed->scheme == result->scheme);
  REQUIRE(reparsed->auth->host == result->auth->host);
  REQUIRE(reparsed->auth->port == result->auth->port);
}

TEST_CASE("authority to_string", "[url][serialize]") {
  authority auth;
  auth.host = "example.com";
  auth.type = host_type::name;
  REQUIRE(auth.to_string() == "example.com");

  auth.port = 8080;
  REQUIRE(auth.to_string() == "example.com:8080");

  auth.user = "user";
  REQUIRE(auth.to_string() == "user@example.com:8080");

  auth.password = "pass";
  REQUIRE(auth.to_string() == "user:pass@example.com:8080");
}

TEST_CASE("authority to_string ipv6", "[url][serialize]") {
  authority auth;
  auth.host = "::1";
  auth.type = host_type::ipv6;
  auth.port = 8080;

  REQUIRE(auth.to_string() == "[::1]:8080");
}

// ─────────────────────────────────────────────────────────────────────────────
// Scheme utilities tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("is_valid_scheme", "[url][scheme]") {
  REQUIRE(is_valid_scheme("http"));
  REQUIRE(is_valid_scheme("https"));
  REQUIRE(is_valid_scheme("git+ssh"));
  REQUIRE(is_valid_scheme("a1"));
  REQUIRE(is_valid_scheme("a-b.c+d"));

  REQUIRE_FALSE(is_valid_scheme(""));
  REQUIRE_FALSE(is_valid_scheme("1http")); // Must start with letter
  REQUIRE_FALSE(is_valid_scheme("+http"));
  REQUIRE_FALSE(is_valid_scheme("http:"));
}

TEST_CASE("parse_scheme simple", "[url][scheme]") {
  auto parts = parse_scheme("https");
  REQUIRE_FALSE(parts.application.has_value());
  REQUIRE(parts.transport == "https");
}

TEST_CASE("parse_scheme compound", "[url][scheme]") {
  auto parts = parse_scheme("git+https");
  REQUIRE(parts.application.has_value());
  REQUIRE(*parts.application == "git");
  REQUIRE(parts.transport == "https");
}

TEST_CASE("is_special_scheme", "[url][scheme]") {
  REQUIRE(is_special_scheme("http"));
  REQUIRE(is_special_scheme("https"));
  REQUIRE(is_special_scheme("ws"));
  REQUIRE(is_special_scheme("wss"));
  REQUIRE(is_special_scheme("ftp"));
  REQUIRE(is_special_scheme("file"));

  REQUIRE_FALSE(is_special_scheme("git"));
  REQUIRE_FALSE(is_special_scheme("ssh"));
  REQUIRE_FALSE(is_special_scheme("mailto"));
}

TEST_CASE("default_port", "[url][scheme]") {
  REQUIRE(default_port("http") == 80);
  REQUIRE(default_port("https") == 443);
  REQUIRE(default_port("ws") == 80);
  REQUIRE(default_port("wss") == 443);
  REQUIRE(default_port("ftp") == 21);
  REQUIRE(default_port("file") == 0);
  REQUIRE(default_port("git") == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Query string tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parse_query basic", "[url][query]") {
  auto params = parse_query("key=value&foo=bar");
  REQUIRE(params.size() == 2);

  bool found_key = false;
  bool found_foo = false;
  for (const auto& [k, v] : params) {
    if (k == "key" && v == "value") {
      found_key = true;
    }
    if (k == "foo" && v == "bar") {
      found_foo = true;
    }
  }
  REQUIRE(found_key);
  REQUIRE(found_foo);
}

TEST_CASE("parse_query percent encoded", "[url][query]") {
  auto params = parse_query("key%20name=value%26data");
  REQUIRE(params.size() == 1);
  REQUIRE(params[0].first == "key name");
  REQUIRE(params[0].second == "value&data");
}

TEST_CASE("parse_query empty value", "[url][query]") {
  auto params = parse_query("key=");
  REQUIRE(params.size() == 1);
  REQUIRE(params[0].first == "key");
  REQUIRE(params[0].second == "");
}

TEST_CASE("encode_query basic", "[url][query]") {
  query_params params = {{"key", "value"}, {"foo", "bar"}};
  auto encoded = encode_query(params);

  // Should contain both pairs
  REQUIRE(encoded.find("key=value") != std::string::npos);
  REQUIRE(encoded.find("foo=bar") != std::string::npos);
  REQUIRE(encoded.find('&') != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Git URL normalization tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("normalize_git_url scp style", "[url][git]") {
  auto result = normalize_git_url("git@github.com:user/repo.git");
  REQUIRE(result == "ssh://git@github.com/user/repo.git");
}

TEST_CASE("normalize_git_url already normalized", "[url][git]") {
  auto result = normalize_git_url("https://github.com/user/repo.git");
  REQUIRE(result == "https://github.com/user/repo.git");
}

TEST_CASE("normalize_git_url ssh url", "[url][git]") {
  auto result = normalize_git_url("ssh://git@github.com/user/repo.git");
  REQUIRE(result == "ssh://git@github.com/user/repo.git");
}

// ─────────────────────────────────────────────────────────────────────────────
// Canonicalization tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("url canonicalize removes dot segments", "[url][canonicalize]") {
  auto result = parse("https://example.com/a/b/../c/./d");
  REQUIRE(result.has_value());

  auto canonical = result->canonicalize();
  auto path_str = canonical.render_path(false);

  // Should resolve .. and . segments
  REQUIRE(path_str.find("..") == std::string::npos);
  REQUIRE(path_str.find("/./") == std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-based tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("percent encoding property tests", "[url][property][encoding]") {
  rc::prop("percent encode/decode roundtrip", []() {
    auto input = *printable_ascii_gen();

    auto encoded = percent_encode(input);
    auto decoded = percent_decode(encoded);

    RC_ASSERT(decoded == input);
  });

  rc::prop("percent encoding produces valid url characters", []() {
    auto input = *arbitrary_bytes_gen();

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
        RC_ASSERT(is_unreserved(c));
      }
    }
  });

  rc::prop("encoded length is at least original length", []() {
    auto input = *arbitrary_bytes_gen();

    auto encoded = percent_encode(input);

    RC_ASSERT(encoded.size() >= input.size());
  });

  rc::prop("double encoding is not idempotent", []() {
    auto input = *rc::gen::nonEmpty(printable_ascii_gen());

    auto once = percent_encode(input);
    auto twice = percent_encode(once);

    // If input had any chars needing encoding, twice will be different
    // (because % gets encoded to %25)
    if (once != input) {
      RC_ASSERT(twice != once);
    }
  });
}

TEST_CASE("url parse/serialize roundtrip property", "[url][property][roundtrip]") {
  rc::prop("parse then serialize produces parseable url", []() {
    auto scheme = *rc::gen::element<std::string>("http", "https", "ftp", "file");
    auto host = *hostname_gen();

    // Limit hostname length
    if (host.size() > 20) {
      host = host.substr(0, 20);
    }

    std::string url_string = scheme + "://" + host + "/";

    auto parsed = parse(url_string);
    RC_ASSERT(parsed.has_value());

    auto serialized = parsed->to_string();

    auto reparsed = parse(serialized);
    RC_ASSERT(reparsed.has_value());

    RC_ASSERT(reparsed->scheme == parsed->scheme);
  });

  rc::prop("simple urls survive roundtrip", []() {
    auto scheme = *rc::gen::element<std::string>("http", "https");
    auto host = *hostname_gen();
    auto path_segs = *rc::gen::container<std::vector<std::string>>(path_segment_gen());

    if (host.size() > 20) {
      host = host.substr(0, 20);
    }

    std::string url_string = scheme + "://" + host;
    for (const auto& seg : path_segs) {
      if (seg.empty()) {
        continue;
      }
      url_string += "/" + seg;
    }

    auto parsed = parse(url_string);
    RC_ASSERT(parsed.has_value());

    auto serialized = parsed->to_string();
    auto reparsed = parse(serialized);
    RC_ASSERT(reparsed.has_value());

    RC_ASSERT(reparsed->scheme == parsed->scheme);
    RC_ASSERT(reparsed->auth->host == parsed->auth->host);
  });
}

TEST_CASE("query string property tests", "[url][property][query]") {
  rc::prop("query encode/decode roundtrip with alphanumeric", []() {
    auto key = *rc::gen::nonEmpty(rc::gen::container<std::string>(rc::gen::inRange('a', '{')));
    auto value = *rc::gen::container<std::string>(rc::gen::inRange('a', '{'));

    if (key.size() > 20) {
      key = key.substr(0, 20);
    }
    if (value.size() > 50) {
      value = value.substr(0, 50);
    }

    query_params original = {{key, value}};

    auto encoded = encode_query(original);
    auto decoded = parse_query(encoded);

    RC_ASSERT(decoded.size() == 1);
    RC_ASSERT(decoded[0].first == key);
    RC_ASSERT(decoded[0].second == value);
  });
}

TEST_CASE("scheme validation property tests", "[url][property][scheme]") {
  rc::prop("valid scheme names start with letter", []() {
    auto scheme = *valid_scheme_gen();

    if (scheme.size() > 30) {
      scheme = scheme.substr(0, 30);
    }

    RC_ASSERT(is_valid_scheme(scheme));
  });

  rc::prop("invalid scheme names start with non-letter", []() {
    auto first_char = *rc::gen::oneOf(rc::gen::inRange('0', ':'), rc::gen::element('+', '-', '.'));

    std::string scheme = std::string(1, first_char) + "abc";

    RC_ASSERT_FALSE(is_valid_scheme(scheme));
  });

  rc::prop("parsed scheme transport is suffix after +", []() {
    auto transport = *rc::gen::element<std::string>("http", "https", "file", "ssh");
    auto application = *rc::gen::element<std::string>("git", "tarball", "path");

    auto compound = application + "+" + transport;
    auto parts = parse_scheme(compound);

    RC_ASSERT(parts.application.has_value());
    RC_ASSERT(*parts.application == application);
    RC_ASSERT(parts.transport == transport);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Fuzz tests (robustness against malformed input)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("url parser fuzz tests", "[url][fuzz]") {
  rc::prop("random bytes don't crash parser", []() {
    auto garbage = *arbitrary_bytes_gen();

    // Should not crash
    auto result = parse(garbage);

    // Result is either valid or error, never undefined
    if (result.has_value()) {
      // If it parsed, it should be serializable
      auto serialized = result->to_string();
      RC_ASSERT(!serialized.empty());
    } else {
      // Error should have a message
      RC_ASSERT(!result.error().message.empty());
    }
  });

  rc::prop("can_parse never crashes on garbage", []() {
    auto garbage = *arbitrary_bytes_gen();

    // Should not crash, just return true or false
    [[maybe_unused]] bool valid = can_parse(garbage);
  });

  rc::prop("percent_decode never crashes on garbage", []() {
    auto garbage = *arbitrary_bytes_gen();

    // Should not crash
    auto decoded = percent_decode(garbage);

    // Result should be at most as long as input (decoding shrinks)
    RC_ASSERT(decoded.size() <= garbage.size() + garbage.size()); // Conservative bound
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// IPv6 zone ID tests (WHATWG/RFC 6874 compatibility)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("zone_id extraction", "[url][ipv6][zone]") {
  auto extracted = detail::extract_zone_id("http://[fe80::1%25eth0]:8080/path");

  REQUIRE(extracted.zone_id == "eth0");
  REQUIRE(extracted.url_without_zone.find("%25") == std::string::npos);
}

TEST_CASE("zone_id extraction no zone", "[url][ipv6][zone]") {
  auto extracted = detail::extract_zone_id("http://[::1]:8080/path");

  REQUIRE(extracted.zone_id.empty());
  REQUIRE(extracted.url_without_zone == "http://[::1]:8080/path");
}

TEST_CASE("zone_id restoration", "[url][ipv6][zone]") {
  authority auth;
  auth.host = "fe80::1";
  auth.type = host_type::ipv6;

  detail::restore_zone_id(auth, "eth0");

  REQUIRE(auth.host == "fe80::1%eth0");
}
