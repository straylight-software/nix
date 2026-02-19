#include "nix/cmd/command.h"
#include "nix/store/store-api.h"

using namespace nix;

struct cmd_path_from_hash_part_t : StoreCommand {
  std::string hashPart;

  cmd_path_from_hash_part_t() {
    expectArgs({
        .label = "hash-part",
        .handler = {&hashPart},
    });
  }

  std::string description() override { return "get a store path from its hash part"; }

  std::string doc() override {
    return
#include "path-from-hash-part.md"
        ;
  }

  void run(ref<Store> store) override {
    if (auto storePath = store->queryPathFromHashPart(hashPart))
      logger->cout(store->printStorePath(*storePath));
    else
      throw Error("there is no store path corresponding to '%s'", hashPart);
  }
};

static auto rCmdPathFromHashPart =
    registerCommand2<cmd_path_from_hash_part_t>({"store", "path-from-hash-part"});
