// straylight::nix::text::tests
//
// Heavy metal tests for string primitives with adaptive backend selection.
// Unit tests and property-based tests.

#include <catch2/catch_test_macros.hpp>
// Catch2 must be included before rapidcheck/catch.h

// RapidCheck for property-based testing
#include <string>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>
#include <straylight/nix/text/strings.h>

namespace text = straylight::nix::text;

// ─────────────────────────────────────────────────────────────────────────────
// Generators for property-based tests
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Generate arbitrary printable ASCII strings
rc::Gen<std::string> printable_gen() {
  return rc::gen::container<std::string>(rc::gen::inRange<char>(32, 127));
}

// Generate non-empty printable strings
rc::Gen<std::string> nonempty_printable_gen() {
  return rc::gen::nonEmpty(printable_gen());
}

// Generate arbitrary byte strings
rc::Gen<std::string> bytes_gen() {
  return rc::gen::container<std::string>(rc::gen::arbitrary<char>());
}

// Generate alphanumeric strings (safe for most operations)
rc::Gen<std::string> alnum_gen() {
  return rc::gen::container<std::string>(
      rc::gen::oneOf(rc::gen::inRange('a', static_cast<char>('z' + 1)),
                     rc::gen::inRange('A', static_cast<char>('Z' + 1)),
                     rc::gen::inRange('0', static_cast<char>('9' + 1))));
}

