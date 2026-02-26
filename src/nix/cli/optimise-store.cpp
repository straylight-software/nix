#include <atomic>

#include "nix/cmd/command.h"
#include "nix/main/shared.h"
#include "nix/store/store-api.h"

struct cmd_optimise_store_t : nix::StoreCommand {
  std::string description() override {
    return "replace identical files in the store by hard links";
  }

  std::string doc() override {
    return
#include "optimise-store.md"
        ;
  }

  void run(nix::ref<nix::store_t> store) override { store->optimiseStore(); }
};

static auto r_cmd_optimise_store =
    nix::registerCommand2<cmd_optimise_store_t>({"store", "optimise"});
