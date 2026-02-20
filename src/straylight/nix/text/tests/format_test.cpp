// straylight::nix::text::format tests
//
// Tests for format primitives: std::format wrapper and boost::format compatibility

// IMPORTANT: Catch2 v3 MUST be included BEFORE rapidcheck/catch.h
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "straylight/nix/text/format.h"

namespace fmt = straylight::nix::text;

// ─────────────────────────────────────────────────────────────────────────────
// Modern format() tests (std::format syntax)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("format() with no arguments returns string unchanged", "[format]") {
  REQUIRE(fmt::format("hello") == "hello");
  REQUIRE(fmt::format(std::string("world")) == "world");
  REQUIRE(fmt::format(std::string_view("test")) == "test");
}

TEST_CASE("format() with positional arguments", "[format]") {
  REQUIRE(fmt::format("{} {}", "hello", "world") == "hello world");
  REQUIRE(fmt::format("{0} {1}", "a", "b") == "a b");
  REQUIRE(fmt::format("{1} {0}", "a", "b") == "b a");
  REQUIRE(fmt::format("{0} {0}", "x") == "x x");
}

TEST_CASE("format() with numeric types", "[format]") {
  REQUIRE(fmt::format("{}", 42) == "42");
  REQUIRE(fmt::format("{}", -17) == "-17");
  REQUIRE(fmt::format("{}", 3.14159) == "3.14159");
  REQUIRE(fmt::format("{:d}", 255) == "255");
  REQUIRE(fmt::format("{:x}", 255) == "ff");
  REQUIRE(fmt::format("{:X}", 255) == "FF");
  REQUIRE(fmt::format("{:o}", 8) == "10");
  REQUIRE(fmt::format("{:b}", 5) == "101");
}

TEST_CASE("format() with width and alignment", "[format]") {
  REQUIRE(fmt::format("{:5}", 42) == "   42");
  REQUIRE(fmt::format("{:<5}", 42) == "42   ");
  REQUIRE(fmt::format("{:>5}", 42) == "   42");
  REQUIRE(fmt::format("{:^5}", 42) == " 42  ");
  REQUIRE(fmt::format("{:05}", 42) == "00042");
}

TEST_CASE("format() with strings", "[format]") {
  REQUIRE(fmt::format("{}", "test") == "test");
  REQUIRE(fmt::format("{:10}", "test") == "test      ");
  REQUIRE(fmt::format("{:>10}", "test") == "      test");
}

// ─────────────────────────────────────────────────────────────────────────────
// Legacy fmt() tests (boost::format compatibility)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("fmt() with no arguments returns string unchanged", "[fmt][legacy]") {
  REQUIRE(fmt::fmt("hello") == "hello");
  REQUIRE(fmt::fmt("no placeholders here") == "no placeholders here");
}

TEST_CASE("fmt() with printf-style %s", "[fmt][legacy]") {
  REQUIRE(fmt::fmt("%s", "hello") == "hello");
  REQUIRE(fmt::fmt("%s %s", "hello", "world") == "hello world");
  REQUIRE(fmt::fmt("prefix %s suffix", "middle") == "prefix middle suffix");
}

TEST_CASE("fmt() with printf-style %d", "[fmt][legacy]") {
  REQUIRE(fmt::fmt("%d", 42) == "42");
  REQUIRE(fmt::fmt("%d + %d = %d", 1, 2, 3) == "1 + 2 = 3");
}

TEST_CASE("fmt() with boost-style positional %N%", "[fmt][legacy]") {
  REQUIRE(fmt::fmt("%1%", "hello") == "hello");
  REQUIRE(fmt::fmt("%1% %2%", "hello", "world") == "hello world");
  REQUIRE(fmt::fmt("%2% %1%", "a", "b") == "b a");
  REQUIRE(fmt::fmt("%1% + %1% = %2%", 2, 4) == "2 + 2 = 4");
}

TEST_CASE("fmt() escapes literal %", "[fmt][legacy]") {
  REQUIRE(fmt::fmt("100%%") == "100%");
  REQUIRE(fmt::fmt("%s is 100%%", "success") == "success is 100%");
}

TEST_CASE("fmt() handles curly braces in format string", "[fmt][legacy]") {
  // Curly braces should be escaped for std::format
  REQUIRE(fmt::fmt("{literal}") == "{literal}");
  REQUIRE(fmt::fmt("%s {in braces}", "value") == "value {in braces}");
}

TEST_CASE("fmt() with mixed content", "[fmt][legacy]") {
  REQUIRE(fmt::fmt("path: %s, line: %d", "/nix/store/abc", 42) == "path: /nix/store/abc, line: 42");
}

