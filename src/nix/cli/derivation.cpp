#include "nix/cmd/command.h"

struct cmd_derivation_t : nix::NixMultiCommand {
  cmd_derivation_t()
      : NixMultiCommand("derivation", nix::RegisterCommand::getCommandsFor({"derivation"})) {}

  std::string description() override {
    return "Work with derivations, Nix's notion of a build plan.";
  }

  nix::category_t category() override { return nix::catUtility; }
};

static auto r_cmd_derivation = nix::registerCommand<cmd_derivation_t>("derivation");
