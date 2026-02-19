#include "nix/cmd/installables.h"

#include <queue>
#include <regex>

#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/cmd/common-eval-args.h"
#include "nix/cmd/installable-attr-path.h"
#include "nix/cmd/installable-derived-path.h"
#include "nix/cmd/installable-flake.h"
#include "nix/expr/attr-path.h"
#include "nix/expr/eval-cache.h"
#include "nix/expr/eval-inline.h"
#include "nix/expr/eval-settings.h"
#include "nix/expr/eval.h"
#include "nix/expr/get-drvs.h"
#include "nix/fetchers/registry.h"
#include "nix/flake/flake.h"
#include "nix/main/shared.h"
#include "nix/store/build-result.h"
#include "nix/store/derivations.h"
#include "nix/store/globals.h"
#include "nix/store/outputs-spec.h"
#include "nix/store/store-api.h"
#include "nix/util/exit.h"
#include "nix/util/strings-inline.h"
#include "nix/util/url.h"
#include "nix/util/users.h"
#include "nix/util/util.h"

namespace nix {

void complete_flake_input_attr_path(add_completions_t& completions, ref<EvalState> eval_state,
                                const std::vector<FlakeRef>& flake_refs, std::string_view prefix) {
  for (auto& flake_ref : flake_refs) {
    auto flake = flake::get_flake(*eval_state, flake_ref, fetchers::UseRegistries::All);
    for (auto& input : flake.inputs)
      if (has_prefix(input.first, prefix))
        completions.add(input.first);
  }
}

MixFlakeOptions::MixFlakeOptions() {
  auto category = "Common flake-related options";

  add_flag({
      .long_name = "recreate-lock-file",
      .description = R"(
    Recreate the flake's lock file from scratch.

    > **DEPRECATED**
    >
    > use [`nix flake update`](@docroot@/command-ref/new-cli/nix3-flake-update.md) instead.
        )",
      .category = category,
      .handler = {[&]() {
        lock_flags.recreateLockFile = true;
        warn("'--recreate-lock-file' is deprecated and will be removed in a future version; use "
             "'nix flake update' instead.");
      }},
  });

  add_flag({
      .long_name = "no-update-lock-file",
      .description = "Do not allow any updates to the flake's lock file.",
      .category = category,
      .handler = {&lock_flags.updateLockFile, false},
  });

  add_flag({
      .long_name = "no-write-lock-file",
      .description = "Do not write the flake's newly generated lock file.",
      .category = category,
      .handler = {&lock_flags.writeLockFile, false},
  });

  add_flag({
      .long_name = "no-registries",
      .description = R"(
    Don't allow lookups in the flake registries.

    > **DEPRECATED**
    >
    > use [`--no-use-registries`](@docroot@/command-ref/conf-file.md#conf-use-registries) instead.
        )",
      .category = category,
      .handler = {[&]() {
        lock_flags.use_registries = false;
        warn("'--no-registries' is deprecated; use '--no-use-registries'");
      }},
  });

  add_flag({
      .long_name = "commit-lock-file",
      .description = "Commit changes to the flake's lock file.",
      .category = category,
      .handler = {&lock_flags.commitLockFile, true},
  });