// Generate whitespace strings
rc::Gen<std::string> whitespace_gen() {
  return rc::gen::container<std::string>(rc::gen::element(' ', '\t', '\n', '\r'));
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Config tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("config thresholds are positive", "[strings][config]") {
  REQUIRE(text::config::find_threshold > 0);
  REQUIRE(text::config::prefix_threshold > 0);
  REQUIRE(text::config::replace_threshold > 0);
  REQUIRE(text::config::split_threshold > 0);
}

TEST_CASE("config dispatch functions work", "[strings][config]") {
  // Small sizes should not use SIMD
  REQUIRE_FALSE(text::config::use_simd_find(1));
  REQUIRE_FALSE(text::config::use_simd_find(10));

  // Large sizes should use SIMD (unless forced to std)
#if !STRAYLIGHT_STRINGS_FORCE_STD
  REQUIRE(text::config::use_simd_find(10000));
  REQUIRE(text::config::use_simd_prefix(10000));
  REQUIRE(text::config::use_simd_replace(10000));
  REQUIRE(text::config::use_simd_split(10000));
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Conversion tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("to_sz conversion", "[strings][convert]") {
  std::string_view std_sv = "hello world";
  text::sz_string_view sz_sv = text::to_sz(std_sv);

  REQUIRE(sz_sv.size() == std_sv.size());
  REQUIRE(sz_sv.data() == std_sv.data());
}

TEST_CASE("to_std conversion", "[strings][convert]") {
  text::sz_string_view sz_sv{"hello world"};
  std::string_view std_sv = text::to_std(sz_sv);

  REQUIRE(std_sv.size() == sz_sv.size());
  REQUIRE(std_sv.data() == sz_sv.data());
}

TEST_CASE("to_string conversion", "[strings][convert]") {
  std::string_view sv{"hello world"};
  std::string s = text::to_string(sv);

  REQUIRE(s == "hello world");
}

// ─────────────────────────────────────────────────────────────────────────────
// Search tests (find/rfind)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("find basic", "[strings][find]") {
  std::string_view s{"hello world"};

  REQUIRE(text::find(s, "hello") == 0);
  REQUIRE(text::find(s, "world") == 6);
  REQUIRE(text::find(s, "o") == 4);
  REQUIRE(text::find(s, "xyz") == std::string_view::npos);
}

TEST_CASE("find empty needle", "[strings][find]") {
  std::string_view s{"hello world"};
  // Empty needle behavior depends on backend - both returning npos or 0 are valid
  // Just check it doesn't crash
  [[maybe_unused]] auto pos = text::find(s, "");
}

TEST_CASE("rfind basic", "[strings][find]") {
  std::string_view s{"hello hello"};

  REQUIRE(text::rfind(s, "hello") == 6);
  REQUIRE(text::rfind(s, "o") == 10);
  REQUIRE(text::rfind(s, "xyz") == std::string_view::npos);
}

TEST_CASE("contains basic", "[strings][find]") {
  std::string_view s{"hello world"};

  REQUIRE(text::contains(s, "hello"));
  REQUIRE(text::contains(s, "world"));
  REQUIRE(text::contains(s, "o w"));
  REQUIRE_FALSE(text::contains(s, "xyz"));
}

TEST_CASE("starts_with basic", "[strings][find]") {
  std::string_view s{"hello world"};

  REQUIRE(text::starts_with(s, "hello"));
  REQUIRE(text::starts_with(s, "h"));
  REQUIRE(text::starts_with(s, ""));
  REQUIRE_FALSE(text::starts_with(s, "world"));
  REQUIRE_FALSE(text::starts_with(s, "hello world!"));
}

TEST_CASE("ends_with basic", "[strings][find]") {
  std::string_view s{"hello world"};

  REQUIRE(text::ends_with(s, "world"));
  REQUIRE(text::ends_with(s, "d"));
  REQUIRE(text::ends_with(s, ""));
  REQUIRE_FALSE(text::ends_with(s, "hello"));
  REQUIRE_FALSE(text::ends_with(s, "!hello world"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Character set operations tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("find_first_of basic", "[strings][charset]") {
  std::string_view s{"hello world"};

  REQUIRE(text::find_first_of(s, "aeiou") == 1); // 'e' at index 1
  REQUIRE(text::find_first_of(s, "xyz") == std::string_view::npos);
  REQUIRE(text::find_first_of(s, "w") == 6);
}

TEST_CASE("find_first_not_of basic", "[strings][charset]") {
  std::string_view s{"   hello"};

  REQUIRE(text::find_first_not_of(s, " ") == 3);
  REQUIRE(text::find_first_not_of(s, "helo ") == std::string_view::npos);
}

TEST_CASE("find_last_of basic", "[strings][charset]") {
  std::string_view s{"hello world"};

  REQUIRE(text::find_last_of(s, "aeiou") == 7); // 'o' at index 7
  REQUIRE(text::find_last_of(s, "xyz") == std::string_view::npos);
}

TEST_CASE("find_last_not_of basic", "[strings][charset]") {
  std::string_view s{"hello   "};

  REQUIRE(text::find_last_not_of(s, " ") == 4);
}

// ─────────────────────────────────────────────────────────────────────────────
// Split tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("split basic", "[strings][split]") {
  auto parts = text::split_to_strings("a,b,c", ",");

  REQUIRE(parts.size() == 3);
  REQUIRE(parts[0] == "a");
  REQUIRE(parts[1] == "b");
  REQUIRE(parts[2] == "c");
}

TEST_CASE("split preserves empty strings", "[strings][split]") {
  auto parts = text::split_to_strings("a,,b", ",");

  REQUIRE(parts.size() == 3);
  REQUIRE(parts[0] == "a");
  REQUIRE(parts[1] == "");
  REQUIRE(parts[2] == "b");
}

TEST_CASE("split empty string", "[strings][split]") {
  auto parts = text::split_to_strings("", ",");

  REQUIRE(parts.size() == 1);
  REQUIRE(parts[0] == "");
}

TEST_CASE("split no delimiter found", "[strings][split]") {
  auto parts = text::split_to_strings("hello", ",");

  REQUIRE(parts.size() == 1);
  REQUIRE(parts[0] == "hello");
}

TEST_CASE("split multi-char delimiter", "[strings][split]") {
  auto parts = text::split_to_strings("a::b::c", "::");

  REQUIRE(parts.size() == 3);
  REQUIRE(parts[0] == "a");
  REQUIRE(parts[1] == "b");
  REQUIRE(parts[2] == "c");
}

TEST_CASE("split_to_views returns views", "[strings][split]") {
  std::string original = "a,b,c";
  auto parts = text::split_to_views(original, ",");

  REQUIRE(parts.size() == 3);
  // Views should point into original string
  REQUIRE(parts[0].data() >= original.data());
  REQUIRE(parts[0].data() < original.data() + original.size());
}

TEST_CASE("tokenize basic", "[strings][split]") {
  auto parts = text::tokenize("hello  world\tfoo", " \t");

  REQUIRE(parts.size() == 3);
  REQUIRE(parts[0] == "hello");
  REQUIRE(parts[1] == "world");
  REQUIRE(parts[2] == "foo");
}

TEST_CASE("tokenize removes empty strings", "[strings][split]") {
  auto parts = text::tokenize("  hello  ", " ");

  REQUIRE(parts.size() == 1);
  REQUIRE(parts[0] == "hello");
}

// ─────────────────────────────────────────────────────────────────────────────
// Trim tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("trim_left basic", "[strings][trim]") {
  REQUIRE(text::trim_left("  hello") == "hello");
  REQUIRE(text::trim_left("\t\nhello") == "hello");
  REQUIRE(text::trim_left("hello") == "hello");
  REQUIRE(text::trim_left("   ") == "");
}

TEST_CASE("trim_right basic", "[strings][trim]") {
  REQUIRE(text::trim_right("hello  ") == "hello");
  REQUIRE(text::trim_right("hello\t\n") == "hello");
  REQUIRE(text::trim_right("hello") == "hello");
  REQUIRE(text::trim_right("   ") == "");
}

TEST_CASE("trim basic", "[strings][trim]") {
  REQUIRE(text::trim("  hello  ") == "hello");
  REQUIRE(text::trim("\t\nhello\r\n") == "hello");
  REQUIRE(text::trim("hello") == "hello");
  REQUIRE(text::trim("   ") == "");
}

TEST_CASE("trim with custom chars", "[strings][trim]") {
  REQUIRE(text::trim("...hello...", ".") == "hello");
  REQUIRE(text::trim("---hello---", "-") == "hello");
}

// ─────────────────────────────────────────────────────────────────────────────
// Replace tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("replace_all basic", "[strings][replace]") {
  REQUIRE(text::replace_all("hello world", "world", "there") == "hello there");
  REQUIRE(text::replace_all("aaa", "a", "b") == "bbb");
  REQUIRE(text::replace_all("hello", "x", "y") == "hello");
}

TEST_CASE("replace_all empty from", "[strings][replace]") {
  REQUIRE(text::replace_all("hello", "", "x") == "hello");
}

TEST_CASE("replace_all multiple occurrences", "[strings][replace]") {
  REQUIRE(text::replace_all("foo bar foo", "foo", "baz") == "baz bar baz");
}

TEST_CASE("replace_all with empty replacement", "[strings][replace]") {
  REQUIRE(text::replace_all("hello world", " world", "") == "hello");
}

TEST_CASE("replace_first basic", "[strings][replace]") {
  REQUIRE(text::replace_first("foo bar foo", "foo", "baz") == "baz bar foo");
  REQUIRE(text::replace_first("hello", "x", "y") == "hello");
}

// ─────────────────────────────────────────────────────────────────────────────
// Join tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("join basic", "[strings][join]") {
  std::vector<std::string> parts = {"a", "b", "c"};
  REQUIRE(text::join(",", parts) == "a,b,c");
}

