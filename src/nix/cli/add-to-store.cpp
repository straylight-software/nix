#include "nix/cmd/command.h"
#include "nix/cmd/misc-store-flags.h"
#include "nix/main/common-args.h"
#include "nix/store/store-api.h"
#include "nix/util/archive.h"
#include "nix/util/git.h"
#include "nix/util/posix-source-accessor.h"

struct cmd_add_to_store_t : nix::MixDryRun, nix::StoreCommand {
  nix::Path path;
  std::optional<std::string> name_part;
  nix::content_address_method_t ca_method = nix::content_address_method_t::raw_t::nix_archive;
  nix::hash_algorithm_t hash_algo = nix::hash_algorithm_t::SHA256;

  cmd_add_to_store_t() {
    // FIXME: completion
    expect_arg("path", &path);

    add_flag({
        .long_name = "name",
        .short_name = 'n',
        .description = "Override the name component of the store path. It defaults to the base "
                       "name of *path*.",
        .labels = {"name"},
        .handler = {&name_part},
    });

    add_flag(nix::flag::content_address_method(&ca_method));

    add_flag(nix::flag::hash_algo(&hash_algo));
  }

  void run(nix::ref<nix::store_t> store) override {
    if (!name_part) {
      name_part = nix::base_name_of(path);
    }

    auto source_path =
        nix::posix_source_accessor_t::create_at_root(nix::make_parent_canonical(path));

    auto store_path =
        dry_run ? store->computeStorePath(*name_part, source_path, ca_method, hash_algo, {}).first
                : store->addToStoreSlow(*name_part, source_path, ca_method, hash_algo, {}).path;

    nix::logger->cout("%s", store->printStorePath(store_path));
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
  cmd_add_file_t() { ca_method = nix::content_address_method_t::raw_t::flat; }

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

static auto r_cmd_add_file = nix::registerCommand2<cmd_add_file_t>({"store", "add-file"});
static auto r_cmd_add_path = nix::registerCommand2<cmd_add_path_t>({"store", "add-path"});
static auto r_cmd_add = nix::registerCommand2<cmd_add_t>({"store", "add"});
