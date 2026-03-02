#include "nix/cmd/command.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/gc-store.h"
#include "nix/store/store-api.h"
#include "nix/store/store-cast.h"

struct cmd_store_delete_t : nix::StorePathsCommand {
  nix::GCOptions options{.action = nix::GCOptions::gcDeleteSpecific};

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

  void run(nix::ref<nix::store_t> store, nix::store_paths_t&& store_paths) override {
    auto& gc_store = nix::require<nix::GcStore>(*store);

    for (auto& path : store_paths) {
      options.pathsToDelete.insert(path);
    }

    nix::GCResults results;
    nix::PrintFreed freed(true, results);
    gc_store.collectGarbage(options, results);
  }
};

static auto r_cmd_store_delete = nix::registerCommand2<cmd_store_delete_t>({"store", "delete"});
