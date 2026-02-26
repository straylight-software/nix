#include <filesystem>
#include <iomanip>

#include <nlohmann/json.hpp>

#include "nix/cmd/markdown.h"
#include "nix/expr/attr-path.h"
#include "nix/expr/eval-cache.h"
#include "nix/expr/eval-inline.h"
#include "nix/expr/eval-settings.h"
#include "nix/expr/eval.h"
#include "nix/expr/get-drvs.h"
#include "nix/expr/parallel-eval.h"
#include "nix/fetchers/fetch-to-store.h"
#include "nix/fetchers/fetchers.h"
#include "nix/fetchers/registry.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/derivations.h"
#include "nix/store/globals.h"
#include "nix/store/local-fs-store.h"
#include "nix/store/outputs-spec.h"
#include "nix/store/store-open.h"
#include "nix/util/exit.h"
#include "nix/util/signals.h"
#include "nix/util/strings-inline.h"
#include "nix/util/users.h"

// FIXME is this supposed to be private or not?
#include "flake-command.h"


struct cmd_flake_update_t;

flake_command_t::flake_command_t() {
  expect_args({.label = "flake-url",
               .optional = true,
               .handler = {&flakeUrl},
               .completer = {[&](add_completions_t& completions, size_t, std::string_view prefix) {
                 complete_flake_ref(completions, getStore(), prefix);
               }}});
}

flake_ref_t flake_command_t::get_flake_ref() {
  return parse_flake_ref(fetch_settings, flakeUrl,
                         std::filesystem::current_path().string()); // FIXME
}

LockedFlake flake_command_t::lock_flake() {
  return flake::lock_flake(flake_settings, *getEvalState(), get_flake_ref(), lock_flags);
}

std::vector<flake_ref_t> flake_command_t::get_flake_refs_for_completion() {
  return {// Like getFlakeRef but with expandTilde called first
          parse_flake_ref(fetch_settings, expand_tilde(flakeUrl),
                          std::filesystem::current_path().string())};
}

struct cmd_flake_update_t : flake_command_t {
public:
  std::string description() override { return "update flake lock file"; }

  cmd_flake_update_t() {
    clear_expected_args();
    add_flag({
        .long_name = "flake",
        .description = "The flake to operate on. Default is the current directory.",
        .labels = {"flake-url"},
        .handler = {&flakeUrl},
        .completer = {[&](add_completions_t& completions, size_t, std::string_view prefix) {
          complete_flake_ref(completions, getStore(), prefix);
        }},
    });
    expect_args({
        .label = "inputs",
        .optional = true,
        .handler = {[&](std::vector<std::string> inputs_to_update) {
          for (const auto& inputToUpdate : inputs_to_update) {
            InputAttrPath inputAttrPath;
            try {
              inputAttrPath = flake::parse_input_attr_path(inputToUpdate);
            } catch (Error& e) {
              warn("Invalid flake input '%s'. To update a specific flake, use 'nix flake update "
                   "--flake %s' instead.",
                   inputToUpdate, inputToUpdate);
              throw e;
            }
            if (lock_flags.inputUpdates.contains(inputAttrPath))
              warn("input_t '%s' was specified multiple times. You may have done this by accident.",
                   print_input_attr_path(inputAttrPath));
            lock_flags.inputUpdates.insert(inputAttrPath);
          }
        }},
        .completer = {[&](add_completions_t& completions, size_t, std::string_view prefix) {
          complete_flake_input_attr_path(completions, getEvalState(),
                                         get_flake_refs_for_completion(), prefix);
        }},
    });

    /* Remove flags that don't make sense. */
    remove_flag("no-update-lock-file");
    remove_flag("no-write-lock-file");
  }

  std::string doc() override {
    return
#include "flake-update.md"
        ;
  }

  void run(nix::ref<nix::store_t> store) override {
    settings.tarballTtl = 0;
    auto update_all = lock_flags.inputUpdates.empty();

    lock_flags.recreateLockFile = update_all;
    lock_flags.writeLockFile = true;
    lock_flags.applyNixConfig = true;
    lock_flags.require_lockable = false;

    lock_flake();
  }
};

struct cmd_flake_lock_t : flake_command_t {
  std::string description() override { return "create missing lock file entries"; }

  cmd_flake_lock_t() {
    /* Remove flags that don't make sense. */
    remove_flag("no-write-lock-file");
  }

  std::string doc() override {
    return
#include "flake-lock.md"
        ;
  }

  void run(nix::ref<nix::store_t> store) override {
    settings.tarballTtl = 0;

    lock_flags.writeLockFile = true;
    lock_flags.failOnUnlocked = true;
    lock_flags.applyNixConfig = true;
    lock_flags.require_lockable = false;

    lock_flake();
  }
};

static void enumerate_outputs(
    eval_state_t& state, value_t& v_flake,
    std::function<void(std::string_view name, value_t& vProvide, const pos_idx_t pos)> callback) {
  auto pos = v_flake.determinePos(no_pos);
  state.forceAttrs(v_flake, pos, "while evaluating a flake to get its outputs");

  auto a_outputs = v_flake.attrs()->get(state.symbols.create("outputs"));
  assert(a_outputs);

  state.forceAttrs(*a_outputs->value, pos, "while evaluating the outputs of a flake");

  auto sHydraJobs = state.symbols.create("hydraJobs");

  /* Hack: ensure that hydraJobs is evaluated before anything
     else. This way we can disable IFD for hydraJobs and then enable
     it for other outputs. */
  if (auto attr = a_outputs->value->attrs()->get(sHydraJobs))
    callback(state.symbols[attr->name], *attr->value, attr->pos);

  for (auto& attr : *a_outputs->value->attrs()) {
    if (attr.name != sHydraJobs)
      callback(state.symbols[attr.name], *attr.value, attr.pos);
  }
}

struct cmd_flake_metadata_t : flake_command_t, MixJSON {
  std::string description() override { return "show flake metadata"; }

  std::string doc() override {
    return
#include "flake-metadata.md"
        ;
  }

