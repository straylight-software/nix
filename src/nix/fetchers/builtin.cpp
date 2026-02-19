#include <nlohmann/json.hpp>

#include "nix/fetchers/fetch-settings.h"
#include "nix/fetchers/fetchers.h"
#include "nix/store/builtins.h"
#include "nix/store/filetransfer.h"
#include "nix/store/parsed-derivations.h"
#include "nix/store/store-open.h"
#include "nix/util/archive.h"

namespace nix {

static void builtinFetchTree(const BuiltinBuilderContext& ctx) {
  experimentalFeatureSettings.require(Xp::BuildTimeFetchTree);

  auto out = get(ctx.drv.outputs, "out");
  if (!out)
    throw Error("'builtin:fetch-tree' requires an 'out' output");

  if (!(ctx.drv.type().isFixed() || ctx.drv.type().isImpure()))
    throw Error("'builtin:fetch-tree' must be a fixed-output or impure derivation");

  if (!ctx.drv.structuredAttrs)
    throw Error("'builtin:fetch-tree' must have '__structuredAttrs = true'");

  setenv("NIX_CACHE_HOME", ctx.tmpDirInSandbox.c_str(), 1);

  using namespace fetchers;

  fetchers::Settings myFetchSettings;
  myFetchSettings.accessTokens = fetchSettings.accessTokens.get();

  // Make sure we don't use the FileTransfer object of the parent
  // since it's in a broken state after the fork. We also must not
  // delete it, so hang on to the shared_ptr.
  // FIXME: move FileTransfer into fetchers::Settings.
  static auto prevFileTransfer = resetFileTransfer();

  // FIXME: disable use of the git/tarball cache

  auto input = Input::fromAttrs(myFetchSettings,
                                jsonToAttrs(ctx.drv.structuredAttrs->structuredAttrs.at("input")));

  std::cerr << fmt("fetching '%s'...\n", input.to_string());

  /* Functions like downloadFile() expect a store. We can't use the
     real one since we're in a forked process. FIXME: use recursive
     Nix's daemon so we can use the real store? */
  auto tmpStore = openStore(ctx.tmpDirInSandbox + "/nix");

  auto [accessor, lockedInput] = input.getAccessor(myFetchSettings, *tmpStore);

  auto source = sinkToSource([&](Sink& sink) { accessor->dumpPath(CanonPath::root, sink); });

  restorePath(ctx.outputs.at("out"), *source);
}

static RegisterBuiltinBuilder registerUnpackChannel("fetch-tree", builtinFetchTree);

} // namespace nix
