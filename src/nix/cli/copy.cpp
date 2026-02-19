#include "nix/cmd/command.h"
#include "nix/main/shared.h"
#include "nix/store/local-fs-store.h"
#include "nix/store/store-api.h"

using namespace nix;

struct cmd_copy_t : virtual CopyCommand, virtual BuiltPathsCommand, MixProfile, MixNoCheckSigs {
  std::optional<std::filesystem::path> out_link;

  SubstituteFlag substitute = NoSubstitute;

  cmd_copy_t() : BuiltPathsCommand(true) {
    add_flag({
        .long_name = "out-link",
        .short_name = 'o',
        .description = "Create symlinks prefixed with *path* to the top-level store paths fetched "
                       "from the source store.",
        .labels = {"path"},
        .handler = {&out_link},
        .completer = complete_path,
    });
    add_flag({
        .long_name = "substitute-on-destination",
        .short_name = 's',
        .description =
            "Whether to try substitutes on the destination store (only supported by SSH stores).",
        .handler = {&substitute, Substitute},
    });

    realiseMode = Realise::Outputs;
  }

  std::string description() override { return "copy paths between Nix stores"; }

  std::string doc() override {
    return
#include "copy.md"
        ;
  }

  category_t category() override { return catSecondary; }

  void run(ref<Store> src_store, BuiltPaths&& all_paths, BuiltPaths&& root_paths) override {
    auto dst_store = getDstStore();

    RealisedPath::Set stuff_to_copy;

    for (auto& builtPath : all_paths) {
      auto theseRealisations = builtPath.toRealisedPaths(*src_store);
      stuff_to_copy.insert(theseRealisations.begin(), theseRealisations.end());
    }

    copy_paths(*src_store, *dst_store, stuff_to_copy, NoRepair, check_sigs, substitute);

    updateProfile(root_paths);

    if (out_link) {
      if (auto store2 = dst_store.dynamic_pointer_cast<local_fs_store>())
        create_out_links(*out_link, root_paths, *store2);
      else
        throw Error("'--out-link' is not supported for this Nix store");
    }
  }
};

static auto r_cmd_copy = registerCommand<cmd_copy_t>("copy");