  void run(nix::ref<nix::store_t> store) override {
    lock_flags.require_lockable = false;
    auto locked_flake = lock_flake();
    auto& flake = locked_flake.flake;

    /* Hack to show the store path if available. */
    std::optional<store_path_t> store_path;
    if (store->isInStore(flake.path.path.abs())) {
      auto path = store->toStorePath(flake.path.path.abs()).first;
      if (store->isValidPath(path))
        store_path = path;
    }

    if (json) {
      nlohmann::json j;
      if (flake.description)
        j["description"] = *flake.description;
      j["originalUrl"] = flake.original_ref.to_string();
      j["original"] = fetchers::attrs_to_json(flake.original_ref.toAttrs());
      j["resolvedUrl"] = flake.resolved_ref.to_string();
      j["resolved"] = fetchers::attrs_to_json(flake.resolved_ref.toAttrs());
      j["url"] = flake.locked_ref.to_string(); // FIXME: rename to lockedUrl
      // "locked" is a misnomer - this is the result of the
      // attempt to lock.
      j["locked"] = fetchers::attrs_to_json(flake.locked_ref.toAttrs());
      if (auto rev = flake.locked_ref.input.getRev())
        j["revision"] = rev->to_string(hash_format_t::base16, false);
      if (auto dirtyRev = fetchers::maybe_get_str_attr(flake.locked_ref.toAttrs(), "dirtyRev"))
        j["dirtyRevision"] = *dirtyRev;
      if (auto rev_count = flake.locked_ref.input.get_rev_count())
        j["revCount"] = *rev_count;
      if (auto last_modified = flake.locked_ref.input.get_last_modified())
        j["lastModified"] = *last_modified;
      if (store_path)
        j["path"] = store->printStorePath(*store_path);
      j["locks"] = locked_flake.lock_file.to_json().first;
      if (auto fingerprint = locked_flake.get_fingerprint(*store, fetch_settings))
        j["fingerprint"] = fingerprint->to_string(hash_format_t::base16, false);
      printJSON(j);
    } else {
      logger->cout(ANSI_BOLD "Resolved URL:" ANSI_NORMAL "  %s", flake.resolved_ref.to_string());
      if (flake.locked_ref.input.isLocked(fetch_settings))
        logger->cout(ANSI_BOLD "Locked URL:" ANSI_NORMAL "    %s", flake.locked_ref.to_string());
      if (flake.description)
        logger->cout(ANSI_BOLD "Description:" ANSI_NORMAL "   %s", *flake.description);
      if (store_path)
        logger->cout(ANSI_BOLD "Path:" ANSI_NORMAL "          %s",
                     store->printStorePath(*store_path));
      if (auto rev = flake.locked_ref.input.getRev())
        logger->cout(ANSI_BOLD "Revision:" ANSI_NORMAL "      %s",
                     rev->to_string(hash_format_t::base16, false));
      if (auto dirtyRev = fetchers::maybe_get_str_attr(flake.locked_ref.toAttrs(), "dirtyRev"))
        logger->cout(ANSI_BOLD "Revision:" ANSI_NORMAL "      %s", *dirtyRev);
      if (auto rev_count = flake.locked_ref.input.get_rev_count())
        logger->cout(ANSI_BOLD "Revisions:" ANSI_NORMAL "     %s", *rev_count);
      if (auto last_modified = flake.locked_ref.input.get_last_modified())
        logger->cout(ANSI_BOLD "Last modified:" ANSI_NORMAL " %s",
                     std::put_time(std::localtime(&*last_modified), "%F %T"));
      if (auto fingerprint = locked_flake.get_fingerprint(*store, fetch_settings))
        logger->cout(ANSI_BOLD "Fingerprint:" ANSI_NORMAL "   %s",
                     fingerprint->to_string(hash_format_t::base16, false));

      if (!locked_flake.lock_file.root->inputs.empty())
        logger->cout(ANSI_BOLD "Inputs:" ANSI_NORMAL);

      std::set<ref<Node>> visited{locked_flake.lock_file.root};

      [&](this const auto& recurse, const Node& node, const std::string& prefix) -> void {
        for (const auto& [i, input] : enumerate(node.inputs)) {
          bool last = i + 1 == node.inputs.size();

          if (auto locked_node = std::get_if<0>(&input.second)) {
            std::string lastModifiedStr = "";
            if (auto last_modified = (*locked_node)->locked_ref.input.get_last_modified())
              lastModifiedStr = fmt(" (%s)", std::put_time(std::gmtime(&*last_modified), "%F %T"));
            logger->cout("%s" ANSI_BOLD "%s" ANSI_NORMAL ": %s%s",
                         prefix + (last ? tree_last : tree_conn), input.first,
                         (*locked_node)->locked_ref.to_string(true), lastModifiedStr);

            bool firstVisit = visited.insert(*locked_node).second;

            if (firstVisit)
              recurse(**locked_node, prefix + (last ? tree_null : tree_line));
          } else if (auto follows = std::get_if<1>(&input.second)) {
            logger->cout("%s" ANSI_BOLD "%s" ANSI_NORMAL " follows input '%s'",
                         prefix + (last ? tree_last : tree_conn), input.first,
                         print_input_attr_path(*follows));
          }
        }
      }(*locked_flake.lock_file.root, "");
    }
  }
};

struct cmd_flake_info_t : cmd_flake_metadata_t {
  void run(nix::ref<nix::store_t> store) override {
    warn("'nix flake info' is a deprecated alias for 'nix flake metadata'");
    cmd_flake_metadata_t::run(store);
  }
};

struct cmd_flake_check_t : flake_command_t {
  bool build = true;
  bool check_all_systems = false;

  cmd_flake_check_t() {
    add_flag({
        .long_name = "no-build",
        .description = "Do not build checks.",
        .handler = {&build, false},
    });
    add_flag({
        .long_name = "all-systems",
        .description = "Check the outputs for all systems.",
        .handler = {&check_all_systems, true},
    });
  }

  std::string description() override {
    return "check whether the flake evaluates and run its tests";
  }

  std::string doc() override {
    return
#include "flake-check.md"
        ;
  }

