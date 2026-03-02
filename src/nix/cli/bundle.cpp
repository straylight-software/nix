#include "nix/cmd/command-installable-value.h"
#include "nix/cmd/installable-flake.h"
#include "nix/expr/eval-inline.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/globals.h"
#include "nix/store/local-fs-store.h"
#include "nix/store/store-api.h"

struct cmd_bundle_t : nix::InstallableValueCommand {
  std::string bundler = "github:NixOS/bundlers";
  std::optional<nix::Path> out_link;

  cmd_bundle_t() {
    add_flag({
        .long_name = "bundler",
        .description = nix::fmt("Use a custom bundler instead of the default (`%s`).", bundler),
        .labels = {"flake-url"},
        .handler = {&bundler},
        .completer = {[&](nix::add_completions_t& completions, size_t, std::string_view prefix) {
          nix::complete_flake_ref(completions, getStore(), prefix);
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

  category_t category() override { return nix::catSecondary; }

  // FIXME: cut&paste from CmdRun.
  nix::strings_t getDefaultFlakeAttrPaths() override {
    nix::strings_t res{"apps." + nix::settings.thisSystem.get() + ".default",
                       "defaultApp." + nix::settings.thisSystem.get()};
    for (auto& s : SourceExprCommand::getDefaultFlakeAttrPaths()) {
      res.push_back(s);
    }
    return res;
  }

  nix::strings_t getDefaultFlakeAttrPathPrefixes() override {
    nix::strings_t res{"apps." + nix::settings.thisSystem.get() + "."};
    for (auto& s : SourceExprCommand::getDefaultFlakeAttrPathPrefixes()) {
      res.push_back(s);
    }
    return res;
  }

  void run(nix::ref<nix::store_t> store, nix::ref<nix::InstallableValue> installable) override {
    auto eval_state = getEvalState();

    auto val = installable->toValue(*eval_state).first;

    auto [bundlerFlakeRef, bundlerName, extendedOutputsSpec] =
        nix::parse_flake_ref_with_fragment_and_extended_outputs_spec(
            nix::fetch_settings, bundler, std::filesystem::current_path().string());
    const nix::flake::LockFlags lock_flags{.writeLockFile = false};
    nix::InstallableFlake bundler{this,
                                  eval_state,
                                  std::move(bundlerFlakeRef),
                                  bundlerName,
                                  std::move(extendedOutputsSpec),
                                  {"bundlers." + nix::settings.thisSystem.get() + ".default",
                                   "defaultBundler." + nix::settings.thisSystem.get()},
                                  {"bundlers." + nix::settings.thisSystem.get() + "."},
                                  lock_flags};

    auto v_res = eval_state->allocValue();
    eval_state->callFunction(*bundler.toValue(*eval_state).first, *val, *v_res, nix::no_pos);

    if (!eval_state->is_derivation(*v_res)) {
      throw nix::Error("the bundler '%s' does not produce a derivation", bundler.what());
    }

    auto attr1 = v_res->attrs()->get(eval_state->s.drv_path);
    if (!attr1) {
      throw nix::Error("the bundler '%s' does not produce a derivation", bundler.what());
    }

    nix::NixStringContext context2;
    auto drv_path = eval_state->coerceToStorePath(attr1->pos, *attr1->value, context2, "");

    eval_state->waitForAllPaths();

    drv_path.requireDerivation();

    auto attr2 = v_res->attrs()->get(eval_state->s.out_path);
    if (!attr2) {
      throw nix::Error("the bundler '%s' does not produce a derivation", bundler.what());
    }

    auto out_path = eval_state->coerceToStorePath(attr2->pos, *attr2->value, context2, "");

    eval_state->waitForAllPaths();

    store->build_paths({
        nix::derived_path_t::Built{
            .drv_path = nix::makeConstantStorePathRef(drv_path),
            .outputs = nix::OutputsSpec::All{},
        },
    });

    if (!out_link) {
      auto* attr = v_res->attrs()->get(eval_state->s.name);
      if (!attr) {
        throw nix::Error("attribute 'name' missing");
      }
      out_link = eval_state->forceStringNoCtx(*attr->value, attr->pos, "");
    }

    // TODO: will crash if not a localFSStore?
    store.dynamic_pointer_cast<nix::local_fs_store>()->addPermRoot(out_path,
                                                                   nix::abs_path(*out_link));
  }
};

static auto r2 = nix::registerCommand<cmd_bundle_t>("bundle");
