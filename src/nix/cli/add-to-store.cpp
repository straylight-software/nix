#include "nix/cmd/command.h"
#include "nix/cmd/misc-store-flags.h"
#include "nix/main/common-args.h"
#include "nix/store/store-api.h"
#include "nix/util/archive.h"
#include "nix/util/git.h"
#include "nix/util/posix-source-accessor.h"

using namespace nix;

struct cmd_add_to_store_t : MixDryRun, StoreCommand {
  Path path;
  std::optional<std::string> namePart;
  ContentAddressMethod caMethod = ContentAddressMethod::raw_t::NixArchive;
  hash_algorithm_t hashAlgo = hash_algorithm_t::SHA256;

  cmd_add_to_store_t() {
    // FIXME: completion
    expectArg("path", &path);

    addFlag({
        .longName = "name",
        .shortName = 'n',
        .description = "Override the name component of the store path. It defaults to the base "
                       "name of *path*.",
        .labels = {"name"},
        .handler = {&namePart},
    });

    addFlag(flag::contentAddressMethod(&caMethod));

    addFlag(flag::hashAlgo(&hashAlgo));
  }

  void run(ref<Store> store) override {
    if (!namePart)
      namePart = baseNameOf(path);

    auto sourcePath = posix_source_accessor_t::createAtRoot(makeParentCanonical(path));

    auto storePath =
        dryRun ? store->computeStorePath(*namePart, sourcePath, caMethod, hashAlgo, {}).first
               : store->addToStoreSlow(*namePart, sourcePath, caMethod, hashAlgo, {}).path;

    logger->cout("%s", store->printStorePath(storePath));
  }
};

struct cmd_add_t : cmd_add_to_store_t {
  std::string description() override { return "Add a file or directory to the Nix store"; }

  std::string doc() override {
    return
#include "add.md"
        ;
  }
};

struct cmd_add_file_t : cmd_add_to_store_t {
  cmd_add_file_t() { caMethod = ContentAddressMethod::raw_t::Flat; }

  std::string description() override {
    return "Deprecated. Use [`nix store add --mode "
           "flat`](@docroot@/command-ref/new-cli/nix3-store-add.md) instead.";
  }
};

struct cmd_add_path_t : cmd_add_to_store_t {
  std::string description() override {
    return "Deprecated alias to [`nix store "
           "add`](@docroot@/command-ref/new-cli/nix3-store-add.md).";
  }
};

static auto rCmdAddFile = registerCommand2<cmd_add_file_t>({"store", "add-file"});
static auto rCmdAddPath = registerCommand2<cmd_add_path_t>({"store", "add-path"});
static auto rCmdAdd = registerCommand2<cmd_add_t>({"store", "add"});
