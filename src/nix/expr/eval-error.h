#pragma once

#include "nix/util/error.h"
#include "nix/util/pos-idx.h"

namespace nix {

struct Env;
struct expr_t;
struct value_t;

class eval_state_t;
template <class T>
class EvalErrorBuilder;

/**
 * Base class for all errors that occur during evaluation.
 *
 * Most subclasses should inherit from `EvalError` instead of this class.
 */
class EvalBaseError : public Error {
  template <class T>
  friend class EvalErrorBuilder;

public:
  eval_state_t& state;

  EvalBaseError(eval_state_t& state, error_info_t&& errorInfo) : Error(errorInfo), state(state) {}

  template <typename... args_t>
  explicit EvalBaseError(eval_state_t& state, const std::string& formatString,
                         const args_t&... formatArgs)
      : Error(formatString, formatArgs...), state(state) {}
};

/**
 * `EvalError` is the base class for almost all errors that occur during evaluation.
 *
 * All instances of `EvalError` should show a degree of purity that allows them to be
 * cached in pure mode. This means that they should not depend on the configuration or the overall
 * environment.
 */
make_error(EvalError, EvalBaseError);
make_error(ParseError, Error);
make_error(AssertionError, EvalError);
make_error(ThrownError, AssertionError);
make_error(Abort, EvalError);
make_error(TypeError, EvalError);
make_error(UndefinedVarError, EvalError);
make_error(MissingArgumentError, EvalError);
make_error(InfiniteRecursionError, EvalError);
make_error(IFDError, EvalBaseError);

struct InvalidPathError : public EvalError {
public:
  Path path;

  InvalidPathError(eval_state_t& state, const Path& path)
      : EvalError(state, "path '%s' is not valid", path) {}
};

/**
 * `EvalErrorBuilder`s may only be constructed by `eval_state_t`. The `debugThrow`
 * method must be the final method in any such `EvalErrorBuilder` usage, and it
 * handles deleting the object.
 */
template <class T>
class EvalErrorBuilder final {
  friend class eval_state_t;

  template <typename... args_t>
  explicit EvalErrorBuilder(eval_state_t& state, const args_t&... args) : error(T(state, args...)) {}

public:
  T error;

  [[nodiscard, gnu::noinline]] EvalErrorBuilder<T>& with_exit_status(unsigned int exitStatus);

  [[nodiscard, gnu::noinline]] EvalErrorBuilder<T>& at_pos(pos_idx_t pos);

  [[nodiscard, gnu::noinline]] EvalErrorBuilder<T>& at_pos(value_t& value, pos_idx_t fallback = no_pos);

  [[nodiscard, gnu::noinline]] EvalErrorBuilder<T>& withTrace(pos_idx_t pos,
                                                              const std::string_view text);

  [[nodiscard, gnu::noinline]] EvalErrorBuilder<T>& withFrameTrace(pos_idx_t pos,
                                                                   const std::string_view text);

  [[nodiscard, gnu::noinline]] EvalErrorBuilder<T>& withSuggestions(suggestions_t& s);

  [[nodiscard, gnu::noinline]] EvalErrorBuilder<T>& withFrame(const Env& e, const expr_t& ex);

  [[nodiscard, gnu::noinline]] EvalErrorBuilder<T>& add_trace(pos_idx_t pos, hint_fmt_t hint);

  [[nodiscard, gnu::noinline]] EvalErrorBuilder<T>& setIsFromExpr();

  template <typename... args_t>
  [[nodiscard, gnu::noinline]] EvalErrorBuilder<T>&
  add_trace(pos_idx_t pos, std::string_view formatString, const args_t&... formatArgs);

  /**
   * Delete the `EvalErrorBuilder` and throw the underlying exception.
   */
  [[gnu::noinline, gnu::noreturn]] void debugThrow();

  /**
   * A programming error or fatal condition occurred. Abort the process for core dump and debugging.
   * This does not print a proper backtrace, because unwinding the stack is destructive.
   */
  [[gnu::noinline, gnu::noreturn]] void panic();
};

} // namespace nix
