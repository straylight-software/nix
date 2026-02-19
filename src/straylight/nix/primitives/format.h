// straylight::nix::primitives::format
//
// Modern string formatting built on std::format (C++23).
//
// Provides a migration path from boost::format with support for:
// - std::format native syntax: "{}", "{0}", "{:d}"
// - Legacy boost-style: "%s", "%1%", "%|1$5d|" (converted at compile-time where possible)
//
// Usage:
//   // Modern (preferred)
//   auto s = format("Hello {}!", name);
//   auto s = format("x={}, y={}", x, y);
//   auto s = format("{0} + {0} = {1}", x, x+x);
//
//   // Legacy compatibility (for gradual migration)
//   auto s = fmt("Hello %s!", name);       // printf-style
//   auto s = fmt("x=%1%, y=%2%", x, y);    // boost positional
//
// HintFmt replacement:
//   auto h = hint("expected {}, got {}", Magenta(expected), actual);

#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace straylight::nix::primitives {

// ─────────────────────────────────────────────────────────────────────────────
// ANSI color codes (matching nix/util/ansicolor.h)
// ─────────────────────────────────────────────────────────────────────────────

inline constexpr std::string_view kAnsiNormal = "\033[0m";
inline constexpr std::string_view kAnsiRed = "\033[31;1m";
inline constexpr std::string_view kAnsiGreen = "\033[32;1m";
inline constexpr std::string_view kAnsiYellow = "\033[33;1m";
inline constexpr std::string_view kAnsiBlue = "\033[34;1m";
inline constexpr std::string_view kAnsiMagenta = "\033[35;1m";
inline constexpr std::string_view kAnsiCyan = "\033[36;1m";
inline constexpr std::string_view kAnsiBold = "\033[1m";
inline constexpr std::string_view kAnsiFaint = "\033[2m";

// ─────────────────────────────────────────────────────────────────────────────
// Color wrappers for formatted output
// ─────────────────────────────────────────────────────────────────────────────

/// Wrap a value to be printed in magenta (for hints/errors)
template <typename T>
struct Magenta {
  T const& value;
  constexpr explicit Magenta(T const& v) noexcept : value(v) {}
};

template <typename T>
Magenta(T const&) -> Magenta<T>;

/// Wrap a value to reset color before printing
template <typename T>
struct Uncolored {
  T const& value;
  constexpr explicit Uncolored(T const& v) noexcept : value(v) {}
};

template <typename T>
Uncolored(T const&) -> Uncolored<T>;

/// Wrap a value to be printed in a specific color
template <typename T>
struct Colored {
  T const& value;
  std::string_view color;
  constexpr Colored(T const& v, std::string_view c) noexcept : value(v), color(c) {}
};

template <typename T>
Colored(T const&, std::string_view) -> Colored<T>;

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specializations for color wrappers
// ─────────────────────────────────────────────────────────────────────────────

} // namespace straylight::nix::primitives

template <typename T>
struct std::formatter<straylight::nix::primitives::Magenta<T>> : std::formatter<T> {
  template <typename FormatContext>
  auto format(straylight::nix::primitives::Magenta<T> const& m, FormatContext& ctx) const {
    auto out = ctx.out();
    out = std::copy(straylight::nix::primitives::kAnsiMagenta.begin(),
                    straylight::nix::primitives::kAnsiMagenta.end(), out);
    ctx.advance_to(out);
    out = std::formatter<T>::format(m.value, ctx);
    out = std::copy(straylight::nix::primitives::kAnsiNormal.begin(),
                    straylight::nix::primitives::kAnsiNormal.end(), out);
    return out;
  }
};

template <typename T>
struct std::formatter<straylight::nix::primitives::Uncolored<T>> : std::formatter<T> {
  template <typename FormatContext>
  auto format(straylight::nix::primitives::Uncolored<T> const& u, FormatContext& ctx) const {
    auto out = ctx.out();
    out = std::copy(straylight::nix::primitives::kAnsiNormal.begin(),
                    straylight::nix::primitives::kAnsiNormal.end(), out);
    ctx.advance_to(out);
    return std::formatter<T>::format(u.value, ctx);
  }
};

