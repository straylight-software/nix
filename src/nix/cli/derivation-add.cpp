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

  void run(ref<Store> store) override {
    auto json = nlohmann::json::parse(drainFD(STDIN_FILENO));

    auto drv = Derivation::parseJsonAndValidate(*store, json);

    auto drvPath = writeDerivation(*store, drv, NoRepair, /* read only */ dryRun);

    writeDerivation(*store, drv, NoRepair, dryRun);

    logger->cout("%s", store->printStorePath(drvPath));
  }
};

static auto rCmdAddDerivation = registerCommand2<cmd_add_derivation_t>({"derivation", "add"});