  add_flag({
      .long_name = "update-input",
      .description = R"(
    Update a specific flake input (ignoring its previous entry in the lock file).

    > **DEPRECATED**
    >
    > use [`nix flake update`](@docroot@/command-ref/new-cli/nix3-flake-update.md) instead.
        )",
      .category = category,
      .labels = {"input-path"},
      .handler = {[&](std::string s) {
        warn("'--update-input' is a deprecated alias for 'flake update' and will be removed in a "
             "future version.");
        lock_flags.inputUpdates.insert(flake::parse_input_attr_path(s));
      }},
      .completer = {[&](add_completions_t& completions, size_t, std::string_view prefix) {
        complete_flake_input_attr_path(completions, getEvalState(), get_flake_refs_for_completion(),
                                   prefix);
      }},
  });

  add_flag({
      .long_name = "override-input",
      .description = "Override a specific flake input (e.g. `dwarffs/nixpkgs`). This implies "
                     "`--no-write-lock-file`.",
      .category = category,
      .labels = {"input-path", "flake-url"},
      .handler = {[&](std::string inputAttrPath, std::string flake_ref) {
        lock_flags.writeLockFile = false;
        lock_flags.inputOverrides.insert_or_assign(
            flake::parse_input_attr_path(inputAttrPath),
            parse_flake_ref(fetch_settings, flake_ref, abs_path(get_command_base_dir()).string(), true));
      }},
      .completer = {[&](add_completions_t& completions, size_t n, std::string_view prefix) {
        if (n == 0) {
          complete_flake_input_attr_path(completions, getEvalState(), get_flake_refs_for_completion(),
                                     prefix);
        } else if (n == 1) {
          complete_flake_ref(completions, getEvalState()->store, prefix);
        }
      }},
  });

  add_flag({
      .long_name = "reference-lock-file",
      .description = "Read the given lock file instead of `flake.lock` within the top-level flake.",
      .category = category,
      .labels = {"flake-lock-path"},
      .handler = {[&](std::string lock_file_path) {
        lock_flags.referenceLockFilePath = {get_fs_source_accessor(), canon_path_t(abs_path(lock_file_path))};
      }},
      .completer = complete_path,
  });

  add_flag({
      .long_name = "output-lock-file",
      .description =
          "Write the given lock file instead of `flake.lock` within the top-level flake.",
      .category = category,
      .labels = {"flake-lock-path"},
      .handler = {[&](std::string lock_file_path) { lock_flags.output_lock_file_path = lock_file_path; }},
      .completer = complete_path,
  });

  add_flag({
      .long_name = "inputs-from",
      .description = "Use the inputs of the specified flake as registry entries.",
      .category = category,
      .labels = {"flake-url"},
      .handler = {[&](std::string flake_ref) {
        auto eval_state = getEvalState();
        auto flake = flake::lock_flake(
            flake_settings, *eval_state,
            parse_flake_ref(fetch_settings, flake_ref, abs_path(get_command_base_dir()).string()),
            {.writeLockFile = false});
        for (auto& [inputName, input] : flake.lock_file.root->inputs) {
          auto input2 = flake.lock_file.findInput({inputName}); // resolve 'follows' nodes
          if (auto input3 = std::dynamic_pointer_cast<const flake::LockedNode>(input2)) {
            fetchers::Attrs extra_attrs;

            if (!input3->locked_ref.subdir.empty()) {
              extra_attrs["dir"] = input3->locked_ref.subdir;
            }

            override_registry(fetchers::Input::fromAttrs(fetch_settings,
                                                        {{"type", "indirect"}, {"id", inputName}}),
                             input3->locked_ref.input, extra_attrs);
          }
        }
      }},
      .completer = {[&](add_completions_t& completions, size_t, std::string_view prefix) {
        complete_flake_ref(completions, getEvalState()->store, prefix);
      }},
  });
}

SourceExprCommand::SourceExprCommand() {
  add_flag({
      .long_name = "file",
      .short_name = 'f',
      .description =
          "Interpret [*installables*](@docroot@/command-ref/new-cli/nix.md#installables) as "
          "attribute paths relative to the Nix expression stored in *file*. "
          "If *file* is the character -, then a Nix expression is read from standard input. "
          "Implies `--impure`.",
      .category = installablesCategory,
      .labels = {"file"},
      .handler = {&file},
      .completer = complete_path,
  });

  add_flag({
      .long_name = "expr",
      .description =
          "Interpret [*installables*](@docroot@/command-ref/new-cli/nix.md#installables) as "
          "attribute paths relative to the Nix expression *expr*.",
      .category = installablesCategory,
      .labels = {"expr"},
      .handler = {&expr},
  });
}

MixReadOnlyOption::MixReadOnlyOption() {
  add_flag({
      .long_name = "read-only",
      .description = "Do not instantiate each evaluated derivation. "
                     "This improves performance, but can cause errors when accessing "
                     "store paths of derivations during evaluation.",
      .handler = {&settings.readOnlyMode, true},
  });
}

strings_t SourceExprCommand::getDefaultFlakeAttrPaths() {
  return {"packages." + settings.thisSystem.get() + ".default",
          "defaultPackage." + settings.thisSystem.get()};
}

strings_t SourceExprCommand::getDefaultFlakeAttrPathPrefixes() {
  return {// As a convenience, look for the attribute in
          // 'outputs.packages'.
          "packages." + settings.thisSystem.get() + ".",
          // As a temporary hack until Nixpkgs is properly converted
          // to provide a clean 'packages' set, look in 'legacyPackages'.
          "legacyPackages." + settings.thisSystem.get() + "."};
}

