// straylight::nix::text::adapters::nix_fmt_adapter
//
// Compatibility shim to migrate Nix codebase from boost::format to std::format.
//
// This adapter provides drop-in replacements for:
// - nix::fmt_()        -> Uses straylight::nix::text::fmt()
// - nix::hint_fmt_t    -> Uses straylight::nix::text::Hint
// - nix::magenta_t<T>  -> Uses straylight::nix::text::Magenta<T>
// - nix::uncolored_t<T> -> Uses straylight::nix::text::Uncolored<T>
//
// Migration strategy:
// 1. Include this header instead of "nix/util/fmt.h"
// 2. Replace boost::format patterns with std::format syntax gradually
// 3. The shim handles both old boost-style and new std::format syntax
//
// Format string compatibility:
// - %s, %d, %f        -> converted to {N:spec} at runtime
// - %1%, %2%, ...     -> converted to {0}, {1}, ... (0-indexed)
// - %|1$5d|           -> converted to {0:5d}
// - %%                -> literal %
// - {}                -> passed through (std::format native)

#pragma once

#include <cctype>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "straylight/nix/text/format.h"

namespace nix {

// ─────────────────────────────────────────────────────────────────────────────
// ANSI color constants (aliased from primitives)
// ─────────────────────────────────────────────────────────────────────────────

// These match the original ANSI_* macros from nix/util/ansicolor.h
// Using inline constexpr instead of macros for type safety
inline constexpr std::string_view ANSI_NORMAL = straylight::nix::text::kAnsiNormal;
inline constexpr std::string_view ANSI_BOLD = straylight::nix::text::kAnsiBold;
inline constexpr std::string_view ANSI_FAINT = straylight::nix::text::kAnsiFaint;
inline constexpr std::string_view ANSI_ITALIC = "\033[3m";
inline constexpr std::string_view ANSI_RED = straylight::nix::text::kAnsiRed;
inline constexpr std::string_view ANSI_GREEN = straylight::nix::text::kAnsiGreen;
inline constexpr std::string_view ANSI_WARNING =
    straylight::nix::text::kAnsiMagenta; // Nix uses magenta for warnings
inline constexpr std::string_view ANSI_BLUE = straylight::nix::text::kAnsiBlue;
inline constexpr std::string_view ANSI_MAGENTA = straylight::nix::text::kAnsiMagenta;
inline constexpr std::string_view ANSI_CYAN = straylight::nix::text::kAnsiCyan;

// ─────────────────────────────────────────────────────────────────────────────
// Color wrappers (aliased from primitives)
// ─────────────────────────────────────────────────────────────────────────────

/// Alias magenta_t to primitives::Magenta for source compatibility.
/// Original Nix code uses magenta_t<T> for warning highlighting.
template <typename T>
using magenta_t = straylight::nix::text::Magenta<T>;

/// Alias uncolored_t to primitives::Uncolored for source compatibility.
/// Used to prevent automatic coloring in hint_fmt_t context.
template <typename T>
using uncolored_t = straylight::nix::text::Uncolored<T>;

/// Additional color wrapper using the generic Colored type
template <typename T>
using colored_t = straylight::nix::text::Colored<T>;

// Provide ostream operators for backward compatibility with code
// that streams these types directly (not via std::format)
template <typename T>
std::ostream& operator<<(std::ostream& out, const magenta_t<T>& m) {
  return out << straylight::nix::text::kAnsiMagenta << m.value
             << straylight::nix::text::kAnsiNormal;
}

template <typename T>
std::ostream& operator<<(std::ostream& out, const uncolored_t<T>& u) {
  return out << straylight::nix::text::kAnsiNormal << u.value;
}

template <typename T>
std::ostream& operator<<(std::ostream& out, const colored_t<T>& c) {
  return out << c.color << c.value << straylight::nix::text::kAnsiNormal;
}

// ─────────────────────────────────────────────────────────────────────────────
// format_helper template (for backward compatibility)
// ─────────────────────────────────────────────────────────────────────────────

/// Helper to apply multiple arguments to a formatter.
/// With std::format this is unnecessary, but kept for source compatibility.
/// Usage: format_helper(formatter, a0, a1, ...) is equivalent to formatter % a0 % a1 % ...
template <typename F>
inline void format_helper(F& /*f*/) {}

template <typename F, typename T, typename... Args>
inline void format_helper(F& f, const T& x, const Args&... args) {
  format_helper(f % x, args...);
}

// ─────────────────────────────────────────────────────────────────────────────
// fmt_() function replacement
// ─────────────────────────────────────────────────────────────────────────────

/// Format a string using boost::format compatible syntax.
/// This is a drop-in replacement for the original nix::fmt_().
///
/// Single-argument overloads return the string unchanged (original behavior).
inline std::string fmt_(const std::string& s) {
  return s;
}

inline std::string fmt_(std::string_view s) {
  return std::string(s);
}

inline std::string fmt_(const char* s) {
  return std::string(s);
}

/// Multi-argument overload: converts boost-style format string and formats.
///
/// Supports:
/// - Printf-style: %s, %d, %f, %x, etc.
/// - Boost positional: %1%, %2%, ...
/// - Boost extended: %|1$5d|
/// - Literal percent: %%
template <typename... Args>
std::string fmt_(const std::string& format_str, const Args&... args) {
  return straylight::nix::text::fmt(format_str, args...);
}

// ─────────────────────────────────────────────────────────────────────────────
// hint_fmt_t class replacement
// ─────────────────────────────────────────────────────────────────────────────

/// Drop-in replacement for nix::hint_fmt_t.
///
/// Key differences from the original:
/// - Uses std::format internally instead of boost::format
/// - Format string conversion happens at construction time
/// - The operator% interface is preserved for backward compatibility
///
/// Original behavior preserved:
/// - Arguments are wrapped in magenta by default
/// - uncolored_t<T> prevents automatic coloring
/// - Can be constructed from format string + args or literal string
class hint_fmt_t {
public:
  /// Construct from a literal string (no formatting).
  /// This matches the original behavior: hint_fmt_t("literal") does not parse format specifiers.
  explicit hint_fmt_t(const std::string& literal) : str_(literal), finalized_(true) {}

