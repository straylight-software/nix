#pragma once
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

#include <cstring>
#include <list>
#include <memory>
#include <optional>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "nix/util/fmt.h"
#include "nix/util/suggestions.h"

namespace nix {

typedef enum {
  lvl_error = 0,
  lvl_warn,
  lvl_notice,
  lvl_info,
  lvl_talkative,
  lvl_chatty,
  lvl_debug,
  lvl_vomit
} verbosity_t;

/**
 * The lines of code surrounding an error.
 */
struct lines_of_code_t {
  std::optional<std::string> prev_line_of_code;
  std::optional<std::string> err_line_of_code;
  std::optional<std::string> next_line_of_code;
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

void print_code_lines(std::ostream& out, const std::string& prefix, const pos_t& err_pos,
                    const lines_of_code_t& loc);

/**
 * When a stack frame is printed.
 */
enum struct trace_print_t {
  /**
   * The default behavior; always printed when `--show-trace` is set.
   */
  Default,
  /** always printed. Produced by `builtins.addErrorContext`. */
  always,
};

struct trace_t {
  std::shared_ptr<const pos_t> pos;
  hint_fmt_t hint;
  trace_print_t print = trace_print_t::Default;
};

inline std::strong_ordering operator<=>(const trace_t& lhs, const trace_t& rhs);

struct error_info_t {
  verbosity_t level;
  hint_fmt_t msg;
  std::shared_ptr<const pos_t> pos;
  std::list<trace_t> traces;
  /**
   * Some messages are generated directly by expressions; notably `builtins.warn`, `abort`, `throw`.
   * These may be rendered differently, so that users can distinguish them.
   */
  bool is_from_expr = false;

  /**
   * exit_t status.
   */
  unsigned int status = 1;

  suggestions_t suggestions;

  static std::optional<std::string> program_name;
};

std::ostream& show_error_info(std::ostream& out, const error_info_t& einfo, bool show_trace);

/**
 * base_error_t should generally not be caught, as it has Interrupted as
 * a subclass. Catch Error instead.
 */
class base_error_t : public std::exception {
protected:
  mutable error_info_t err;

  /**
   * Cached formatted contents of `err.msg`.
   */
  mutable std::optional<std::string> what_;
  /**
   * Format `err.msg` and set `what_` to the resulting value.
   */
  const std::string& calc_what() const;

public:
  base_error_t(const base_error_t&) = default;
  base_error_t& operator=(const base_error_t&) = default;
  base_error_t& operator=(base_error_t&&) = default;

  template <typename... Args>
  base_error_t(unsigned int status, const Args&... args)
      : err{.level = lvl_error, .msg = hint_fmt_t(args...), .status = status} {}

  template <typename... Args>
  explicit base_error_t(const std::string& fs, const Args&... args)
      : err{.level = lvl_error, .msg = hint_fmt_t(fs, args...)} {}

  template <typename... Args>
  base_error_t(const suggestions_t& sug, const Args&... args)
      : err{.level = lvl_error, .msg = hint_fmt_t(args...), .suggestions = sug} {}

  base_error_t(hint_fmt_t hint) : err{.level = lvl_error, .msg = hint} {}

  base_error_t(error_info_t&& e) : err(std::move(e)) {}

  base_error_t(const error_info_t& e) : err(e) {}

  /** The error message without "error: " prefixed to it. */
  std::string message() { return err.msg.str(); }

  const char* what() const noexcept override { return calc_what().c_str(); }

  const std::string& msg() const { return calc_what(); }

  const error_info_t& info() const {
    calc_what();
    return err;
  }

  void with_exit_status(unsigned int status) { err.status = status; }

  void at_pos(std::shared_ptr<const pos_t> pos) { err.pos = pos; }

  void push_trace(trace_t trace) { err.traces.push_front(trace); }

  /**
   * Prepends an item to the error trace, as is usual for extra context.
   *
   * @param pos Nullable source position to put in trace item
   * @param fs Format string, see `hint_fmt_t`
   * @param args... Format string arguments.
   */
  template <typename... Args>
  void add_trace(std::shared_ptr<const pos_t>&& pos, std::string_view fs, const Args&... args) {
    add_trace(std::move(pos), hint_fmt_t(std::string(fs), args...));
  }

  /**
   * Prepends an item to the error trace, as is usual for extra context.
   *
   * @param pos Nullable source position to put in trace item
   * @param hint Formatted error message
   * @param print Optional, whether to always print (used by `addErrorContext`)
   */
  void add_trace(std::shared_ptr<const pos_t>&& pos, hint_fmt_t hint,
                trace_print_t print = trace_print_t::Default);

  bool has_trace() const { return !err.traces.empty(); }

  const error_info_t& info() { return err; };
};

#define make_error(newClass, superClass)                                                            \
  class newClass : public superClass {                                                             \
  public:                                                                                          \
    using superClass::superClass;                                                                  \
  }

make_error(Error, base_error_t);
make_error(UsageError, Error);
make_error(UnimplementedError, Error);

/**
 * To use in catch-blocks.
 */
make_error(SystemError, Error);

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
class sys_error_t : public SystemError {
public:
  int err_no;

  /**
   * Construct using the explicitly-provided error number. `strerror`
   * will be used to try to add additional information to the message.
   */
  template <typename... Args>
  sys_error_t(int err_no, const Args&... args) : SystemError(""), err_no(err_no) {
    auto hf = hint_fmt_t(args...);
    err.msg = hint_fmt_t("%1%: %2%", uncolored_t(hf.str()), strerror(err_no));
  }

  /**
   * Construct using the ambient `errno`.
   *
   * Be sure to not perform another `errno`-modifying operation before
   * calling this constructor!
   */
  template <typename... Args>
  sys_error_t(const Args&... args) : sys_error_t(errno, args...) {}
};

#ifdef _WIN32
namespace windows {
class WinError;
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
void throw_exception_self_check();

/**
 * Print a message and std::terminate().
 */
[[noreturn]]
void panic(std::string_view msg);

/**
 * Print a basic error message with source position and std::terminate().
 *
 * @note: This assumes that the logger is operational
 */
[[gnu::noinline, gnu::cold, noreturn]] void
unreachable(std::source_location loc = std::source_location::current());

} // namespace nix
