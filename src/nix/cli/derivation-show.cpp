// FIXME: integrate this with `nix path-info`?
// FIXME: rename to 'nix store derivation show'?

#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/main/common-args.h"
#include "nix/store/derivations.h"
#include "nix/store/store-api.h"
#include "nix/util/archive.h"

using namespace nix;
using json = nlohmann::json;

struct cmd_show_derivation_t : InstallablesCommand, MixPrintJSON {
  bool recursive = false;

  cmd_show_derivation_t() {
    addFlag({
        .longName = "recursive",
        .shortName = 'r',
        .description = "Include the dependencies of the specified derivations.",
        .handler = {&recursive, true},
    });
  }

  std::string description() override { return "show the contents of a store derivation"; }

  std::string doc() override {
    return
#include "derivation-show.md"
        ;
  }

  category_t category() override { return catUtility; }

  void run(ref<Store> store, Installables&& installables) override {
    auto drvPaths = Installable::toDerivations(store, installables, true);

    if (recursive) {
      StorePathSet closure;
      store->computeFSClosure(drvPaths, closure);
      drvPaths = std::move(closure);
    }

    json jsonRoot = json::object();

    for (auto& drvPath : drvPaths) {
      if (!drvPath.isDerivation())
        continue;

      jsonRoot[drvPath.to_string()] = store->readDerivation(drvPath);
    }
    printJSON(nlohmann::json{
        {"version", expectedJsonVersionDerivation},
        {"derivations", std::move(jsonRoot)},
    });
  }
};

static auto rCmdShowDerivation = registerCommand2<cmd_show_derivation_t>({"derivation", "show"});
