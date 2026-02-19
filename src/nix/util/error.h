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
  lvlError = 0,
  lvlWarn,
  lvlNotice,
  lvlInfo,
  lvlTalkative,
  lvlChatty,
  lvlDebug,
  lvlVomit
} verbosity_t;

/**
 * The lines of code surrounding an error.
 */
struct lines_of_code_t {
  std::optional<std::string> prevLineOfCode;
  std::optional<std::string> errLineOfCode;
  std::optional<std::string> nextLineOfCode;
};

/* NOTE: position.hh recursively depends on source-path.hh -> source-accessor.hh
   -> hash.hh -> configuration.hh -> experimental-features.hh -> error.hh -> Pos.
   There are other such cycles.
   Thus, Pos has to be an incomplete type in this header. But since error_info_t/trace_t
   have to refer to Pos, they have to use pointer indirection via std::shared_ptr
   to break the recursive header dependency.
   FIXME: Untangle this mess. Should there be AbstractPos as there used to be before
   4feb7d9f71? */
struct Pos;

void printCodeLines(std::ostream& out, const std::string& prefix, const Pos& errPos,
                    const lines_of_code_t& loc);

/**
 * When a stack frame is printed.
 */
enum struct trace_print_t {
  /**
   * The default behavior; always printed when `--show-trace` is set.
   */
  Default,
  /** Always printed. Produced by `builtins.addErrorContext`. */
  Always,
};

struct trace_t {
  std::shared_ptr<const Pos> pos;
  hint_fmt_t hint;
  trace_print_t print = trace_print_t::Default;
};

inline std::strong_ordering operator<=>(const trace_t& lhs, const trace_t& rhs);

struct error_info_t {
  verbosity_t level;
  hint_fmt_t msg;
  std::shared_ptr<const Pos> pos;
  std::list<trace_t> traces;
  /**
   * Some messages are generated directly by expressions; notably `builtins.warn`, `abort`, `throw`.
   * These may be rendered differently, so that users can distinguish them.
   */
  bool isFromExpr = false;

  /**
   * exit_t status.
   */
  unsigned int status = 1;

  suggestions_t suggestions;

  static std::optional<std::string> programName;
};

std::ostream& showErrorInfo(std::ostream& out, const error_info_t& einfo, bool showTrace);

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
  const std::string& calcWhat() const;

public:
  base_error_t(const base_error_t&) = default;
  base_error_t& operator=(const base_error_t&) = default;
  base_error_t& operator=(base_error_t&&) = default;

  template <typename... Args>
  base_error_t(unsigned int status, const Args&... args)
      : err{.level = lvlError, .msg = hint_fmt_t(args...), .status = status} {}

  template <typename... Args>
  explicit base_error_t(const std::string& fs, const Args&... args)
      : err{.level = lvlError, .msg = hint_fmt_t(fs, args...)} {}

  template <typename... Args>
  base_error_t(const suggestions_t& sug, const Args&... args)
      : err{.level = lvlError, .msg = hint_fmt_t(args...), .suggestions = sug} {}

  base_error_t(hint_fmt_t hint) : err{.level = lvlError, .msg = hint} {}

  base_error_t(error_info_t&& e) : err(std::move(e)) {}

  base_error_t(const error_info_t& e) : err(e) {}

  /** The error message without "error: " prefixed to it. */
  std::string message() { return err.msg.str(); }

  const char* what() const noexcept override { return calcWhat().c_str(); }

  const std::string& msg() const { return calcWhat(); }

  const error_info_t& info() const {
    calcWhat();
    return err;
  }

  void withExitStatus(unsigned int status) { err.status = status; }

  void atPos(std::shared_ptr<const Pos> pos) { err.pos = pos; }

  void pushTrace(trace_t trace) { err.traces.push_front(trace); }

  /**
   * Prepends an item to the error trace, as is usual for extra context.
   *
   * @param pos Nullable source position to put in trace item
   * @param fs Format string, see `hint_fmt_t`
   * @param args... Format string arguments.
   */
  template <typename... Args>
  void addTrace(std::shared_ptr<const Pos>&& pos, std::string_view fs, const Args&... args) {
    addTrace(std::move(pos), hint_fmt_t(std::string(fs), args...));
  }

  /**
   * Prepends an item to the error trace, as is usual for extra context.
   *
   * @param pos Nullable source position to put in trace item
   * @param hint Formatted error message
   * @param print Optional, whether to always print (used by `addErrorContext`)
   */
  void addTrace(std::shared_ptr<const Pos>&& pos, hint_fmt_t hint,
                trace_print_t print = trace_print_t::Default);

  bool hasTrace() const { return !err.traces.empty(); }

  const error_info_t& info() { return err; };
};

#define MakeError(newClass, superClass)                                                            \
  class newClass : public superClass {                                                             \
  public:                                                                                          \
    using superClass::superClass;                                                                  \
  }

MakeError(Error, base_error_t);
MakeError(UsageError, Error);
MakeError(UnimplementedError, Error);

/**
 * To use in catch-blocks.
 */
MakeError(SystemError, Error);

/**
 * POSIX system error, created using `errno`, `strerror` friends.
 *
 * Throw this, but prefer not to catch this, and catch `SystemError`
 * instead. This allows implementations to freely switch between this
 * and `windows::WinError` without breaking catch blocks.
 *
 * However, it is permissible to catch this and rethrow so long as
 * certain conditions are not met (e.g. to catch only if `errNo =
 * EFooBar`). In that case, try to also catch the equivalent `windows::WinError`
 * code.
 *
 * @todo Rename this to `PosixError` or similar. At this point Windows
 * support is too WIP to justify the code churn, but if it is finished
 * then a better identifier becomes moe worth it.
 */
class sys_error_t : public SystemError {
public:
  int errNo;

  /**
   * Construct using the explicitly-provided error number. `strerror`
   * will be used to try to add additional information to the message.
   */
  template <typename... Args>
  sys_error_t(int errNo, const Args&... args) : SystemError(""), errNo(errNo) {
    auto hf = hint_fmt_t(args...);
    err.msg = hint_fmt_t("%1%: %2%", uncolored_t(hf.str()), strerror(errNo));
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
void throwExceptionSelfCheck();

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
