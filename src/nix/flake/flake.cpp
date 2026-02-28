#include "nix/flake/flake.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <assert.h>
#include <stdint.h>

#include <boost/container/detail/std_fwd.hpp>
#include <boost/core/pointer_traits.hpp>
#include <boost/unordered/detail/foa/table.hpp>
#include <nlohmann/json.hpp>

#include "nix/expr/attr-set.h"
#include "nix/expr/eval-cache.h"
#include "nix/expr/eval-error.h"
#include "nix/expr/eval-inline.h"
#include "nix/expr/eval-settings.h"
#include "nix/expr/eval.h"
#include "nix/expr/nixexpr.h"
#include "nix/expr/symbol-table.h"
#include "nix/expr/value-to-json.h"
#include "nix/expr/value.h"
#include "nix/expr/value/context.h"
#include "nix/fetchers/attrs.h"
#include "nix/fetchers/fetch-settings.h"
#include "nix/fetchers/fetch-to-store.h"
#include "nix/fetchers/fetchers.h"
#include "nix/fetchers/input-cache.h"
#include "nix/fetchers/registry.h"
#include "nix/flake/flakeref.h"
#include "nix/flake/lockfile.h"
#include "nix/flake/settings.h"
#include "nix/store/path.h"
#include "nix/store/store-api.h"
#include "nix/util/canon-path.h"
#include "nix/util/configuration.h"
#include "nix/util/environment-variables.h"
#include "nix/util/error.h"
#include "nix/util/experimental-features.h"
#include "nix/util/file-system.h"
#include "nix/util/finally.h"
#include "nix/util/fmt.h"
#include "nix/util/hash.h"
#include "nix/util/logging.h"
#include "nix/util/memory-source-accessor.h"
#include "nix/util/mounted-source-accessor.h"
#include "nix/util/pos-idx.h"
#include "nix/util/pos-table.h"
#include "nix/util/position.h"
#include "nix/util/ref.h"
#include "nix/util/source-path.h"
#include "nix/util/terminal.h"
#include "nix/util/types.h"
#include "nix/util/util.h"