  void run(nix::ref<nix::store_t> store) override {
    if (!build) {
      settings.readOnlyMode = true;
      eval_settings.enableImportFromDerivation.set_default(false);
    }

    auto state = getEvalState();

    lock_flags.applyNixConfig = true;
    auto flake = lock_flake();
    auto local_system = std::string(settings.thisSystem.get());

    std::atomic_bool has_errors = false;
    auto report_error = [&](const Error& e) {
      try {
        throw e;
      } catch (Interrupted& e) {
        throw;
      } catch (Error& e) {
        if (settings.keep_going) {
          logError(e.info());
          has_errors = true;
        } else
          throw;
      }
    };

    sync_t<std::vector<derived_path_t>> drvPaths_;
    sync_t<std::set<std::string>> omittedSystems;
    sync_t<std::map<derived_path_t, std::vector<AttrPath>>> derivedPathToAttrPaths_;

    // FIXME: rewrite to use EvalCache.

    auto resolve = [&](pos_idx_t p) { return state->positions[p]; };

    auto arg_has_name = [&](symbol_t arg, std::string_view expected) {
      std::string_view name = state->symbols[arg];
      return name == expected || name == "_" ||
             (has_prefix(name, "_") && name.substr(1) == expected);
    };

    auto check_system_name = [&](std::string_view system, const pos_idx_t pos) {
      // FIXME: what's the format of "system"?
      if (system.find('-') == std::string::npos)
        report_error(Error("'%s' is not a valid system type, at %s", system, resolve(pos)));
    };

    auto check_system_type = [&](std::string_view system, const pos_idx_t pos) {
      if (!check_all_systems && system != local_system) {
        omittedSystems.lock()->insert(std::string(system));
        return false;
      } else {
        return true;
      }
    };

    auto check_derivation = [&](const std::string& attr_path, value_t& v,
                                const pos_idx_t pos) -> std::optional<store_path_t> {
      try {
        activity_t act(*logger, lvl_info, act_unknown, fmt("checking derivation %s", attr_path));
        auto package_info = get_derivation(*state, v, false);
        if (!package_info)
          throw Error("flake attribute '%s' is not a derivation", attr_path);
        else {
          // FIXME: check meta attributes
          auto store_path = package_info->queryDrvPath();
          if (store_path) {
            logger->log(lvl_info, fmt("derivation evaluated to %s",
                                      store->printStorePath(store_path.value())));
          }
          return store_path;
        }
      } catch (Error& e) {
        e.add_trace(resolve(pos), hint_fmt_t("while checking the derivation '%s'", attr_path));
        report_error(e);
      }
      return std::nullopt;
    };

    FutureVector futures(*state->executor);

    auto check_app = [&](const std::string& attr_path, value_t& v, const pos_idx_t pos) {
      try {
        activity_t act(*logger, lvl_info, act_unknown, fmt("checking app '%s'", attr_path));
        state->forceAttrs(v, pos, "");
        if (auto attr = v.attrs()->get(state->symbols.create("type")))
          state->forceStringNoCtx(*attr->value, attr->pos, "");
        else
          throw Error("app '%s' lacks attribute 'type'", attr_path);

        if (auto attr = v.attrs()->get(state->symbols.create("program"))) {
          if (attr->name == state->symbols.create("program")) {
            NixStringContext context;
            state->forceString(*attr->value, context, attr->pos, "");
          }
        } else
          throw Error("app '%s' lacks attribute 'program'", attr_path);

        if (auto attr = v.attrs()->get(state->symbols.create("meta"))) {
          state->forceAttrs(*attr->value, attr->pos, "");
          if (auto dAttr = attr->value->attrs()->get(state->symbols.create("description")))
            state->forceStringNoCtx(*dAttr->value, dAttr->pos, "");
          else
            logWarning({
                .msg_ = hint_fmt_t("app '%s' lacks attribute 'meta.description'", attr_path),
            });
        } else
          logWarning({
              .msg_ = hint_fmt_t("app '%s' lacks attribute 'meta'", attr_path),
          });

        for (auto& attr : *v.attrs()) {
          std::string_view name(state->symbols[attr.name]);
          if (name != "type" && name != "program" && name != "meta")
            throw Error("app '%s' has unsupported attribute '%s'", attr_path, name);
        }
      } catch (Error& e) {
        e.add_trace(resolve(pos), hint_fmt_t("while checking the app definition '%s'", attr_path));
        report_error(e);
      }
    };

    auto check_overlay = [&](std::string_view attr_path, value_t& v, const pos_idx_t pos) {
      try {
        activity_t act(*logger, lvl_info, act_unknown, fmt("checking overlay '%s'", attr_path));
        state->forceValue(v, pos);
        if (!v.isLambda()) {
          throw Error("overlay is not a function, but %s instead", show_type(v));
        }
        if (v.lambda().fun->getFormals() || !arg_has_name(v.lambda().fun->arg, "final"))
          throw Error("overlay does not take an argument named 'final'");
        // FIXME: if we have a 'nixpkgs' input, use it to
        // evaluate the overlay.
      } catch (Error& e) {
        e.add_trace(resolve(pos), hint_fmt_t("while checking the overlay '%s'", attr_path));
        report_error(e);
      }
    };

    auto check_module = [&](std::string_view attr_path, value_t& v, const pos_idx_t pos) {
      try {
        activity_t act(*logger, lvl_info, act_unknown,
                       fmt("checking NixOS module '%s'", attr_path));
        state->forceValue(v, pos);
      } catch (Error& e) {
        e.add_trace(resolve(pos), hint_fmt_t("while checking the NixOS module '%s'", attr_path));
        report_error(e);
      }
    };

    std::function<void(const std::string& attr_path, value_t& v, const pos_idx_t pos)>
        checkHydraJobs;

    checkHydraJobs = [&](const std::string& attr_path, value_t& v, const pos_idx_t pos) {
      try {
        activity_t act(*logger, lvl_info, act_unknown, fmt("checking Hydra job '%s'", attr_path));
        state->forceAttrs(v, pos, "");

        if (state->is_derivation(v))
          throw Error("jobset should not be a derivation at top-level");

        for (auto& attr : *v.attrs())
          futures.spawn(1, [&, attr_path]() {
            state->forceAttrs(*attr.value, attr.pos, "");
            auto attrPath2 = concat_strings(attr_path, ".", state->symbols[attr.name]);
            if (state->is_derivation(*attr.value)) {
              activity_t act(*logger, lvl_info, act_unknown,
                             fmt("checking Hydra job '%s'", attrPath2));
              check_derivation(attrPath2, *attr.value, attr.pos);
            } else
              checkHydraJobs(attrPath2, *attr.value, attr.pos);
          });

      } catch (Error& e) {
        e.add_trace(resolve(pos), hint_fmt_t("while checking the Hydra jobset '%s'", attr_path));
        report_error(e);
      }
    };

    auto check_nix_os_configuration = [&](const std::string& attr_path, value_t& v,
                                          const pos_idx_t pos) {
      try {
        activity_t act(*logger, lvl_info, act_unknown,
                       fmt("checking NixOS configuration '%s'", attr_path));
        bindings_t& bindings = bindings_t::emptyBindings;
        auto v_toplevel =
            find_along_attr_path(*state, "config.system.build.toplevel", bindings, v).first;
        state->forceValue(*v_toplevel, pos);
        if (!state->is_derivation(*v_toplevel))
          throw Error("attribute 'config.system.build.toplevel' is not a derivation");
      } catch (Error& e) {
        e.add_trace(resolve(pos),
                    hint_fmt_t("while checking the NixOS configuration '%s'", attr_path));
        report_error(e);
      }
    };

    auto check_template = [&](std::string_view attr_path, value_t& v, const pos_idx_t pos) {
      try {
        activity_t act(*logger, lvl_info, act_unknown, fmt("checking template '%s'", attr_path));

        state->forceAttrs(v, pos, "");

        if (auto attr = v.attrs()->get(state->symbols.create("path"))) {
          if (attr->name == state->symbols.create("path")) {
            NixStringContext context;
            auto path = state->coerceToPath(attr->pos, *attr->value, context, "");
            if (!path.path_exists())
              throw Error("template '%s' refers to a non-existent path '%s'", attr_path, path);
            // TODO: recursively check the flake in 'path'.
          }
        } else
          throw Error("template '%s' lacks attribute 'path'", attr_path);

        if (auto attr = v.attrs()->get(state->symbols.create("description")))
          state->forceStringNoCtx(*attr->value, attr->pos, "");
        else
          throw Error("template '%s' lacks attribute 'description'", attr_path);

        for (auto& attr : *v.attrs()) {
          std::string_view name(state->symbols[attr.name]);
          if (name != "path" && name != "description" && name != "welcomeText")
            throw Error("template '%s' has unsupported attribute '%s'", attr_path, name);
        }
      } catch (Error& e) {
        e.add_trace(resolve(pos), hint_fmt_t("while checking the template '%s'", attr_path));
        report_error(e);
      }
    };

    auto check_bundler = [&](const std::string& attr_path, value_t& v, const pos_idx_t pos) {
      try {
        activity_t act(*logger, lvl_info, act_unknown, fmt("checking bundler '%s'", attr_path));
        state->forceValue(v, pos);
        if (!v.isLambda())
          throw Error("bundler must be a function");
        // TODO: check types of inputs/outputs?
      } catch (Error& e) {
        e.add_trace(resolve(pos), hint_fmt_t("while checking the template '%s'", attr_path));
        report_error(e);
      }
    };

    auto check_flake = [&]() {
      activity_t act(*logger, lvl_info, act_unknown, "evaluating flake");

      auto v_flake = state->allocValue();
      flake::call_flake(*state, flake, *v_flake);

      enumerate_outputs(
          *state, *v_flake, [&](std::string_view name, value_t& v_output, const pos_idx_t pos) {
            futures.spawn(2, [&, name, pos]() {
              activity_t act(*logger, lvl_info, act_unknown,
                             fmt("checking flake output '%s'", name));

              try {
                eval_settings.enableImportFromDerivation.set_default(name != "hydraJobs");

                state->forceValue(v_output, pos);

                std::string_view replacement =
                    name == "defaultPackage"    ? "packages.<system>.default"
                    : name == "defaultApp"      ? "apps.<system>.default"
                    : name == "defaultTemplate" ? "templates.default"
                    : name == "defaultBundler"  ? "bundlers.<system>.default"
                    : name == "overlay"         ? "overlays.default"
                    : name == "devShell"        ? "devShells.<system>.default"
                    : name == "nixosModule"     ? "nixosModules.default"
                                                : "";
                if (replacement != "")
                  warn("flake output attribute '%s' is deprecated; use '%s' instead", name,
                       replacement);

                if (name == "checks") {
                  state->forceAttrs(v_output, pos, "");
                  for (auto& attr : *v_output.attrs())
                    futures.spawn(3, [&, name]() {
                      const auto& attr_name = state->symbols[attr.name];
                      check_system_name(attr_name, attr.pos);
                      if (check_system_type(attr_name, attr.pos)) {
                        state->forceAttrs(*attr.value, attr.pos, "");
                        for (auto& attr2 : *attr.value->attrs()) {
                          auto drv_path = check_derivation(
                              fmt("%s.%s.%s", name, attr_name, state->symbols[attr2.name]),
                              *attr2.value, attr2.pos);
                          if (drv_path && attr_name == settings.thisSystem.get()) {
                            auto derived_path = derived_path_t::Built{
                                .drv_path = makeConstantStorePathRef(*drv_path),
                                .outputs = OutputsSpec::All{},
                            };
                            (*derivedPathToAttrPaths_.lock())[derived_path].push_back(
                                {state->symbols.create("checks"), attr.name, attr2.name});
                            drvPaths_.lock()->push_back(std::move(derived_path));
                          }
                        }
                      }
                    });
                }

                else if (name == "formatter") {
                  state->forceAttrs(v_output, pos, "");
                  for (auto& attr : *v_output.attrs()) {
                    const auto& attr_name = state->symbols[attr.name];
                    check_system_name(attr_name, attr.pos);
                    if (check_system_type(attr_name, attr.pos)) {
                      check_derivation(fmt("%s.%s", name, attr_name), *attr.value, attr.pos);
                    };
                  }
                }

                else if (name == "packages" || name == "devShells") {
                  state->forceAttrs(v_output, pos, "");
                  for (auto& attr : *v_output.attrs())
                    futures.spawn(3, [&, name]() {
                      const auto& attr_name = state->symbols[attr.name];
                      check_system_name(attr_name, attr.pos);
                      if (check_system_type(attr_name, attr.pos)) {
                        state->forceAttrs(*attr.value, attr.pos, "");
                        for (auto& attr2 : *attr.value->attrs())
                          check_derivation(
                              fmt("%s.%s.%s", name, attr_name, state->symbols[attr2.name]),
                              *attr2.value, attr2.pos);
                      };
                    });
                }

                else if (name == "apps") {
                  state->forceAttrs(v_output, pos, "");
                  for (auto& attr : *v_output.attrs()) {
                    const auto& attr_name = state->symbols[attr.name];
                    check_system_name(attr_name, attr.pos);
                    if (check_system_type(attr_name, attr.pos)) {
                      state->forceAttrs(*attr.value, attr.pos, "");
                      for (auto& attr2 : *attr.value->attrs())
                        check_app(fmt("%s.%s.%s", name, attr_name, state->symbols[attr2.name]),
                                  *attr2.value, attr2.pos);
                    };
                  }
                }

                else if (name == "defaultPackage" || name == "devShell") {
                  state->forceAttrs(v_output, pos, "");
                  for (auto& attr : *v_output.attrs()) {
                    const auto& attr_name = state->symbols[attr.name];
                    check_system_name(attr_name, attr.pos);
                    if (check_system_type(attr_name, attr.pos)) {
                      check_derivation(fmt("%s.%s", name, attr_name), *attr.value, attr.pos);
                    };
                  }
                }

                else if (name == "defaultApp") {
                  state->forceAttrs(v_output, pos, "");
                  for (auto& attr : *v_output.attrs()) {
                    const auto& attr_name = state->symbols[attr.name];
                    check_system_name(attr_name, attr.pos);
                    if (check_system_type(attr_name, attr.pos)) {
                      check_app(fmt("%s.%s", name, attr_name), *attr.value, attr.pos);
                    };
                  }
                }

                else if (name == "legacyPackages") {
                  state->forceAttrs(v_output, pos, "");
                  for (auto& attr : *v_output.attrs()) {
                    check_system_name(state->symbols[attr.name], attr.pos);
                    check_system_type(state->symbols[attr.name], attr.pos);
                    // FIXME: do getDerivations?
                  }
                }

                else if (name == "overlay")
                  check_overlay(name, v_output, pos);

                else if (name == "overlays") {
                  state->forceAttrs(v_output, pos, "");
                  for (auto& attr : *v_output.attrs())
                    check_overlay(fmt("%s.%s", name, state->symbols[attr.name]), *attr.value,
                                  attr.pos);
                }

                else if (name == "nixosModule")
                  check_module(name, v_output, pos);

                else if (name == "nixosModules") {
                  state->forceAttrs(v_output, pos, "");
                  for (auto& attr : *v_output.attrs())
                    check_module(fmt("%s.%s", name, state->symbols[attr.name]), *attr.value,
                                 attr.pos);
                }

                else if (name == "nixosConfigurations") {
                  state->forceAttrs(v_output, pos, "");
                  for (auto& attr : *v_output.attrs())
                    check_nix_os_configuration(fmt("%s.%s", name, state->symbols[attr.name]),
                                               *attr.value, attr.pos);
                }

                else if (name == "hydraJobs")
                  checkHydraJobs(std::string(name), v_output, pos);

                else if (name == "defaultTemplate")
                  check_template(name, v_output, pos);

                else if (name == "templates") {
                  state->forceAttrs(v_output, pos, "");
                  for (auto& attr : *v_output.attrs())
                    check_template(fmt("%s.%s", name, state->symbols[attr.name]), *attr.value,
                                   attr.pos);
                }

                else if (name == "defaultBundler") {
                  state->forceAttrs(v_output, pos, "");
                  for (auto& attr : *v_output.attrs()) {
                    const auto& attr_name = state->symbols[attr.name];
                    check_system_name(attr_name, attr.pos);
                    if (check_system_type(attr_name, attr.pos)) {
                      check_bundler(fmt("%s.%s", name, attr_name), *attr.value, attr.pos);
                    };
                  }
                }

                else if (name == "bundlers") {
                  state->forceAttrs(v_output, pos, "");
                  for (auto& attr : *v_output.attrs()) {
                    const auto& attr_name = state->symbols[attr.name];
                    check_system_name(attr_name, attr.pos);
                    if (check_system_type(attr_name, attr.pos)) {
                      state->forceAttrs(*attr.value, attr.pos, "");
                      for (auto& attr2 : *attr.value->attrs()) {
                        check_bundler(fmt("%s.%s.%s", name, attr_name, state->symbols[attr2.name]),
                                      *attr2.value, attr2.pos);
                      }
                    };
                  }
                }

                else if (name == "lib" || name == "darwinConfigurations" ||
                         name == "darwinModules" || name == "flakeModule" ||
                         name == "flakeModules" || name == "herculesCI" ||
                         name == "homeConfigurations" || name == "homeModule" ||
                         name == "homeModules" || name == "nixopsConfigurations")
                  // Known but unchecked community attribute
                  ;

                else
                  warn("unknown flake output '%s'", name);

              } catch (Error& e) {
                e.add_trace(resolve(pos), hint_fmt_t("while checking flake output '%s'", name));
                report_error(e);
              }
            });
          });
    };

    futures.spawn(1, check_flake);
    futures.finishAll();

    auto drv_paths(drvPaths_.lock());
    auto derived_path_to_attr_paths(derivedPathToAttrPaths_.lock());

    if (build && !drv_paths->empty()) {
      // TODO: This filtering of substitutable paths is a temporary workaround until
      // https://github.com/NixOS/nix/issues/5025 (union stores) is implemented.
      //
      // Once union stores are available, this code should be replaced with a proper
      // union store configuration. Ideally, we'd use a union of multiple destination
      // stores to preserve the current behavior where different substituters can
      // cache different check results.
      //
      // For now, we skip building derivations whose outputs are already available
      // via substitution, as `nix flake check` only needs to verify buildability,
      // not actually produce the outputs.
      state->waitForAllPaths();
      auto missing = store->query_missing(*drv_paths);

      std::vector<derived_path_t> toBuild;
      std::set<derived_path_t> toBuildSet;
      for (auto& path : missing.willBuild) {
        auto derived_path = derived_path_t::Built{
            .drv_path = makeConstantStorePathRef(path),
            .outputs = OutputsSpec::All{},
        };
        toBuild.emplace_back(derived_path);
        toBuildSet.insert(std::move(derived_path));
      }

      for (auto& [derived_path, attrPaths] : *derived_path_to_attr_paths)
        if (!toBuildSet.contains(derived_path))
          for (auto& attr_path : attrPaths)
            notice("✅ " ANSI_BOLD "%s" ANSI_NORMAL ANSI_ITALIC ANSI_FAINT
                   " (previously built)" ANSI_NORMAL,
                   attr_path.to_string(*state));

      // FIXME: should start building while evaluating.
      activity_t act(*logger, lvl_info, act_unknown,
                     fmt("running %d flake checks", toBuild.size()));
      auto build_results = store->build_paths_with_results(toBuild);
      assert(build_results.size() == toBuild.size());

      // Report successes first.
      for (auto& buildResult : build_results)
        if (buildResult.tryGetSuccess())
          for (auto& attr_path : (*derived_path_to_attr_paths)[buildResult.path])
            notice("✅ " ANSI_BOLD "%s" ANSI_NORMAL, attr_path.to_string(*state));

      // Then cancelled builds.
      for (auto& buildResult : build_results)
        if (buildResult.isCancelled())
          for (auto& attr_path : (*derived_path_to_attr_paths)[buildResult.path])
            notice("❓ " ANSI_BOLD "%s" ANSI_NORMAL ANSI_FAINT " (cancelled)",
                   attr_path.to_string(*state));

      // Then failures.
      for (auto& buildResult : build_results)
        if (auto failure = buildResult.tryGetFailure(); failure && !buildResult.isCancelled())
          try {
            has_errors = true;
            for (auto& attr_path : (*derived_path_to_attr_paths)[buildResult.path])
              printError("❌ " ANSI_RED "%s" ANSI_NORMAL, attr_path.to_string(*state));
            failure->rethrow();
          } catch (Error& e) {
            logError(e.info());
          }
    }

    if (!omittedSystems.lock()->empty()) {
      // TODO: empty system is not visible; render all as nix strings?
      warn("The check omitted these incompatible systems: %s\n"
           "Use '--all-systems' to check all.",
           concat_strings_sep(", ", *omittedSystems.lock()));
    }

    if (has_errors)
      throw exit_t(1);
  };
};

