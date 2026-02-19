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

  void run(ref<Store> store, StorePaths&& storePaths) override {
    for (auto& path : storePaths)
      store->repairPath(path);
  }
};

static auto rStoreRepair = registerCommand2<cmd_store_repair_t>({"store", "repair"});
