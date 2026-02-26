#include "nix/cmd/command.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/gc-store.h"
#include "nix/store/store-api.h"
#include "nix/store/store-cast.h"

struct cmd_store_gc_t : nix::StoreCommand, nix::MixDryRun {
  nix::GCOptions options;

  cmd_store_gc_t() {
    add_flag({
        .long_name = "max",
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

  void run(nix::ref<nix::store_t> store) override {
    auto& gc_store = nix::require<nix::GcStore>(*store);

    options.action = dry_run ? nix::GCOptions::gcReturnDead : nix::GCOptions::gcDeleteDead;
    nix::GCResults results;
    nix::PrintFreed freed(options.action == nix::GCOptions::gcDeleteDead, results);
    gc_store.collectGarbage(options, results);
  }
};

static auto r_cmd_store_gc = nix::registerCommand2<cmd_store_gc_t>({"store", "gc"});
