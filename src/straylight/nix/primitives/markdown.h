// straylight::nix::primitives::markdown - Terminal markdown rendering
//
// Simple markdown renderer for terminal output with ANSI colors.
// Supports headers, bold, italic, code spans, code blocks, lists, and links.
// Word wrapping to terminal width with configurable color themes.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace straylight::nix::primitives {

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

inline constexpr std::size_t default_terminal_width = 80;
inline constexpr std::size_t max_header_level = 6;
inline constexpr std::size_t code_block_indent_spaces = 4;
inline constexpr std::size_t fence_marker_length = 3;
inline constexpr std::size_t min_horizontal_rule_chars = 3;
inline constexpr std::size_t blockquote_margin = 4;

// ─────────────────────────────────────────────────────────────────────────────
// ANSI escape codes
// ─────────────────────────────────────────────────────────────────────────────

namespace ansi {

inline constexpr std::string_view reset{"\033[0m"};
inline constexpr std::string_view bold{"\033[1m"};
inline constexpr std::string_view dim{"\033[2m"};
inline constexpr std::string_view italic{"\033[3m"};
inline constexpr std::string_view underline{"\033[4m"};

// foreground colors
inline constexpr std::string_view black{"\033[30m"};
inline constexpr std::string_view red{"\033[31m"};
inline constexpr std::string_view green{"\033[32m"};
inline constexpr std::string_view yellow{"\033[33m"};
inline constexpr std::string_view blue{"\033[34m"};
inline constexpr std::string_view magenta{"\033[35m"};
inline constexpr std::string_view cyan{"\033[36m"};
inline constexpr std::string_view white{"\033[37m"};

// bright foreground colors
inline constexpr std::string_view bright_black{"\033[90m"};
inline constexpr std::string_view bright_red{"\033[91m"};
inline constexpr std::string_view bright_green{"\033[92m"};
inline constexpr std::string_view bright_yellow{"\033[93m"};
inline constexpr std::string_view bright_blue{"\033[94m"};
inline constexpr std::string_view bright_magenta{"\033[95m"};
inline constexpr std::string_view bright_cyan{"\033[96m"};
inline constexpr std::string_view bright_white{"\033[97m"};

// background colors
inline constexpr std::string_view background_black{"\033[40m"};
inline constexpr std::string_view background_red{"\033[41m"};
inline constexpr std::string_view background_green{"\033[42m"};
inline constexpr std::string_view background_yellow{"\033[43m"};
inline constexpr std::string_view background_blue{"\033[44m"};
inline constexpr std::string_view background_magenta{"\033[45m"};
inline constexpr std::string_view background_cyan{"\033[46m"};
inline constexpr std::string_view background_white{"\033[47m"};

} // namespace ansi

// ─────────────────────────────────────────────────────────────────────────────
// Color theme configuration
// ─────────────────────────────────────────────────────────────────────────────

/// Color theme for markdown rendering.
/// Each field contains the ANSI escape sequence to apply for that element.
struct markdown_color_theme {
  std::string_view header_1;
  std::string_view header_2;
  std::string_view header_3;
  std::string_view header_other;
  std::string_view bold_text;
  std::string_view italic_text;
  std::string_view code_span;
  std::string_view code_block;
  std::string_view link_text;
  std::string_view link_url;
  std::string_view list_bullet;
  std::string_view horizontal_rule;
  std::string_view blockquote;
  std::string_view reset;
};

/// Default color theme - designed for dark terminals
[[nodiscard]] constexpr markdown_color_theme default_color_theme() noexcept {
  return markdown_color_theme{
      .header_1 = "\033[1;96m",     // bold bright cyan
      .header_2 = "\033[1;94m",     // bold bright blue
      .header_3 = "\033[1;95m",     // bold bright magenta
      .header_other = "\033[1;93m", // bold bright yellow
      .bold_text = "\033[1m",       // bold
      .italic_text = "\033[3m",     // italic
      .code_span = "\033[33m",      // yellow
      .code_block = "\033[2;33m",   // dim yellow
      .link_text = "\033[4;36m",    // underline cyan
      .link_url = "\033[2;36m",     // dim cyan
      .list_bullet = "\033[32m",    // green
      .horizontal_rule = "\033[2m", // dim
      .blockquote = "\033[3;37m",   // italic white
      .reset = "\033[0m",           // reset
  };
}