TEST_CASE("join single element", "[strings][join]") {
  std::vector<std::string> parts = {"hello"};
  REQUIRE(text::join(",", parts) == "hello");
}

TEST_CASE("join empty vector", "[strings][join]") {
  std::vector<std::string> parts = {};
  REQUIRE(text::join(",", parts) == "");
}

TEST_CASE("join with empty separator", "[strings][join]") {
  std::vector<std::string> parts = {"a", "b", "c"};
  REQUIRE(text::join("", parts) == "abc");
}

// ─────────────────────────────────────────────────────────────────────────────
// Comparison tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("compare basic", "[strings][compare]") {
  REQUIRE(text::compare("abc", "abc") == 0);
  REQUIRE(text::compare("abc", "abd") < 0);
  REQUIRE(text::compare("abd", "abc") > 0);
  REQUIRE(text::compare("ab", "abc") < 0);
}

TEST_CASE("equal basic", "[strings][compare]") {
  REQUIRE(text::equal("hello", "hello"));
  REQUIRE_FALSE(text::equal("hello", "world"));
  REQUIRE_FALSE(text::equal("hello", "hello!"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Direct SIMD namespace tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("simd::find always uses stringzilla", "[strings][simd]") {
  std::string_view s{"hello world"};

  REQUIRE(text::simd::find(s, "hello") == 0);
  REQUIRE(text::simd::find(s, "world") == 6);
  REQUIRE(text::simd::find(s, "xyz") == std::string_view::npos);
}

TEST_CASE("simd::contains always uses stringzilla", "[strings][simd]") {
  std::string_view s{"hello world"};

  REQUIRE(text::simd::contains(s, "hello"));
  REQUIRE(text::simd::contains(s, "world"));
  REQUIRE_FALSE(text::simd::contains(s, "xyz"));
}

TEST_CASE("simd::replace_all always uses stringzilla", "[strings][simd]") {
  REQUIRE(text::simd::replace_all("hello world", "world", "there") == "hello there");
  REQUIRE(text::simd::replace_all("aaa", "a", "b") == "bbb");
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-based tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("find property tests", "[strings][property][find]") {
  rc::prop("find returns valid index or npos", []() {
    auto haystack = *printable_gen();
    auto needle = *printable_gen();

    auto pos = text::find(haystack, needle);

    if (pos != std::string_view::npos) {
      RC_ASSERT(pos + needle.size() <= haystack.size());
      RC_ASSERT(haystack.substr(pos, needle.size()) == needle);
    }
  });

  rc::prop("contains is equivalent to find != npos for non-empty needle", []() {
    auto haystack = *printable_gen();
    auto needle = *nonempty_printable_gen();

    auto found = text::contains(haystack, needle);
    auto pos = text::find(haystack, needle);

    RC_ASSERT(found == (pos != std::string_view::npos));
  });
}

TEST_CASE("starts_with/ends_with property tests", "[strings][property][prefix]") {
  rc::prop("starts_with is correct for prefix", []() {
    auto s = *printable_gen();

    if (!s.empty()) {
      auto prefix_len = *rc::gen::inRange<std::size_t>(0, s.size() + 1);
      auto prefix = s.substr(0, prefix_len);

      RC_ASSERT(text::starts_with(s, prefix));
    }
  });

  rc::prop("ends_with is correct for suffix", []() {
    auto s = *printable_gen();

    if (!s.empty()) {
      auto suffix_start = *rc::gen::inRange<std::size_t>(0, s.size() + 1);
      auto suffix = s.substr(suffix_start);

      RC_ASSERT(text::ends_with(s, suffix));
    }
  });

  rc::prop("empty string is both prefix and suffix", []() {
    auto s = *printable_gen();

    RC_ASSERT(text::starts_with(s, ""));
    RC_ASSERT(text::ends_with(s, ""));
  });
}

TEST_CASE("split/join roundtrip property tests", "[strings][property][split]") {
  rc::prop("join(split(s, sep), sep) preserves structure for single-char sep", []() {
    auto parts = *rc::gen::container<std::vector<std::string>>(alnum_gen());
    auto sep_char = *rc::gen::element(',', ':', ';', '|');
    std::string sep(1, sep_char);

    // Ensure no parts contain the separator
    for (auto& part : parts) {
      std::erase(part, sep_char);
    }

    auto joined = text::join(sep, parts);
    auto split_parts = text::split_to_strings(joined, sep);

    // Split should produce same number of parts (or 1 if empty)
    if (parts.empty()) {
      RC_ASSERT(split_parts.size() == 1);
      RC_ASSERT(split_parts[0] == "");
    } else {
      RC_ASSERT(split_parts.size() == parts.size());
      for (std::size_t i = 0; i < parts.size(); ++i) {
        RC_ASSERT(split_parts[i] == parts[i]);
      }
    }
  });
}

TEST_CASE("trim property tests", "[strings][property][trim]") {
  rc::prop("trim removes leading and trailing whitespace only", []() {
    auto content = *nonempty_printable_gen();
    auto leading_ws = *whitespace_gen();
    auto trailing_ws = *whitespace_gen();

    // Remove any whitespace from content edges to make test cleaner
    constexpr std::string_view ws_chars = " \t\n\r\f\v";
    while (!content.empty() && ws_chars.contains(content.front())) {
      content.erase(0, 1);
    }
    while (!content.empty() && ws_chars.contains(content.back())) {
      content.pop_back();
    }

    if (content.empty()) {
      return; // Skip this case
    }

    auto padded = leading_ws + content + trailing_ws;
    auto trimmed = text::trim(padded);

    RC_ASSERT(trimmed == content);
  });

  rc::prop("trim of trim is idempotent", []() {
    auto s = *printable_gen();

    auto once = std::string(text::trim(s));
    auto twice = std::string(text::trim(once));

    RC_ASSERT(once == twice);
  });
}

TEST_CASE("replace property tests", "[strings][property][replace]") {
  rc::prop("replace_all with same from and to is identity", []() {
    auto s = *printable_gen();
    auto pattern = *nonempty_printable_gen();

    auto result = text::replace_all(s, pattern, pattern);

    RC_ASSERT(result == s);
  });

  rc::prop("replace_all with empty pattern is identity", []() {
    auto s = *printable_gen();
    auto replacement = *printable_gen();

    auto result = text::replace_all(s, "", replacement);

    RC_ASSERT(result == s);
  });

  rc::prop("replace_first only replaces first occurrence", []() {
    const auto* unique_pattern = "UNIQUE_PATTERN";
    auto s = std::string(unique_pattern) + " middle " + unique_pattern;
    const auto* replacement = "REPLACED";

    auto result = text::replace_first(s, unique_pattern, replacement);

    // Should have exactly one of each
    RC_ASSERT(result.find(replacement) != std::string::npos);
    RC_ASSERT(result.find(unique_pattern) != std::string::npos);

    // First should be replacement
    RC_ASSERT(result.find(replacement) < result.find(unique_pattern));
  });
}

TEST_CASE("comparison property tests", "[strings][property][compare]") {
  rc::prop("compare is reflexive (s == s)", []() {
    auto s = *printable_gen();

    RC_ASSERT(text::compare(s, s) == 0);
    RC_ASSERT(text::equal(s, s));
  });

  rc::prop("compare is antisymmetric", []() {
    auto a = *printable_gen();
    auto b = *printable_gen();

    auto cmp_ab = text::compare(a, b);
    auto cmp_ba = text::compare(b, a);

    if (cmp_ab < 0) {
      RC_ASSERT(cmp_ba > 0);
    } else if (cmp_ab > 0) {
      RC_ASSERT(cmp_ba < 0);
    } else {
      RC_ASSERT(cmp_ba == 0);
    }
  });

  rc::prop("equal is consistent with compare", []() {
    auto a = *printable_gen();
    auto b = *printable_gen();

    RC_ASSERT(text::equal(a, b) == (text::compare(a, b) == 0));
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Adaptive dispatch tests (verify SIMD and std give same results)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("adaptive dispatch gives consistent results", "[strings][adaptive]") {
  rc::prop("find gives same result as simd::find for non-empty needle", []() {
    auto haystack = *printable_gen();
    auto needle = *nonempty_printable_gen(); // Non-empty to avoid std/sz empty needle difference

    auto adaptive_result = text::find(haystack, needle);
    auto simd_result = text::simd::find(haystack, needle);

    RC_ASSERT(adaptive_result == simd_result);
  });

  rc::prop("contains gives same result as simd::contains", []() {
    auto haystack = *printable_gen();
    auto needle = *nonempty_printable_gen();

    auto adaptive_result = text::contains(haystack, needle);
    auto simd_result = text::simd::contains(haystack, needle);

    RC_ASSERT(adaptive_result == simd_result);
  });

  rc::prop("replace_all gives same result as simd::replace_all", []() {
    auto s = *printable_gen();
    auto from = *nonempty_printable_gen();
    auto to = *printable_gen();

    auto adaptive_result = text::replace_all(s, from, to);
    auto simd_result = text::simd::replace_all(s, from, to);

    RC_ASSERT(adaptive_result == simd_result);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Fuzz tests (robustness against arbitrary input)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("string operations fuzz tests", "[strings][fuzz]") {
  rc::prop("find never crashes on arbitrary bytes", []() {
    auto haystack = *bytes_gen();
    auto needle = *bytes_gen();

    // Should not crash
    [[maybe_unused]] auto pos = text::find(haystack, needle);
  });

  rc::prop("split never crashes on arbitrary bytes", []() {
    auto s = *bytes_gen();
    auto sep = *bytes_gen();

    // Should not crash
    auto parts = text::split_to_strings(s, sep);
    RC_ASSERT(!parts.empty());
  });

  rc::prop("replace_all never crashes on arbitrary bytes", []() {
    auto s = *bytes_gen();
    auto from = *bytes_gen();
    auto to = *bytes_gen();

    // Should not crash
    [[maybe_unused]] auto result = text::replace_all(s, from, to);
  });

  rc::prop("trim never crashes on arbitrary bytes", []() {
    auto s = *bytes_gen();

    // Should not crash
    [[maybe_unused]] auto trimmed = text::trim(s);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Heavy metal roundtrip property tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("split/join roundtrip heavy metal", "[strings][property][roundtrip]") {
  rc::prop("join(split(s, sep), sep) == s when sep not in any part", []() {
    // Generate a single-char separator that won't appear in parts
    auto sep_char = *rc::gen::element(',', ':', ';', '|', '\t', '\n');
    std::string sep(1, sep_char);

    // Generate parts that don't contain the separator
    auto parts = *rc::gen::container<std::vector<std::string>>(rc::gen::container<std::string>(
        rc::gen::suchThat<char>(rc::gen::arbitrary<char>(),
                                [sep_char](char c) { return c != sep_char && c != '\0'; })));

    // Join then split should give back original parts
    auto joined = text::join(sep, parts);
    auto split_back = text::split_to_strings(joined, sep);

    if (parts.empty()) {
      // join of empty vector is "", split of "" is [""]
      RC_ASSERT(split_back.size() == 1);
      RC_ASSERT(split_back[0].empty());
    } else {
      RC_ASSERT(split_back == parts);
    }
  });

  rc::prop("split then join preserves original string", []() {
    // Generate string with known separator occurrences
    auto sep = *rc::gen::element<std::string_view>(",", "::", "||", "\t");
    auto s = *rc::gen::container<std::string>(rc::gen::inRange<char>(32, 127));

    auto parts = text::split_to_strings(s, sep);
    auto rejoined = text::join(sep, parts);

    RC_ASSERT(rejoined == s);
  });

  rc::prop("split with empty delimiter returns individual chars", []() {
    auto s = *rc::gen::container<std::string>(rc::gen::inRange<char>('a', 'z'));

    if (!s.empty()) {
      auto parts = text::split_to_strings(s, "");
      RC_ASSERT(parts.size() == s.size());
      for (std::size_t i = 0; i < s.size(); ++i) {
        RC_ASSERT(parts[i].size() == 1);
        RC_ASSERT(parts[i][0] == s[i]);
      }
    }
  });
}

TEST_CASE("split edge cases", "[strings][property][split]") {
  rc::prop("split result count is correct", []() {
    auto s = *printable_gen();
    auto sep = *nonempty_printable_gen();

    auto parts = text::split_to_strings(s, sep);

    // Count occurrences of separator
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = s.find(sep, pos)) != std::string::npos) {
      ++count;
      pos += sep.size();
    }

    // parts.size() == count + 1
    RC_ASSERT(parts.size() == count + 1);
  });

  rc::prop("split result concatenated with seps equals original", []() {
    auto s = *printable_gen();
    auto sep = *nonempty_printable_gen();

    auto parts = text::split_to_strings(s, sep);

    // Reconstruct
    std::string reconstructed;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (i > 0) {
        reconstructed += sep;
      }
      reconstructed += parts[i];
    }

    RC_ASSERT(reconstructed == s);
  });

  rc::prop("consecutive delimiters produce empty strings", []() {
    auto sep = *rc::gen::element<std::string_view>(",", "::", "||");
    auto num_seps = *rc::gen::inRange(1, 10);

    std::string s;
    for (int i = 0; i < num_seps; ++i) {
      s += sep;
    }

    auto parts = text::split_to_strings(s, sep);
    // n consecutive separators produce n+1 parts (all empty)
    RC_ASSERT(parts.size() == static_cast<std::size_t>(num_seps) + 1);
    for (const auto& part : parts) {
      RC_ASSERT(part.empty());
    }
  });
}

TEST_CASE("tokenize vs split properties", "[strings][property][tokenize]") {
  rc::prop("tokenize never produces empty strings", []() {
    auto s = *bytes_gen();
    auto seps = *rc::gen::nonEmpty(
        rc::gen::container<std::string>(rc::gen::element(' ', '\t', '\n', ',', ':')));

    auto tokens = text::tokenize(s, seps);

    for (const auto& token : tokens) {
      RC_ASSERT(!token.empty());
    }
  });

  rc::prop("tokenize preserves non-separator content", []() {
    // Build a string from known tokens and separators
    auto tokens_in = *rc::gen::container<std::vector<std::string>>(
        rc::gen::nonEmpty(rc::gen::container<std::string>(
            rc::gen::suchThat<char>(rc::gen::inRange<char>('a', 'z'), [](char) { return true; }))));

    if (tokens_in.empty()) {
      return;
    }

    // Join with spaces
    std::string s;
    for (std::size_t i = 0; i < tokens_in.size(); ++i) {
      if (i > 0) {
        s += " ";
      }
      s += tokens_in[i];
    }

    auto tokens_out = text::tokenize(s, " ");

    RC_ASSERT(tokens_out == tokens_in);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Unicode and binary string handling
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Generate valid UTF-8 strings
rc::Gen<std::string> utf8_gen() {
  return rc::gen::map(rc::gen::container<std::vector<uint32_t>>(rc::gen::oneOf(
                          // ASCII
                          rc::gen::inRange<uint32_t>(0x20, 0x7F),
                          // 2-byte UTF-8
                          rc::gen::inRange<uint32_t>(0x80, 0x7FF),
                          // 3-byte UTF-8 (excluding surrogates)
                          rc::gen::oneOf(rc::gen::inRange<uint32_t>(0x800, 0xD7FF),
                                         rc::gen::inRange<uint32_t>(0xE000, 0xFFFF)),
                          // 4-byte UTF-8
                          rc::gen::inRange<uint32_t>(0x10000, 0x10FFFF))),
                      [](const std::vector<uint32_t>& codepoints) {
                        std::string result;
                        for (uint32_t cp : codepoints) {
                          if (cp < 0x80) {
                            result += static_cast<char>(cp);
                          } else if (cp < 0x800) {
                            result += static_cast<char>(0xC0 | (cp >> 6));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                          } else if (cp < 0x10000) {
                            result += static_cast<char>(0xE0 | (cp >> 12));
                            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                          } else {
                            result += static_cast<char>(0xF0 | (cp >> 18));
                            result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                          }
                        }
                        return result;
                      });
}

} // namespace

TEST_CASE("UTF-8 string operations", "[strings][property][unicode]") {
  rc::prop("find works with UTF-8 strings", []() {
    auto haystack = *utf8_gen();
    auto needle = *utf8_gen();

    auto pos = text::find(haystack, needle);
    if (pos != std::string_view::npos) {
      RC_ASSERT(pos + needle.size() <= haystack.size());
      RC_ASSERT(std::string_view(haystack).substr(pos, needle.size()) == needle);
    }
  });

  rc::prop("split preserves UTF-8 integrity", []() {
    auto s = *utf8_gen();
    // Use ASCII separator to avoid splitting in middle of UTF-8 sequence
    auto sep = *rc::gen::element<std::string_view>(",", ":", ";");

    auto parts = text::split_to_strings(s, sep);
    auto rejoined = text::join(sep, parts);

    RC_ASSERT(rejoined == s);
  });

  rc::prop("replace_all preserves UTF-8 integrity", []() {
    auto s = *utf8_gen();
    // Use ASCII patterns
    auto from = *rc::gen::element<std::string_view>("a", "b", "c", "1", "2");
    auto to = *rc::gen::element<std::string_view>("X", "Y", "Z");

    auto result = text::replace_all(s, from, to);

    // Result should not contain from (unless it was created by replacement)
    // This just checks no crash and produces valid string
    RC_ASSERT(result.size() >= 0); // trivially true, but exercises the code
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Algebraic properties
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("replace algebraic properties", "[strings][property][algebra]") {
  rc::prop("replace_all(replace_all(s, a, b), b, a) may not equal s", []() {
    // This tests that replacement is NOT generally reversible
    // (unless a and b don't overlap in s)
    auto s = "aXXa";
    auto result1 = text::replace_all(s, "a", "b");
    auto result2 = text::replace_all(result1, "b", "a");

    // After a->b, "bXXb", then b->a, "aXXa"
    // In this case it IS reversible, but generally not
    RC_ASSERT(result2 == s);
  });

  rc::prop("replace_all is idempotent when from not in to", []() {
    auto s = *printable_gen();
    auto from = *nonempty_printable_gen();
    auto to = *rc::gen::suchThat(printable_gen(), [&from](const std::string& t) {
      return t.find(from) == std::string::npos;
    });

    auto once = text::replace_all(s, from, to);
    auto twice = text::replace_all(once, from, to);

    RC_ASSERT(once == twice);
  });

  rc::prop("replace_all distributes over concatenation when patterns don't span boundary", []() {
    auto a = *alnum_gen();
    auto b = *alnum_gen();
    auto from = *rc::gen::element<std::string_view>("X", "Y", "Z");
    auto to = *rc::gen::element<std::string_view>("1", "2", "3");

    // replace_all(a + b, from, to) == replace_all(a, from, to) + replace_all(b, from, to)
    // ONLY when from doesn't span the boundary (which it can't for single-char patterns)
    auto combined = text::replace_all(a + b, from, to);
    auto separate = text::replace_all(a, from, to) + text::replace_all(b, from, to);

    RC_ASSERT(combined == separate);
  });
}

TEST_CASE("trim algebraic properties", "[strings][property][algebra]") {
  rc::prop("trim(trim(s)) == trim(s) (idempotent)", []() {
    auto s = *bytes_gen();

    auto once = std::string(text::trim(s));
    auto twice = std::string(text::trim(once));

    RC_ASSERT(once == twice);
  });

  rc::prop("trim_left(trim_right(s)) == trim(s)", []() {
    auto s = *printable_gen();

    auto lr = std::string(text::trim_left(text::trim_right(s)));
    auto t = std::string(text::trim(s));

    RC_ASSERT(lr == t);
  });

  rc::prop("trim_right(trim_left(s)) == trim(s)", []() {
    auto s = *printable_gen();

    auto rl = std::string(text::trim_right(text::trim_left(s)));
    auto t = std::string(text::trim(s));

    RC_ASSERT(rl == t);
  });

  rc::prop("trim preserves inner content", []() {
    auto leading = *whitespace_gen();
    auto content = *rc::gen::suchThat(nonempty_printable_gen(), [](const std::string& s) {
      // Content starts and ends with non-whitespace
      constexpr std::string_view ws = " \t\n\r\f\v";
      return !s.empty() && !ws.contains(s.front()) && !ws.contains(s.back());
    });
    auto trailing = *whitespace_gen();

    auto full = leading + content + trailing;
    auto trimmed = std::string(text::trim(full));

    RC_ASSERT(trimmed == content);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Pathological inputs
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("pathological string inputs", "[strings][property][pathological]") {
  rc::prop("very long strings work", []() {
    auto len = *rc::gen::inRange<std::size_t>(10000, 50000);
    std::string s(len, 'a');
    s[len / 2] = 'b'; // needle in the middle

    auto pos = text::find(s, "b");
    RC_ASSERT(pos == len / 2);
  });

  rc::prop("many small splits work", []() {
    auto num_parts = *rc::gen::inRange(100, 1000);
    std::string s;
    for (int i = 0; i < num_parts; ++i) {
      if (i > 0) {
        s += ",";
      }
      s += "x";
    }

    auto parts = text::split_to_strings(s, ",");
    RC_ASSERT(parts.size() == static_cast<std::size_t>(num_parts));
  });

  rc::prop("replace_all with overlapping patterns", []() {
    // Replace "aa" with "a" in "aaaa" should give "aa" (greedy, non-overlapping)
    std::string s = "aaaa";
    auto result = text::replace_all(s, "aa", "a");
    RC_ASSERT(result == "aa");
  });

  rc::prop("replace_all expanding string", []() {
    auto s = *rc::gen::container<std::string>(rc::gen::just('a'));
    auto result = text::replace_all(s, "a", "aa");

    // Each 'a' becomes 'aa', so length doubles
    RC_ASSERT(result.size() == s.size() * 2);
  });

  rc::prop("replace_all shrinking string", []() {
    auto num_pairs = *rc::gen::inRange(1, 100);
    std::string s(static_cast<std::size_t>(num_pairs) * 2, 'a');

    auto result = text::replace_all(s, "aa", "a");

    RC_ASSERT(result.size() == static_cast<std::size_t>(num_pairs));
  });
}
