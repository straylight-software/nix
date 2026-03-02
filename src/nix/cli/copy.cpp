#include "nix/cmd/command.h"
#include "nix/main/shared.h"
#include "nix/store/local-fs-store.h"
#include "nix/store/store-api.h"

struct cmd_copy_t : virtual nix::CopyCommand,
                    virtual nix::BuiltPathsCommand,
                    nix::MixProfile,
                    nix::MixNoCheckSigs {
  std::optional<std::filesystem::path> out_link;

  nix::SubstituteFlag substitute = nix::NoSubstitute;

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
        .handler = {&substitute, nix::Substitute},
    });

    realiseMode = nix::Realise::Outputs;
  }

  std::string description() override { return "copy paths between Nix stores"; }

  std::string doc() override {
    return
#include "copy.md"
        ;
  }

  category_t category() override { return nix::catSecondary; }

  void run(nix::ref<nix::store_t> src_store, nix::BuiltPaths&& all_paths,
           nix::BuiltPaths&& root_paths) override {
    auto dst_store = getDstStore();

    nix::RealisedPath::Set stuff_to_copy;

    for (auto& builtPath : all_paths) {
      auto theseRealisations = builtPath.toRealisedPaths(*src_store);
      stuff_to_copy.insert(theseRealisations.begin(), theseRealisations.end());
    }

    nix::copy_paths(*src_store, *dst_store, stuff_to_copy, nix::NoRepair, check_sigs, substitute);

    updateProfile(root_paths);

    if (out_link) {
      if (auto store2 = dst_store.dynamic_pointer_cast<nix::local_fs_store>()) {
        nix::create_out_links(*out_link, root_paths, *store2);
      } else {
        throw nix::Error("'--out-link' is not supported for this Nix store");
      }
    }
  }
};

static auto r_cmd_copy = nix::registerCommand<cmd_copy_t>("copy");
