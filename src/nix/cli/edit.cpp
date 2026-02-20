#include <unistd.h>

#include "nix/cmd/command-installable-value.h"
#include "nix/cmd/editor-for.h"
#include "nix/expr/attr-path.h"
#include "nix/expr/eval.h"
#include "nix/main/shared.h"
#include "nix/util/current-process.h"

using namespace nix;

struct cmd_edit_t : InstallableValueCommand {
  std::string description() override {
    return "open the Nix expression of a Nix package in $EDITOR";
  }

  std::string doc() override {
    return
#include "edit.md"
        ;
  }

  category_t category() override { return catSecondary; }

  void run(ref<store_t> store, ref<InstallableValue> installable) override {
    auto state = getEvalState();

    const auto [file, line] = [&] {
      auto [v, pos] = installable->toValue(*state);

      try {
        return find_package_filename(*state, *v, installable->what());
      } catch (NoPositionInfo&) {
        throw Error("cannot find position information for '%s", installable->what());
      }
    }();

    logger->stop();

    auto args = editor_for(file, line);

    restore_process_context();

    execvp(args.front().c_str(), strings_to_char_ptrs(args).data());

    std::string command;
    for (const auto& arg : args)
      command += " '" + arg + "'";
    throw sys_error_t("cannot run command%s", command);
  }
};

static auto r_cmd_edit = registerCommand<cmd_edit_t>("edit");
