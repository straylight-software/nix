#ifndef NIX_UTIL_ERROR_H
#define NIX_UTIL_ERROR_H
/**
 * @file
 *
 * @brief This file defines two main structs/classes used in nix error handling.
 *
 * error_info_t provides a standard payload of error information, with conversion to string
 * happening in the logger rather than at the call site.
 *
 * base_error_t is the ancestor of nix specific exceptions (and Interrupted), and contains
 * an error_info_t.
 *
 * error_info_t structs are sent to the logger as part of an exception, or directly with the
 * logError or logWarning macros.
 * See libutil/tests/logging.cc for usage examples.
 */

#include <cerrno>
#include <compare>
#include <cstdint>
#include <cstring>
#include <exception>
#include <list>
#include <memory>
#include <optional>
#include <ostream>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

#include "nix/util/fmt.h"
#include "nix/util/suggestions.h"

namespace nix {

enum class verbosity_t : std::uint8_t {
  lvl_error = 0,
  lvl_warn,
  lvl_notice,
  lvl_info,
  lvl_talkative,
  lvl_chatty,
  lvl_debug,
  lvl_vomit
};

// Bring verbosity levels into nix:: namespace for backward compatibility
inline constexpr verbosity_t lvl_error = verbosity_t::lvl_error;
inline constexpr verbosity_t lvl_warn = verbosity_t::lvl_warn;
inline constexpr verbosity_t lvl_notice = verbosity_t::lvl_notice;
inline constexpr verbosity_t lvl_info = verbosity_t::lvl_info;
inline constexpr verbosity_t lvl_talkative = verbosity_t::lvl_talkative;
inline constexpr verbosity_t lvl_chatty = verbosity_t::lvl_chatty;
inline constexpr verbosity_t lvl_debug = verbosity_t::lvl_debug;
inline constexpr verbosity_t lvl_vomit = verbosity_t::lvl_vomit;

/**
 * The lines of code surrounding an error.
 */
// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
struct lines_of_code_t {
  std::optional<std::string> prev_line_of_code_;
  std::optional<std::string> err_line_of_code_;
  std::optional<std::string> next_line_of_code_;
};

/* NOTE: position.hh recursively depends on source-path.hh -> source-accessor.hh
   -> hash.hh -> configuration.hh -> experimental-features.hh -> error.hh -> pos_t.
   There are other such cycles.
   Thus, pos_t has to be an incomplete type in this header. But since error_info_t/trace_t
   have to refer to pos_t, they have to use pointer indirection via std::shared_ptr
   to break the recursive header dependency.
   FIXME: Untangle this mess. Should there be AbstractPos as there used to be before
   4feb7d9f71? */
struct pos_t;

auto print_code_lines(std::ostream& out, const std::string& prefix, const pos_t& err_pos,
                      const lines_of_code_t& loc) -> void;

/**
 * When a stack frame is printed.
 */
enum struct trace_print_t : std::uint8_t {
  /**
   * The default behavior; always printed when `--show-trace` is set.
   */
  default_print,
  /** always printed. Produced by `builtins.addErrorContext`. */
  always,
};

// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
struct trace_t {
  std::shared_ptr<const pos_t> pos_;
  hint_fmt_t hint_;
  trace_print_t print_ = trace_print_t::default_print;
};

[[nodiscard]] inline auto operator<=>(const trace_t& lhs, const trace_t& rhs)
    -> std::strong_ordering;

// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
struct error_info_t {
  verbosity_t level_;
  hint_fmt_t msg_;
  std::shared_ptr<const pos_t> pos_;
  std::list<trace_t> traces_;
  /**
   * Some messages are generated directly by expressions; notably `builtins.warn`, `abort`, `throw`.
   * These may be rendered differently, so that users can distinguish them.
   */
  bool is_from_expr_ = false;

  /**
   * Exit status.
   */
  unsigned int status_ = 1;

  suggestions_t suggestions_;