/// Minimal color theme - just structural emphasis
[[nodiscard]] constexpr markdown_color_theme minimal_color_theme() noexcept {
  return markdown_color_theme{
      .header_1 = "\033[1m",        // bold
      .header_2 = "\033[1m",        // bold
      .header_3 = "\033[1m",        // bold
      .header_other = "\033[1m",    // bold
      .bold_text = "\033[1m",       // bold
      .italic_text = "\033[3m",     // italic
      .code_span = "\033[7m",       // reverse video
      .code_block = "\033[2m",      // dim
      .link_text = "\033[4m",       // underline
      .link_url = "\033[2m",        // dim
      .list_bullet = "",            // no styling
      .horizontal_rule = "\033[2m", // dim
      .blockquote = "\033[3m",      // italic
      .reset = "\033[0m",           // reset
  };
}

/// No color theme - plain text output
[[nodiscard]] constexpr markdown_color_theme no_color_theme() noexcept {
  return markdown_color_theme{
      .header_1 = "",
      .header_2 = "",
      .header_3 = "",
      .header_other = "",
      .bold_text = "",
      .italic_text = "",
      .code_span = "",
      .code_block = "",
      .link_text = "",
      .link_url = "",
      .list_bullet = "",
      .horizontal_rule = "",
      .blockquote = "",
      .reset = "",
  };
}

// ─────────────────────────────────────────────────────────────────────────────
// Render options
// ─────────────────────────────────────────────────────────────────────────────

/// Options for markdown rendering
struct markdown_render_options {
  std::size_t terminal_width = default_terminal_width;
  markdown_color_theme theme = default_color_theme();
  std::size_t tab_width = 4;
  std::string_view list_bullet_char = "•";
  std::string_view horizontal_rule_char = "─";
  bool preserve_line_breaks = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Implementation details
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// Count leading characters matching the given character
[[nodiscard]] inline std::size_t count_leading(std::string_view text, char character) noexcept {
  std::size_t count = 0;
  while (count < text.size() && text[count] == character) {
    ++count;
  }
  return count;
}

/// Skip leading whitespace and return remaining view
[[nodiscard]] inline std::string_view skip_whitespace(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size() && (text[index] == ' ' || text[index] == '\t')) {
    ++index;
  }
  return text.substr(index);
}

/// Find the next occurrence of a delimiter, respecting escape sequences
[[nodiscard]] inline std::size_t find_delimiter(std::string_view text, std::string_view delimiter,
                                                std::size_t start = 0) noexcept {
  for (std::size_t index = start; index < text.size(); ++index) {
    // skip escaped characters
    if (text[index] == '\\' && index + 1 < text.size()) {
      ++index;
      continue;
    }
    if (text.substr(index, delimiter.size()) == delimiter) {
      return index;
    }
  }
  return std::string_view::npos;
}

/// Calculate visible length of text (excluding ANSI escape sequences)
[[nodiscard]] inline std::size_t visible_length(std::string_view text) noexcept {
  std::size_t length = 0;
  bool in_escape = false;

  for (char character : text) {
    if (character == '\033') {
      in_escape = true;
    } else if (in_escape && character == 'm') {
      in_escape = false;
    } else if (!in_escape) {
      ++length;
    }
  }
  return length;
}

