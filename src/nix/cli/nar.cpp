#include "nix/cmd/command.h"

struct cmd_nar_t : nix::NixMultiCommand {
  cmd_nar_t() : NixMultiCommand("nar", nix::RegisterCommand::getCommandsFor({"nar"})) {}

  std::string description() override { return "create or inspect NAR files"; }

  std::string doc() override {
    return
#include "nar.md"
        ;
  }

  nix::category_t category() override { return nix::catUtility; }
};

static auto r_cmd_nar = nix::registerCommand<cmd_nar_t>("nar");
