#include "nix/cmd/command.h"
#include "nix/store/store-api.h"

struct cmd_path_from_hash_part_t : nix::StoreCommand {
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

  void run(nix::ref<nix::store_t> store) override {
    if (auto store_path = store->queryPathFromHashPart(hash_part)) {
      nix::logger->cout(store->printStorePath(*store_path));
    } else {
      throw nix::Error("there is no store path corresponding to '%s'", hash_part);
    }
  }
};

static auto r_cmd_path_from_hash_part =
    nix::registerCommand2<cmd_path_from_hash_part_t>({"store", "path-from-hash-part"});
