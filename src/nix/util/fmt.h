#pragma once
///@file

#include <string>

#include <boost/format.hpp>

#include "nix/util/ansicolor.h"

namespace nix {

/**
 * A helper for writing `boost::format` expressions.
 *
 * These are equivalent:
 *
 * ```
 * format_helper(formatter, a_0, ..., a_n)
 * formatter % a_0 % ... % a_n
 * ```
 *
 * With a single argument, `format_helper(s)` is a no-op.
 */
template <class F>
inline auto format_helper(F& /*f*/) -> void {}

template <class F, typename T, typename... args_t>
inline auto format_helper(F& formatter, const T& arg, const args_t&... args) -> void {
  // Interpolate one argument and then recurse.
  format_helper(formatter % arg, args...);
}

/**
 * Set the correct exceptions for `fmt`.
 */
inline auto set_exceptions(boost::format& fmt) -> void {
  fmt.exceptions(boost::io::all_error_bits ^ boost::io::too_many_args_bit ^
                 boost::io::too_few_args_bit);
}

/**
 * A helper for writing a `boost::format` expression to a string.
 *
 * These are (roughly) equivalent:
 *
 * ```
 * fmt(formatString, a_0, ..., a_n)
 * (boost::format(formatString) % a_0 % ... % a_n).str()
 * ```
 *
 * However, when called with a single argument, the string is returned
 * unchanged.
 *
 * If you write code like this:
 *
 * ```
 * std::cout << boost::format(stringFromUserInput) << std::endl;
 * ```
 *
 * And `stringFromUserInput` contains formatting placeholders like `%s`, then
 * the code will crash at runtime. `fmt` helps you avoid this pitfall.
 */
inline auto fmt(const std::string& str) -> std::string {
  return str;
}

inline auto fmt(std::string_view str) -> std::string {
  return std::string(str);
}

inline auto fmt(const char* str) -> std::string {
  return str;
}

template <typename... args_t>
inline auto fmt(const std::string& fs, const args_t&... args) -> std::string {
  boost::format formatter(fs);
  set_exceptions(formatter);
  format_helper(formatter, args...);
  return formatter.str();
}

/**
 * Values wrapped in this struct are printed in magenta.
 *
 * By default, arguments to `hint_fmt_t` are printed in magenta. To avoid this,
 * either wrap the argument in `uncolored_t` or add a specialization of
 * `hint_fmt_t::operator%`.
 */
template <class T>
struct magenta_t {
  magenta_t(const T& val) : value(val) {}

  const T& value;
};

template <class T>
auto operator<<(std::ostream& out, const magenta_t<T>& arg) -> std::ostream& {
  return out << ANSI_WARNING << arg.value << ANSI_NORMAL;
}

/**
 * Values wrapped in this class are printed without coloring.
 *
 * Specifically, the color is reset to normal before printing the value.
 *
 * By default, arguments to `hint_fmt_t` are printed in magenta (see `magenta_t`).
 */
template <class T>
struct uncolored_t {
  uncolored_t(const T& val) : value(val) {}

  const T& value;
};

template <class T>
auto operator<<(std::ostream& out, const uncolored_t<T>& arg) -> std::ostream& {
  return out << ANSI_NORMAL << arg.value;
}

/**
 * A wrapper around `boost::format` which colors interpolated arguments in
 * magenta by default.
 */
class hint_fmt_t {
private:
  boost::format fmt_{};

public:
  /**
   * Format the given string literally, without interpolating format
   * placeholders.
   */
  hint_fmt_t(const std::string& literal) : hint_fmt_t("%s", uncolored_t(literal)) {}

  static auto from_format_string(const std::string& format) -> hint_fmt_t {
    return hint_fmt_t(boost::format(format));
  }

  /**
   * Interpolate the given arguments into the format string.
   */
  template <typename... args_t>
  hint_fmt_t(const std::string& format, const args_t&... args)
      : hint_fmt_t(boost::format(format), args...) {}

  hint_fmt_t(const hint_fmt_t& hf) : fmt_(hf.fmt_) {}

  template <typename... args_t>
  hint_fmt_t(boost::format&& format, const args_t&... args) : fmt_(std::move(format)) {
    set_exceptions(fmt_);
    format_helper(*this, args...);
  }

  template <class T>
  auto operator%(const T& value) -> hint_fmt_t& {
    fmt_ % magenta_t(value);
    return *this;
  }

  template <class T>
  auto operator%(const uncolored_t<T>& value) -> hint_fmt_t& {
    fmt_ % value.value;
    return *this;
  }

  auto operator=(hint_fmt_t const& rhs) -> hint_fmt_t& = default;

  [[nodiscard]] auto str() const -> std::string { return fmt_.str(); }
};

auto operator<<(std::ostream& os, const hint_fmt_t& hf) -> std::ostream&;

} // namespace nix
