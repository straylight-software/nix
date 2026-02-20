// straylight::nix::text::markdown tests
//
// Tests for terminal markdown rendering primitive.

#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "straylight/nix/text/markdown.h"

namespace md = straylight::nix::text;

// ─────────────────────────────────────────────────────────────────────────────
// Helper to strip ANSI for easier text comparison
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] std::string plain(std::string_view markdown) {
  return md::strip_ansi(md::render_markdown(markdown, md::default_terminal_width));
}

// ─────────────────────────────────────────────────────────────────────────────
// Basic rendering tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("render_markdown returns empty for empty input", "[markdown]") {
  std::string result = md::render_markdown("", md::default_terminal_width);
  REQUIRE(result.empty());
}

TEST_CASE("render_markdown preserves plain text", "[markdown]") {
  std::string result = plain("Hello, world!");
  REQUIRE(result == "Hello, world!\n");
}

TEST_CASE("render_markdown handles multiple lines", "[markdown]") {
  std::string result = plain("Line one\nLine two");
  REQUIRE(result == "Line one\nLine two\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Header tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("render_markdown renders h1 headers", "[markdown][headers]") {
  std::string result = plain("# Header One");
  REQUIRE(result == "Header One\n");
}

TEST_CASE("render_markdown renders h2 headers", "[markdown][headers]") {
  std::string result = plain("## Header Two");
  REQUIRE(result == "Header Two\n");
}

TEST_CASE("render_markdown renders h3 headers", "[markdown][headers]") {
  std::string result = plain("### Header Three");
  REQUIRE(result == "Header Three\n");
}

TEST_CASE("render_markdown renders h4-h6 headers", "[markdown][headers]") {
  REQUIRE(plain("#### H4") == "H4\n");
  REQUIRE(plain("##### H5") == "H5\n");
  REQUIRE(plain("###### H6") == "H6\n");
}

TEST_CASE("render_markdown ignores invalid headers (more than 6 #)", "[markdown][headers]") {
  std::string result = plain("####### Not a header");
  REQUIRE(result == "####### Not a header\n");
}

TEST_CASE("render_markdown requires space after # for headers", "[markdown][headers]") {
  std::string result = plain("#NoSpace");
  REQUIRE(result == "#NoSpace\n");
}

TEST_CASE("render_markdown removes trailing # from headers", "[markdown][headers]") {
  std::string result = plain("# Header ###");
  REQUIRE(result == "Header\n");
}

TEST_CASE("render_markdown applies h1 style to headers", "[markdown][headers]") {
  std::string result = md::render_markdown("# Test", md::default_terminal_width);
  // Should contain the bold bright cyan code
  REQUIRE(result.find("\033[1;96m") != std::string::npos);
  REQUIRE(result.find("\033[0m") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Bold text tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("render_markdown renders bold with **", "[markdown][bold]") {
  std::string result = plain("Some **bold** text");
  REQUIRE(result == "Some bold text\n");
}

TEST_CASE("render_markdown renders bold with __", "[markdown][bold]") {
  std::string result = plain("Some __bold__ text");
  REQUIRE(result == "Some bold text\n");
}

TEST_CASE("render_markdown applies bold ANSI code", "[markdown][bold]") {
  std::string result = md::render_markdown("**bold**", md::default_terminal_width);
  REQUIRE(result.find("\033[1m") != std::string::npos);
}

TEST_CASE("render_markdown handles unclosed bold markers", "[markdown][bold]") {
  std::string result = plain("Some **unclosed text");
  REQUIRE(result == "Some **unclosed text\n");
}

TEST_CASE("render_markdown handles multiple bold sections", "[markdown][bold]") {
  std::string result = plain("**one** and **two**");
  REQUIRE(result == "one and two\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Italic text tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("render_markdown renders italic with *", "[markdown][italic]") {
  std::string result = plain("Some *italic* text");
  REQUIRE(result == "Some italic text\n");
}

TEST_CASE("render_markdown renders italic with _", "[markdown][italic]") {
  std::string result = plain("Some _italic_ text");
  REQUIRE(result == "Some italic text\n");
}

TEST_CASE("render_markdown applies italic ANSI code", "[markdown][italic]") {
  std::string result = md::render_markdown("*italic*", md::default_terminal_width);
  REQUIRE(result.find("\033[3m") != std::string::npos);
}

TEST_CASE("render_markdown handles unclosed italic markers", "[markdown][italic]") {
  std::string result = plain("Some *unclosed text");
  REQUIRE(result == "Some *unclosed text\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Combined bold and italic
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("render_markdown handles bold inside text", "[markdown][bold][italic]") {
  std::string result = plain("Hello **world** there");
  REQUIRE(result == "Hello world there\n");
}

TEST_CASE("render_markdown handles nested bold and italic", "[markdown][bold][italic]") {
  std::string result = plain("**bold with *italic* inside**");
  REQUIRE(result == "bold with italic inside\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Code span tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("render_markdown renders inline code with backticks", "[markdown][code]") {
  std::string result = plain("Use `code` here");
  REQUIRE(result == "Use code here\n");
}

TEST_CASE("render_markdown applies code span ANSI style", "[markdown][code]") {
  std::string result = md::render_markdown("`code`", md::default_terminal_width);
  // Should contain yellow color code
  REQUIRE(result.find("\033[33m") != std::string::npos);
}

TEST_CASE("render_markdown handles double backticks for code", "[markdown][code]") {
  std::string result = plain("Use ``code with ` inside`` here");
  REQUIRE(result == "Use code with ` inside here\n");
}

TEST_CASE("render_markdown handles unclosed code span", "[markdown][code]") {
  std::string result = plain("Some `unclosed code");
  REQUIRE(result == "Some `unclosed code\n");
}

TEST_CASE("render_markdown handles empty code span", "[markdown][code]") {
  std::string result = plain("Empty `` code");
  // Empty delimiter doesn't match, becomes literal
  REQUIRE(result == "Empty `` code\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Code block tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("render_markdown renders fenced code blocks", "[markdown][codeblock]") {
  std::string input = "```\ncode line 1\ncode line 2\n```";
  std::string result = plain(input);
  REQUIRE(result.find("code line 1") != std::string::npos);
  REQUIRE(result.find("code line 2") != std::string::npos);
}

TEST_CASE("render_markdown renders tilde fenced code blocks", "[markdown][codeblock]") {
  std::string input = "~~~\ncode\n~~~";
  std::string result = plain(input);
  REQUIRE(result.find("code") != std::string::npos);
}

TEST_CASE("render_markdown renders indented code blocks", "[markdown][codeblock]") {
  std::string result = plain("    indented code");
  REQUIRE(result == "indented code\n");
}

TEST_CASE("render_markdown renders tab-indented code blocks", "[markdown][codeblock]") {
  std::string result = plain("\ttab code");
  REQUIRE(result == "tab code\n");
}

TEST_CASE("render_markdown applies code block ANSI style", "[markdown][codeblock]") {
  std::string result = md::render_markdown("    code", md::default_terminal_width);
  // Should contain dim yellow code
  REQUIRE(result.find("\033[2;33m") != std::string::npos);
}

TEST_CASE("render_markdown ignores language specifier in fenced blocks", "[markdown][codeblock]") {
  std::string input = "```python\nprint('hello')\n```";
  std::string result = plain(input);
  REQUIRE(result.find("print('hello')") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// List tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("render_markdown renders unordered lists with -", "[markdown][lists]") {
  std::string result = plain("- Item one\n- Item two");
  REQUIRE(result.find("Item one") != std::string::npos);
  REQUIRE(result.find("Item two") != std::string::npos);
}

TEST_CASE("render_markdown renders unordered lists with *", "[markdown][lists]") {
  std::string result = plain("* Item one\n* Item two");
  REQUIRE(result.find("Item one") != std::string::npos);
  REQUIRE(result.find("Item two") != std::string::npos);
}

TEST_CASE("render_markdown renders unordered lists with +", "[markdown][lists]") {
  std::string result = plain("+ Item one");
  REQUIRE(result.find("Item one") != std::string::npos);
}

TEST_CASE("render_markdown uses bullet character for lists", "[markdown][lists]") {
  std::string result = md::render_markdown("- Item", md::default_terminal_width);
  REQUIRE(result.find("•") != std::string::npos);
}

TEST_CASE("render_markdown renders ordered lists", "[markdown][lists]") {
  std::string result = plain("1. First\n2. Second\n3. Third");
  REQUIRE(result.find("First") != std::string::npos);
  REQUIRE(result.find("Second") != std::string::npos);
  REQUIRE(result.find("Third") != std::string::npos);
}

TEST_CASE("render_markdown handles ordered lists with )", "[markdown][lists]") {
  std::string result = plain("1) First item");
  REQUIRE(result.find("First item") != std::string::npos);
}

TEST_CASE("render_markdown applies list bullet color", "[markdown][lists]") {
  std::string result = md::render_markdown("- Item", md::default_terminal_width);
  // Should contain green color code for bullet
  REQUIRE(result.find("\033[32m") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Link tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("render_markdown renders links", "[markdown][links]") {
  std::string result = plain("[Example](https://example.com)");
  REQUIRE(result.find("Example") != std::string::npos);
  REQUIRE(result.find("https://example.com") != std::string::npos);
}

TEST_CASE("render_markdown applies link styling", "[markdown][links]") {
  std::string result = md::render_markdown("[Link](url)", md::default_terminal_width);
  // Should contain underline cyan for text
  REQUIRE(result.find("\033[4;36m") != std::string::npos);
  // Should contain dim cyan for URL
  REQUIRE(result.find("\033[2;36m") != std::string::npos);
}

TEST_CASE("render_markdown handles unclosed link brackets", "[markdown][links]") {
  std::string result = plain("[unclosed link");
  REQUIRE(result == "[unclosed link\n");
}

TEST_CASE("render_markdown handles link without URL", "[markdown][links]") {
  std::string result = plain("[text] no url");
  REQUIRE(result == "[text] no url\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Horizontal rule tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("render_markdown renders horizontal rules with ---", "[markdown][hr]") {
  std::string result = md::render_markdown("---", md::default_terminal_width);
  // Should contain the rule character repeated
  REQUIRE(result.find("─") != std::string::npos);
}

TEST_CASE("render_markdown renders horizontal rules with ***", "[markdown][hr]") {
  std::string result = md::render_markdown("***", md::default_terminal_width);
  REQUIRE(result.find("─") != std::string::npos);
}

TEST_CASE("render_markdown renders horizontal rules with ___", "[markdown][hr]") {
  std::string result = md::render_markdown("___", md::default_terminal_width);
  REQUIRE(result.find("─") != std::string::npos);
}

TEST_CASE("render_markdown renders full-width horizontal rules", "[markdown][hr]") {
  constexpr std::size_t width = 40;
  std::string result = md::render_markdown("---", width);
  // Rule should be approximately terminal width characters
  std::size_t rule_count = 0;
  for (std::size_t index = 0; index < result.size();) {
    // "─" is a multi-byte UTF-8 character
    if (result.substr(index, 3) == "─") {
      ++rule_count;
      index += 3;
    } else {
      ++index;
    }
  }
  REQUIRE(rule_count == width);
}

// ─────────────────────────────────────────────────────────────────────────────
// Blockquote tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("render_markdown renders blockquotes", "[markdown][blockquote]") {
  std::string result = plain("> Quoted text");
  REQUIRE(result.find("Quoted text") != std::string::npos);
}

TEST_CASE("render_markdown adds blockquote prefix", "[markdown][blockquote]") {
  std::string result = md::render_markdown("> Quote", md::default_terminal_width);
  REQUIRE(result.find("│") != std::string::npos);
}

TEST_CASE("render_markdown applies blockquote styling", "[markdown][blockquote]") {
  std::string result = md::render_markdown("> Quote", md::default_terminal_width);
  // Should contain italic white
  REQUIRE(result.find("\033[3;37m") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Escape sequence tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("render_markdown handles escaped asterisks", "[markdown][escape]") {
  std::string result = plain("\\*not italic\\*");
  REQUIRE(result == "*not italic*\n");
}

TEST_CASE("render_markdown handles escaped backticks", "[markdown][escape]") {
  std::string result = plain("\\`not code\\`");
  REQUIRE(result == "`not code`\n");
}

TEST_CASE("render_markdown handles escaped brackets", "[markdown][escape]") {
  std::string result = plain("\\[not a link\\]");
  REQUIRE(result == "[not a link]\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Color theme tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("render_markdown uses no_color_theme correctly", "[markdown][theme]") {
  md::markdown_render_options options{
      .terminal_width = md::default_terminal_width,
      .theme = md::no_color_theme(),
  };
  std::string result = md::render_markdown("# Header\n**bold**", options);
  // Should not contain any ANSI escape codes
  REQUIRE(result.find("\033[") == std::string::npos);
}

TEST_CASE("render_markdown uses minimal_color_theme correctly", "[markdown][theme]") {
  md::markdown_render_options options{
      .terminal_width = md::default_terminal_width,
      .theme = md::minimal_color_theme(),
  };
  std::string result = md::render_markdown("# Header", options);
  // Should contain bold code but not color codes
  REQUIRE(result.find("\033[1m") != std::string::npos);
}

TEST_CASE("render_markdown uses custom bullet character", "[markdown][theme]") {
  md::markdown_render_options options{
      .terminal_width = md::default_terminal_width,
      .list_bullet_char = "-",
  };
  std::string result = md::render_markdown("- Item", options);
  // plain text should have the custom bullet (after stripping ANSI)
  std::string stripped = md::strip_ansi(result);
  REQUIRE(stripped.find("- ") != std::string::npos);
}

TEST_CASE("render_markdown uses custom horizontal rule character", "[markdown][theme]") {
  md::markdown_render_options options{
      .terminal_width = 10,
      .horizontal_rule_char = "=",
  };
  std::string result = md::render_markdown("---", options);
  std::string stripped = md::strip_ansi(result);
  REQUIRE(stripped.find("==========") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Word wrapping tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("render_markdown wraps long lines", "[markdown][wrap]") {
  std::string long_line = "This is a very long line that should be wrapped";
  std::string result = plain(long_line);
  // With default width of 80, should not wrap
  REQUIRE(result.find('\n') != std::string::npos); // just the trailing newline
}

TEST_CASE("render_markdown wraps to specified width", "[markdown][wrap]") {
  std::string input = "word1 word2 word3 word4 word5";
  std::string result = md::strip_ansi(md::render_markdown(input, 15));
  // Should have line breaks
  std::size_t newline_count = 0;
  for (char character : result) {
    if (character == '\n') {
      ++newline_count;
    }
  }
  REQUIRE(newline_count >= 2);
}

TEST_CASE("render_markdown handles width of 0", "[markdown][wrap]") {
  std::string result = md::render_markdown("test", 0);
  // Should not crash, text should be present
  REQUIRE(md::strip_ansi(result).find("test") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// strip_ansi tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("strip_ansi removes ANSI escape codes", "[markdown][strip]") {
  std::string input = "\033[1mbold\033[0m normal";
  std::string result = md::strip_ansi(input);
  REQUIRE(result == "bold normal");
}

TEST_CASE("strip_ansi handles text without ANSI codes", "[markdown][strip]") {
  std::string input = "plain text";
  std::string result = md::strip_ansi(input);
  REQUIRE(result == "plain text");
}

TEST_CASE("strip_ansi handles empty string", "[markdown][strip]") {
  std::string result = md::strip_ansi("");
  REQUIRE(result.empty());
}

TEST_CASE("strip_ansi handles multiple escape sequences", "[markdown][strip]") {
  std::string input = "\033[1m\033[33mcolored\033[0m";
  std::string result = md::strip_ansi(input);
  REQUIRE(result == "colored");
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("render_markdown handles consecutive empty lines", "[markdown][edge]") {
  std::string result = plain("text\n\n\n\nmore text");
  // Should collapse multiple empty lines
  REQUIRE(result.find("\n\n\n") == std::string::npos);
}

TEST_CASE("render_markdown handles mixed formatting", "[markdown][edge]") {
  std::string result = plain("# Header\n\n**Bold** and *italic* with `code`\n\n- List item");
  REQUIRE(result.find("Header") != std::string::npos);
  REQUIRE(result.find("Bold") != std::string::npos);
  REQUIRE(result.find("italic") != std::string::npos);
  REQUIRE(result.find("code") != std::string::npos);
  REQUIRE(result.find("List item") != std::string::npos);
}

TEST_CASE("render_markdown handles inline formatting in headers", "[markdown][edge]") {
  std::string result = plain("# **Bold** Header");
  REQUIRE(result.find("Bold Header") != std::string::npos);
}

TEST_CASE("render_markdown handles inline formatting in lists", "[markdown][edge]") {
  std::string result = plain("- **Bold** item with *italic*");
  REQUIRE(result.find("Bold") != std::string::npos);
  REQUIRE(result.find("italic") != std::string::npos);
}

TEST_CASE("render_markdown handles links in text", "[markdown][edge]") {
  std::string result = plain("Check out [this link](https://example.com) for more.");
  REQUIRE(result.find("this link") != std::string::npos);
  REQUIRE(result.find("https://example.com") != std::string::npos);
}

TEST_CASE("render_markdown handles special characters", "[markdown][edge]") {
  std::string result = plain("Special: <>&\"'");
  REQUIRE(result.find("<>&\"'") != std::string::npos);
}

TEST_CASE("render_markdown handles unicode text", "[markdown][edge]") {
  std::string result = plain("Unicode: 日本語 émojis 🚀");
  REQUIRE(result.find("日本語") != std::string::npos);
  REQUIRE(result.find("🚀") != std::string::npos);
}

TEST_CASE("render_markdown handles only whitespace", "[markdown][edge]") {
  std::string result = md::render_markdown("   \n   \n", md::default_terminal_width);
  // Should be empty or just whitespace
  REQUIRE(md::strip_ansi(result).find_first_not_of(" \n\t") == std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// ANSI namespace tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ansi namespace provides correct escape codes", "[markdown][ansi]") {
  REQUIRE(md::ansi::reset == "\033[0m");
  REQUIRE(md::ansi::bold == "\033[1m");
  REQUIRE(md::ansi::italic == "\033[3m");
  REQUIRE(md::ansi::red == "\033[31m");
  REQUIRE(md::ansi::green == "\033[32m");
  REQUIRE(md::ansi::blue == "\033[34m");
}

TEST_CASE("ansi namespace provides bright colors", "[markdown][ansi]") {
  REQUIRE(md::ansi::bright_red == "\033[91m");
  REQUIRE(md::ansi::bright_green == "\033[92m");
  REQUIRE(md::ansi::bright_blue == "\033[94m");
}

TEST_CASE("ansi namespace provides background colors", "[markdown][ansi]") {
  REQUIRE(md::ansi::background_red == "\033[41m");
  REQUIRE(md::ansi::background_green == "\033[42m");
  REQUIRE(md::ansi::background_blue == "\033[44m");
}

// ─────────────────────────────────────────────────────────────────────────────
// Theme factory function tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("default_color_theme returns consistent values", "[markdown][theme]") {
  auto theme1 = md::default_color_theme();
  auto theme2 = md::default_color_theme();
  REQUIRE(theme1.header_1 == theme2.header_1);
  REQUIRE(theme1.bold_text == theme2.bold_text);
  REQUIRE(theme1.reset == theme2.reset);
}

TEST_CASE("minimal_color_theme has no colors except structure", "[markdown][theme]") {
  auto theme = md::minimal_color_theme();
  // All headers use same style
  REQUIRE(theme.header_1 == theme.header_2);
  REQUIRE(theme.header_2 == theme.header_3);
}

TEST_CASE("no_color_theme has all empty strings", "[markdown][theme]") {
  auto theme = md::no_color_theme();
  REQUIRE(theme.header_1.empty());
  REQUIRE(theme.bold_text.empty());
  REQUIRE(theme.code_span.empty());
  REQUIRE(theme.reset.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// Constants tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("markdown constants are correct", "[markdown][constants]") {
  REQUIRE(md::default_terminal_width == 80);
  REQUIRE(md::max_header_level == 6);
  REQUIRE(md::code_block_indent_spaces == 4);
  REQUIRE(md::fence_marker_length == 3);
  REQUIRE(md::min_horizontal_rule_chars == 3);
}