Args::completer_closure_t SourceExprCommand::getCompleteInstallable() {
  return [this](add_completions_t& completions, size_t, std::string_view prefix) {
    completeInstallable(completions, prefix);
  };
}

void SourceExprCommand::completeInstallable(add_completions_t& completions, std::string_view prefix) {
  try {
    if (file) {
      completions.set_type(add_completions_t::Type::attrs);

      eval_settings.pureEval = false;
      auto state = getEvalState();
      auto e = state->parseExprFromFile(resolve_expr_path(lookup_file_arg(*state, file->string())));

      Value root;
      state->eval(e, root);

      auto auto_args = getAutoArgs(*state);

      std::string prefix_ = std::string(prefix);
      auto sep = prefix_.rfind('.');
      std::string searchWord;
      if (sep != std::string::npos) {
        searchWord = prefix_.substr(sep + 1, std::string::npos);
        prefix_ = prefix_.substr(0, sep);
      } else {
        searchWord = prefix_;
        prefix_ = "";
      }

      auto [v, pos] = find_along_attr_path(*state, prefix_, *auto_args, root);
      Value& v1(*v);
      state->forceValue(v1, pos);
      Value v2;
      state->autoCallFunction(*auto_args, v1, v2);

      if (v2.type() == nAttrs) {
        for (auto& i : *v2.attrs()) {
          std::string_view name = state->symbols[i.name];
          if (name.find(searchWord) == 0) {
            if (prefix_ == "")
              completions.add(std::string(name));
            else
              completions.add(prefix_ + "." + name);
          }
        }
      }
    } else {
      complete_flake_ref_with_fragment(completions, getEvalState(), lock_flags,
                                   getDefaultFlakeAttrPathPrefixes(), getDefaultFlakeAttrPaths(),
                                   prefix);
    }
  } catch (EvalError&) {
    // Don't want eval errors to mess-up with the completion engine, so let's just swallow them
  }
}

void complete_flake_ref_with_fragment(add_completions_t& completions, ref<EvalState> eval_state,
                                  flake::LockFlags lock_flags, strings_t attr_path_prefixes,
                                  const strings_t& default_flake_attr_paths, std::string_view prefix) {
  /* Look for flake output attributes that match the
     prefix. */
  try {
    auto hash = prefix.find('#');
    if (hash == std::string::npos) {
      complete_flake_ref(completions, eval_state->store, prefix);
    } else {
      completions.set_type(add_completions_t::Type::attrs);

      auto fragment = prefix.substr(hash + 1);
      std::string prefix_root = "";
      if (fragment.starts_with(".")) {
        fragment = fragment.substr(1);
        prefix_root = ".";
      }
      auto flake_ref_s = std::string(prefix.substr(0, hash));

      // TODO: ideally this would use the command base directory instead of assuming ".".
      auto flake_ref = parse_flake_ref(fetch_settings, expand_tilde(flake_ref_s),
                                    std::filesystem::current_path().string());

      auto eval_cache = open_eval_cache(
          *eval_state,
          make_ref<flake::LockedFlake>(lock_flake(flake_settings, *eval_state, flake_ref, lock_flags)));

      auto root = eval_cache->get_root();

      if (prefix_root == ".") {
        attr_path_prefixes.clear();
      }
      /* Complete 'fragment' relative to all the
         attrpath prefixes as well as the root of the
         flake. */
      attr_path_prefixes.push_back("");

      for (auto& attrPathPrefixS : attr_path_prefixes) {
        auto attrPathPrefix = AttrPath::parse(*eval_state, attrPathPrefixS);
        auto attrPathS = attrPathPrefixS + std::string(fragment);
        auto attr_path = AttrPath::parse(*eval_state, attrPathS);

        std::string lastAttr;
        if (!attr_path.empty() && !has_suffix(attrPathS, ".")) {
          lastAttr = eval_state->symbols[attr_path.back()];
          attr_path.pop_back();
        }

        auto attr = root->find_along_attr_path(attr_path);
        if (!attr)
          continue;

        for (auto& attr2 : (*attr)->getAttrs()) {
          if (has_prefix(eval_state->symbols[attr2], lastAttr)) {
            auto attrPath2 = (*attr)->getAttrPath(attr2);
            /* Strip the attrpath prefix. */
            attrPath2.erase(attrPath2.begin(), attrPath2.begin() + attrPathPrefix.size());
            // FIXME: handle names with dots
            completions.add(flake_ref_s + "#" + prefix_root + attrPath2.to_string(*eval_state));
          }
        }
      }

      /* And add an empty completion for the default
         attrpaths. */
      if (fragment.empty()) {
        for (auto& attr_path : default_flake_attr_paths) {
          auto attr = root->find_along_attr_path(AttrPath::parse(*eval_state, attr_path));
          if (!attr)
            continue;
          completions.add(flake_ref_s + "#" + prefix_root);
        }
      }
    }
  } catch (Error& e) {
    warn(e.msg());
  }
}