namespace nix {
struct source_accessor_t;

namespace flake {

static void force_trivial_value(eval_state_t& state, value_t& value, const pos_idx_t pos) {
  if (value.isTrivial())
    state.forceValue(value, pos);
}

static void expect_type(eval_state_t& state, ValueType type, value_t& value, const pos_idx_t pos) {
  force_trivial_value(state, value, pos);
  auto t = value.type();
  if (t != type)
    throw Error("expected %s but got %s at %s", show_type(type), show_type(t),
                state.positions[pos]);
}

static std::pair<std::map<FlakeId, FlakeInput>, fetchers::Attrs>
parseFlakeInputs(eval_state_t& state, value_t* value, const pos_idx_t pos,
                 const InputAttrPath& lock_root_attr_path, const source_path_t& flake_dir,
                 bool allowSelf);

static void parse_flake_input_attr(eval_state_t& state, const nix::attr_t& attr,
                                   fetchers::Attrs& attrs) {
// Allow selecting a subset of enum values
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
  switch (attr.value->type()) {
    case nString:
      attrs.emplace(state.symbols[attr.name], std::string(attr.value->string_view()));
      break;
    case nBool:
      attrs.emplace(state.symbols[attr.name], explicit_t<bool>{attr.value->boolean()});
      break;
    case nInt: {
      auto int_value = attr.value->integer().value;
      if (int_value < 0)
        state
            .error<EvalError>("negative value given for flake input attribute %1%: %2%",
                              state.symbols[attr.name], int_value)
            .debugThrow();
      attrs.emplace(state.symbols[attr.name], uint64_t(int_value));
      break;
    }
    default:
      if (attr.name == state.symbols.create("publicKeys")) {
        experimental_feature_settings.require(xp_t::verified_fetches);
        NixStringContext empty_context = {};
        attrs.emplace(
            state.symbols[attr.name],
            print_value_as_json(state, true, *attr.value, attr.pos, empty_context).dump());
      } else
        state
            .error<TypeError>(
                "flake input attribute '%s' is %s while a string, Boolean, or integer is expected",
                state.symbols[attr.name], show_type(*attr.value))
            .debugThrow();
  }
#pragma GCC diagnostic pop
}

static FlakeInput parse_flake_input(eval_state_t& state, value_t* value, const pos_idx_t pos,
                                    const InputAttrPath& lock_root_attr_path,
                                    const source_path_t& flake_dir) {
  expect_type(state, nAttrs, *value, pos);

  FlakeInput input;

  auto s_inputs = state.symbols.create("inputs");
  auto s_url = state.symbols.create("url");
  auto s_flake = state.symbols.create("flake");
  auto s_follows = state.symbols.create("follows");
  auto s_build_time = state.symbols.create("buildTime");

  fetchers::Attrs attrs;
  std::optional<std::string> url;

  for (auto& attr : *value->attrs()) {
    try {
      if (attr.name == s_url) {
        force_trivial_value(state, *attr.value, pos);
        if (attr.value->type() == nString)
          url = attr.value->string_view();
        else if (attr.value->type() == nPath) {
          auto path = attr.value->path();
          if (path.accessor != flake_dir.accessor)
            throw Error("input attribute path '%s' at %s must be in the same source tree as %s",
                        path, state.positions[attr.pos], flake_dir);
          url = "path:" + flake_dir.path.make_relative(path.path);
        } else
          throw Error("expected a string or a path but got %s at %s", show_type(attr.value->type()),
                      state.positions[attr.pos]);
        attrs.emplace("url", *url);
      } else if (attr.name == s_flake) {
        expect_type(state, nBool, *attr.value, attr.pos);
        input.is_flake = attr.value->boolean();
      } else if (attr.name == s_build_time) {
        expect_type(state, nBool, *attr.value, attr.pos);
        input.buildTime = attr.value->boolean();
        if (input.buildTime)
          experimental_feature_settings.require(xp_t::build_time_fetch_tree);
      } else if (attr.name == s_inputs) {
        input.overrides =
            parseFlakeInputs(state, attr.value, attr.pos, lock_root_attr_path, flake_dir, false)
                .first;
      } else if (attr.name == s_follows) {
        expect_type(state, nString, *attr.value, attr.pos);
        auto follows(parse_input_attr_path(attr.value->string_view()));
        follows.insert(follows.begin(), lock_root_attr_path.begin(), lock_root_attr_path.end());
        input.follows = follows;
      } else
        parse_flake_input_attr(state, attr, attrs);
    } catch (Error& e) {
      e.add_trace(state.positions[attr.pos],
                  hint_fmt_t("while evaluating flake attribute '%s'", state.symbols[attr.name]));
      throw;
    }
  }

  if (attrs.count("type"))
    try {
      input.ref = flake_ref_t::fromAttrs(state.fetch_settings, attrs);
    } catch (Error& e) {
      e.add_trace(state.positions[pos], hint_fmt_t("while evaluating flake input"));
      throw;
    }
  else {
    attrs.erase("url");
    if (!attrs.empty())
      throw Error("unexpected flake input attribute '%s', at %s", attrs.begin()->first,
                  state.positions[pos]);
    if (url)
      input.ref = parse_flake_ref(state.fetch_settings, *url, {}, true, input.is_flake, true);
  }

  if (input.ref && input.follows)
    throw Error("flake input has both a flake reference and a follows attribute, at %s",
                state.positions[pos]);

  return input;
}

static std::pair<std::map<FlakeId, FlakeInput>, fetchers::Attrs>
parseFlakeInputs(eval_state_t& state, value_t* value, const pos_idx_t pos,
                 const InputAttrPath& lock_root_attr_path, const source_path_t& flake_dir,
                 bool allowSelf) {
  std::map<FlakeId, FlakeInput> inputs;
  fetchers::Attrs selfAttrs;

  expect_type(state, nAttrs, *value, pos);

  for (auto& inputAttr : *value->attrs()) {
    auto inputName = state.symbols[inputAttr.name];
    if (inputName == "self") {
      if (!allowSelf)
        throw Error("'self' input attribute not allowed at %s", state.positions[inputAttr.pos]);
      expect_type(state, nAttrs, *inputAttr.value, inputAttr.pos);
      for (auto& attr : *inputAttr.value->attrs())
        parse_flake_input_attr(state, attr, selfAttrs);
    } else {
      inputs.emplace(inputName, parse_flake_input(state, inputAttr.value, inputAttr.pos,
                                                  lock_root_attr_path, flake_dir));
    }
  }

  return {inputs, selfAttrs};
}

static flake_t read_flake(eval_state_t& state, const flake_ref_t& original_ref,
                          const flake_ref_t& resolved_ref, const flake_ref_t& locked_ref,
                          const source_path_t& root_dir, const InputAttrPath& lock_root_attr_path) {
  auto flake_dir = root_dir / canon_path_t(resolved_ref.subdir);
  auto flake_path = flake_dir / "flake.nix";

  // NOTE evalFile forces vInfo to be an attrset because mustBeTrivial is true.
  value_t v_info;
  state.evalFile(flake_path, v_info, true);

  flake_t flake{
      .original_ref = original_ref,
      .resolved_ref = resolved_ref,
      .locked_ref = locked_ref,
      .path = flake_path,
  };

  if (auto description = v_info.attrs()->get(state.s.description)) {
    expect_type(state, nString, *description->value, description->pos);
    flake.description = description->value->string_view();
  }

  auto s_inputs = state.symbols.create("inputs");

  if (auto inputs = v_info.attrs()->get(s_inputs)) {
    auto [flakeInputs, selfAttrs] =
        parseFlakeInputs(state, inputs->value, inputs->pos, lock_root_attr_path, flake_dir, true);
    flake.inputs = std::move(flakeInputs);
    flake.selfAttrs = std::move(selfAttrs);
  }

  auto s_outputs = state.symbols.create("outputs");

  if (auto outputs = v_info.attrs()->get(s_outputs)) {
    expect_type(state, nFunction, *outputs->value, outputs->pos);

    if (outputs->value->isLambda()) {
      if (auto formals = outputs->value->lambda().fun->getFormals()) {
        for (auto& formal : formals->formals) {
          if (formal.name != state.s.self)
            flake.inputs.emplace(
                state.symbols[formal.name],
                FlakeInput{.ref = parse_flake_ref(state.fetch_settings,
                                                  std::string(state.symbols[formal.name]))});
        }
      }
    }

  } else
    throw Error("flake '%s' lacks attribute 'outputs'", resolved_ref);

  auto s_nix_config = state.symbols.create("nixConfig");

  if (auto nixConfig = v_info.attrs()->get(s_nix_config)) {
    expect_type(state, nAttrs, *nixConfig->value, nixConfig->pos);

    for (auto& setting : *nixConfig->value->attrs()) {
      force_trivial_value(state, *setting.value, setting.pos);
      if (setting.value->type() == nString)
        flake.config.settings.emplace(
            state.symbols[setting.name],
            std::string(state.forceStringNoCtx(*setting.value, setting.pos, "")));
      else if (setting.value->type() == nPath) {
        auto store_path = fetch_to_store(state.fetch_settings, *state.store, setting.value->path(),
                                         FetchMode::Copy);
        flake.config.settings.emplace(state.symbols[setting.name],
                                      state.store->printStorePath(store_path));
      } else if (setting.value->type() == nInt)
        flake.config.settings.emplace(state.symbols[setting.name],
                                      state.forceInt(*setting.value, setting.pos, "").value);
      else if (setting.value->type() == nBool)
        flake.config.settings.emplace(
            state.symbols[setting.name],
            explicit_t<bool>{state.forceBool(*setting.value, setting.pos, "")});
      else if (setting.value->type() == nList) {
        std::vector<std::string> ss;
        for (auto elem : setting.value->list_view()) {
          if (elem->type() != nString)
            state
                .error<TypeError>("list element in flake configuration setting '%s' is %s while a "
                                  "string is expected",
                                  state.symbols[setting.name], show_type(*elem))
                .debugThrow();
          ss.emplace_back(state.forceStringNoCtx(*elem, setting.pos, ""));
        }
        flake.config.settings.emplace(state.symbols[setting.name], ss);
      } else
        state
            .error<TypeError>("flake configuration setting '%s' is %s", state.symbols[setting.name],
                              show_type(*setting.value))
            .debugThrow();
    }
  }

  for (auto& attr : *v_info.attrs()) {
    if (attr.name != state.s.description && attr.name != s_inputs && attr.name != s_outputs &&
        attr.name != s_nix_config)
      throw Error("flake '%s' has an unsupported attribute '%s', at %s", resolved_ref,
                  state.symbols[attr.name], state.positions[attr.pos]);
  }

  return flake;
}

static flake_ref_t apply_self_attrs(const flake_ref_t& ref, const flake_t& flake) {
  auto new_ref(ref);

  string_set_t allowed_attrs{"submodules", "lfs"};

  for (auto& attr : flake.selfAttrs) {
    if (!allowed_attrs.contains(attr.first))
      throw Error("flake 'self' attribute '%s' is not supported", attr.first);

    // Check if the input scheme actually supports this attribute.
    // For example, github: inputs don't support submodules - only git: inputs do.
    if (new_ref.input.scheme) {
      auto& scheme_allowed = new_ref.input.scheme->allowed_attrs();
      if (scheme_allowed.find(attr.first) == scheme_allowed.end()) {
        throw Error("input '%s' does not support the '%s' attribute. "
                    "Consider using 'git+https://' instead of '%s:' to enable this feature.",
                    new_ref.input.to_string(), attr.first, new_ref.input.getType());
      }
    }

    new_ref.input.attrs.insert_or_assign(attr.first, attr.second);
  }

  return new_ref;
}

static flake_t get_flake(eval_state_t& state, const flake_ref_t& original_ref,
                         fetchers::UseRegistries use_registries,
                         const InputAttrPath& lock_root_attr_path, bool require_lockable) {
  // Fetch a lazy tree first.
  auto cached_input = state.inputCache->get_accessor(state.fetch_settings, *state.store,
                                                     original_ref.input, use_registries);

  auto subdir =
      fetchers::maybe_get_str_attr(cached_input.extra_attrs, "dir").value_or(original_ref.subdir);
  auto resolved_ref = flake_ref_t(std::move(cached_input.resolved_input), subdir);
  auto locked_ref = flake_ref_t(std::move(cached_input.lockedInput), subdir);

  // Parse/eval flake.nix to get at the input.self attributes.
  auto flake = read_flake(state, original_ref, resolved_ref, locked_ref, {cached_input.accessor},
                          lock_root_attr_path);

  // Re-fetch the tree if necessary.
  auto new_locked_ref = apply_self_attrs(locked_ref, flake);

  if (locked_ref != new_locked_ref) {
    debug("refetching input '%s' due to self attribute", new_locked_ref);
    // FIXME: need to remove attrs that are invalidated by the changed input attrs, such as
    // 'narHash'.
    new_locked_ref.input.attrs.erase("narHash");
    auto cached_input2 = state.inputCache->get_accessor(
        state.fetch_settings, *state.store, new_locked_ref.input, fetchers::UseRegistries::No);
    cached_input.accessor = cached_input2.accessor;
    locked_ref = flake_ref_t(std::move(cached_input2.lockedInput), new_locked_ref.subdir);
  }

  // Re-parse flake.nix from the store.
  return read_flake(state, original_ref, resolved_ref, locked_ref,
                    state.store_path(state.mountInput(locked_ref.input, original_ref.input,
                                                      cached_input.accessor, require_lockable)),
                    lock_root_attr_path);
}

flake_t get_flake(eval_state_t& state, const flake_ref_t& original_ref,
                  fetchers::UseRegistries use_registries, bool require_lockable) {
  return get_flake(state, original_ref, use_registries, {}, require_lockable);
}

static lock_file_t read_lock_file(const fetchers::settings_t& fetch_settings,
                                  const source_path_t& lock_file_path) {
  return lock_file_path.path_exists()
             ? lock_file_t(fetch_settings, lock_file_path.read_file(), fmt("%s", lock_file_path))
             : lock_file_t();
}

/* Compute an in-memory lock file for the specified top-level flake,
   and optionally write it to file, if the flake is writable. */
LockedFlake lock_flake(const settings_t& settings, eval_state_t& state, const flake_ref_t& top_ref,
                       const LockFlags& lock_flags) {
  auto use_registries = lock_flags.use_registries.value_or(settings.use_registries);
  auto use_registries_top =
      use_registries ? fetchers::UseRegistries::All : fetchers::UseRegistries::No;
  auto use_registries_inputs =
      use_registries ? fetchers::UseRegistries::Limited : fetchers::UseRegistries::No;

  auto flake = get_flake(state, top_ref, use_registries_top, {}, false);

  if (lock_flags.applyNixConfig) {
    flake.config.apply(settings);
    state.store->setOptions();
  }

  try {
    if (!state.fetch_settings.allowDirty && lock_flags.referenceLockFilePath) {
      throw Error(
          "reference lock file was provided, but the `allow-dirty` setting is set to false");
    }

    auto old_lock_file = read_lock_file(
        state.fetch_settings, lock_flags.referenceLockFilePath.value_or(flake.lock_file_path()));

    debug("old lock file: %s", old_lock_file);

    struct override_target_t {
      FlakeInput input;
      source_path_t source_path;
      std::optional<InputAttrPath> parent_input_attr_path; // FIXME: rename to inputAttrPathPrefix?
    };

    std::map<InputAttrPath, override_target_t> overrides;
    std::set<InputAttrPath> explicitCliOverrides;
    std::set<InputAttrPath> overridesUsed, updatesUsed;
    std::map<ref<Node>, source_path_t> nodePaths;

    for (auto& i : lock_flags.inputOverrides) {
      overrides.emplace(i.first, override_target_t{
                                     .input = FlakeInput{.ref = i.second},
                                     /* Note: any relative overrides
                                        (e.g. `--override-input B/C "path:./foo/bar"`)
                                        are interpreted relative to the top-level
                                        flake. */
                                     .source_path = flake.path,
                                 });
      explicitCliOverrides.insert(i.first);
    }

    lock_file_t new_lock_file;

    std::vector<flake_ref_t> parents;

    std::function<void(const FlakeInputs& flakeInputs, ref<Node> node,
                       const InputAttrPath& inputAttrPathPrefix,
                       std::shared_ptr<const Node> oldNode, const InputAttrPath& followsPrefix,
                       const source_path_t& source_path, bool trustLock)>
        computeLocks;

    computeLocks = [&](
                       /* The inputs of this node, either from flake.nix or
                          flake.lock. */
                       const FlakeInputs& flakeInputs,
                       /* The node whose locks are to be updated.*/
                       ref<Node> node,
                       /* The path to this node in the lock file graph. */
                       const InputAttrPath& inputAttrPathPrefix,
                       /* The old node, if any, from which locks can be
                          copied. */
                       std::shared_ptr<const Node> oldNode,
                       /* The prefix relative to which 'follows' should be
                          interpreted. When a node is initially locked, it's
                          relative to the node's flake; when it's already locked,
                          it's relative to the root of the lock file. */
                       const InputAttrPath& followsPrefix,
                       /* The source path of this node's flake. */
                       const source_path_t& source_path, bool trustLock) {
      debug("computing lock file node '%s'", print_input_attr_path(inputAttrPathPrefix));

      /* Get the overrides (i.e. attributes of the form
         'inputs.nixops.inputs.nixpkgs.url = ...'). */
      auto addOverrides = [&](this const auto& addOverrides, const FlakeInput& input,
                              const InputAttrPath& prefix) -> void {
        for (auto& [idOverride, inputOverride] : input.overrides) {
          auto inputAttrPath(prefix);
          inputAttrPath.push_back(idOverride);
          if (inputOverride.ref || inputOverride.follows)
            overrides.emplace(inputAttrPath,
                              override_target_t{.input = inputOverride,
                                                .source_path = source_path,
                                                .parent_input_attr_path = inputAttrPathPrefix});
          addOverrides(inputOverride, inputAttrPath);
        }
      };

      for (auto& [id, input] : flakeInputs) {
        auto inputAttrPath(inputAttrPathPrefix);
        inputAttrPath.push_back(id);
        addOverrides(input, inputAttrPath);
      }

      /* Check whether this input has overrides for a
         non-existent input. */
      for (auto [inputAttrPath, inputOverride] : overrides) {
        auto inputAttrPath2(inputAttrPath);
        auto follow = inputAttrPath2.back();
        inputAttrPath2.pop_back();
        if (inputAttrPath2 == inputAttrPathPrefix && !flakeInputs.count(follow))
          warn("input '%s' has an override for a non-existent input '%s'",
               print_input_attr_path(inputAttrPathPrefix), follow);
      }

      /* Go over the flake inputs, resolve/fetch them if
         necessary (i.e. if they're new or the flakeref changed
         from what's in the lock file). */
      for (auto& [id, input2] : flakeInputs) {
        auto inputAttrPath(inputAttrPathPrefix);
        inputAttrPath.push_back(id);
        auto inputAttrPathS = print_input_attr_path(inputAttrPath);
        debug("computing input '%s'", inputAttrPathS);

        try {
          /* Do we have an override for this input from one of the
             ancestors? */
          auto i = overrides.find(inputAttrPath);
          bool hasOverride = i != overrides.end();
          bool hasCliOverride = explicitCliOverrides.contains(inputAttrPath);
          if (hasOverride)
            overridesUsed.insert(inputAttrPath);
          auto input = hasOverride ? i->second.input : input2;

          /* Resolve relative 'path:' inputs relative to
             the source path of the overrider. */
          auto overriddenSourcePath = hasOverride ? i->second.source_path : source_path;

          /* Respect the "flakeness" of the input even if we
             override it. */
          if (hasOverride)
            input.is_flake = input2.is_flake;

          /* Resolve 'follows' later (since it may refer to an input
             path we haven't processed yet. */
          if (input.follows) {
            InputAttrPath target;

            target.insert(target.end(), input.follows->begin(), input.follows->end());

            debug("input '%s' follows '%s'", inputAttrPathS, print_input_attr_path(target));
            node->inputs.insert_or_assign(id, target);
            continue;
          }

          if (!input.ref)
            input.ref = flake_ref_t::fromAttrs(state.fetch_settings,
                                               {{"type", "indirect"}, {"id", std::string(id)}});

          auto overriddenParentPath =
              input.ref->input.isRelative()
                  ? std::optional<InputAttrPath>(hasOverride ? i->second.parent_input_attr_path
                                                             : inputAttrPathPrefix)
                  : std::nullopt;

          auto resolveRelativePath = [&]() -> std::optional<source_path_t> {
            if (auto relativePath = input.ref->input.isRelative()) {
              return source_path_t{
                  overriddenSourcePath.accessor,
                  canon_path_t(*relativePath, overriddenSourcePath.path.parent().value())};
            } else
              return std::nullopt;
          };

          /* Get the input flake, resolve 'path:./...'
             flakerefs relative to the parent flake. */
          auto getInputFlake = [&](const flake_ref_t& ref,
                                   const fetchers::UseRegistries use_registries) {
            if (auto resolvedPath = resolveRelativePath()) {
              return read_flake(state, ref, ref, ref, *resolvedPath, inputAttrPath);
            } else {
              return get_flake(state, ref, use_registries_inputs, inputAttrPath, true);
            }
          };

          /* Do we have an entry in the existing lock file?
             And the input is not in updateInputs? */
          std::shared_ptr<LockedNode> oldLock;

          updatesUsed.insert(inputAttrPath);

          if (oldNode && !lock_flags.inputUpdates.count(inputAttrPath))
            if (auto oldLock2 = get(oldNode->inputs, id))
              if (auto oldLock3 = std::get_if<0>(&*oldLock2))
                oldLock = *oldLock3;

          if (oldLock && oldLock->original_ref.canonicalize() == input.ref->canonicalize() &&
              oldLock->parent_input_attr_path == overriddenParentPath && !hasCliOverride) {
            debug("keeping existing input '%s'", inputAttrPathS);

            /* Copy the input from the old lock since its flakeref
               didn't change and there is no override from a
               higher level flake. */
            auto childNode =
                make_ref<LockedNode>(oldLock->locked_ref, oldLock->original_ref, oldLock->is_flake,
                                     oldLock->buildTime, oldLock->parent_input_attr_path);

            node->inputs.insert_or_assign(id, childNode);

            /* If we have this input in updateInputs, then we
               must fetch the flake to update it. */
            auto lb = lock_flags.inputUpdates.lower_bound(inputAttrPath);

            auto mustRefetch = lb != lock_flags.inputUpdates.end() &&
                               lb->size() > inputAttrPath.size() &&
                               std::equal(inputAttrPath.begin(), inputAttrPath.end(), lb->begin());

            FlakeInputs fakeInputs;

            if (!mustRefetch) {
              /* No need to fetch this flake, we can be
                 lazy. However there may be new overrides on the
                 inputs of this flake, so we need to check
                 those. */
              for (auto& i : oldLock->inputs) {
                if (auto locked_node = std::get_if<0>(&i.second)) {
                  fakeInputs.emplace(i.first, FlakeInput{
                                                  .ref = (*locked_node)->original_ref,
                                                  .is_flake = (*locked_node)->is_flake,
                                              });
                } else if (auto follows = std::get_if<1>(&i.second)) {
                  auto overridePath(inputAttrPath);
                  overridePath.push_back(i.first);
                  auto o = overrides.find(overridePath);

                  // Check if this follows declaration is still valid.
                  // A follows can come from two sources:
                  // 1. The nested flake's own flake.nix (internal follows)
                  // 2. An ancestor's flake.nix as an override
                  //
                  // When trustLock=false, we've just fetched this flake, so any follows
                  // should be in overrides (from the flake we just parsed).
                  //
                  // When trustLock=true, we haven't fetched this flake. Internal follows
                  // can be trusted, but ancestor overrides may have been removed.
                  // We detect ancestor overrides by checking if the follows target
                  // points outside the current followsPrefix scope.
                  auto absoluteFollows(followsPrefix);
                  absoluteFollows.insert(absoluteFollows.end(), follows->begin(), follows->end());

                  bool followsIsAncestorOverride = false;
                  if (trustLock && o == overrides.end()) {
                    // The follows is not in overrides. Check if it points outside
                    // the current scope (followsPrefix), which would indicate it
                    // must have been an ancestor override.
                    // A follows that points to followsPrefix or below could be from
                    // the nested flake's own flake.nix.
                    if (absoluteFollows.size() < followsPrefix.size() ||
                        !std::equal(followsPrefix.begin(), followsPrefix.end(),
                                    absoluteFollows.begin())) {
                      // The follows target is outside or diverges from followsPrefix,
                      // so it must have been an ancestor override that was removed.
                      followsIsAncestorOverride = true;
                    }
                  }

                  if (!trustLock || followsIsAncestorOverride) {
                    // We must confirm follows in the lock file are still valid.
                    // If the override disappeared, we have to refetch the flake,
                    // since the follows may have changed.
                    if (o == overrides.end()) {
                      mustRefetch = true;
                      // There's no point populating the rest of the fake inputs,
                      // since we'll refetch the flake anyways.
                      break;
                    }
                  }

                  fakeInputs.emplace(i.first, FlakeInput{
                                                  .follows = absoluteFollows,
                                              });
                }
              }
            }

            if (mustRefetch) {
              auto inputFlake = getInputFlake(oldLock->locked_ref, use_registries_inputs);
              nodePaths.emplace(childNode, inputFlake.path.parent());
              computeLocks(inputFlake.inputs, childNode, inputAttrPath, oldLock, followsPrefix,
                           inputFlake.path, false);
            } else {
              /* Even when we don't refetch the flake, we need to get the correct
                 source path for this input. Otherwise, any relative path inputs
                 in nested subflakes would be resolved relative to the wrong
                 directory (the parent's directory instead of this input's directory).
                 See: https://github.com/NixOS/nix/issues/14762 */
              auto getInputSourcePath = [&]() -> source_path_t {
                if (auto relativePath = oldLock->locked_ref.input.isRelative()) {
                  /* For relative path inputs, resolve relative to the parent's source path. */
                  return source_path_t{
                      source_path.accessor,
                      canon_path_t(*relativePath, source_path.path.parent().value())};
                } else {
                  /* For non-relative inputs, fetch the accessor and compute the source path. */
                  auto cached_input = state.inputCache->get_accessor(
                      state.fetch_settings, *state.store, oldLock->locked_ref.input,
                      fetchers::UseRegistries::No);
                  return state.store_path(state.mountInput(oldLock->locked_ref.input,
                                                           oldLock->original_ref.input,
                                                           cached_input.accessor, true)) /
                         canon_path_t(oldLock->locked_ref.subdir);
                }
              };
              auto childSourcePath = getInputSourcePath();
              nodePaths.emplace(childNode, childSourcePath);
              computeLocks(fakeInputs, childNode, inputAttrPath, oldLock, followsPrefix,
                           childSourcePath / "flake.nix", true);
            }

          } else {
            /* We need to create a new lock file entry. So fetch
               this input. */
            debug("creating new input '%s'", inputAttrPathS);

            if (!lock_flags.allowUnlocked && !input.ref->input.isLocked(state.fetch_settings) &&
                !input.ref->input.isRelative())
              throw Error("cannot update unlocked flake input '%s' in pure mode", inputAttrPathS);

            /* Note: in case of an --override-input, we use
                the *original* ref (input2.ref) for the
                "original" field, rather than the
                override. This ensures that the override isn't
                nuked the next time we update the lock
                file. That is, overrides are sticky unless you
                use --no-write-lock-file. */
            auto inputIsOverride = explicitCliOverrides.contains(inputAttrPath);
            auto ref = (input2.ref && inputIsOverride) ? *input2.ref : *input.ref;

            /* Warn against the use of indirect flakerefs
               (but only at top-level since we don't want
               to annoy users about flakes that are not
               under their control). */
            auto warnRegistry = [&](const flake_ref_t& resolved_ref) {
              if (inputAttrPath.size() == 1 && !input.ref->input.isDirect()) {
                std::ostringstream s;
                print_literal_string(s, resolved_ref.to_string());
                warn("flake_t input '%1%' uses the flake registry. "
                     "Using the registry in flake inputs is deprecated in Determinate Nix. "
                     "To make your flake future-proof, add the following to '%2%':\n"
                     "\n"
                     "  inputs.%1%.url = %3%;\n"
                     "\n"
                     "For more information, see: "
                     "https://github.com/DeterminateSystems/nix-src/issues/37",
                     inputAttrPathS, flake.path, s.str());
              }
            };

            if (input.is_flake) {
              auto inputFlake =
                  getInputFlake(*input.ref, inputIsOverride ? fetchers::UseRegistries::All
                                                            : use_registries_inputs);

              auto childNode = make_ref<LockedNode>(inputFlake.locked_ref, ref, true,
                                                    input.buildTime, overriddenParentPath);

              node->inputs.insert_or_assign(id, childNode);

              /* Guard against circular flake imports.
                 Compare locked refs, not input refs, because two different
                 refs (e.g. 'github:owner/repo' vs 'github:owner/repo?ref=main')
                 can resolve to the same flake. */
              for (auto& parent : parents)
                if (parent == inputFlake.locked_ref)
                  throw Error("found circular import of flake '%s'", parent);
              parents.push_back(inputFlake.locked_ref);
              finally_t cleanup([&]() { parents.pop_back(); });

              /* Recursively process the inputs of this
                 flake, using its own lock file. */
              nodePaths.emplace(childNode, inputFlake.path.parent());
              computeLocks(
                  inputFlake.inputs, childNode, inputAttrPath,
                  read_lock_file(state.fetch_settings, inputFlake.lock_file_path()).root.get_ptr(),
                  inputAttrPath, inputFlake.path, false);

              warnRegistry(inputFlake.resolved_ref);
            }

            else {
              auto [path, locked_ref] = [&]() -> std::tuple<source_path_t, flake_ref_t> {
                // Handle non-flake 'path:./...' inputs.
                if (auto resolvedPath = resolveRelativePath()) {
                  return {*resolvedPath, *input.ref};
                } else {
                  auto cached_input = state.inputCache->get_accessor(
                      state.fetch_settings, *state.store, input.ref->input, use_registries_inputs);

                  auto resolved_ref =
                      flake_ref_t(std::move(cached_input.resolved_input), input.ref->subdir);
                  auto locked_ref =
                      flake_ref_t(std::move(cached_input.lockedInput), input.ref->subdir);

                  warnRegistry(resolved_ref);

                  return {state.store_path(state.mountInput(locked_ref.input, input.ref->input,
                                                            cached_input.accessor, true, true)),
                          locked_ref};
                }
              }();

              auto childNode = make_ref<LockedNode>(locked_ref, ref, false, input.buildTime,
                                                    overriddenParentPath);

              nodePaths.emplace(childNode, path);

              node->inputs.insert_or_assign(id, childNode);
            }
          }

        } catch (Error& e) {
          e.add_trace({}, "while updating the flake input '%s'", inputAttrPathS);
          throw;
        }
      }
    };

    nodePaths.emplace(new_lock_file.root, flake.path.parent());

    computeLocks(flake.inputs, new_lock_file.root, {},
                 lock_flags.recreateLockFile ? nullptr : old_lock_file.root.get_ptr(), {},
                 flake.path, false);

    for (auto& i : lock_flags.inputOverrides)
      if (!overridesUsed.count(i.first))
        warn("the flag '--override-input %s %s' does not match any input",
             print_input_attr_path(i.first), i.second);

    for (auto& i : lock_flags.inputUpdates)
      if (!updatesUsed.count(i))
        warn("'%s' does not match any input of this flake", print_input_attr_path(i));

    /* Check 'follows' inputs. */
    new_lock_file.check();

    debug("new lock file: %s", new_lock_file);

    auto source_path = top_ref.input.get_source_path();

    /* Check whether we need to / can write the new lock file. */
    if (new_lock_file != old_lock_file || lock_flags.output_lock_file_path) {
      auto diff = lock_file_t::diff(old_lock_file, new_lock_file);

      if (lock_flags.writeLockFile) {
        if (source_path || lock_flags.output_lock_file_path) {
          if (auto unlockedInput = new_lock_file.isUnlocked(state.fetch_settings)) {
            if (lock_flags.failOnUnlocked)
              throw Error(
                  "Not writing lock file of flake '%s' because it has an unlocked input ('%s'). "
                  "Use '--allow-dirty-locks' to allow this anyway.",
                  top_ref, *unlockedInput);
            if (state.fetch_settings.warn_dirty)
              warn("not writing lock file of flake '%s' because it has an unlocked input ('%s')",
                   top_ref, *unlockedInput);
          } else {
            if (!lock_flags.updateLockFile)
              throw Error("flake '%s' requires lock file changes but they're not allowed due to "
                          "'--no-update-lock-file'",
                          top_ref);

            auto new_lock_file_s = fmt("%s\n", new_lock_file);

            if (lock_flags.output_lock_file_path) {
              if (lock_flags.commitLockFile)
                throw Error("'--commit-lock-file' and '--output-lock-file' are incompatible");
              write_file(*lock_flags.output_lock_file_path, new_lock_file_s);
            } else {
              auto rel_path = (top_ref.subdir == "" ? "" : top_ref.subdir + "/") + "flake.lock";
              auto output_lock_file_path = *source_path / rel_path;

              bool lock_file_exists = path_exists(output_lock_file_path);

              auto s = chomp(diff);
              if (lock_file_exists) {
                if (s.empty())
                  warn("updating lock file %s", output_lock_file_path);
                else
                  warn("updating lock file %s:\n%s", output_lock_file_path, s);
              } else
                warn("creating lock file %s: \n%s", output_lock_file_path, s);

              std::optional<std::string> commitMessage = std::nullopt;

              if (lock_flags.commitLockFile) {
                std::string cm;

                cm = settings.commitLockFileSummary.get();

                if (cm == "") {
                  cm = fmt("%s: %s", rel_path, lock_file_exists ? "Update" : "Add");
                }

                cm += "\n\nFlake lock file updates:\n\n";
                cm += filter_ansi_escapes(diff, true);
                commitMessage = cm;
              }

              top_ref.input.putFile(
                  canon_path_t((top_ref.subdir == "" ? "" : top_ref.subdir + "/") + "flake.lock"),
                  new_lock_file_s, commitMessage);

              flake.lock_file_path().invalidate_cache();
            }

            /* Rewriting the lockfile changed the top-level
               repo, so we should re-read it. FIXME: we could
               also just clear the 'rev' field... */
            auto prev_locked_ref = flake.locked_ref;
            flake = get_flake(state, top_ref, use_registries_top, lock_flags.require_lockable);

            if (lock_flags.commitLockFile && flake.locked_ref.input.getRev() &&
                prev_locked_ref.input.getRev() != flake.locked_ref.input.getRev())
              warn("committed new revision '%s'", flake.locked_ref.input.getRev()->git_rev());
          }
        } else
          throw Error("cannot write modified lock file of flake '%s' (use '--no-write-lock-file' "
                      "to ignore)",
                      top_ref);
      } else {
        warn("not writing modified lock file of flake '%s':\n%s", top_ref, chomp(diff));
        flake.force_dirty = true;
      }
    }

    return LockedFlake{.flake = std::move(flake),
                       .lock_file = std::move(new_lock_file),
                       .nodePaths = std::move(nodePaths)};

  } catch (Error& e) {
    e.add_trace({}, "while updating the lock file of flake '%s'", flake.locked_ref.to_string());
    throw;
  }
}

static ref<source_accessor_t> make_internal_fs() {
  auto internal_fs = make_ref<memory_source_accessor_t>(memory_source_accessor_t{});
  internal_fs->set_path_display("«flakes-internal»", "");
  internal_fs->add_file(canon_path_t("call-flake.nix"),
#include "call-flake.nix.gen.h" // IWYU pragma: keep
  );
  return internal_fs;
}

static auto internal_fs = make_internal_fs();

static value_t* require_internal_file(eval_state_t& state, canon_path_t path) {
  source_path_t p{internal_fs, path};
  auto v = state.allocValue();
  state.evalFile(p, *v); // has caching
  return v;
}

void call_flake(eval_state_t& state, const LockedFlake& locked_flake, value_t& v_res) {
  auto [lockFileStr, keyMap] = locked_flake.lock_file.to_string();

  auto overrides = state.buildBindings(locked_flake.nodePaths.size());

  for (auto& [node, source_path] : locked_flake.nodePaths) {
    auto override = state.buildBindings(2);

    auto& vSourceInfo = override.alloc(state.symbols.create("sourceInfo"));

    auto locked_node = node.dynamic_pointer_cast<const LockedNode>();

    auto [store_path, subdir] = state.store->toStorePath(source_path.path.abs());

    emit_tree_attrs(state, store_path,
                    locked_node ? locked_node->locked_ref.input
                                : locked_flake.flake.locked_ref.input,
                    vSourceInfo, false, !locked_node && locked_flake.flake.force_dirty);

    auto key = keyMap.find(node);
    assert(key != keyMap.end());

    override.alloc(state.symbols.create("dir")).mk_string(canon_path_t(subdir).rel(), state.mem);

    overrides.alloc(state.symbols.create(key->second)).mkAttrs(override);
  }

  auto& v_overrides = state.allocValue()->mkAttrs(overrides);

  value_t* v_call_flake = require_internal_file(state, canon_path_t("call-flake.nix"));

  auto v_locks = state.allocValue();
  v_locks->mk_string(lockFileStr, state.mem);

  value_t* args[] = {v_locks, &v_overrides};
  state.callFunction(*v_call_flake, args, v_res, no_pos);
}

std::optional<Fingerprint>
LockedFlake::get_fingerprint(store_t& store, const fetchers::settings_t& fetch_settings) const {
  if (lock_file.isUnlocked(fetch_settings))
    return std::nullopt;

  auto fingerprint = flake.locked_ref.input.get_fingerprint(store);
  if (!fingerprint)
    return std::nullopt;

  *fingerprint += fmt(";%s;%s", flake.locked_ref.subdir, lock_file);

  /* Include rev_count and last_modified because they're not
     necessarily implied by the content fingerprint (e.g. for
     tarball flakes) but can influence the evaluation result. */
  if (auto rev_count = flake.locked_ref.input.get_rev_count())
    *fingerprint += fmt(";revCount=%d", *rev_count);
  if (auto last_modified = flake.locked_ref.input.get_last_modified())
    *fingerprint += fmt(";lastModified=%d", *last_modified);

  // FIXME: as an optimization, if the flake contains a lock file
  // and we haven't changed it, then it's sufficient to use
  // flake.sourceInfo.storePath for the fingerprint.
  return hash_string(hash_algorithm_t::SHA256, *fingerprint);
}

flake_t::~flake_t() {}

ref<eval_cache::EvalCache> open_eval_cache(eval_state_t& state,
                                           ref<const LockedFlake> locked_flake) {
  auto fingerprint = state.settings.useEvalCache && state.settings.pureEval
                         ? locked_flake->get_fingerprint(*state.store, state.fetch_settings)
                         : std::nullopt;
  auto root_loader = [&state, locked_flake]() {
    /* For testing whether the evaluation cache is
       complete. */
    if (get_env("NIX_ALLOW_EVAL").value_or("1") == "0")
      throw Error("not everything is cached, but evaluation is not allowed");

    auto v_flake = state.allocValue();
    call_flake(state, *locked_flake, *v_flake);

    state.forceAttrs(*v_flake, no_pos, "while parsing cached flake data");

    auto a_outputs = v_flake->attrs()->get(state.symbols.create("outputs"));
    assert(a_outputs);

    return a_outputs->value;
  };

  if (fingerprint) {
    auto search = state.evalCaches.find(fingerprint.value());
    if (search == state.evalCaches.end()) {
      search = state.evalCaches
                   .emplace(fingerprint.value(),
                            make_ref<eval_cache::EvalCache>(fingerprint, state, root_loader))
                   .first;
    }
    return search->second;
  } else {
    return make_ref<eval_cache::EvalCache>(std::nullopt, state, root_loader);
  }
}

} // namespace flake

} // namespace nix
