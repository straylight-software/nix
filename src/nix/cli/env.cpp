#include <queue>

#include <boost/unordered/unordered_flat_set.hpp>

#include "nix/cmd/command.h"
#include "nix/expr/eval.h"
#include "nix/util/environment-variables.h"
#include "nix/util/executable-path.h"
#include "nix/util/mounted-source-accessor.h"
#include "nix/util/strings.h"
#include "run.h"

struct cmd_env_t : nix::NixMultiCommand {
  cmd_env_t() : NixMultiCommand("env", nix::RegisterCommand::getCommandsFor({"env"})) {}

  std::string description() override { return "manipulate the process environment"; }

  category_t category() override { return nix::catUtility; }
};

static auto r_cmd_env = nix::registerCommand<cmd_env_t>("env");

struct cmd_shell_t : nix::InstallablesCommand, nix::MixEnvironment {
  using InstallablesCommand::run;

  std::vector<std::string> command = {nix::get_env("SHELL").value_or("bash")};

  cmd_shell_t() {
    add_flag({
        .long_name = "command",
        .short_name = 'c',
        .description = "Command and arguments to be executed, defaulting to `$SHELL`",
        .labels = {"command", "args"},
        .handler = {[&](std::vector<std::string> ss) {
          if (ss.empty())
            throw nix::UsageError("--command requires at least one argument");
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

  void run(nix::ref<nix::store_t> store, nix::Installables&& installables) override {
    auto state = getEvalState();

    auto out_paths = nix::Installable::toStorePaths(getEvalStore(), store, nix::Realise::Outputs,
                                                    nix::OperateOn::Output, installables);

    boost::unordered_flat_set<nix::store_path_t, std::hash<nix::store_path_t>> done;
    std::queue<nix::store_path_t> todo;
    for (auto& path : out_paths)
      todo.push(path);

    setEnviron();

    std::vector<std::string> pathAdditions;

    while (!todo.empty()) {
      auto path = todo.front();
      todo.pop();
      if (!done.insert(path).second)
        continue;

      auto bin_dir =
          state->storeFS->resolve_symlinks(nix::canon_path_t(store->printStorePath(path)) / "bin");
      if (!store->isInStore(bin_dir.abs()))
        throw nix::Error("path '%s' is not in the Nix store", bin_dir);

      pathAdditions.push_back(bin_dir.abs());

      auto prop_path =
          state->storeFS->resolve_symlinks(nix::canon_path_t(store->printStorePath(path)) /
                                           "nix-support" / "propagated-user-env-packages");
      if (auto st = state->storeFS->maybe_lstat(prop_path);
          st && st->type == nix::source_accessor_t::t_regular) {
        for (auto& p : nix::tokenize_string<nix::Paths>(state->storeFS->read_file(prop_path)))
          todo.push(store->parseStorePath(p));
      }
    }

    // TODO: split losslessly; empty means .
    auto unix_path = nix::executable_path_t::load();
    unix_path.directories.insert(unix_path.directories.begin(), pathAdditions.begin(),
                                 pathAdditions.end());
    auto unix_path_string = unix_path.render();
    nix::set_env_os(OS_STR("PATH"), unix_path_string.c_str());

    nix::strings_t args;
    for (auto& arg : command)
      args.push_back(arg);

    // Release our references to eval caches to ensure they are persisted to disk, because
    // we are about to exec out of this process without running C++ destructors.
    state->evalCaches.clear();

    exec_program_in_store(store, nix::use_lookup_path_t::use, *command.begin(), args);
  }
};

static auto r_cmd_shell = nix::registerCommand2<cmd_shell_t>({"env", "shell"});
