// FIXME: rename to 'nix plan add' or 'nix derivation add'?

#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/main/common-args.h"
#include "nix/store/derivations.h"
#include "nix/store/store-api.h"
#include "nix/util/archive.h"

struct cmd_add_derivation_t : nix::MixDryRun, nix::StoreCommand {
  std::string description() override { return "Add a store derivation"; }

  std::string doc() override {
    return
#include "derivation-add.md"
        ;
  }

  nix::category_t category() override { return nix::catUtility; }

  void run(nix::ref<nix::store_t> store) override {
    auto json = nlohmann::json::parse(nix::drain_fd(STDIN_FILENO));

    auto drv = nix::derivation_t::parseJsonAndValidate(*store, json);

    auto drv_path = nix::write_derivation(*store, drv, nix::NoRepair, /* read only */ dry_run);

    nix::write_derivation(*store, drv, nix::NoRepair, dry_run);

    nix::logger->cout("%s", store->printStorePath(drv_path));
  }
};

static auto r_cmd_add_derivation =
    nix::registerCommand2<cmd_add_derivation_t>({"derivation", "add"});
