#include "nix/cmd/command.h"

using namespace nix;

struct cmd_nar_t : NixMultiCommand {
  cmd_nar_t() : NixMultiCommand("nar", RegisterCommand::getCommandsFor({"nar"})) {}

  std::string description() override { return "create or inspect NAR files"; }

  std::string doc() override {
    return
#include "nar.md"
        ;
  }

  category_t category() override { return catUtility; }
};

static auto rCmdNar = registerCommand<cmd_nar_t>("nar");
