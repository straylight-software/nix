#include "nix/expr/eval-error.h"

#include "nix/expr/eval.h"
#include "nix/expr/value.h"

namespace nix {

template <class T>
EvalErrorBuilder<T>& EvalErrorBuilder<T>::with_exit_status(unsigned int exitStatus) {
  error.with_exit_status(exitStatus);
  return *this;
}

template <class T>
EvalErrorBuilder<T>& EvalErrorBuilder<T>::at_pos(pos_idx_t pos) {
  error.at_pos(error.state.positions[pos]);
  return *this;
}

template <class T>
EvalErrorBuilder<T>& EvalErrorBuilder<T>::at_pos(Value& value, pos_idx_t fallback) {
  return at_pos(value.determinePos(fallback));
}

template <class T>
EvalErrorBuilder<T>& EvalErrorBuilder<T>::withTrace(pos_idx_t pos, const std::string_view text) {
  error.add_trace(error.state.positions[pos], text);
  return *this;
}

template <class T>
EvalErrorBuilder<T>& EvalErrorBuilder<T>::withSuggestions(suggestions_t& s) {
  error.set_suggestions(s);
  return *this;
}

template <class T>
EvalErrorBuilder<T>& EvalErrorBuilder<T>::withFrame(const Env& env, const Expr& expr) {
  // NOTE: This is abusing side-effects.
  // TODO: check compatibility with nested debugger calls.
  // TODO: What side-effects??
  error.state.debugTraces.push_front(
      DebugTrace{.pos = expr.getPos(),
                 .expr = expr,
                 .env = env,
                 .hint = hint_fmt_t("Fake frame for debugging purposes"),
                 .isError = true});
  return *this;
}

template <class T>
EvalErrorBuilder<T>& EvalErrorBuilder<T>::add_trace(pos_idx_t pos, hint_fmt_t hint) {
  error.add_trace(error.state.positions[pos], hint);
  return *this;
}

template <class T>
template <typename... Args>
EvalErrorBuilder<T>& EvalErrorBuilder<T>::add_trace(pos_idx_t pos, std::string_view formatString,
                                                    const Args&... formatArgs) {
  add_trace(error.state.positions[pos], hint_fmt_t(std::string(formatString), formatArgs...));
  return *this;
}

template <class T>
EvalErrorBuilder<T>& EvalErrorBuilder<T>::setIsFromExpr() {
  error.set_is_from_expr(true);
  return *this;
}

template <class T>
void EvalErrorBuilder<T>::debugThrow() {
  error.state.runDebugRepl(&error);

  // `EvalState` is the only class that can construct an `EvalErrorBuilder`,
  // and it does so in dynamic storage. This is the final method called on
  // any such instance and must delete itself before throwing the underlying
  // error.
  auto error = std::move(this->error);
  delete this;

  throw error;
}

template <class T>
void EvalErrorBuilder<T>::panic() {
  logError(error.info());
  printError("This is a bug! An unexpected condition occurred, causing the Nix evaluator to have "
             "to stop. If you could share a reproducible example or a core dump, please open an "
             "issue at https://github.com/NixOS/nix/issues");
  abort();
}

template class EvalErrorBuilder<EvalBaseError>;
template class EvalErrorBuilder<EvalError>;
template class EvalErrorBuilder<AssertionError>;
template class EvalErrorBuilder<ThrownError>;
template class EvalErrorBuilder<Abort>;
template class EvalErrorBuilder<TypeError>;
template class EvalErrorBuilder<UndefinedVarError>;
template class EvalErrorBuilder<MissingArgumentError>;
template class EvalErrorBuilder<InfiniteRecursionError>;
template class EvalErrorBuilder<InvalidPathError>;
template class EvalErrorBuilder<IFDError>;

} // namespace nix