void complete_flake_ref(add_completions_t& completions, ref<Store> store, std::string_view prefix) {
  if (prefix == "")
    completions.add(".");

  Args::complete_dir(completions, 0, prefix);

  /* Look for registry entries that match the prefix. */
  for (auto& registry : fetchers::get_registries(fetch_settings, *store)) {
    for (auto& entry : registry->entries) {
      auto from = entry.from.to_string();
      if (!has_prefix(prefix, "flake:") && has_prefix(from, "flake:")) {
        std::string from2(from, 6);
        if (has_prefix(from2, prefix))
          completions.add(from2);
      } else {
        if (has_prefix(from, prefix))
          completions.add(from);
      }
    }
  }
}

DerivedPathWithInfo Installable::toDerivedPath() {
  auto buildables = to_derived_paths();
  if (buildables.size() != 1)
    throw Error("installable '%s' evaluates to %d derivations, where only one is expected", what(),
                buildables.size());
  return std::move(buildables[0]);
}

static StorePath get_deriver(ref<Store> store, const Installable& i, const StorePath& drv_path) {
  auto derivers = store->queryValidDerivers(drv_path);
  if (derivers.empty())
    throw Error("'%s' does not have a known deriver", i.what());
  // FIXME: use all derivers?
  return *derivers.begin();
}

Installables SourceExprCommand::parseInstallables(ref<Store> store, std::vector<std::string> ss) {
  Installables result;

  if (file || expr) {
    if (file && expr)
      throw UsageError("'--file' and '--expr' are exclusive");

    // FIXME: backward compatibility hack
    if (file) {
      if (eval_settings.pureEval && eval_settings.pureEval.overridden)
        throw UsageError("'--file' is not compatible with '--pure-eval'");
      eval_settings.pureEval = false;
    }

    auto state = getEvalState();
    auto vFile = state->allocValue();

    if (file == "-") {
      auto e = state->parseStdin();
      state->eval(e, *vFile);
    } else if (file) {
      auto dir = abs_path(get_command_base_dir());
      state->evalFile(lookup_file_arg(*state, file->string(), &dir), *vFile);
    } else {
      auto dir = abs_path(get_command_base_dir());
      auto e = state->parseExprFromString(*expr, state->root_path(dir.string()));
      state->eval(e, *vFile);
    }

    for (auto& s : ss) {
      auto [prefix, extendedOutputsSpec] = ExtendedOutputsSpec::parse(s);
      result.push_back(make_ref<InstallableAttrPath>(InstallableAttrPath::parse(
          state, *this, vFile, std::move(prefix), std::move(extendedOutputsSpec))));
    }

  } else {
    for (auto& s : ss) {
      std::exception_ptr ex;

      auto [prefix_, extendedOutputsSpec_] = ExtendedOutputsSpec::parse(s);
      // To avoid clang's pedantry
      auto prefix = std::move(prefix_);
      auto extendedOutputsSpec = std::move(extendedOutputsSpec_);

      if (prefix.find('/') != std::string::npos) {
        try {
          result.push_back(make_ref<InstallableDerivedPath>(
              InstallableDerivedPath::parse(store, prefix, extendedOutputsSpec.raw)));
          continue;
        } catch (BadStorePath&) {
        } catch (...) {
          if (!ex)
            ex = std::current_exception();
        }
      }

      try {
        auto [flake_ref, fragment] = parse_flake_ref_with_fragment(fetch_settings, std::string{prefix},
                                                              abs_path(get_command_base_dir()));
        result.push_back(make_ref<InstallableFlake>(
            this, getEvalState(), std::move(flake_ref), fragment, std::move(extendedOutputsSpec),
            getDefaultFlakeAttrPaths(), getDefaultFlakeAttrPathPrefixes(), lock_flags));
        continue;
      } catch (...) {
        ex = std::current_exception();
      }

      std::rethrow_exception(ex);
    }
  }

  return result;
}

