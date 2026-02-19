#include <nlohmann/json.hpp>

#include "nix/fetchers/fetch-settings.h"
#include "nix/fetchers/fetchers.h"
#include "nix/store/builtins.h"
#include "nix/store/filetransfer.h"
#include "nix/store/parsed-derivations.h"
#include "nix/store/store-open.h"
#include "nix/util/archive.h"

namespace nix {

static void builtin_fetch_tree(const BuiltinBuilderContext& ctx) {
  experimental_feature_settings.require(xp_t::build_time_fetch_tree);

  auto out = get(ctx.drv.outputs, "out");
  if (!out)
    throw Error("'builtin:fetch-tree' requires an 'out' output");

  if (!(ctx.drv.type().isFixed() || ctx.drv.type().is_impure()))
    throw Error("'builtin:fetch-tree' must be a fixed-output or impure derivation");

  if (!ctx.drv.structured_attrs)
    throw Error("'builtin:fetch-tree' must have '__structuredAttrs = true'");

  setenv("NIX_CACHE_HOME", ctx.tmp_dir_in_sandbox.c_str(), 1);

  using namespace fetchers;

  fetchers::settings_t my_fetch_settings;
  my_fetch_settings.accessTokens = fetch_settings.accessTokens.get();

  // Make sure we don't use the FileTransfer object of the parent
  // since it's in a broken state after the fork. We also must not
  // delete it, so hang on to the shared_ptr.
  // FIXME: move FileTransfer into fetchers::Settings.
  static auto prev_file_transfer = reset_file_transfer();

  // FIXME: disable use of the git/tarball cache

  auto input = Input::fromAttrs(my_fetch_settings,
                                json_to_attrs(ctx.drv.structured_attrs->structured_attrs.at("input")));

  std::cerr << fmt("fetching '%s'...\n", input.to_string());

  /* Functions like download_file() expect a store. We can't use the
     real one since we're in a forked process. FIXME: use recursive
     Nix's daemon so we can use the real store? */
  auto tmp_store = open_store(ctx.tmp_dir_in_sandbox + "/nix");

  auto [accessor, lockedInput] = input.get_accessor(my_fetch_settings, *tmp_store);

  auto source = sink_to_source([&](Sink& sink) { accessor->dump_path(canon_path_t::root, sink); });

  restore_path(ctx.outputs.at("out"), *source);
}

static RegisterBuiltinBuilder register_unpack_channel("fetch-tree", builtin_fetch_tree);

} // namespace nix
