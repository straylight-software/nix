#include "nix/cmd/command.h"

using namespace nix;

struct cmd_store_t : NixMultiCommand {
  cmd_store_t() : NixMultiCommand("store", RegisterCommand::getCommandsFor({"store"})) {
    get_aliases() = {
        {"ping", {alias_status_t::deprecated, {"info"}}},
    };
  }

  std::string description() override { return "manipulate a Nix store"; }

  category_t category() override { return catUtility; }
};

static auto r_cmd_store = registerCommand<cmd_store_t>("store");
