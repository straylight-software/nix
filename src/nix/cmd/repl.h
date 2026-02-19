#pragma once

#include "nix/expr/eval.h"

namespace nix {

struct AbstractNixRepl {
  ref<EvalState> state;
  Bindings* auto_args;

  AbstractNixRepl(ref<EvalState> state) : state(state) {}

  virtual ~AbstractNixRepl() {}

  typedef std::vector<std::pair<Value*, std::string>> AnnotatedValues;

  /**
   * Run a nix executable
   *
   * @todo this is a layer violation
   *
   * @param program_name Name of the command, e.g. `nix` or `nix-env`.
   * @param args aguments to the command.
   */
  using RunNix = void(const std::string& program_name, const strings_t& args,
                      const std::optional<std::string>& input);

  /**
   * @param run_nix Function to run the nix CLI to support various
   * `:<something>` commands. Optional; if not provided,
   * everything else will still work fine, but those commands won't.
   */
  static std::unique_ptr<AbstractNixRepl> create(const LookupPath& lookup_path,
                                                 nix::ref<Store> store, ref<EvalState> state,
                                                 std::function<AnnotatedValues()> get_values,
                                                 RunNix* run_nix = nullptr);

  static ReplExitStatus runSimple(ref<EvalState> eval_state, const ValMap& extraEnv);

  virtual void init_env() = 0;

  virtual ReplExitStatus main_loop() = 0;
};

} // namespace nix