ref<Installable> SourceExprCommand::parseInstallable(ref<Store> store,
                                                     const std::string& installable) {
  auto installables = parseInstallables(store, {installable});
  assert(installables.size() == 1);
  return installables.front();
}

static SingleBuiltPath get_built_path(ref<Store> eval_store, ref<Store> store,
                                    const SingleDerivedPath& b) {
  return std::visit(overloaded{
                        [&](const SingleDerivedPath::opaque_t& bo) -> SingleBuiltPath {
                          return SingleBuiltPath::opaque_t{bo.path};
                        },
                        [&](const SingleDerivedPath::Built& bfd) -> SingleBuiltPath {
                          auto drv_path = get_built_path(eval_store, store, *bfd.drv_path);
                          // Resolving this instead of `bfd` will yield the same result, but avoid
                          // duplicative work.
                          SingleDerivedPath::Built truncatedBfd{
                              .drv_path = makeConstantStorePathRef(drv_path.out_path()),
                              .output = bfd.output,
                          };
                          auto output_path = resolve_derived_path(*store, truncatedBfd, &*eval_store);
                          return SingleBuiltPath::Built{
                              .drv_path = make_ref<SingleBuiltPath>(std::move(drv_path)),
                              .output = {bfd.output, output_path},
                          };
                        },
                    },
                    b.raw());
}

const BuiltPathWithResult& InstallableWithBuildResult::getSuccess() const {
  if (auto* failure = std::get_if<Failure>(&result)) {
    auto failure2 = failure->tryGetFailure();
    assert(failure2);
    failure2->rethrow();
  } else
    return *std::get_if<Success>(&result);
}

void Installable::throwBuildErrors(std::vector<InstallableWithBuildResult>& build_results,
                                   const Store& store) {
  for (auto& buildResult : build_results) {
    if (std::get_if<InstallableWithBuildResult::Failure>(&buildResult.result)) {
      // Report success first.
      for (auto& buildResult : build_results) {
        if (std::get_if<InstallableWithBuildResult::Success>(&buildResult.result))
          notice("✅ " ANSI_BOLD "%s" ANSI_NORMAL, buildResult.installable->what());
      }

      // Then cancelled builds.
      for (auto& buildResult : build_results) {
        if (auto failure = std::get_if<InstallableWithBuildResult::Failure>(&buildResult.result)) {
          if (failure->isCancelled())
            notice("❓ " ANSI_BOLD "%s" ANSI_NORMAL ANSI_FAINT " (cancelled)",
                   buildResult.installable->what());
        }
      }

      // Then failures.
      for (auto& buildResult : build_results) {
        if (auto failure = std::get_if<InstallableWithBuildResult::Failure>(&buildResult.result)) {
          if (failure->isCancelled())
            continue;
          auto failure2 = failure->tryGetFailure();
          assert(failure2);
          printError("❌ " ANSI_RED "%s" ANSI_NORMAL, buildResult.installable->what());
          try {
            failure2->rethrow();
          } catch (Error& e) {
            logError(e.info());
          }
        }
      }

      throw exit_t(1);
    }
  }
}

std::vector<BuiltPathWithResult> Installable::build(ref<Store> eval_store, ref<Store> store,
                                                    Realise mode, const Installables& installables,
                                                    BuildMode bMode) {
  auto results = build2(eval_store, store, mode, installables, bMode);
  throwBuildErrors(results, *store);
  std::vector<BuiltPathWithResult> res;
  for (auto& b : results)
    res.push_back(b.getSuccess());
  return res;
}