template <typename T>
struct std::formatter<straylight::nix::primitives::Colored<T>> : std::formatter<T> {
  template <typename FormatContext>
  auto format(straylight::nix::primitives::Colored<T> const& c, FormatContext& ctx) const {
    auto out = ctx.out();
    out = std::copy(c.color.begin(), c.color.end(), out);
    ctx.advance_to(out);
    out = std::formatter<T>::format(c.value, ctx);
    out = std::copy(straylight::nix::primitives::kAnsiNormal.begin(),
                    straylight::nix::primitives::kAnsiNormal.end(), out);
    return out;
  }
};

namespace straylight::nix::primitives {

// ─────────────────────────────────────────────────────────────────────────────
// Modern format function (std::format wrapper)
// ─────────────────────────────────────────────────────────────────────────────

/// Format a string using std::format syntax.
/// This is the preferred API for new code.
template <typename... Args>
[[nodiscard]] std::string format(std::format_string<Args...> fmt_str, Args&&... args) {
  return std::format(fmt_str, std::forward<Args>(args)...);
}

// Single-argument overload: return string unchanged (no formatting)
[[nodiscard]] inline std::string format(std::string_view s) {
  return std::string(s);
}
[[nodiscard]] inline std::string format(std::string const& s) {
  return s;
}
[[nodiscard]] inline std::string format(char const* s) {
  return std::string(s);
}

// ─────────────────────────────────────────────────────────────────────────────
// Legacy format string conversion (boost::format -> std::format)
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// Convert a boost::format style string to std::format style at runtime.
/// Handles:
/// - %s, %d, %f, %x, %o, %e, %g -> {}
/// - %1%, %2%, ... -> {0}, {1}, ...
/// - %|1$5d| -> {0:5d} (width specifier)
/// - %% -> (literal %)
[[nodiscard]] inline std::string convert_boost_format(std::string_view boost_fmt) {
  std::string result;
  result.reserve(boost_fmt.size() * 2);

  std::size_t i = 0;
  std::size_t arg_index = 0; // For %s/%d style (sequential)

  while (i < boost_fmt.size()) {
    if (boost_fmt[i] != '%') {
      // Handle { and } escaping for std::format
      if (boost_fmt[i] == '{') {
        result += "{{";
      } else if (boost_fmt[i] == '}') {
        result += "}}";
      } else {
        result += boost_fmt[i];
      }
      ++i;
      continue;
    }

    // Found '%'
    ++i;
    if (i >= boost_fmt.size()) {
      result += '%'; // Trailing %
      break;
    }

    // %% -> literal %
    if (boost_fmt[i] == '%') {
      result += '%';
      ++i;
      continue;
    }

    // Check for %|...|  (boost extended format)
    if (boost_fmt[i] == '|') {
      ++i;
      std::size_t start = i;

      // Find closing |
      while (i < boost_fmt.size() && boost_fmt[i] != '|') {
        ++i;
      }

      if (i < boost_fmt.size()) {
        std::string_view spec(boost_fmt.data() + start, i - start);

        // Parse %|N$...| style
        std::size_t dollar = spec.find('$');
        if (dollar != std::string_view::npos) {
          // Extract position
          int pos = 0;
          auto [ptr, ec] = std::from_chars(spec.data(), spec.data() + dollar, pos);
          if (ec == std::errc{} && pos > 0) {
            std::string_view fmt_spec = spec.substr(dollar + 1);
            result += '{';
            result += std::to_string(pos - 1); // Convert 1-based to 0-based
            if (!fmt_spec.empty()) {
              result += ':';
              result += fmt_spec;
            }
            result += '}';
          }
        }
        ++i; // Skip closing |
      }
      continue;
    }

    // Check for %N% (boost positional)
    if (boost_fmt[i] >= '0' && boost_fmt[i] <= '9') {
      std::size_t start = i;
      while (i < boost_fmt.size() && boost_fmt[i] >= '0' && boost_fmt[i] <= '9') {
        ++i;
      }
      if (i < boost_fmt.size() && boost_fmt[i] == '%') {
        // It's %N%
        int pos = 0;
        std::from_chars(boost_fmt.data() + start, boost_fmt.data() + i, pos);
        result += '{';
        result += std::to_string(pos - 1); // Convert 1-based to 0-based
        result += '}';
        ++i; // Skip trailing %
        continue;
      }
      // Not %N%, rewind and treat as printf-style with width
      i = start;
    }

    // Printf-style: %s, %d, %f, etc. with optional flags/width/precision
    // Skip flags: -, +, space, #, 0
    while (i < boost_fmt.size() &&
           (boost_fmt[i] == '-' || boost_fmt[i] == '+' || boost_fmt[i] == ' ' ||
            boost_fmt[i] == '#' || boost_fmt[i] == '0')) {
      ++i;
    }

    // Skip width
    while (i < boost_fmt.size() && boost_fmt[i] >= '0' && boost_fmt[i] <= '9') {
      ++i;
    }

    // Skip precision
    if (i < boost_fmt.size() && boost_fmt[i] == '.') {
      ++i;
      while (i < boost_fmt.size() && boost_fmt[i] >= '0' && boost_fmt[i] <= '9') {
        ++i;
      }
    }

    // Skip length modifiers (h, l, ll, z, etc.)
    while (i < boost_fmt.size() &&
           (boost_fmt[i] == 'h' || boost_fmt[i] == 'l' || boost_fmt[i] == 'z' ||
            boost_fmt[i] == 'j' || boost_fmt[i] == 't' || boost_fmt[i] == 'L')) {
      ++i;
    }

    // Conversion specifier
    if (i < boost_fmt.size()) {
      char conv = boost_fmt[i];
      ++i;

      // Map printf conversion to std::format
      result += '{';
      result += std::to_string(arg_index++);
      switch (conv) {
        case 'd':
        case 'i':
          result += ":d";
          break;
        case 'u':
          result += ":d";
          break;
        case 'x':
          result += ":x";
          break;
        case 'X':
          result += ":X";
          break;
        case 'o':
          result += ":o";
          break;
        case 'f':
        case 'F':
          result += ":f";
          break;
        case 'e':
          result += ":e";
          break;
        case 'E':
          result += ":E";
          break;
        case 'g':
          result += ":g";
          break;
        case 'G':
          result += ":G";
          break;
        case 'a':
          result += ":a";
          break;
        case 'A':
          result += ":A";
          break;
        case 'c':
          result += ":c";
          break;
        case 's':
        case 'p':
        default:
          // No format spec needed
          break;
      }
      result += '}';
    }
  }

  return result;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Legacy fmt() function (boost::format compatible)
// ─────────────────────────────────────────────────────────────────────────────

/// Format using boost::format style syntax (for migration).
/// Converts format string at runtime and delegates to std::vformat.
template <typename... Args>
[[nodiscard]] std::string fmt(std::string_view boost_fmt, Args&&... args) {
  if constexpr (sizeof...(Args) == 0) {
    // Even with no args, we need to handle %% -> % conversion
    // Check if there's anything to convert
    if (boost_fmt.find('%') == std::string_view::npos) {
      return std::string(boost_fmt);
    }
    // Process %% escapes
    std::string result;
    result.reserve(boost_fmt.size());
    for (std::size_t i = 0; i < boost_fmt.size(); ++i) {
      if (boost_fmt[i] == '%' && i + 1 < boost_fmt.size() && boost_fmt[i + 1] == '%') {
        result += '%';
        ++i; // Skip second %
      } else {
        result += boost_fmt[i];
      }
    }
    return result;
  } else {
    std::string std_fmt = detail::convert_boost_format(boost_fmt);
    return std::vformat(std_fmt, std::make_format_args(args...));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// HintFmt replacement - formats with magenta highlighting by default
// ─────────────────────────────────────────────────────────────────────────────

/// A formatted hint string with ANSI color support.
/// Arguments are wrapped in magenta by default unless explicitly Uncolored.
class Hint {
public:
  /// Construct from a literal string (no formatting)
  explicit Hint(std::string_view literal) : str_(literal) {}

  /// Construct from a format string and arguments
  template <typename... Args>
  explicit Hint(std::format_string<Args...> fmt_str, Args&&... args)
      : str_(std::format(fmt_str, std::forward<Args>(args)...)) {}

  /// Get the formatted string
  [[nodiscard]] std::string const& str() const noexcept { return str_; }

  /// Implicit conversion to string_view
  [[nodiscard]] operator std::string_view() const noexcept { return str_; }

private:
  std::string str_;
};

/// Create a hint with magenta-highlighted arguments
template <typename... Args>
[[nodiscard]] Hint hint(std::format_string<Args...> fmt_str, Args&&... args) {
  return Hint(fmt_str, std::forward<Args>(args)...);
}

/// Create a hint with all arguments wrapped in Magenta
template <typename... Args>
[[nodiscard]] Hint magenta_hint(std::format_string<Magenta<std::decay_t<Args>>...> fmt_str,
                                Args&&... args) {
  return Hint(fmt_str, Magenta(std::forward<Args>(args))...);
}

} // namespace straylight::nix::primitives