  static std::optional<std::string> program_name;
};

[[nodiscard]] auto show_error_info(std::ostream& out, const error_info_t& einfo, bool show_trace)
    -> std::ostream&;

// Returns formatted error info as a string (avoids need for ostringstream at call sites)
[[nodiscard]] auto format_error_info(const error_info_t& einfo, bool show_trace) -> std::string;

/**
 * base_error_t should generally not be caught, as it has Interrupted as
 * a subclass. Catch Error instead.
 */
class base_error_t : public std::exception {
protected:
  mutable error_info_t err_;

  /**
   * Cached formatted contents of `err_.msg`.
   */
  // NOLINTNEXTLINE(readability-redundant-member-init)
  mutable std::optional<std::string> what_{};

  /**
   * Format `err_.msg` and set `what_` to the resulting value.
   */
  [[nodiscard]] auto calc_what() const -> const std::string&;

public:
  ~base_error_t() override = default;
  base_error_t(const base_error_t&) = default;
  base_error_t(base_error_t&&) noexcept = default;
  [[nodiscard]] auto operator=(const base_error_t&) -> base_error_t& = default;
  [[nodiscard]] auto operator=(base_error_t&&) -> base_error_t& = default;

  template <typename... args_t>
  base_error_t(unsigned int status, const args_t&... args)
      : err_{.level_ = verbosity_t::lvl_error,
             .msg_ = hint_fmt_t(args...),
             .pos_ = nullptr,
             .traces_ = {},
             .status_ = status,
             .suggestions_ = {}} {}

  template <typename... args_t>
  explicit base_error_t(const std::string& fmt_str, const args_t&... args)
      : err_{.level_ = verbosity_t::lvl_error,
             .msg_ = hint_fmt_t(fmt_str, args...),
             .pos_ = nullptr,
             .traces_ = {},
             .suggestions_ = {}} {}

  template <typename... args_t>
  base_error_t(const suggestions_t& sug, const args_t&... args)
      : err_{.level_ = verbosity_t::lvl_error,
             .msg_ = hint_fmt_t(args...),
             .pos_ = nullptr,
             .traces_ = {},
             .suggestions_ = sug} {}

  base_error_t(const hint_fmt_t& hint)
      : err_{.level_ = verbosity_t::lvl_error,
             .msg_ = hint,
             .pos_ = nullptr,
             .traces_ = {},
             .suggestions_ = {}} {}

  // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved,readability-redundant-member-init)
  explicit base_error_t(error_info_t&& info) : err_(std::move(info)), what_{} {}

  // NOLINTNEXTLINE(readability-redundant-member-init)
  explicit base_error_t(const error_info_t& info) : err_(info), what_{} {}

  /** The error message without "error: " prefixed to it. */
  [[nodiscard]] auto message() const -> std::string { return err_.msg_.str(); }

  [[nodiscard]] auto what() const noexcept -> const char* override { return calc_what().c_str(); }

  [[nodiscard]] auto msg() const -> const std::string& { return calc_what(); }

  [[nodiscard]] auto info() const -> const error_info_t& {
    (void)calc_what();
    return err_;
  }

  auto with_exit_status(unsigned int status) -> void { err_.status_ = status; }

  auto at_pos(std::shared_ptr<const pos_t> pos) -> void { err_.pos_ = std::move(pos); }

  auto set_suggestions(const suggestions_t& sug) -> void { err_.suggestions_ = sug; }

  auto set_is_from_expr(bool value) -> void { err_.is_from_expr_ = value; }

  auto push_trace(const trace_t& trace) -> void { err_.traces_.push_front(trace); }

  /**
   * Prepends an item to the error trace, as is usual for extra context.
   *
   * @param pos Nullable source position to put in trace item
   * @param fmt_str Format string, see `hint_fmt_t`
   * @param args... Format string arguments.
   */
  template <typename... args_t>
  // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
  auto add_trace(std::shared_ptr<const pos_t>&& pos, std::string_view fmt_str,
                 const args_t&... args) -> void {
    add_trace(std::move(pos), hint_fmt_t(std::string(fmt_str), args...));
  }