std::vector<InstallableWithBuildResult> Installable::build2(ref<Store> eval_store, ref<Store> store,
                                                            Realise mode,
                                                            const Installables& installables,
                                                            BuildMode bMode) {
  if (mode == Realise::Nothing)
    settings.readOnlyMode = true;

  struct Aux {
    ref<ExtraPathInfo> info;
    ref<Installable> installable;
  };

  std::vector<DerivedPath> pathsToBuild;
  std::map<DerivedPath, std::vector<Aux>> backmap;

  for (auto& i : installables) {
    for (auto b : i->to_derived_paths()) {
      pathsToBuild.push_back(b.path);
      backmap[b.path].push_back({.info = b.info, .installable = i});
    }
  }

  std::vector<InstallableWithBuildResult> res;

  switch (mode) {
    case Realise::Nothing:
    case Realise::Derivation:
      print_missing(store, pathsToBuild, lvl_error);

      for (auto& path : pathsToBuild) {
        for (auto& aux : backmap[path]) {
          std::visit(
              overloaded{
                  [&](const DerivedPath::Built& bfd) {
                    auto outputs = resolve_derived_path(*store, bfd, &*eval_store);
                    res.push_back({.installable = aux.installable,
                                   .result = InstallableWithBuildResult::Success{
                                       .path =
                                           BuiltPath::Built{
                                               .drv_path = make_ref<SingleBuiltPath>(
                                                   get_built_path(eval_store, store, *bfd.drv_path)),
                                               .outputs = outputs,
                                           },
                                       .info = aux.info}});
                  },
                  [&](const DerivedPath::opaque_t& bo) {
                    res.push_back({.installable = aux.installable,
                                   .result = InstallableWithBuildResult::Success{
                                       .path = BuiltPath::opaque_t{bo.path}, .info = aux.info}});
                  },
              },
              path.raw());
        }
      }

      break;

    case Realise::Outputs: {
      if (settings.print_missing)
        print_missing(store, pathsToBuild, lvl_info);

      auto build_results = store->build_paths_with_results(pathsToBuild, bMode, eval_store);
      for (auto& buildResult : build_results) {
        if (buildResult.tryGetFailure()) {
          for (auto& aux : backmap[buildResult.path]) {
            res.push_back({.installable = aux.installable, .result = buildResult});
          }
          continue;
        }
        auto& success = std::get<nix::BuildResult::Success>(buildResult.inner);
        for (auto& aux : backmap[buildResult.path]) {
          std::visit(overloaded{
                         [&](const DerivedPath::Built& bfd) {
                           std::map<std::string, StorePath> outputs;
                           for (auto& [output_name, realisation] : success.built_outputs)
                             outputs.emplace(output_name, realisation.out_path);
                           res.push_back(
                               {.installable = aux.installable,
                                .result = InstallableWithBuildResult::Success{
                                    .path =
                                        BuiltPath::Built{
                                            .drv_path = make_ref<SingleBuiltPath>(
                                                get_built_path(eval_store, store, *bfd.drv_path)),
                                            .outputs = outputs,
                                        },
                                    .info = aux.info,
                                    .result = buildResult}});
                         },
                         [&](const DerivedPath::opaque_t& bo) {
                           res.push_back({.installable = aux.installable,
                                          .result = InstallableWithBuildResult::Success{
                                              .path = BuiltPath::opaque_t{bo.path},
                                              .info = aux.info,
                                              .result = buildResult}});
                         },
                     },
                     buildResult.path.raw());
        }
      }

      break;
    }

    default:
      assert(false);
  }

  return res;
}

BuiltPaths Installable::to_built_paths(ref<Store> eval_store, ref<Store> store, Realise mode,
                                     OperateOn operateOn, const Installables& installables) {
  if (operateOn == OperateOn::Output) {
    BuiltPaths res;
    for (auto& p : Installable::build(eval_store, store, mode, installables))
      res.push_back(p.path);
    return res;
  } else {
    if (mode == Realise::Nothing)
      settings.readOnlyMode = true;

    BuiltPaths res;
    for (auto& drv_path : Installable::toDerivations(store, installables, true))
      res.emplace_back(BuiltPath::opaque_t{drv_path});
    return res;
  }
}

StorePathSet Installable::toStorePathSet(ref<Store> eval_store, ref<Store> store, Realise mode,
                                         OperateOn operateOn, const Installables& installables) {
  StorePathSet out_paths;
  for (auto& path : to_built_paths(eval_store, store, mode, operateOn, installables)) {
    auto thisOutPaths = path.out_paths();
    out_paths.insert(thisOutPaths.begin(), thisOutPaths.end());
  }
  return out_paths;
}

