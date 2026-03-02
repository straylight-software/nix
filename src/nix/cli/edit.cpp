#include <unistd.h>

#include "nix/cmd/command-installable-value.h"
#include "nix/cmd/editor-for.h"
#include "nix/expr/attr-path.h"
#include "nix/expr/eval.h"
#include "nix/main/shared.h"
#include "nix/util/current-process.h"

struct cmd_edit_t : nix::InstallableValueCommand {
  std::string description() override {
    return "open the Nix expression of a Nix package in $EDITOR";
  }

  std::string doc() override {
    return
#include "edit.md"
        ;
  }

  category_t category() override { return nix::catSecondary; }

  void run(nix::ref<nix::store_t> store, nix::ref<nix::InstallableValue> installable) override {
    auto state = getEvalState();

    const auto [file, line] = [&] {
      auto [v, pos] = installable->toValue(*state);

      try {
        return nix::find_package_filename(*state, *v, installable->what());
      } catch (nix::NoPositionInfo&) {
        throw nix::Error("cannot find position information for '%s", installable->what());
      }
    }();

    nix::logger->stop();

    auto args = nix::editor_for(file, line);

    nix::restore_process_context();

    execvp(args.front().c_str(), nix::strings_to_char_ptrs(args).data());

    std::string command;
    for (const auto& arg : args) {
      command += " '" + arg + "'";
    }
    throw nix::sys_error_t("cannot run command%s", command);
  }
};

static auto r_cmd_edit = nix::registerCommand<cmd_edit_t>("edit");