/// Word wrap text to fit within width, preserving ANSI codes
[[nodiscard]] inline std::string word_wrap(std::string_view text, std::size_t width,
                                           std::string_view indent = "",
                                           std::string_view continuation_indent = "") {
  if (width == 0) {
    return std::string{text};
  }

  std::string result;
  result.reserve(text.size() + (text.size() / width) * 2);

  std::size_t line_visible_length = 0;
  std::size_t word_start = 0;
  bool first_line = true;
  bool in_escape = false;

  auto append_indent = [&]() {
    if (first_line) {
      result.append(indent);
      line_visible_length = visible_length(indent);
      first_line = false;
    } else {
      result.append(continuation_indent);
      line_visible_length = visible_length(continuation_indent);
    }
  };

  auto flush_word = [&](std::size_t end) {
    if (word_start >= end) {
      return;
    }

    std::string_view word = text.substr(word_start, end - word_start);
    std::size_t word_visible_length = visible_length(word);

    // check if we need a new line
    if (line_visible_length > 0 && line_visible_length + word_visible_length > width) {
      result.push_back('\n');
      append_indent();
    } else if (result.empty()) {
      append_indent();
    }

    result.append(word);
    line_visible_length += word_visible_length;
    word_start = end;
  };

  for (std::size_t index = 0; index < text.size(); ++index) {
    char character = text[index];

    if (character == '\033') {
      in_escape = true;
      continue;
    }

    if (in_escape) {
      if (character == 'm') {
        in_escape = false;
      }
      continue;
    }

    if (character == ' ' || character == '\t') {
      flush_word(index);
      // skip consecutive whitespace
      while (index + 1 < text.size() && (text[index + 1] == ' ' || text[index + 1] == '\t')) {
        ++index;
      }
      word_start = index + 1;
      // add single space if not at line start
      if (line_visible_length > visible_length(first_line ? indent : continuation_indent)) {
        result.push_back(' ');
        ++line_visible_length;
      }
    } else if (character == '\n') {
      flush_word(index);
      result.push_back('\n');
      first_line = false;
      append_indent();
      word_start = index + 1;
    }
  }

  // flush any remaining word
  flush_word(text.size());

  return result;
}

// forward declaration for recursive calls
[[nodiscard]] std::string render_inline(std::string_view text, const markdown_color_theme& theme);

/// Render bold text
[[nodiscard]] inline std::string render_bold(std::string_view text,
                                             const markdown_color_theme& theme) {
  std::string result;
  result.append(theme.bold_text);
  result.append(render_inline(text, theme));
  result.append(theme.reset);
  return result;
}

/// Render italic text
[[nodiscard]] inline std::string render_italic(std::string_view text,
                                               const markdown_color_theme& theme) {
  std::string result;
  result.append(theme.italic_text);
  result.append(render_inline(text, theme));
  result.append(theme.reset);
  return result;
}

/// Render code span
[[nodiscard]] inline std::string render_code_span(std::string_view text,
                                                  const markdown_color_theme& theme) {
  std::string result;
  result.append(theme.code_span);
  result.append(text);
  result.append(theme.reset);
  return result;
}

/// Render link
[[nodiscard]] inline std::string render_link(std::string_view link_text, std::string_view link_url,
                                             const markdown_color_theme& theme) {
  std::string result;
  result.append(theme.link_text);
  result.append(link_text);
  result.append(theme.reset);
  result.append(" (");
  result.append(theme.link_url);
  result.append(link_url);
  result.append(theme.reset);
  result.push_back(')');
  return result;
}

