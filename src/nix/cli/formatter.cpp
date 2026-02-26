#include "nix/cmd/command.h"
#include "nix/cmd/installable-derived-path.h"
#include "nix/cmd/installable-flake.h"
#include "nix/cmd/installable-value.h"
#include "nix/expr/eval.h"
#include "nix/store/globals.h"
#include "nix/store/local-fs-store.h"
#include "nix/util/environment-variables.h"
#include "run.h"

struct cmd_formatter_t : nix::NixMultiCommand {
  cmd_formatter_t()
      : nix::NixMultiCommand("formatter", nix::RegisterCommand::getCommandsFor({"formatter"})) {}

  std::string description() override { return "build or run the formatter"; }

  nix::category_t category() override { return nix::catSecondary; }
};

static auto r_cmd_formatter = nix::registerCommand<cmd_formatter_t>("formatter");

/** common_t implementation bits for the `nix formatter` subcommands. */
struct mix_formatter_t : nix::SourceExprCommand {
  nix::strings_t getDefaultFlakeAttrPaths() override {
    return nix::strings_t{"formatter." + nix::settings.thisSystem.get()};
  }

  nix::strings_t getDefaultFlakeAttrPathPrefixes() override { return nix::strings_t{}; }
};

struct cmd_formatter_run_t : mix_formatter_t, nix::MixJSON {
  std::vector<std::string> args;

  cmd_formatter_run_t() { expect_args({.label = "args", .handler = {&args}}); }

  std::string description() override { return "reformat your code in the standard style"; }

  std::string doc() override {
    return
#include "formatter-run.md"
        ;
  }

  category_t category() override { return catSecondary; }

  void run(ref<store_t> store) override {
    auto eval_state = getEvalState();
    auto eval_store = getEvalStore();

    auto installable_ = parseInstallable(store, ".").cast<nix::InstallableFlake>();
    auto& installable = nix::InstallableValue::require(*installable_);
    auto app = installable.toApp(*eval_state).resolve(eval_store, store);

    auto maybe_flake_dir = installable_->flake_ref.input.get_source_path();
    assert(maybe_flake_dir.has_value());
    auto flake_dir = maybe_flake_dir.value();

    nix::strings_t program_args{app.program.string()};

    // Propagate arguments from the CLI
    for (auto& i : args) {
      program_args.push_back(i);
    }

    // Add the path to the flake as an environment variable. This enables formatters to format the
    // entire flake even if run from a subdirectory.
    nix::string_map_t env = nix::get_env();
    env["PRJ_ROOT"] = flake_dir.string();

    // Release our references to eval caches to ensure they are persisted to disk, because
    // we are about to exec out of this process without running C++ destructors.
    eval_state->evalCaches.clear();

    nix::exec_program_in_store(store, nix::use_lookup_path_t::dont_use, app.program.string(),
                               program_args,
                               std::nullopt, // Use default system
                               env);
  };
};

static auto r_formatter_run = nix::registerCommand2<cmd_formatter_run_t>({"formatter", "run"});

struct cmd_formatter_build_t : mix_formatter_t, nix::MixOutLinkByDefault {
  cmd_formatter_build_t() {}

  std::string description() override { return "build the current flake's formatter"; }

  std::string doc() override {
    return
#include "formatter-build.md"
        ;
  }

  nix::category_t category() override { return nix::catSecondary; }

  void run(nix::ref<nix::store_t> store) override {
    auto eval_state = getEvalState();
    auto eval_store = getEvalStore();

    auto installable_ = parseInstallable(store, ".");
    auto& installable = nix::InstallableValue::require(*installable_);
    auto unresolved_app = installable.toApp(*eval_state);
    auto app = unresolved_app.resolve(eval_store, store);
    auto buildables = unresolved_app.build(eval_store, store);
    createOutLinksMaybe(buildables, store);

    nix::logger->cout("%s", app.program);
  };
};

static auto r_formatter_build =
    nix::registerCommand2<cmd_formatter_build_t>({"formatter", "build"});

struct cmd_fmt_t : cmd_formatter_run_t {
  void run(nix::ref<nix::store_t> store) override { cmd_formatter_run_t::run(store); }
};

static auto r_fmt = nix::registerCommand<cmd_fmt_t>("fmt");