static strings_t default_template_attr_paths_prefixes{"templates."};
static strings_t default_template_attr_paths = {"templates.default", "defaultTemplate"};

struct cmd_flake_init_common_t : virtual args_t, EvalCommand {
  std::string template_url = "https://flakehub.com/f/DeterminateSystems/flake-templates/0.1";
  Path dest_dir;

  const LockFlags lock_flags{.writeLockFile = false};

  cmd_flake_init_common_t() {
    add_flag({
        .long_name = "template",
        .short_name = 't',
        .description = "The template to use.",
        .labels = {"template"},
        .handler = {&template_url},
        .completer = {[&](add_completions_t& completions, size_t, std::string_view prefix) {
          complete_flake_ref_with_fragment(completions, getEvalState(), lock_flags,
                                           default_template_attr_paths_prefixes,
                                           default_template_attr_paths, prefix);
        }},
    });
  }

  void run(nix::ref<nix::store_t> store) override {
    auto flake_dir = abs_path(dest_dir);

    auto eval_state = getEvalState();

    auto [templateFlakeRef, templateName] = parse_flake_ref_with_fragment(
        fetch_settings, template_url, std::filesystem::current_path().string());

    auto installable =
        InstallableFlake(nullptr, eval_state, std::move(templateFlakeRef), templateName,
                         ExtendedOutputsSpec::Default(), default_template_attr_paths,
                         default_template_attr_paths_prefixes, lock_flags);

    auto cursor = installable.getCursor(*eval_state);

    auto template_dir_attr = cursor->get_attr("path")->forceValue();
    NixStringContext context;
    auto template_dir = eval_state->coerceToPath(no_pos, template_dir_attr, context, "");

    std::vector<std::filesystem::path> changedFiles;
    std::vector<std::filesystem::path> conflictedFiles;

    [&](this const auto& copy_dir, const source_path_t& from,
        const std::filesystem::path& to) -> void {
      create_dirs(to);

      for (auto& [name, entry] : from.read_directory()) {
        check_interrupt();
        auto from2 = from / name;
        auto to2 = to / name;
        auto st = from2.lstat();
        auto to_st = std::filesystem::symlink_status(to2);
        if (st.type == source_accessor_t::t_directory)
          copy_dir(from2, to2);
        else if (st.type == source_accessor_t::t_regular) {
          auto contents = from2.read_file();
          if (std::filesystem::exists(to_st)) {
            auto contents2 = read_file(to2.string());
            if (contents != contents2) {
              printError(
                  "refusing to overwrite existing file '%s'\n please merge it manually with '%s'",
                  to2.string(), from2);
              conflictedFiles.push_back(to2);
            } else {
              notice("skipping identical file: %s", from2);
            }
            continue;
          } else
            write_file(to2, contents);
        } else if (st.type == source_accessor_t::t_symlink) {
          auto target = from2.read_link();
          if (std::filesystem::exists(to_st)) {
            if (std::filesystem::read_symlink(to2) != target) {
              printError(
                  "refusing to overwrite existing file '%s'\n please merge it manually with '%s'",
                  to2.string(), from2);
              conflictedFiles.push_back(to2);
            } else {
              notice("skipping identical file: %s", from2);
            }
            continue;
          } else
            create_symlink(target, os_string_to_string(path_view_ng_t{to2}));
        } else
          throw Error("path '%s' needs to be a symlink, file, or directory but instead is a %s",
                      from2, st.type_string());
        changedFiles.push_back(to2);
        notice("wrote: %s", to2);
      }
    }(template_dir, flake_dir);

    if (!changedFiles.empty() &&
        std::filesystem::exists(std::filesystem::path{flake_dir} / ".git")) {
      strings_t args = {"-C", flake_dir, "add", "--intent-to-add", "--force", "--"};
      for (auto& s : changedFiles)
        args.emplace_back(s.string());
      run_program("git", true, args);
    }

    if (auto welcomeText = cursor->maybeGetAttr("welcomeText")) {
      notice("\n");
      notice(render_markdown_to_terminal(welcomeText->get_string()));
    }

    if (!conflictedFiles.empty())
      throw Error("encountered %d conflicts - see above", conflictedFiles.size());
  }
};

