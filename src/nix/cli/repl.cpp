#include "nix/cmd/repl.h"

#include "nix/cmd/command.h"
#include "nix/cmd/installable-value.h"
#include "nix/expr/eval-settings.h"
#include "nix/expr/eval.h"
#include "nix/store/globals.h"
#include "nix/store/store-open.h"
#include "nix/util/config-global.h"
#include "nix/util/processes.h"
#include "self-exe.h"

namespace nix {

void run_nix(const std::string& program, const strings_t& args,
             const std::optional<std::string>& input = {}) {
  auto subprocess_env = get_env();
  subprocess_env["NIX_CONFIG"] = global_config.to_key_value();
  // isInteractive avoid grabling interactive commands
  run_program2(run_options_t{
      .program = get_nix_bin(program).string(),
      .args = args,
      .environment = subprocess_env,
      .input = input,
      .is_interactive = true,
  });

  return;
}

struct cmd_repl_t : RawInstallablesCommand {
  cmd_repl_t() { eval_settings.pureEval = false; }

  /**
   * This command is stable before the others
   */
  std::optional<experimental_feature_t> experimental_feature() override { return std::nullopt; }

  std::vector<std::string> files;

  strings_t getDefaultFlakeAttrPaths() override { return {""}; }

  bool force_impure_by_default() override { return true; }

  std::string description() override {
    return "start an interactive environment for evaluating Nix expressions";
  }

  std::string doc() override {
    return
#include "repl.md"
        ;
  }

  void applyDefaultInstallables(std::vector<std::string>& raw_installables) override {
    if (raw_installables.empty() && (file.has_value() || expr.has_value())) {
      raw_installables.push_back(".");
    }
  }

  void run(ref<store_t> store, std::vector<std::string>&& raw_installables) override {
    auto state = getEvalState();
    auto get_values = [&]() -> AbstractNixRepl::AnnotatedValues {
      auto installables = parseInstallables(store, raw_installables);
      AbstractNixRepl::AnnotatedValues values;
      for (auto& installable_ : installables) {
        auto& installable = InstallableValue::require(*installable_);
        auto what = installable.what();
        if (file) {
          auto [val, pos] = installable.toValue(*state);
          auto what = installable.what();
          state->forceValue(*val, pos);
          auto auto_args = getAutoArgs(*state);
          auto valPost = state->allocValue();
          state->autoCallFunction(*auto_args, *val, *valPost);
          state->forceValue(*valPost, pos);
          values.push_back({valPost, what});
        } else {
          auto [val, pos] = installable.toValue(*state);
          values.push_back({val, what});
        }
      }
      return values;
    };
    auto repl = AbstractNixRepl::create(lookup_path, open_store(), state, get_values, run_nix);
    repl->auto_args = getAutoArgs(*repl->state);
    repl->init_env();
    repl->main_loop();
  }
};

static auto r_cmd_repl = registerCommand<cmd_repl_t>("repl");

} // namespace nix
