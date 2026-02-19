#include <queue>

#include <boost/unordered/unordered_flat_set.hpp>

#include "nix/cmd/command.h"
#include "nix/expr/eval.h"
#include "nix/util/environment-variables.h"
#include "nix/util/executable-path.h"
#include "nix/util/mounted-source-accessor.h"
#include "nix/util/strings.h"
#include "run.h"

using namespace nix;

struct cmd_env_t : NixMultiCommand {
  cmd_env_t() : NixMultiCommand("env", RegisterCommand::getCommandsFor({"env"})) {}

  std::string description() override { return "manipulate the process environment"; }

  category_t category() override { return catUtility; }
};

static auto rCmdEnv = registerCommand<cmd_env_t>("env");

struct cmd_shell_t : InstallablesCommand, MixEnvironment {
  using InstallablesCommand::run;

  std::vector<std::string> command = {getEnv("SHELL").value_or("bash")};

  cmd_shell_t() {
    addFlag({
        .longName = "command",
        .shortName = 'c',
        .description = "Command and arguments to be executed, defaulting to `$SHELL`",
        .labels = {"command", "args"},
        .handler = {[&](std::vector<std::string> ss) {
          if (ss.empty())
            throw UsageError("--command requires at least one argument");
          command = ss;
        }},
    });
  }

  std::string description() override {
    return "run a shell in which the specified packages are available";
  }

  std::string doc() override {
    return
#include "shell.md"
        ;
  }

  void run(ref<Store> store, Installables&& installables) override {
    auto state = getEvalState();

    auto outPaths = Installable::toStorePaths(getEvalStore(), store, Realise::Outputs,
                                              OperateOn::Output, installables);

    boost::unordered_flat_set<StorePath, std::hash<StorePath>> done;
    std::queue<StorePath> todo;
    for (auto& path : outPaths)
      todo.push(path);

    setEnviron();

    std::vector<std::string> pathAdditions;

    while (!todo.empty()) {
      auto path = todo.front();
      todo.pop();
      if (!done.insert(path).second)
        continue;

      auto binDir = state->storeFS->resolveSymlinks(canon_path_t(store->printStorePath(path)) / "bin");
      if (!store->isInStore(binDir.abs()))
        throw Error("path '%s' is not in the Nix store", binDir);

      pathAdditions.push_back(binDir.abs());

      auto propPath = state->storeFS->resolveSymlinks(
          canon_path_t(store->printStorePath(path)) / "nix-support" / "propagated-user-env-packages");
      if (auto st = state->storeFS->maybeLstat(propPath);
          st && st->type == SourceAccessor::tRegular) {
        for (auto& p : tokenizeString<Paths>(state->storeFS->readFile(propPath)))
          todo.push(store->parseStorePath(p));
      }
    }

    // TODO: split losslessly; empty means .
    auto unixPath = executable_path_t::load();
    unixPath.directories.insert(unixPath.directories.begin(), pathAdditions.begin(),
                                pathAdditions.end());
    auto unixPathString = unixPath.render();
    setEnvOs(OS_STR("PATH"), unixPathString.c_str());

    strings_t args;
    for (auto& arg : command)
      args.push_back(arg);

    // Release our references to eval caches to ensure they are persisted to disk, because
    // we are about to exec out of this process without running C++ destructors.
    state->evalCaches.clear();

    execProgramInStore(store, use_lookup_path_t::Use, *command.begin(), args);
  }
};

static auto rCmdShell = registerCommand2<cmd_shell_t>({"env", "shell"});
