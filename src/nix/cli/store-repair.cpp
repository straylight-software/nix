#include "nix/cmd/command.h"
#include "nix/store/store-api.h"

struct cmd_store_repair_t : nix::StorePathsCommand {
  std::string description() override { return "repair store paths"; }

  std::string doc() override {
    return
#include "store-repair.md"
        ;
  }

  void run(nix::ref<nix::store_t> store, nix::store_paths_t&& store_paths) override {
    for (auto& path : store_paths)
      store->repairPath(path);
  }
};

static auto r_store_repair = nix::registerCommand2<cmd_store_repair_t>({"store", "repair"});