StorePaths Installable::toStorePaths(ref<Store> eval_store, ref<Store> store, Realise mode,
                                     OperateOn operateOn, const Installables& installables) {
  StorePaths out_paths;
  for (auto& path : to_built_paths(eval_store, store, mode, operateOn, installables)) {
    auto thisOutPaths = path.out_paths();
    out_paths.insert(out_paths.end(), thisOutPaths.begin(), thisOutPaths.end());
  }
  return out_paths;
}

StorePath Installable::toStorePath(ref<Store> eval_store, ref<Store> store, Realise mode,
                                   OperateOn operateOn, ref<Installable> installable) {
  auto paths = toStorePathSet(eval_store, store, mode, operateOn, {installable});

  if (paths.size() != 1)
    throw Error("argument '%s' should evaluate to one store path", installable->what());

  return *paths.begin();
}

StorePathSet Installable::toDerivations(ref<Store> store, const Installables& installables,
                                        bool useDeriver) {
  StorePathSet drv_paths;

  for (const auto& i : installables)
    for (const auto& b : i->to_derived_paths())
      std::visit(
          overloaded{
              [&](const DerivedPath::opaque_t& bo) {
                drv_paths.insert(
                    bo.path.is_derivation() ? bo.path
                    : useDeriver
                        ? get_deriver(store, *i, bo.path)
                        : throw Error("argument '%s' did not evaluate to a derivation", i->what()));
              },
              [&](const DerivedPath::Built& bfd) {
                drv_paths.insert(resolve_derived_path(*store, *bfd.drv_path));
              },
          },
          b.path.raw());

  return drv_paths;
}

RawInstallablesCommand::RawInstallablesCommand() {
  add_flag({
      .long_name = "stdin",
      .description = "Read installables from the standard input. No default installable applied.",
      .handler = {&readFromStdIn, true},
  });

  expect_args({
      .label = "installables",
      .handler = {&raw_installables},
      .completer = getCompleteInstallable(),
  });
}

void RawInstallablesCommand::applyDefaultInstallables(std::vector<std::string>& raw_installables) {
  if (raw_installables.empty()) {
    // FIXME: commands like "nix profile add" should not have a
    // default, probably.
    raw_installables.push_back(".");
  }
}

std::vector<FlakeRef> RawInstallablesCommand::get_flake_refs_for_completion() {
  applyDefaultInstallables(raw_installables);
  std::vector<FlakeRef> res;
  res.reserve(raw_installables.size());
  for (const auto& i : raw_installables)
    res.push_back(parse_flake_ref_with_fragment(fetch_settings, expand_tilde(i),
                                            abs_path(get_command_base_dir()).string())
                      .first);
  return res;
}

void RawInstallablesCommand::run(ref<Store> store) {
  if (readFromStdIn && !isatty(STDIN_FILENO)) {
    std::string word;
    while (std::cin >> word) {
      raw_installables.emplace_back(std::move(word));
    }
  } else {
    applyDefaultInstallables(raw_installables);
  }
  run(store, std::move(raw_installables));
}

std::vector<FlakeRef> InstallableCommand::get_flake_refs_for_completion() {
  return {parse_flake_ref_with_fragment(fetch_settings, expand_tilde(_installable),
                                    abs_path(get_command_base_dir()).string())
              .first};
}

void InstallablesCommand::run(ref<Store> store, std::vector<std::string>&& raw_installables) {
  auto installables = parseInstallables(store, raw_installables);
  run(store, std::move(installables));
}

InstallableCommand::InstallableCommand() : SourceExprCommand() {
  expect_args({
      .label = "installable",
      .optional = true,
      .handler = {&_installable},
      .completer = getCompleteInstallable(),
  });
}

void InstallableCommand::preRun(ref<Store> store) {}

void InstallableCommand::run(ref<Store> store) {
  preRun(store);
  auto installable = parseInstallable(store, _installable);
  run(store, std::move(installable));
}

void BuiltPathsCommand::applyDefaultInstallables(std::vector<std::string>& raw_installables) {
  if (raw_installables.empty() && !all)
    raw_installables.push_back(".");
}

BuiltPaths to_built_paths(const std::vector<BuiltPathWithResult>& built_paths_with_result) {
  BuiltPaths res;
  for (auto& i : built_paths_with_result)
    res.push_back(i.path);
  return res;
}

} // namespace nix