struct cmd_flake_init_t : cmd_flake_init_common_t {
  std::string description() override {
    return "create a flake in the current directory from a template";
  }

  std::string doc() override {
    return
#include "flake-init.md"
        ;
  }

  cmd_flake_init_t() { dest_dir = "."; }
};

struct cmd_flake_new_t : cmd_flake_init_common_t {
  std::string description() override {
    return "create a flake in the specified directory from a template";
  }

  std::string doc() override {
    return
#include "flake-new.md"
        ;
  }

  cmd_flake_new_t() {
    expect_args({.label = "dest-dir", .handler = {&dest_dir}, .completer = complete_path});
  }
};

struct cmd_flake_clone_t : flake_command_t {
  std::filesystem::path dest_dir;

  std::string description() override { return "clone flake repository"; }

  std::string doc() override {
    return
#include "flake-clone.md"
        ;
  }

  cmd_flake_clone_t() {
    add_flag({
        .long_name = "dest",
        .short_name = 'f',
        .description = "Clone the flake to path *dest*.",
        .labels = {"path"},
        .handler = {&dest_dir},
    });
  }

  void run(nix::ref<nix::store_t> store) override {
    if (dest_dir.empty())
      throw Error("missing flag '--dest'");

    get_flake_ref().resolve(fetch_settings, *store).input.clone(fetch_settings, *store, dest_dir);
  }
};

