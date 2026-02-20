#include "nix/cmd/command.h"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "nix/cmd/legacy.h"
#include "nix/cmd/markdown.h"
#include "nix/cmd/repl.h"
#include "nix/expr/nixexpr.h"
#include "nix/store/derivations.h"
#include "nix/store/local-fs-store.h"
#include "nix/store/profiles.h"
#include "nix/store/store-open.h"
#include "nix/util/environment-variables.h"
#include "nix/util/strings.h"

namespace nix {

RegisterCommand::commands_t& RegisterCommand::commands() {
  static RegisterCommand::commands_t commands;
  return commands;
}

RegisterLegacyCommand::commands_t& RegisterLegacyCommand::commands() {
  static RegisterLegacyCommand::commands_t commands;
  return commands;
}

nix::commands_t RegisterCommand::getCommandsFor(const std::vector<std::string>& prefix) {
  nix::commands_t res;
  for (auto& [name, command] : RegisterCommand::commands())
    if (name.size() == prefix.size() + 1) {
      bool equal = true;
      for (size_t i = 0; i < prefix.size(); ++i)
        if (name[i] != prefix[i])
          equal = false;
      if (equal)
        res.insert_or_assign(name[prefix.size()], command);
    }
  return res;
}

nlohmann::json NixMultiCommand::to_json() {
  // FIXME: use Command::toJSON() as well.
  return multi_command_t::to_json();
}

void NixMultiCommand::run() {
  if (!get_command()) {
    string_set_t subCommandTextLines;
    for (auto& [name, _] : get_commands()) {
      subCommandTextLines.insert(fmt("- `%s`", name));
    }
    std::string markdownError =
        fmt("`nix %s` requires a sub-command. Available sub-commands:\n\n%s\n", get_command_name(),
            concat_strings_sep("\n", subCommandTextLines));
    throw UsageError(render_markdown_to_terminal(markdownError));
  }
  get_command()->second->run();
}

StoreCommand::StoreCommand() {}

ref<store_t> StoreCommand::getStore() {
  if (!_store)
    _store = createStore();
  return ref<store_t>(_store);
}

ref<store_t> StoreCommand::createStore() {
  return open_store();
}

void StoreCommand::run() {
  run(getStore());
}

CopyCommand::CopyCommand() {
  add_flag({
      .long_name = "from",
      .description = "URL of the source Nix store.",
      .labels = {"store-uri"},
      .handler = {&srcUri},
  });

  add_flag({
      .long_name = "to",
      .description = "URL of the destination Nix store.",
      .labels = {"store-uri"},
      .handler = {&dst_uri},
  });
}

ref<store_t> CopyCommand::createStore() {
  return srcUri.empty() ? StoreCommand::createStore() : open_store(srcUri);
}

ref<store_t> CopyCommand::getDstStore() {
  if (srcUri.empty() && dst_uri.empty())
    throw UsageError("you must pass '--from' and/or '--to'");

  return dst_uri.empty() ? open_store() : open_store(dst_uri);
}

EvalCommand::EvalCommand() {
  add_flag({
      .long_name = "debugger",
      .description = "Start an interactive environment if evaluation fails.",
      .category = MixEvalArgs::category,
      .handler = {&startReplOnEvalErrors, true},
  });
}

EvalCommand::~EvalCommand() {
  if (eval_state)
    eval_state->maybePrintStats();
}

ref<store_t> EvalCommand::getEvalStore() {
  if (!eval_store)
    eval_store = evalStoreUrl ? open_store(*evalStoreUrl) : getStore();
  return ref<store_t>(eval_store);
}

ref<eval_state_t> EvalCommand::getEvalState() {
  if (!eval_state) {
    if (startReplOnEvalErrors && eval_settings.evalCores != 1U) {
      // Disable parallel eval if the debugger is enabled, since
      // they're incompatible at the moment.
      warn("using the debugger disables multi-threaded evaluation");
      eval_settings.evalCores = 1;
    }

    eval_state = std::allocate_shared<eval_state_t>(traceable_allocator<eval_state_t>(),
                                                    lookup_path, getEvalStore(), fetch_settings,
                                                    eval_settings, getStore());

    eval_state->repair = repair;

    if (startReplOnEvalErrors) {
      eval_state->debugRepl = &AbstractNixRepl::runSimple;
    };
  }
  return ref<eval_state_t>(eval_state);
}

MixOperateOnOptions::MixOperateOnOptions() {
  add_flag({
      .long_name = "derivation",
      .description =
          "Operate on the [store derivation](@docroot@/glossary.md#gloss-store-derivation) rather "
          "than its outputs.",
      .category = installablesCategory,
      .handler = {&operateOn, OperateOn::derivation_t},
  });
}

BuiltPathsCommand::BuiltPathsCommand(bool recursive) : recursive(recursive) {
  if (recursive)
    add_flag({
        .long_name = "no-recursive",
        .description = "Apply operation to specified paths only.",
        .category = installablesCategory,
        .handler = {&this->recursive, false},
    });
  else
    add_flag({
        .long_name = "recursive",
        .short_name = 'r',
        .description = "Apply operation to closure of the specified paths.",
        .category = installablesCategory,
        .handler = {&this->recursive, true},
    });

  add_flag({
      .long_name = "all",
      .description = "Apply the operation to every store path.",
      .category = installablesCategory,
      .handler = {&all, true},
  });
}

void BuiltPathsCommand::run(ref<store_t> store, Installables&& installables) {
  BuiltPaths root_paths, all_paths;

  if (all) {
    if (installables.size())
      throw UsageError("'--all' does not expect arguments");
    // XXX: Only uses opaque paths, ignores all the realisations
    for (auto& p : store->query_all_valid_paths())
      root_paths.emplace_back(BuiltPath::opaque_t{p});
    all_paths = root_paths;
  } else {
    root_paths =
        Installable::to_built_paths(getEvalStore(), store, realiseMode, operateOn, installables);
    all_paths = root_paths;

    if (recursive) {
      // XXX: This only computes the store path closure, ignoring
      // intermediate realisations
      store_path_set_t pathsRoots, pathsClosure;
      for (auto& root : root_paths) {
        auto rootFromThis = root.out_paths();
        pathsRoots.insert(rootFromThis.begin(), rootFromThis.end());
      }
      store->computeFSClosure(pathsRoots, pathsClosure);
      for (auto& path : pathsClosure)
        all_paths.emplace_back(BuiltPath::opaque_t{path});
    }
  }

  run(store, std::move(all_paths), std::move(root_paths));
}

StorePathsCommand::StorePathsCommand(bool recursive) : BuiltPathsCommand(recursive) {}

void StorePathsCommand::run(ref<store_t> store, BuiltPaths&& all_paths, BuiltPaths&& root_paths) {
  store_path_set_t store_paths;
  for (auto& builtPath : all_paths)
    for (auto& p : builtPath.out_paths())
      store_paths.insert(p);

  auto sorted = store->topoSortPaths(store_paths);
  std::reverse(sorted.begin(), sorted.end());

  run(store, std::move(sorted));
}

void StorePathCommand::run(ref<store_t> store, store_paths_t&& store_paths) {
  if (store_paths.size() != 1)
    throw UsageError("this command requires exactly one store path");

  run(store, *store_paths.begin());
}

MixProfile::MixProfile() {
  add_flag({
      .long_name = "profile",
      .description = "The profile to operate on.",
      .labels = {"path"},
      .handler = {&profile},
      .completer = complete_path,
  });
}

void MixProfile::updateProfile(const store_path_t& store_path) {
  if (!profile)
    return;
  auto store = getDstStore().dynamic_pointer_cast<local_fs_store>();
  if (!store)
    throw Error("'--profile' is not supported for this Nix store");
  auto profile2 = abs_path(*profile);
  switch_link(profile2, create_generation(*store, profile2, store_path));
}

void MixProfile::updateProfile(const BuiltPaths& buildables) {
  if (!profile)
    return;

  store_paths_t result;

  for (auto& buildable : buildables) {
    std::visit(overloaded{
                   [&](const BuiltPath::opaque_t& bo) { result.push_back(bo.path); },
                   [&](const BuiltPath::Built& bfd) {
                     for (auto& output : bfd.outputs) {
                       result.push_back(output.second);
                     }
                   },
               },
               buildable.raw());
  }

  if (result.size() != 1)
    throw UsageError(
        "'--profile' requires that the arguments produce a single store path, but there are %d",
        result.size());

  updateProfile(result[0]);
}

MixDefaultProfile::MixDefaultProfile() {
  profile = get_default_profile().string();
}

MixEnvironment::MixEnvironment() : ignoreEnvironment(false) {
  add_flag({
      .long_name = "ignore-env",
      .aliases = {"ignore-environment"},
      .short_name = 'i',
      .description =
          "Clear the entire environment, except for those specified with `--keep-env-var`.",
      .category = environment_variables_category,
      .handler = {&ignoreEnvironment, true},
  });

  add_flag({
      .long_name = "keep-env-var",
      .aliases = {"keep"},
      .short_name = 'k',
      .description = "Keep the environment variable *name*, when using `--ignore-env`.",
      .category = environment_variables_category,
      .labels = {"name"},
      .handler = {[&](std::string s) { keepVars.insert(s); }},
  });

  add_flag({
      .long_name = "unset-env-var",
      .aliases = {"unset"},
      .short_name = 'u',
      .description = "Unset the environment variable *name*.",
      .category = environment_variables_category,
      .labels = {"name"},
      .handler = {[&](std::string name) {
        if (setVars.contains(name))
          throw UsageError("Cannot unset environment variable '%s' that is set with '%s'", name,
                           "--set-env-var");

        unsetVars.insert(name);
      }},
  });

  add_flag({
      .long_name = "set-env-var",
      .short_name = 's',
      .description = "Sets an environment variable *name* with *value*.",
      .category = environment_variables_category,
      .labels = {"name", "value"},
      .handler = {[&](std::string name, std::string value) {
        if (unsetVars.contains(name))
          throw UsageError("Cannot set environment variable '%s' that is unset with '%s'", name,
                           "--unset-env-var");

        if (setVars.contains(name))
          throw UsageError(
              "Duplicate definition of environment variable '%s' with '%s' is ambiguous", name,
              "--set-env-var");

        setVars.insert_or_assign(name, value);
      }},
  });
}

void MixEnvironment::setEnviron() {
  if (ignoreEnvironment && !unsetVars.empty())
    throw UsageError("--unset-env-var does not make sense with --ignore-env");

  if (!ignoreEnvironment && !keepVars.empty())
    throw UsageError("--keep-env-var does not make sense without --ignore-env");

  auto env = get_env();

  if (ignoreEnvironment)
    std::erase_if(env, [&](const auto& var) { return !keepVars.contains(var.first); });

  for (const auto& [name, value] : setVars)
    env[name] = value;

  if (!unsetVars.empty())
    std::erase_if(env, [&](const auto& var) { return unsetVars.contains(var.first); });

  replace_env(env);

  return;
}

void create_out_links(const std::filesystem::path& out_link, const BuiltPaths& buildables,
                      local_fs_store& store) {
  for (const auto& [_i, buildable] : enumerate(buildables)) {
    auto i = _i;
    std::visit(overloaded{
                   [&](const BuiltPath::opaque_t& bo) {
                     auto symlink = out_link;
                     if (i)
                       symlink += fmt("-%d", i);
                     store.addPermRoot(bo.path, abs_path(symlink).string());
                   },
                   [&](const BuiltPath::Built& bfd) {
                     for (auto& output : bfd.outputs) {
                       auto symlink = out_link;
                       if (i)
                         symlink += fmt("-%d", i);
                       if (output.first != "out")
                         symlink += fmt("-%s", output.first);
                       store.addPermRoot(output.second, abs_path(symlink).string());
                     }
                   },
               },
               buildable.raw());
  }
}

void MixOutLinkBase::createOutLinksMaybe(const std::vector<BuiltPathWithResult>& buildables,
                                         ref<store_t>& store) {
  if (out_link != "")
    if (auto store2 = store.dynamic_pointer_cast<local_fs_store>())
      create_out_links(out_link, to_built_paths(buildables), *store2);
}

} // namespace nix
