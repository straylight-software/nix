#include "nix/cmd/command.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/gc-store.h"
#include "nix/store/store-api.h"
#include "nix/store/store-cast.h"

using namespace nix;

struct cmd_store_gc_t : StoreCommand, MixDryRun {
  GCOptions options;

  cmd_store_gc_t() {
    addFlag({
        .longName = "max",
        .description = "Stop after freeing *n* bytes of disk space.",
        .labels = {"n"},
        .handler = {&options.maxFreed},
    });
  }

  std::string description() override { return "perform garbage collection on a Nix store"; }

  std::string doc() override {
    return
#include "store-gc.md"
        ;
  }

  void run(ref<Store> store) override {
    auto& gcStore = require<GcStore>(*store);

    options.action = dryRun ? GCOptions::gcReturnDead : GCOptions::gcDeleteDead;
    GCResults results;
    PrintFreed freed(options.action == GCOptions::gcDeleteDead, results);
    gcStore.collectGarbage(options, results);
  }
};

static auto rCmdStoreGC = registerCommand2<cmd_store_gc_t>({"store", "gc"});