struct cmd_flake_archive_t : flake_command_t, MixJSON, MixDryRun, MixNoCheckSigs {
  std::string dst_uri;

  SubstituteFlag substitute = NoSubstitute;

  cmd_flake_archive_t() {
    add_flag({
        .long_name = "to",
        .description = "URI of the destination Nix store",
        .labels = {"store-uri"},
        .handler = {&dst_uri},
    });
  }

  std::string description() override { return "copy a flake and all its inputs to a store"; }

  std::string doc() override {
    return
#include "flake-archive.md"
        ;
  }

  void run(nix::ref<nix::store_t> store) override {
    auto flake = lock_flake();

    store_path_set_t sources;

    auto store_path = dry_run ? flake.flake.locked_ref.input.computeStorePath(*store)
                              : std::get<store_path_t>(flake.flake.locked_ref.input.fetch_to_store(
                                    fetch_settings, *store));

    sources.insert(store_path);

    // FIXME: use graph output, handle cycles.
    std::function<nlohmann::json(const Node& node)> traverse;
    traverse = [&](const Node& node) {
      nlohmann::json jsonObj2 = json ? json::object() : nlohmann::json(nullptr);
      for (auto& [inputName, input] : node.inputs) {
        if (auto input_node = std::get_if<0>(&input)) {
          std::optional<store_path_t> store_path;
          if (!(*input_node)->locked_ref.input.isRelative()) {
            store_path =
                dry_run
                    ? (*input_node)->locked_ref.input.computeStorePath(*store)
                    : std::get<store_path_t>(
                          (*input_node)->locked_ref.input.fetch_to_store(fetch_settings, *store));
            sources.insert(*store_path);
          }
          if (json) {
            auto& jsonObj3 = jsonObj2[inputName];
            if (store_path)
              jsonObj3["path"] = store->printStorePath(*store_path);
            jsonObj3["inputs"] = traverse(**input_node);
          } else
            traverse(**input_node);
        }
      }
      return jsonObj2;
    };

    if (json) {
      nlohmann::json json_root = {
          {"path", store->printStorePath(store_path)},
          {"inputs", traverse(*flake.lock_file.root)},
      };
      printJSON(json_root);
    } else {
      traverse(*flake.lock_file.root);
    }

    if (!dry_run && !dst_uri.empty()) {
      ref<store_t> dst_store = dst_uri.empty() ? open_store() : open_store(dst_uri);

      copy_paths(*store, *dst_store, sources, NoRepair, check_sigs, substitute);
    }
  }
};