  /// Construct from a string_view literal.
  explicit hint_fmt_t(std::string_view literal) : str_(literal), finalized_(true) {}

  /// Construct from a C-string literal.
  explicit hint_fmt_t(const char* literal) : str_(literal), finalized_(true) {}

  /// Construct from format string and arguments.
  /// Arguments are automatically wrapped in magenta.
  template <typename... Args>
  explicit hint_fmt_t(const std::string& format_str, const Args&... args)
      : str_(format_with_magenta(format_str, args...)), finalized_(true) {}

  /// Copy constructor.
  hint_fmt_t(const hint_fmt_t& other) = default;

  /// Move constructor.
  hint_fmt_t(hint_fmt_t&& other) noexcept = default;

  /// Copy assignment.
  hint_fmt_t& operator=(const hint_fmt_t& other) = default;

  /// Move assignment.
  hint_fmt_t& operator=(hint_fmt_t&& other) noexcept = default;

  /// Factory for constructing from a format string without arguments.
  /// Preserves the format string for later operator% application.
  static hint_fmt_t from_format_string(const std::string& format_str) {
    hint_fmt_t result;
    result.format_str_ = format_str;
    result.finalized_ = false;
    return result;
  }

  /// Append an argument with magenta coloring (original behavior).
  /// Note: This deferred formatting mode is less efficient than
  /// constructing with all arguments upfront.
  template <typename T>
  hint_fmt_t& operator%(const T& value) {
    if (finalized_) {
      // Already finalized, append to string (unusual usage)
      std::ostringstream oss;
      oss << straylight::nix::text::kAnsiMagenta << value << straylight::nix::text::kAnsiNormal;
      str_ += oss.str();
    } else {
      // Store for deferred formatting
      std::ostringstream oss;
      oss << straylight::nix::text::kAnsiMagenta << value << straylight::nix::text::kAnsiNormal;
      pending_args_.push_back(oss.str());
    }
    return *this;
  }

  /// Append an uncolored argument.
  template <typename T>
  hint_fmt_t& operator%(const uncolored_t<T>& value) {
    if (finalized_) {
      std::ostringstream oss;
      oss << value.value;
      str_ += oss.str();
    } else {
      std::ostringstream oss;
      oss << value.value;
      pending_args_.push_back(oss.str());
    }
    return *this;
  }

  /// Get the formatted string.
  [[nodiscard]] std::string str() const {
    if (!finalized_ && !format_str_.empty()) {
      // Deferred formatting with pending args
      return format_deferred();
    }
    return str_;
  }

  /// Implicit conversion to string (for compatibility).
  operator std::string() const { return str(); }

private:
  hint_fmt_t() = default;

  /// Format with all arguments wrapped in magenta.
  template <typename... Args>
  static std::string format_with_magenta(const std::string& format_str, const Args&... args) {
    // Convert boost format to std::format and apply with magenta wrapping
    return straylight::nix::text::fmt(format_str, wrap_in_magenta(args)...);
  }

  /// Wrap a value in magenta unless it's already uncolored.
  template <typename T>
  static auto wrap_in_magenta(const T& v) {
    return magenta_t<T>(v);
  }

  template <typename T>
  static const T& wrap_in_magenta(const uncolored_t<T>& v) {
    return v.value;
  }

  template <typename T>
  static const magenta_t<T>& wrap_in_magenta(const magenta_t<T>& v) {
    return v; // Already wrapped
  }

  /// Handle deferred formatting with operator% args.
  std::string format_deferred() const {
    // For deferred mode, we need to replace placeholders with stored args
    // This is a simplified implementation - the original boost::format
    // is more sophisticated. We use a basic sequential replacement.
    std::string result;
    std::string fmt = format_str_;
    size_t arg_idx = 0;
    size_t i = 0;

    while (i < fmt.size()) {
      if (fmt[i] == '%' && i + 1 < fmt.size()) {
        if (fmt[i + 1] == '%') {
          result += '%';
          i += 2;
          continue;
        }
        // Skip format spec and replace with arg
        if (arg_idx < pending_args_.size()) {
          result += pending_args_[arg_idx++];
        }
        // Skip the format specifier
        ++i;
        // Skip digits for %N% style
        while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') {
          ++i;
        }
        if (i < fmt.size() && fmt[i] == '%') {
          ++i;
        } else {
          // Skip format chars like s, d, etc.
          while (i < fmt.size() && std::isalpha(fmt[i])) {
            ++i;
          }
        }
      } else {
        result += fmt[i++];
      }
    }
    return result;
  }

  std::string str_;
  std::string format_str_;
  std::vector<std::string> pending_args_;
  bool finalized_ = false;
};

/// Stream output operator for hint_fmt_t.
inline std::ostream& operator<<(std::ostream& os, const hint_fmt_t& hf) {
  return os << hf.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// Additional utilities
// ─────────────────────────────────────────────────────────────────────────────

/// Modern format function alias (for gradual migration to native std::format syntax).
using straylight::nix::text::format;

/// Modern hint function (for gradual migration).
using straylight::nix::text::hint;
using straylight::nix::text::Hint;

// Convenience color wrapper factories (matching new style)
using straylight::nix::text::Colored;
using straylight::nix::text::Magenta;
using straylight::nix::text::Uncolored;

} // namespace nix
