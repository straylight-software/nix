// FIXME: rename to 'nix plan add' or 'nix derivation add'?

#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/main/common-args.h"
#include "nix/store/derivations.h"
#include "nix/store/store-api.h"
#include "nix/util/archive.h"

using namespace nix;
using json = nlohmann::json;

struct cmd_add_derivation_t : MixDryRun, StoreCommand {
  std::string description() override { return "Add a store derivation"; }

  std::string doc() override {
    return
#include "derivation-add.md"
        ;
  }

  category_t category() override { return catUtility; }

  void run(ref<store_t> store) override {
    auto json = nlohmann::json::parse(drain_fd(STDIN_FILENO));

    auto drv = derivation_t::parseJsonAndValidate(*store, json);

    auto drv_path = write_derivation(*store, drv, NoRepair, /* read only */ dry_run);

    write_derivation(*store, drv, NoRepair, dry_run);

    logger->cout("%s", store->printStorePath(drv_path));
  }
};

static auto r_cmd_add_derivation = registerCommand2<cmd_add_derivation_t>({"derivation", "add"});