struct cmd_flake_show_t : flake_command_t, MixJSON {
  bool show_legacy = false;
  bool show_all_systems = false;

  cmd_flake_show_t() {
    add_flag({
        .long_name = "legacy",
        .description = "Show the contents of the `legacyPackages` output.",
        .handler = {&show_legacy, true},
    });
    add_flag({
        .long_name = "all-systems",
        .description = "Show the contents of outputs for all systems.",
        .handler = {&show_all_systems, true},
    });
  }

  std::string description() override { return "show the outputs provided by a flake"; }

  std::string doc() override {
    return
#include "flake-show.md"
        ;
  }

  void run(nix::ref<nix::store_t> store) override {
    eval_settings.enableImportFromDerivation.set_default(false);

    auto state = getEvalState();
    auto flake = make_ref<LockedFlake>(lock_flake());
    auto local_system = std::string(settings.thisSystem.get());

    auto cache = open_eval_cache(*state, flake);

    auto j = nlohmann::json::object();

    std::function<void(eval_cache::AttrCursor & visitor, nlohmann::json & result)> visit;

    FutureVector futures(*state->executor);

    visit = [&](eval_cache::AttrCursor& visitor, nlohmann::json& j) {
      auto attr_path = visitor.getAttrPath();
      auto attrPathS = attr_path.resolve(*state);

      activity_t act(*logger, lvl_info, act_unknown,
                     fmt("evaluating '%s'", attr_path.to_string(*state)));

      try {
        auto recurse = [&]() {
          for (const auto& attr : visitor.getAttrs()) {
            const auto& attr_name = state->symbols[attr];
            auto visitor2 = visitor.get_attr(attr_name);
            auto& j2 = *j.emplace(attr_name, nlohmann::json::object()).first;
            futures.spawn(1, [&, visitor2]() { visit(*visitor2, j2); });
          }
        };

        auto showDerivation = [&]() {
          auto name = visitor.get_attr(state->s.name)->get_string();
          std::optional<std::string> description;
          if (auto aMeta = visitor.maybeGetAttr(state->s.meta)) {
            if (auto aDescription = aMeta->maybeGetAttr(state->s.description))
              description = aDescription->get_string();
          }
          j.emplace("type", "derivation");
          if (!json)
            j.emplace("subtype", attr_path.size() == 2 && attrPathS[0] == "devShell"
                                     ? "development environment"
                                 : attr_path.size() >= 2 && attrPathS[0] == "devShells"
                                     ? "development environment"
                                 : attr_path.size() == 3 && attrPathS[0] == "checks" ? "derivation"
                                 : attr_path.size() >= 1 && attrPathS[0] == "hydraJobs"
                                     ? "derivation"
                                     : "package");
          j.emplace("name", name);
          if (description)
            j.emplace("description", *description);
        };

        auto omit = [&](std::string_view flag) {
          if (json)
            logger->warn(fmt("%s omitted (use '%s' to show)", attr_path.to_string(*state), flag));
          else {
            j.emplace("type", "omitted");
            j.emplace("message",
                      fmt(ANSI_WARNING "omitted" ANSI_NORMAL " (use '%s' to show)", flag));
          }
        };

        if (attr_path.size() == 0 ||
            (attr_path.size() == 1 &&
             (attrPathS[0] == "defaultPackage" || attrPathS[0] == "devShell" ||
              attrPathS[0] == "formatter" || attrPathS[0] == "nixosConfigurations" ||
              attrPathS[0] == "nixosModules" || attrPathS[0] == "defaultApp" ||
              attrPathS[0] == "templates" || attrPathS[0] == "overlays")) ||
            ((attr_path.size() == 1 || attr_path.size() == 2) &&
             (attrPathS[0] == "checks" || attrPathS[0] == "packages" ||
              attrPathS[0] == "devShells" || attrPathS[0] == "apps"))) {
          recurse();
        }

        else if ((attr_path.size() == 2 &&
                  (attrPathS[0] == "defaultPackage" || attrPathS[0] == "devShell" ||
                   attrPathS[0] == "formatter")) ||
                 (attr_path.size() == 3 &&
                  (attrPathS[0] == "checks" || attrPathS[0] == "packages" ||
                   attrPathS[0] == "devShells"))) {
          if (!show_all_systems && std::string(attrPathS[1]) != local_system) {
            omit("--all-systems");
          } else {
            try {
              if (visitor.is_derivation())
                showDerivation();
              else {
                auto name = visitor.getAttrPathStr(state->s.name);
                logger->warn(fmt("%s is not a derivation", name));
              }
            } catch (IFDError& e) {
              logger->warn(fmt("%s omitted due to use of import from derivation",
                               attr_path.to_string(*state)));
            }
          }
        }

        else if (attr_path.size() > 0 && attrPathS[0] == "hydraJobs") {
          try {
            if (visitor.is_derivation())
              showDerivation();
            else
              recurse();
          } catch (IFDError& e) {
            logger->warn(fmt("%s omitted due to use of import from derivation",
                             attr_path.to_string(*state)));
          }
        }

        else if (attr_path.size() > 0 && attrPathS[0] == "legacyPackages") {
          if (attr_path.size() == 1)
            recurse();
          else if (!show_legacy) {
            omit("--legacy");
          } else if (!show_all_systems && std::string(attrPathS[1]) != local_system) {
            omit("--all-systems");
          } else {
            try {
              if (visitor.is_derivation())
                showDerivation();
              else if (attr_path.size() <= 2)
                // FIXME: handle recurseIntoAttrs
                recurse();
            } catch (IFDError& e) {
              logger->warn(fmt("%s omitted due to use of import from derivation",
                               attr_path.to_string(*state)));
            }
          }
        }

        else if ((attr_path.size() == 2 && attrPathS[0] == "defaultApp") ||
                 (attr_path.size() == 3 && attrPathS[0] == "apps")) {
          auto aType = visitor.maybeGetAttr("type");
          std::optional<std::string> description;
          if (auto aMeta = visitor.maybeGetAttr(state->s.meta)) {
            if (auto aDescription = aMeta->maybeGetAttr(state->s.description))
              description = aDescription->get_string();
          }
          if (!aType || aType->get_string() != "app")
            state->error<EvalError>("not an app definition").debugThrow();
          j.emplace("type", "app");
          if (description)
            j.emplace("description", *description);
        }

        else if ((attr_path.size() == 1 && attrPathS[0] == "defaultTemplate") ||
                 (attr_path.size() == 2 && attrPathS[0] == "templates")) {
          auto description = visitor.get_attr("description")->get_string();
          j.emplace("type", "template");
          j.emplace("description", description);
        }

        else {
          auto [type, description] =
              (attr_path.size() == 1 && attrPathS[0] == "overlay") ||
                      (attr_path.size() == 2 && attrPathS[0] == "overlays")
                  ? std::make_pair("nixpkgs-overlay", "Nixpkgs overlay")
              : attr_path.size() == 2 && attrPathS[0] == "nixosConfigurations"
                  ? std::make_pair("nixos-configuration", "NixOS configuration")
              : (attr_path.size() == 1 && attrPathS[0] == "nixosModule") ||
                      (attr_path.size() == 2 && attrPathS[0] == "nixosModules")
                  ? std::make_pair("nixos-module", "NixOS module")
                  : std::make_pair("unknown", "unknown");
          j.emplace("type", type);
          j.emplace("description", description);
        }
      } catch (EvalError& e) {
        if (!(attr_path.size() > 0 && attrPathS[0] == "legacyPackages"))
          throw;
      }
    };

    futures.spawn(1, [&]() { visit(*cache->get_root(), j); });
    futures.finishAll();

    if (json)
      printJSON(j);
    else {
      // For frameworks it's important that structures are as
      // lazy as possible to prevent infinite recursions,
      // performance issues and errors that aren't related to
      // the thing to evaluate. As a consequence, they have to
      // emit more attributes than strictly (sic) necessary.
      // However, these attributes with empty values are not
      // useful to the user so we omit them.
      std::function<bool(const nlohmann::json& j)> hasContent;

      hasContent = [&](const nlohmann::json& j) -> bool {
        if (j.find("type") != j.end())
          return true;
        else {
          for (auto& j2 : j)
            if (hasContent(j2))
              return true;
          return false;
        }
      };

      // Render the JSON into a tree representation.
      std::function<void(nlohmann::json j, const std::string& headerPrefix,
                         const std::string& nextPrefix)>
          render;

      render = [&](nlohmann::json j, const std::string& headerPrefix,
                   const std::string& nextPrefix) {
        if (j.find("type") != j.end()) {
          std::string s;

          std::string type = j["type"];
          if (type == "omitted") {
            s = j["message"];
          } else if (type == "derivation") {
            s = (std::string)j["subtype"] + " '" + (std::string)j["name"] + "'";
          } else {
            s = type;
          }

          logger->cout("%s: %s", headerPrefix, s);
          return;
        }

        logger->cout("%s", headerPrefix);

        auto nonEmpty = nlohmann::json::object();
        for (const auto& j2 : j.items()) {
          if (hasContent(j2.value()))
            nonEmpty[j2.key()] = j2.value();
        }

        for (const auto& [i, j2] : enumerate(nonEmpty.items())) {
          bool last = i + 1 == nonEmpty.size();
          render(j2.value(),
                 fmt(ANSI_GREEN "%s%s" ANSI_NORMAL ANSI_BOLD "%s" ANSI_NORMAL, nextPrefix,
                     last ? tree_last : tree_conn, j2.key()),
                 nextPrefix + (last ? tree_null : tree_line));
        }
      };

      render(j, fmt(ANSI_BOLD "%s" ANSI_NORMAL, flake->flake.locked_ref), "");
    }
  }
};

