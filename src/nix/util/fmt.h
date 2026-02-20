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
inline void format_helper(F& /*f*/) {}

template <class F, typename T, typename... args_t>
inline void format_helper(F& f, const T& x, const args_t&... args) {
  // Interpolate one argument and then recurse.
  format_helper(f % x, args...);
}

/**
 * Set the correct exceptions for `fmt`.
 */
inline void set_exceptions(boost::format& fmt) {
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
inline std::string fmt(const std::string& s) {
  return s;
}

inline std::string fmt(std::string_view s) {
  return std::string(s);
}

inline std::string fmt(const char* s) {
  return s;
}

template <typename... args_t>
inline std::string fmt(const std::string& fs, const args_t&... args) {
  boost::format f(fs);
  set_exceptions(f);
  format_helper(f, args...);
  return f.str();
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
  magenta_t(const T& s) : value(s) {}

  const T& value;
};

template <class T>
auto operator<<(std::ostream& out, const magenta_t<T>& y) -> std::ostream& {
  return out << ANSI_WARNING << y.value << ANSI_NORMAL;
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
  uncolored_t(const T& s) : value(s) {}

  const T& value;
};

template <class T>
auto operator<<(std::ostream& out, const uncolored_t<T>& y) -> std::ostream& {
  return out << ANSI_NORMAL << y.value;
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

  [[nodiscard]] std::string str() const { return fmt_.str(); }
};

auto operator<<(std::ostream& os, const hint_fmt_t& hf) -> std::ostream&;

} // namespace nix
