#include "nix/cmd/command.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/gc-store.h"
#include "nix/store/store-api.h"
#include "nix/store/store-cast.h"

using namespace nix;

struct cmd_store_delete_t : StorePathsCommand {
  GCOptions options{.action = GCOptions::gcDeleteSpecific};

  cmd_store_delete_t() {
    addFlag({
        .longName = "ignore-liveness",
        .description = "Do not check whether the paths are reachable from a root.",
        .handler = {&options.ignoreLiveness, true},
    });
  }

  std::string description() override { return "delete paths from the Nix store"; }

  std::string doc() override {
    return
#include "store-delete.md"
        ;
  }

  void run(ref<Store> store, StorePaths&& storePaths) override {
    auto& gcStore = require<GcStore>(*store);

    for (auto& path : storePaths)
      options.pathsToDelete.insert(path);

    GCResults results;
    PrintFreed freed(true, results);
    gcStore.collectGarbage(options, results);
  }
};

static auto rCmdStoreDelete = registerCommand2<cmd_store_delete_t>({"store", "delete"});