/// Render inline formatting (bold, italic, code spans, links)
[[nodiscard]] inline std::string render_inline(std::string_view text,
                                               const markdown_color_theme& theme) {
  std::string result;
  result.reserve(text.size() * 2);

  std::size_t index = 0;
  while (index < text.size()) {
    // escape sequence - pass through next character
    if (text[index] == '\\' && index + 1 < text.size()) {
      result.push_back(text[index + 1]);
      index += 2;
      continue;
    }

    // code span with backticks
    if (text[index] == '`') {
      std::size_t delimiter_length = 1;
      while (index + delimiter_length < text.size() && text[index + delimiter_length] == '`') {
        ++delimiter_length;
      }

      std::string_view delimiter = text.substr(index, delimiter_length);
      std::size_t end = find_delimiter(text, delimiter, index + delimiter_length);

      if (end != std::string_view::npos) {
        result.append(render_code_span(
            text.substr(index + delimiter_length, end - index - delimiter_length), theme));
        index = end + delimiter_length;
        continue;
      }
    }

    // bold with **
    if (index + 1 < text.size() && text[index] == '*' && text[index + 1] == '*') {
      std::size_t end = find_delimiter(text, "**", index + 2);
      if (end != std::string_view::npos) {
        result.append(render_bold(text.substr(index + 2, end - index - 2), theme));
        index = end + 2;
        continue;
      }
    }

    // bold with __
    if (index + 1 < text.size() && text[index] == '_' && text[index + 1] == '_') {
      std::size_t end = find_delimiter(text, "__", index + 2);
      if (end != std::string_view::npos) {
        result.append(render_bold(text.substr(index + 2, end - index - 2), theme));
        index = end + 2;
        continue;
      }
    }

    // italic with * (but not **)
    if (text[index] == '*' && (index + 1 >= text.size() || text[index + 1] != '*')) {
      std::size_t end = find_delimiter(text, "*", index + 1);
      if (end != std::string_view::npos) {
        result.append(render_italic(text.substr(index + 1, end - index - 1), theme));
        index = end + 1;
        continue;
      }
    }

    // italic with _ (but not __)
    if (text[index] == '_' && (index + 1 >= text.size() || text[index + 1] != '_')) {
      std::size_t end = find_delimiter(text, "_", index + 1);
      if (end != std::string_view::npos) {
        result.append(render_italic(text.substr(index + 1, end - index - 1), theme));
        index = end + 1;
        continue;
      }
    }

    // links [text](url)
    if (text[index] == '[') {
      std::size_t text_end = find_delimiter(text, "]", index + 1);
      if (text_end != std::string_view::npos && text_end + 1 < text.size() &&
          text[text_end + 1] == '(') {
        std::size_t url_end = find_delimiter(text, ")", text_end + 2);
        if (url_end != std::string_view::npos) {
          std::string_view parsed_link_text = text.substr(index + 1, text_end - index - 1);
          std::string_view parsed_link_url = text.substr(text_end + 2, url_end - text_end - 2);
          result.append(render_link(parsed_link_text, parsed_link_url, theme));
          index = url_end + 1;
          continue;
        }
      }
    }

    // regular character
    result.push_back(text[index]);
    ++index;
  }

  return result;
}

/// Check if line is a horizontal rule
[[nodiscard]] inline bool is_horizontal_rule(std::string_view line) noexcept {
  if (line.size() < min_horizontal_rule_chars) {
    return false;
  }
  char first = line[0];
  if (first != '-' && first != '*' && first != '_') {
    return false;
  }
  for (char character : line) {
    if (character != first) {
      return false;
    }
  }
  return true;
}

/// Render a header line
[[nodiscard]] inline std::string render_header(std::string_view text, std::size_t level,
                                               const markdown_color_theme& theme) {
  std::string_view style;
  switch (level) {
    case 1:
      style = theme.header_1;
      break;
    case 2:
      style = theme.header_2;
      break;
    case 3:
      style = theme.header_3;
      break;
    default:
      style = theme.header_other;
      break;
  }

  std::string result;
  result.append(style);
  result.append(render_inline(text, theme));
  result.append(theme.reset);
  return result;
}

/// Render a blockquote line
[[nodiscard]] inline std::string render_blockquote(std::string_view text, std::size_t width,
                                                   const markdown_color_theme& theme) {
  std::string result;
  result.append(theme.blockquote);
  result.append("│ ");
  std::string inline_rendered = render_inline(text, theme);
  std::size_t effective_width = width > blockquote_margin ? width - blockquote_margin : width;
  result.append(word_wrap(inline_rendered, effective_width, "", "  "));
  result.append(theme.reset);
  return result;
}