struct cmd_flake_prefetch_t : flake_command_t, MixJSON {
  std::optional<std::filesystem::path> out_link;

  cmd_flake_prefetch_t() {
    add_flag({
        .long_name = "out-link",
        .short_name = 'o',
        .description = "Create symlink named *path* to the resulting store path.",
        .labels = {"path"},
        .handler = {&out_link},
        .completer = complete_path,
    });
  }

  std::string description() override {
    return "download the source tree denoted by a flake reference into the Nix store";
  }

  std::string doc() override {
    return
#include "flake-prefetch.md"
        ;
  }

  void run(ref<store_t> store) override {
    auto original_ref = get_flake_ref();
    auto resolved_ref = original_ref.resolve(fetch_settings, *store);
    auto [accessor, locked_ref] = resolved_ref.lazyFetch(getEvalState()->fetch_settings, *store);
    auto store_path = fetch_to_store(getEvalState()->fetch_settings, *store, accessor,
                                     FetchMode::Copy, locked_ref.input.get_name());
    auto hash = store->queryPathInfo(store_path)->nar_hash;

    if (json) {
      auto res = nlohmann::json::object();
      res["storePath"] = store->printStorePath(store_path);
      res["hash"] = hash.to_string(hash_format_t::sri, true);
      res["original"] = fetchers::attrs_to_json(resolved_ref.toAttrs());
      res["locked"] = fetchers::attrs_to_json(locked_ref.toAttrs());
      res["locked"].erase("__final"); // internal for now
      printJSON(res);
    } else {
      notice("Downloaded '%s' to '%s' (hash '%s').", locked_ref.to_string(),
             store->printStorePath(store_path), hash.to_string(hash_format_t::sri, true));
    }

    if (out_link) {
      if (auto store2 = store.dynamic_pointer_cast<local_fs_store>())
        create_out_links(*out_link, {BuiltPath::opaque_t{store_path}}, *store2);
      else
        throw Error("'--out-link' is not supported for this Nix store");
    }
  }
};

struct cmd_flake_t : NixMultiCommand {
  cmd_flake_t() : NixMultiCommand("flake", RegisterCommand::getCommandsFor({"flake"})) {}

  std::string description() override { return "manage Nix flakes"; }

  std::string doc() override {
    return
#include "flake.md"
        ;
  }
};

static auto r_cmd_flake = registerCommand<cmd_flake_t>("flake");
static auto r_cmd_flake_archive = registerCommand2<cmd_flake_archive_t>({"flake", "archive"});
static auto r_cmd_flake_check = registerCommand2<cmd_flake_check_t>({"flake", "check"});
static auto r_cmd_flake_clone = registerCommand2<cmd_flake_clone_t>({"flake", "clone"});
static auto r_cmd_flake_info = registerCommand2<cmd_flake_info_t>({"flake", "info"});
static auto r_cmd_flake_init = registerCommand2<cmd_flake_init_t>({"flake", "init"});
static auto r_cmd_flake_lock = registerCommand2<cmd_flake_lock_t>({"flake", "lock"});
static auto r_cmd_flake_metadata = registerCommand2<cmd_flake_metadata_t>({"flake", "metadata"});
static auto r_cmd_flake_new = registerCommand2<cmd_flake_new_t>({"flake", "new"});
static auto r_cmd_flake_prefetch = registerCommand2<cmd_flake_prefetch_t>({"flake", "prefetch"});
static auto r_cmd_flake_show = registerCommand2<cmd_flake_show_t>({"flake", "show"});
static auto r_cmd_flake_update = registerCommand2<cmd_flake_update_t>({"flake", "update"});
