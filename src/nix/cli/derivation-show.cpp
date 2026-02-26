// FIXME: integrate this with `nix path-info`?
// FIXME: rename to 'nix store derivation show'?

#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/main/common-args.h"
#include "nix/store/derivations.h"
#include "nix/store/store-api.h"
#include "nix/util/archive.h"

struct cmd_show_derivation_t : nix::InstallablesCommand, nix::MixPrintJSON {
  bool recursive = false;

  cmd_show_derivation_t() {
    add_flag({
        .long_name = "recursive",
        .short_name = 'r',
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

  nix::category_t category() override { return nix::catUtility; }

  void run(nix::ref<nix::store_t> store, nix::Installables&& installables) override {
    auto drv_paths = nix::Installable::toDerivations(store, installables, true);

    if (recursive) {
      nix::store_path_set_t closure;
      store->computeFSClosure(drv_paths, closure);
      drv_paths = std::move(closure);
    }

    nlohmann::json json_root = nlohmann::json::object();

    for (auto& drv_path : drv_paths) {
      if (!drv_path.is_derivation())
        continue;

      json_root[drv_path.to_string()] = store->read_derivation(drv_path);
    }
    printJSON(nlohmann::json{
        {"version", nix::expectedJsonVersionDerivation},
        {"derivations", std::move(json_root)},
    });
  }
};

static auto r_cmd_show_derivation =
    nix::registerCommand2<cmd_show_derivation_t>({"derivation", "show"});
