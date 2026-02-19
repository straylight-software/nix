#include "nix/cmd/command.h"

using namespace nix;

struct cmd_derivation_t : NixMultiCommand {
  cmd_derivation_t()
      : NixMultiCommand("derivation", RegisterCommand::getCommandsFor({"derivation"})) {}

  std::string description() override {
    return "Work with derivations, Nix's notion of a build plan.";
  }

  category_t category() override { return catUtility; }
};

static auto r_cmd_derivation = registerCommand<cmd_derivation_t>("derivation");
