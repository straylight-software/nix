#include "nix/cmd/command.h"
#include "nix/cmd/installable-derived-path.h"
#include "nix/cmd/installable-flake.h"
#include "nix/cmd/installable-value.h"
#include "nix/expr/eval.h"
#include "nix/store/globals.h"
#include "nix/store/local-fs-store.h"
#include "nix/util/environment-variables.h"
#include "run.h"

using namespace nix;

struct cmd_formatter_t : NixMultiCommand {
  cmd_formatter_t() : NixMultiCommand("formatter", RegisterCommand::getCommandsFor({"formatter"})) {}

  std::string description() override { return "build or run the formatter"; }

  category_t category() override { return catSecondary; }
};

static auto rCmdFormatter = registerCommand<cmd_formatter_t>("formatter");

/** common_t implementation bits for the `nix formatter` subcommands. */
struct mix_formatter_t : SourceExprCommand {
  strings_t getDefaultFlakeAttrPaths() override {
    return strings_t{"formatter." + settings.thisSystem.get()};
  }

  strings_t getDefaultFlakeAttrPathPrefixes() override { return strings_t{}; }
};

struct cmd_formatter_run_t : mix_formatter_t, MixJSON {
  std::vector<std::string> args;

  cmd_formatter_run_t() { expectArgs({.label = "args", .handler = {&args}}); }

  std::string description() override { return "reformat your code in the standard style"; }

  std::string doc() override {
    return
#include "formatter-run.md"
        ;
  }

  category_t category() override { return catSecondary; }

  void run(ref<Store> store) override {
    auto evalState = getEvalState();
    auto evalStore = getEvalStore();

    auto installable_ = parseInstallable(store, ".").cast<InstallableFlake>();
    auto& installable = InstallableValue::require(*installable_);
    auto app = installable.toApp(*evalState).resolve(evalStore, store);

    auto maybeFlakeDir = installable_->flakeRef.input.getSourcePath();
    assert(maybeFlakeDir.has_value());
    auto flakeDir = maybeFlakeDir.value();

    strings_t programArgs{app.program.string()};

    // Propagate arguments from the CLI
    for (auto& i : args) {
      programArgs.push_back(i);
    }

    // Add the path to the flake as an environment variable. This enables formatters to format the
    // entire flake even if run from a subdirectory.
    string_map_t env = getEnv();
    env["PRJ_ROOT"] = flakeDir.string();

    // Release our references to eval caches to ensure they are persisted to disk, because
    // we are about to exec out of this process without running C++ destructors.
    evalState->evalCaches.clear();

    execProgramInStore(store, use_lookup_path_t::DontUse, app.program.string(), programArgs,
                       std::nullopt, // Use default system
                       env);
  };
};

static auto rFormatterRun = registerCommand2<cmd_formatter_run_t>({"formatter", "run"});

struct cmd_formatter_build_t : mix_formatter_t, MixOutLinkByDefault {
  cmd_formatter_build_t() {}

  std::string description() override { return "build the current flake's formatter"; }

  std::string doc() override {
    return
#include "formatter-build.md"
        ;
  }

  category_t category() override { return catSecondary; }

  void run(ref<Store> store) override {
    auto evalState = getEvalState();
    auto evalStore = getEvalStore();

    auto installable_ = parseInstallable(store, ".");
    auto& installable = InstallableValue::require(*installable_);
    auto unresolvedApp = installable.toApp(*evalState);
    auto app = unresolvedApp.resolve(evalStore, store);
    auto buildables = unresolvedApp.build(evalStore, store);
    createOutLinksMaybe(buildables, store);

    logger->cout("%s", app.program);
  };
};

static auto rFormatterBuild = registerCommand2<cmd_formatter_build_t>({"formatter", "build"});

struct cmd_fmt_t : cmd_formatter_run_t {
  void run(ref<Store> store) override { cmd_formatter_run_t::run(store); }
};

static auto rFmt = registerCommand<cmd_fmt_t>("fmt");
