#include "nix/cmd/command.h"
#include "nix/store/store-api.h"

using namespace nix;

struct cmd_store_repair_t : StorePathsCommand {
  std::string description() override { return "repair store paths"; }

  std::string doc() override {
    return
#include "store-repair.md"
        ;
  }

  void run(ref<store_t> store, store_paths_t&& store_paths) override {
    for (auto& path : store_paths)
      store->repairPath(path);
  }
};

static auto r_store_repair = registerCommand2<cmd_store_repair_t>({"store", "repair"});
