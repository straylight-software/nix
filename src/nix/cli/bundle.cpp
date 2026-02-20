#include "nix/cmd/command-installable-value.h"
#include "nix/cmd/installable-flake.h"
#include "nix/expr/eval-inline.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/globals.h"
#include "nix/store/local-fs-store.h"
#include "nix/store/store-api.h"

namespace nix::fs {
using namespace std::filesystem;
}

using namespace nix;

struct cmd_bundle_t : InstallableValueCommand {
  std::string bundler = "github:NixOS/bundlers";
  std::optional<Path> out_link;

  cmd_bundle_t() {
    add_flag({
        .long_name = "bundler",
        .description = fmt("Use a custom bundler instead of the default (`%s`).", bundler),
        .labels = {"flake-url"},
        .handler = {&bundler},
        .completer = {[&](add_completions_t& completions, size_t, std::string_view prefix) {
          complete_flake_ref(completions, getStore(), prefix);
        }},
    });

    add_flag({
        .long_name = "out-link",
        .short_name = 'o',
        .description = "Override the name of the symlink to the build result. It defaults to the "
                       "base name of the app.",
        .labels = {"path"},
        .handler = {&out_link},
        .completer = complete_path,
    });
  }

  std::string description() override {
    return "bundle an application so that it works outside of the Nix store";
  }

  std::string doc() override {
    return
#include "bundle.md"
        ;
  }

  category_t category() override { return catSecondary; }

  // FIXME: cut&paste from CmdRun.
  strings_t getDefaultFlakeAttrPaths() override {
    strings_t res{"apps." + settings.thisSystem.get() + ".default",
                "defaultApp." + settings.thisSystem.get()};
    for (auto& s : SourceExprCommand::getDefaultFlakeAttrPaths())
      res.push_back(s);
    return res;
  }

  strings_t getDefaultFlakeAttrPathPrefixes() override {
    strings_t res{"apps." + settings.thisSystem.get() + "."};
    for (auto& s : SourceExprCommand::getDefaultFlakeAttrPathPrefixes())
      res.push_back(s);
    return res;
  }

  void run(ref<store_t> store, ref<InstallableValue> installable) override {
    auto eval_state = getEvalState();

    auto val = installable->toValue(*eval_state).first;

    auto [bundlerFlakeRef, bundlerName, extendedOutputsSpec] =
        parse_flake_ref_with_fragment_and_extended_outputs_spec(fetch_settings, bundler,
                                                        std::filesystem::current_path().string());
    const flake::LockFlags lock_flags{.writeLockFile = false};
    InstallableFlake bundler{this,
                             eval_state,
                             std::move(bundlerFlakeRef),
                             bundlerName,
                             std::move(extendedOutputsSpec),
                             {"bundlers." + settings.thisSystem.get() + ".default",
                              "defaultBundler." + settings.thisSystem.get()},
                             {"bundlers." + settings.thisSystem.get() + "."},
                             lock_flags};

    auto v_res = eval_state->allocValue();
    eval_state->callFunction(*bundler.toValue(*eval_state).first, *val, *v_res, no_pos);

    if (!eval_state->is_derivation(*v_res))
      throw Error("the bundler '%s' does not produce a derivation", bundler.what());

    auto attr1 = v_res->attrs()->get(eval_state->s.drv_path);
    if (!attr1)
      throw Error("the bundler '%s' does not produce a derivation", bundler.what());

    NixStringContext context2;
    auto drv_path = eval_state->coerceToStorePath(attr1->pos, *attr1->value, context2, "");

    eval_state->waitForAllPaths();

    drv_path.requireDerivation();

    auto attr2 = v_res->attrs()->get(eval_state->s.out_path);
    if (!attr2)
      throw Error("the bundler '%s' does not produce a derivation", bundler.what());

    auto out_path = eval_state->coerceToStorePath(attr2->pos, *attr2->value, context2, "");

    eval_state->waitForAllPaths();

    store->build_paths({
        derived_path_t::Built{
            .drv_path = makeConstantStorePathRef(drv_path),
            .outputs = OutputsSpec::All{},
        },
    });

    if (!out_link) {
      auto* attr = v_res->attrs()->get(eval_state->s.name);
      if (!attr)
        throw Error("attribute 'name' missing");
      out_link = eval_state->forceStringNoCtx(*attr->value, attr->pos, "");
    }

    // TODO: will crash if not a localFSStore?
    store.dynamic_pointer_cast<local_fs_store>()->addPermRoot(out_path, abs_path(*out_link));
  }
};

static auto r2 = registerCommand<cmd_bundle_t>("bundle");
