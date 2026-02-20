#pragma once

#include "nix/expr/eval.h"

namespace nix {

struct AbstractNixRepl {
  ref<eval_state_t> state;
  bindings_t* auto_args;

  AbstractNixRepl(ref<eval_state_t> state) : state(state) {}

  virtual ~AbstractNixRepl() {}

  typedef std::vector<std::pair<value_t*, std::string>> AnnotatedValues;

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
                                                 nix::ref<store_t> store, ref<eval_state_t> state,
                                                 std::function<AnnotatedValues()> get_values,
                                                 RunNix* run_nix = nullptr);

  static ReplExitStatus runSimple(ref<eval_state_t> eval_state, const ValMap& extraEnv);

  virtual void init_env() = 0;

  virtual ReplExitStatus main_loop() = 0;
};

} // namespace nix
