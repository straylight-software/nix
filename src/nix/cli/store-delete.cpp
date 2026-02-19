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
    add_flag({
        .long_name = "ignore-liveness",
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

  void run(ref<Store> store, StorePaths&& store_paths) override {
    auto& gc_store = require<GcStore>(*store);

    for (auto& path : store_paths)
      options.pathsToDelete.insert(path);

    GCResults results;
    PrintFreed freed(true, results);
    gc_store.collectGarbage(options, results);
  }
};

static auto r_cmd_store_delete = registerCommand2<cmd_store_delete_t>({"store", "delete"});
