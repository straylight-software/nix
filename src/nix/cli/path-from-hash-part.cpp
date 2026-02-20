#include "nix/cmd/command.h"
#include "nix/store/store-api.h"

using namespace nix;

struct cmd_path_from_hash_part_t : StoreCommand {
  std::string hash_part;

  cmd_path_from_hash_part_t() {
    expect_args({
        .label = "hash-part",
        .handler = {&hash_part},
    });
  }

  std::string description() override { return "get a store path from its hash part"; }

  std::string doc() override {
    return
#include "path-from-hash-part.md"
        ;
  }

  void run(ref<store_t> store) override {
    if (auto store_path = store->queryPathFromHashPart(hash_part))
      logger->cout(store->printStorePath(*store_path));
    else
      throw Error("there is no store path corresponding to '%s'", hash_part);
  }
};

static auto r_cmd_path_from_hash_part =
    registerCommand2<cmd_path_from_hash_part_t>({"store", "path-from-hash-part"});