/// Render an unordered list item
[[nodiscard]] inline std::string render_unordered_list_item(std::string_view text,
                                                            std::size_t width,
                                                            std::string_view bullet,
                                                            const markdown_color_theme& theme) {
  std::string result;
  result.append(theme.list_bullet);
  result.append(bullet);
  result.append(theme.reset);
  result.append(" ");
  std::string inline_rendered = render_inline(text, theme);
  std::size_t effective_width = width > blockquote_margin ? width - blockquote_margin : width;
  result.append(word_wrap(inline_rendered, effective_width, "", "  "));
  return result;
}

/// Render an ordered list item
[[nodiscard]] inline std::string render_ordered_list_item(std::string_view number,
                                                          std::string_view text, std::size_t width,
                                                          const markdown_color_theme& theme) {
  std::string result;
  result.append(theme.list_bullet);
  result.append(number);
  result.append(".");
  result.append(theme.reset);
  result.append(" ");
  std::string inline_rendered = render_inline(text, theme);
  std::string indent(number.size() + 2, ' ');
  std::size_t effective_width = width > indent.size() + 2 ? width - indent.size() - 2 : width;
  result.append(word_wrap(inline_rendered, effective_width, "", indent));
  return result;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Main render function
// ─────────────────────────────────────────────────────────────────────────────

/// Render markdown with full options control
///
/// Supported syntax:
///   - Headers: # H1, ## H2, ### H3, etc.
///   - Bold: **text** or __text__
///   - Italic: *text* or _text_
///   - Code spans: `code` or ``code with backticks``
///   - Code blocks: ``` or indented by 4 spaces
///   - Lists: - item or * item or numbered 1. item
///   - Links: [text](url)
///   - Horizontal rules: ---, ***, ___
///   - Blockquotes: > text
[[nodiscard]] inline std::string render_markdown(std::string_view input,
                                                 const markdown_render_options& options) {
  std::string result;
  result.reserve(input.size() * 2);

  const auto& theme = options.theme;
  std::size_t width = options.terminal_width;

  std::size_t line_start = 0;
  bool in_code_block = false;
  std::string code_block_fence;
  bool previous_line_empty = true;

  while (line_start < input.size()) {
    // find end of current line
    std::size_t line_end = input.find('\n', line_start);
    if (line_end == std::string_view::npos) {
      line_end = input.size();
    }

    std::string_view line = input.substr(line_start, line_end - line_start);
    std::string_view trimmed_line = detail::skip_whitespace(line);

    // code block handling
    if (in_code_block) {
      if (trimmed_line.starts_with(code_block_fence)) {
        in_code_block = false;
        result.append(theme.reset);
        result.push_back('\n');
      } else {
        result.append(theme.code_block);
        result.append(line);
        result.append(theme.reset);
        result.push_back('\n');
      }
      line_start = line_end + 1;
      previous_line_empty = false;
      continue;
    }

    // check for fenced code block start
    if (trimmed_line.starts_with("```") || trimmed_line.starts_with("~~~")) {
      in_code_block = true;
      code_block_fence = std::string{trimmed_line.substr(0, fence_marker_length)};
      result.push_back('\n');
      line_start = line_end + 1;
      previous_line_empty = false;
      continue;
    }

    // check for indented code block (4 spaces or tab)
    if (line.size() >= code_block_indent_spaces && line.starts_with("    ")) {
      result.append(theme.code_block);
      result.append(line.substr(code_block_indent_spaces));
      result.append(theme.reset);
      result.push_back('\n');
      line_start = line_end + 1;
      previous_line_empty = false;
      continue;
    }

    if (!line.empty() && line[0] == '\t') {
      result.append(theme.code_block);
      result.append(line.substr(1));
      result.append(theme.reset);
      result.push_back('\n');
      line_start = line_end + 1;
      previous_line_empty = false;
      continue;
    }

    // empty line
    if (trimmed_line.empty()) {
      if (!previous_line_empty) {
        result.push_back('\n');
      }
      line_start = line_end + 1;
      previous_line_empty = true;
      continue;
    }

    // horizontal rule
    if (detail::is_horizontal_rule(trimmed_line)) {
      result.append(theme.horizontal_rule);
      for (std::size_t index = 0; index < width; ++index) {
        result.append(options.horizontal_rule_char);
      }
      result.append(theme.reset);
      result.push_back('\n');
      line_start = line_end + 1;
      previous_line_empty = false;
      continue;
    }

    // headers
    if (trimmed_line[0] == '#') {
      std::size_t header_level = detail::count_leading(trimmed_line, '#');
      if (header_level <= max_header_level && header_level < trimmed_line.size() &&
          trimmed_line[header_level] == ' ') {
        std::string_view header_text = detail::skip_whitespace(trimmed_line.substr(header_level));

        // remove trailing #s
        while (!header_text.empty() && header_text.back() == '#') {
          header_text.remove_suffix(1);
        }
        while (!header_text.empty() && header_text.back() == ' ') {
          header_text.remove_suffix(1);
        }

        if (!result.empty() && result.back() != '\n') {
          result.push_back('\n');
        }
        result.append(detail::render_header(header_text, header_level, theme));
        result.push_back('\n');
        line_start = line_end + 1;
        previous_line_empty = false;
        continue;
      }
    }

    // blockquote
    if (trimmed_line[0] == '>') {
      std::string_view quote_text = trimmed_line.substr(1);
      if (!quote_text.empty() && quote_text[0] == ' ') {
        quote_text = quote_text.substr(1);
      }

      result.append(detail::render_blockquote(quote_text, width, theme));
      result.push_back('\n');
      line_start = line_end + 1;
      previous_line_empty = false;
      continue;
    }

    // unordered list
    if ((trimmed_line[0] == '-' || trimmed_line[0] == '*' || trimmed_line[0] == '+') &&
        trimmed_line.size() > 1 && trimmed_line[1] == ' ') {
      std::string_view item_text = trimmed_line.substr(2);
      result.append(
          detail::render_unordered_list_item(item_text, width, options.list_bullet_char, theme));
      result.push_back('\n');
      line_start = line_end + 1;
      previous_line_empty = false;
      continue;
    }

    // ordered list
    {
      std::size_t digit_count = 0;
      while (digit_count < trimmed_line.size() && trimmed_line[digit_count] >= '0' &&
             trimmed_line[digit_count] <= '9') {
        ++digit_count;
      }
      if (digit_count > 0 && digit_count < trimmed_line.size() &&
          (trimmed_line[digit_count] == '.' || trimmed_line[digit_count] == ')') &&
          digit_count + 1 < trimmed_line.size() && trimmed_line[digit_count + 1] == ' ') {
        std::string_view number = trimmed_line.substr(0, digit_count);
        std::string_view item_text = trimmed_line.substr(digit_count + 2);
        result.append(detail::render_ordered_list_item(number, item_text, width, theme));
        result.push_back('\n');
        line_start = line_end + 1;
        previous_line_empty = false;
        continue;
      }
    }

    // regular paragraph
    std::string inline_rendered = detail::render_inline(trimmed_line, theme);
    result.append(detail::word_wrap(inline_rendered, width));
    result.push_back('\n');
    line_start = line_end + 1;
    previous_line_empty = false;
  }

  // remove trailing newlines
  while (result.size() > 1 && result[result.size() - 1] == '\n' &&
         result[result.size() - 2] == '\n') {
    result.pop_back();
  }

  return result;
}

/// Render markdown to ANSI-colored terminal output.
///
/// Example:
///   auto output = render_markdown("# Hello\n**bold** and *italic*", 80);
[[nodiscard]] inline std::string render_markdown(std::string_view input,
                                                 std::size_t terminal_width) {
  return render_markdown(input, markdown_render_options{.terminal_width = terminal_width});
}

/// Strip all ANSI escape sequences from text
[[nodiscard]] inline std::string strip_ansi(std::string_view text) {
  std::string result;
  result.reserve(text.size());

  bool in_escape = false;
  for (char character : text) {
    if (character == '\033') {
      in_escape = true;
    } else if (in_escape && character == 'm') {
      in_escape = false;
    } else if (!in_escape) {
      result.push_back(character);
    }
  }

  return result;
}

} // namespace straylight::nix::primitives
