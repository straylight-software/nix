#include "nix/cmd/command.h"

struct cmd_store_t : nix::NixMultiCommand {
  cmd_store_t() : NixMultiCommand("store", nix::RegisterCommand::getCommandsFor({"store"})) {
    get_aliases() = {
        {"ping", {nix::alias_status_t::deprecated, {"info"}}},
    };
  }

  std::string description() override { return "manipulate a Nix store"; }

  nix::category_t category() override { return nix::catUtility; }
};

static auto r_cmd_store = nix::registerCommand<cmd_store_t>("store");