// ─────────────────────────────────────────────────────────────────────────────
// Color wrapper tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Magenta wrapper adds ANSI codes", "[format][color]") {
  auto result = fmt::format("{}", fmt::Magenta("test"));
  REQUIRE(result.find("\033[35;1m") != std::string::npos); // Magenta start
  REQUIRE(result.find("test") != std::string::npos);
  REQUIRE(result.find("\033[0m") != std::string::npos); // Reset
}

TEST_CASE("Uncolored wrapper adds reset code", "[format][color]") {
  auto result = fmt::format("{}", fmt::Uncolored("test"));
  REQUIRE(result.find("\033[0m") != std::string::npos);
  REQUIRE(result.find("test") != std::string::npos);
}

TEST_CASE("Colored wrapper with custom color", "[format][color]") {
  auto result = fmt::format("{}", fmt::Colored("test", fmt::kAnsiRed));
  REQUIRE(result.find("\033[31;1m") != std::string::npos); // Red start
  REQUIRE(result.find("test") != std::string::npos);
  REQUIRE(result.find("\033[0m") != std::string::npos); // Reset
}

TEST_CASE("Magenta with numeric value", "[format][color]") {
  auto result = fmt::format("{}", fmt::Magenta(42));
  REQUIRE(result.find("42") != std::string::npos);
  REQUIRE(result.find("\033[35;1m") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Hint class tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Hint from literal", "[format][hint]") {
  fmt::Hint h("simple message");
  REQUIRE(h.str() == "simple message");
}

TEST_CASE("Hint with formatting", "[format][hint]") {
  fmt::Hint h("{} is {}", "answer", 42);
  REQUIRE(h.str() == "answer is 42");
}

TEST_CASE("Hint with Magenta arguments", "[format][hint]") {
  fmt::Hint h("expected {}, got {}", fmt::Magenta("foo"), fmt::Magenta("bar"));
  REQUIRE(h.str().find("foo") != std::string::npos);
  REQUIRE(h.str().find("bar") != std::string::npos);
  REQUIRE(h.str().find("\033[35;1m") != std::string::npos);
}

TEST_CASE("hint() convenience function", "[format][hint]") {
  auto h = fmt::hint("{} + {} = {}", 1, 2, 3);
  REQUIRE(h.str() == "1 + 2 = 3");
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("format() with empty string", "[format]") {
  REQUIRE(fmt::format("") == "");
  REQUIRE(fmt::format("", 1, 2, 3) == ""); // Extra args ignored? Or error?
}

TEST_CASE("fmt() empty format string", "[fmt][legacy]") {
  REQUIRE(fmt::fmt("") == "");
}

TEST_CASE("format() with special characters", "[format]") {
  REQUIRE(fmt::format("tab:\there") == "tab:\there");
  REQUIRE(fmt::format("newline:\nhere") == "newline:\nhere");
  REQUIRE(fmt::format("quote: \"test\"") == "quote: \"test\"");
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-based tests
// ─────────────────────────────────────────────────────────────────────────────

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

TEST_CASE("format() round-trips strings without placeholders", "[format][property]") {
  rc::prop("format(s) == s for strings without braces", []() {
    // Generate strings without { or } to avoid format issues
    auto s = *rc::gen::container<std::string>(
        rc::gen::oneOf(rc::gen::inRange('a', 'z'), rc::gen::inRange('0', '9'), rc::gen::just(' ')));
    RC_ASSERT(fmt::format(s) == s);
  });
}

TEST_CASE("fmt() with %s produces same as format with {}", "[format][property]") {
  rc::prop("fmt('%s', x) == format('{}', x) for simple strings", []() {
    auto s = *rc::gen::container<std::string>(rc::gen::inRange('a', 'z'));
    if (!s.empty()) {
      RC_ASSERT(fmt::fmt("%s", s) == fmt::format("{}", s));
    }
  });
}

TEST_CASE("fmt() with %d produces same as format with {}", "[format][property]") {
  rc::prop("fmt('%d', n) == format('{}', n) for integers", []() {
    auto n = *rc::gen::inRange(-10000, 10000);
    RC_ASSERT(fmt::fmt("%d", n) == fmt::format("{}", n));
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Heavy metal format string parsing property tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("format string edge cases", "[format][property][edge]") {
  rc::prop("empty format string produces empty result", []() { RC_ASSERT(fmt::format("") == ""); });

  rc::prop("format without placeholders returns input unchanged", []() {
    // Generate strings without { } % to avoid format issues
    auto s = *rc::gen::container<std::string>(rc::gen::suchThat<char>(
        rc::gen::inRange<char>(32, 126), [](char c) { return c != '{' && c != '}' && c != '%'; }));
    RC_ASSERT(fmt::format(s) == s);
  });

  rc::prop("escaped braces produce literal braces", []() {
    auto n = *rc::gen::inRange(1, 10);
    std::string input;
    std::string expected;
    for (int i = 0; i < n; ++i) {
      input += "{{}}";
      expected += "{}";
    }
    // Need an argument to trigger format parsing; use vformat for runtime strings
    RC_ASSERT(std::vformat(input, std::make_format_args()) == expected);
  });

  rc::prop("fmt %% produces single %", []() {
    auto n = *rc::gen::inRange(1, 10);
    std::string input;
    std::string expected;
    for (int i = 0; i < n; ++i) {
      input += "%%";
      expected += "%";
    }
    RC_ASSERT(fmt::fmt(input) == expected);
  });
}

TEST_CASE("format positional arguments", "[format][property][positional]") {
  rc::prop("positional format {0} {1} works like sequential", []() {
    auto a = *rc::gen::inRange(0, 1000);
    auto b = *rc::gen::inRange(0, 1000);

    RC_ASSERT(fmt::format("{0} {1}", a, b) == fmt::format("{} {}", a, b));
  });

  rc::prop("repeated positional argument produces repeated output", []() {
    auto n = *rc::gen::inRange(0, 1000);
    auto result = fmt::format("{0} {0} {0}", n);
    auto expected = fmt::format("{} {} {}", n, n, n);
    RC_ASSERT(result == expected);
  });

  rc::prop("boost positional %1% %2% matches std format", []() {
    auto a = *rc::gen::container<std::string>(rc::gen::inRange<char>('a', 'z'));
    auto b = *rc::gen::container<std::string>(rc::gen::inRange<char>('a', 'z'));

    if (!a.empty() && !b.empty()) {
      auto boost_result = fmt::fmt("%1% %2%", a, b);
      auto std_result = fmt::format("{} {}", a, b);
      RC_ASSERT(boost_result == std_result);
    }
  });

  rc::prop("boost reversed positional %2% %1% swaps arguments", []() {
    auto a = *rc::gen::inRange(0, 1000);
    auto b = *rc::gen::inRange(0, 1000);

    RC_PRE(a != b); // Ensure distinct values to verify swap

    auto result = fmt::fmt("%2% %1%", a, b);
    auto expected = fmt::format("{} {}", b, a);
    RC_ASSERT(result == expected);
  });
}

TEST_CASE("format numeric types", "[format][property][numeric]") {
  rc::prop("format preserves integer value", []() {
    auto n = *rc::gen::arbitrary<int>();
    auto formatted = fmt::format("{}", n);
    auto parsed = std::stoi(formatted);
    RC_ASSERT(parsed == n);
  });

  rc::prop("hex format produces valid hex", []() {
    auto n = *rc::gen::inRange(0, 65535);
    auto hex = fmt::format("{:x}", n);

    // Parse back
    unsigned int parsed = 0;
    auto [ptr, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), parsed, 16);
    RC_ASSERT(ec == std::errc{});
    RC_ASSERT(parsed == static_cast<unsigned int>(n));
  });

  rc::prop("octal format produces valid octal", []() {
    auto n = *rc::gen::inRange(0, 4095);
    auto oct = fmt::format("{:o}", n);

    // Parse back
    unsigned int parsed = 0;
    auto [ptr, ec] = std::from_chars(oct.data(), oct.data() + oct.size(), parsed, 8);
    RC_ASSERT(ec == std::errc{});
    RC_ASSERT(parsed == static_cast<unsigned int>(n));
  });

  rc::prop("binary format produces valid binary", []() {
    auto n = *rc::gen::inRange(0, 255);
    auto bin = fmt::format("{:b}", n);

    // Parse back
    unsigned int parsed = 0;
    auto [ptr, ec] = std::from_chars(bin.data(), bin.data() + bin.size(), parsed, 2);
    RC_ASSERT(ec == std::errc{});
    RC_ASSERT(parsed == static_cast<unsigned int>(n));
  });
}

TEST_CASE("format width and alignment", "[format][property][width]") {
  rc::prop("right-aligned width pads on left", []() {
    auto n = *rc::gen::inRange(0, 99);
    auto width = *rc::gen::inRange(5, 10);

    std::string fmt_str = "{:>" + std::to_string(width) + "}";
    // Use vformat for runtime format strings
    auto result = std::vformat(fmt_str, std::make_format_args(n));

    RC_ASSERT(result.size() == static_cast<std::size_t>(width));
    RC_ASSERT(result.back() != ' ');                        // Number at right
    RC_ASSERT(result.front() == ' ' || result.size() == 2); // Padding on left (or exact fit)
  });

  rc::prop("left-aligned width pads on right", []() {
    auto n = *rc::gen::inRange(0, 99);
    auto width = *rc::gen::inRange(5, 10);

    std::string fmt_str = "{:<" + std::to_string(width) + "}";
    auto result = std::vformat(fmt_str, std::make_format_args(n));

    RC_ASSERT(result.size() == static_cast<std::size_t>(width));
    RC_ASSERT(result.front() != ' '); // Number at left
    RC_ASSERT(result.back() == ' ');  // Padding on right
  });

  rc::prop("zero-padded width pads with zeros", []() {
    auto n = *rc::gen::inRange(0, 99);
    auto width = *rc::gen::inRange(5, 10);

    std::string fmt_str = "{:0" + std::to_string(width) + "}";
    auto result = std::vformat(fmt_str, std::make_format_args(n));

    RC_ASSERT(result.size() == static_cast<std::size_t>(width));
    // All chars should be digits
    for (char c : result) {
      RC_ASSERT(c >= '0' && c <= '9');
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Unicode in format strings
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("format with unicode", "[format][property][unicode]") {
  rc::prop("format preserves UTF-8 content", []() {
    // Some known UTF-8 strings (using regular string literals - source is UTF-8)
    std::vector<std::string> samples = {
        "hello",
        "h\xc3\xa9llo",                                     // héllo - Latin extended
        "\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82", // привет - Cyrillic
        "\xe4\xbd\xa0\xe5\xa5\xbd",                         // 你好 - Chinese
        "\xf0\x9f\x8e\x89",                                 // 🎉 - Emoji
    };

    for (const auto& s : samples) {
      RC_ASSERT(fmt::format("{}", s) == s);
    }
  });

  rc::prop("format with unicode argument and ASCII format string", []() {
    std::string unicode =
        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88"; // 日本語テスト
    auto result = fmt::format("prefix {} suffix", unicode);
    RC_ASSERT(result.find(unicode) != std::string::npos);
    RC_ASSERT(result.find("prefix") == 0);
    RC_ASSERT(result.find("suffix") == result.size() - 6);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Color wrapper property tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("color wrapper properties", "[format][property][color]") {
  rc::prop("Magenta wrapper always adds ANSI codes", []() {
    auto s = *rc::gen::container<std::string>(rc::gen::inRange<char>('a', 'z'));

    auto result = fmt::format("{}", fmt::Magenta(s));

    RC_ASSERT(result.find("\033[35;1m") != std::string::npos);
    RC_ASSERT(result.find("\033[0m") != std::string::npos);
    RC_ASSERT(result.find(s) != std::string::npos);
  });

  rc::prop("Magenta wrapping integers preserves value", []() {
    auto n = *rc::gen::arbitrary<int>();

    auto result = fmt::format("{}", fmt::Magenta(n));
    auto plain = fmt::format("{}", n);

    // Result should contain the plain number
    RC_ASSERT(result.find(plain) != std::string::npos);
  });

  rc::prop("Colored with different colors produces different output", []() {
    auto s = *rc::gen::container<std::string>(rc::gen::inRange<char>('a', 'z'));
    RC_PRE(!s.empty());

    auto red = fmt::format("{}", fmt::Colored(s, fmt::kAnsiRed));
    auto green = fmt::format("{}", fmt::Colored(s, fmt::kAnsiGreen));
    auto blue = fmt::format("{}", fmt::Colored(s, fmt::kAnsiBlue));

    RC_ASSERT(red != green);
    RC_ASSERT(green != blue);
    RC_ASSERT(red != blue);

    // But all contain the original string
    RC_ASSERT(red.find(s) != std::string::npos);
    RC_ASSERT(green.find(s) != std::string::npos);
    RC_ASSERT(blue.find(s) != std::string::npos);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Hint class property tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Hint properties", "[format][property][hint]") {
  rc::prop("Hint preserves formatted content", []() {
    auto a = *rc::gen::inRange(0, 1000);
    auto b = *rc::gen::inRange(0, 1000);

    fmt::Hint h("{} + {} = {}", a, b, a + b);

    auto expected = fmt::format("{} + {} = {}", a, b, a + b);
    RC_ASSERT(h.str() == expected);
  });

  rc::prop("Hint literal constructor", []() {
    auto s = *rc::gen::container<std::string>(rc::gen::suchThat<char>(
        rc::gen::inRange<char>(32, 126), [](char c) { return c != '{' && c != '}'; }));

    fmt::Hint h(s);
    RC_ASSERT(h.str() == s);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Fuzz-like tests for format string parsing
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("format string fuzzing", "[format][fuzz]") {
  rc::prop("convert_boost_format never crashes on arbitrary input", []() {
    auto s = *rc::gen::container<std::string>(rc::gen::arbitrary<char>());

    // Should not crash
    [[maybe_unused]] auto result = fmt::detail::convert_boost_format(s);
  });

  rc::prop("format with no args and no placeholders is identity", []() {
    // Generate safe strings (no format specifiers)
    auto s = *rc::gen::container<std::string>(rc::gen::suchThat<char>(
        rc::gen::inRange<char>(32, 126), [](char c) { return c != '{' && c != '}' && c != '%'; }));

    RC_ASSERT(fmt::format(s) == s);
  });
}