  /**
   * Prepends an item to the error trace, as is usual for extra context.
   *
   * @param pos Nullable source position to put in trace item
   * @param hint Formatted error message
   * @param print Optional, whether to always print (used by `addErrorContext`)
   */
  auto add_trace(std::shared_ptr<const pos_t>&& pos, const hint_fmt_t& hint,
                 trace_print_t print = trace_print_t::default_print) -> void;

  [[nodiscard]] auto has_trace() const -> bool { return !err_.traces_.empty(); }

  [[nodiscard]] auto info() -> const error_info_t& { return err_; }
};

/* NOLINTBEGIN(cppcoreguidelines-macro-usage,readability-identifier-naming,bugprone-macro-parentheses)
 */
#define MAKE_ERROR(newClass, superClass)                                                           \
  class newClass : public superClass {                                                             \
  public:                                                                                          \
    using superClass::superClass;                                                                  \
  }

// Lowercase alias for backward compatibility
#define make_error MAKE_ERROR
/* NOLINTEND(cppcoreguidelines-macro-usage,readability-identifier-naming,bugprone-macro-parentheses)
 */

// NOLINTBEGIN(bugprone-macro-parentheses,readability-identifier-naming)
MAKE_ERROR(Error, base_error_t);
MAKE_ERROR(UsageError, Error);
MAKE_ERROR(UnimplementedError, Error);

/**
 * To use in catch-blocks.
 */
MAKE_ERROR(SystemError, Error);
// NOLINTEND(bugprone-macro-parentheses,readability-identifier-naming)

/**
 * POSIX system error, created using `errno`, `strerror` friends.
 *
 * Throw this, but prefer not to catch this, and catch `SystemError`
 * instead. This allows implementations to freely switch between this
 * and `windows::WinError` without breaking catch blocks.
 *
 * However, it is permissible to catch this and rethrow so long as
 * certain conditions are not met (e.g. to catch only if `err_no =
 * EFooBar`). In that case, try to also catch the equivalent `windows::WinError`
 * code.
 *
 * @todo Rename this to `PosixError` or similar. At this point Windows
 * support is too WIP to justify the code churn, but if it is finished
 * then a better identifier becomes moe worth it.
 */
// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
class sys_error_t : public SystemError {
private:
  int err_no_;

public:
  /**
   * Construct using the explicitly-provided error number. `strerror`
   * will be used to try to add additional information to the message.
   */
  template <typename... args_t>
  sys_error_t(int error_number, const args_t&... args)
      : SystemError(hint_fmt_t("%1%: %2%", uncolored_t(hint_fmt_t(args...).str()),
                               strerror(error_number))), // NOLINT(concurrency-mt-unsafe)
        err_no_(error_number) {}

  /**
   * Construct using the ambient `errno`.
   *
   * Be sure to not perform another `errno`-modifying operation before
   * calling this constructor!
   */
  template <typename... args_t>
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
  sys_error_t(const args_t&... args) : sys_error_t(errno, args...) {}

  [[nodiscard]] auto err_no() const -> int { return err_no_; }
};

#ifdef _WIN32
namespace windows {
class WinError; // NOLINT(readability-identifier-naming)
}
#endif

/**
 * Convenience alias for when we use a `errno`-based error handling
 * function on Unix, and `GetLastError()`-based error handling on on
 * Windows.
 */
using native_sys_error_t =
#ifdef _WIN32
    windows::WinError
#else
    sys_error_t
#endif
    ;

/**
 * Throw an exception for the purpose of checking that exception
 * handling works; see 'initLibUtil()'.
 */
auto throw_exception_self_check() -> void;

/**
 * Print a message and std::terminate().
 */
[[noreturn]]
auto panic(std::string_view msg) -> void;

/**
 * Print a basic error message with source position and std::terminate().
 *
 * @note: This assumes that the logger is operational
 */
[[gnu::noinline, gnu::cold, noreturn]] auto
unreachable(std::source_location loc = std::source_location::current()) -> void;

} // namespace nix

#endif // NIX_UTIL_ERROR_H
