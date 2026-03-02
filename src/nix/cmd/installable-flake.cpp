#include "nix/cmd/installable-flake.h"

#include <queue>
#include <regex>

#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/cmd/common-eval-args.h"
#include "nix/cmd/installable-derived-path.h"
#include "nix/expr/attr-path.h"
#include "nix/expr/eval-cache.h"
#include "nix/expr/eval-inline.h"
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
#include "nix/util/url.h"
#include "nix/util/util.h"

namespace nix {

std::vector<std::string> InstallableFlake::getActualAttrPaths() {
  std::vector<std::string> res;
  if (attrPaths.size() == 1 && attrPaths.front().starts_with(".")) {
    attrPaths.front().erase(0, 1);
    res.push_back(attrPaths.front());
    return res;
  }

  for (auto& prefix : prefixes) {
    res.push_back(prefix + *attrPaths.begin());
  }

  for (auto& s : attrPaths) {
    res.push_back(s);
  }

  return res;
}

static std::string show_attr_paths(const std::vector<std::string>& paths) {
  std::string s;
  for (const auto& [n, i] : enumerate(paths)) {
    if (n > 0) {
      s += n + 1 == paths.size() ? " or " : ", ";
    }
    s += '\'';
    s += i;
    s += '\'';
  }
  return s;
}

InstallableFlake::InstallableFlake(SourceExprCommand* cmd, ref<eval_state_t> state,
                                   flake_ref_t&& flake_ref, std::string_view fragment,
                                   ExtendedOutputsSpec extendedOutputsSpec, strings_t attrPaths,
                                   strings_t prefixes, const flake::LockFlags& lock_flags)
    : InstallableValue(state),
      flake_ref(flake_ref),
      attrPaths(fragment == "" ? attrPaths : strings_t{(std::string)fragment}),
      prefixes(fragment == "" ? strings_t{} : prefixes),
      extendedOutputsSpec(std::move(extendedOutputsSpec)),
      lock_flags(lock_flags) {
  if (cmd && cmd->getAutoArgs(*state)->size()) {
    throw UsageError("'--arg' and '--argstr' are incompatible with flakes");
  }
}

DerivedPathsWithInfo InstallableFlake::to_derived_paths() {
  activity_t act(*logger, lvl_talkative, act_unknown, fmt("evaluating derivation '%s'", what()));

  auto attr = getCursor(*state);

  auto attr_path = attr->getAttrPathStr();

  if (!attr->is_derivation()) {
    // FIXME: use eval cache?
    auto v = attr->forceValue();

    if (std::optional derivedPathWithInfo = trySinglePathToDerivedPaths(
            v, no_pos, fmt("while evaluating the flake output attribute '%s'", attr_path))) {
      return {*derivedPathWithInfo};
    } else {
      throw Error(
          "expected flake output attribute '%s' to be a derivation or path but found %s: %s",
          attr_path, show_type(v), ValuePrinter(*this->state, v, errorPrintOptions));
    }
  }

  auto drv_path = attr->forceDerivation();
  state->waitForPath(drv_path);

  std::optional<NixInt::Inner> priority;

  if (attr->maybeGetAttr(state->s.outputSpecified)) {
  } else if (auto aMeta = attr->maybeGetAttr(state->s.meta)) {
    if (auto aPriority = aMeta->maybeGetAttr("priority")) {
      priority = aPriority->getInt().value;
    }
  }

  return {{
      .path =
          derived_path_t::Built{
              .drv_path = makeConstantStorePathRef(std::move(drv_path)),
              .outputs = std::visit(
                  overloaded{
                      [&](const ExtendedOutputsSpec::Default& d) -> OutputsSpec {
                        string_set_t outputsToInstall;
                        if (auto aOutputSpecified = attr->maybeGetAttr(state->s.outputSpecified)) {
                          if (aOutputSpecified->getBool()) {
                            if (auto aOutputName = attr->maybeGetAttr("outputName")) {
                              outputsToInstall = {aOutputName->get_string()};
                            }
                          }
                        } else if (auto aMeta = attr->maybeGetAttr(state->s.meta)) {
                          if (auto aOutputsToInstall = aMeta->maybeGetAttr("outputsToInstall")) {
                            for (auto& s : aOutputsToInstall->getListOfStrings()) {
                              outputsToInstall.insert(s);
                            }
                          }
                        }

                        if (outputsToInstall.empty()) {
                          outputsToInstall.insert("out");
                        }

                        return OutputsSpec::Names{std::move(outputsToInstall)};
                      },
                      [&](const ExtendedOutputsSpec::explicit_t& e) -> OutputsSpec { return e; },
                  },
                  extendedOutputsSpec.raw),
          },
      .info = make_ref<ExtraPathInfoFlake>(
          ExtraPathInfoValue::value_t{
              .priority = priority,
              .attr_path = attr_path,
              .extendedOutputsSpec = extendedOutputsSpec,
          },
          ExtraPathInfoFlake::flake_t{
              .original_ref = flake_ref,
              .locked_ref = getLockedFlake()->flake.locked_ref,
          }),
  }};
}

std::pair<value_t*, pos_idx_t> InstallableFlake::toValue(eval_state_t& state) {
  return {&getCursor(state)->forceValue(), no_pos};
}

std::vector<ref<eval_cache::AttrCursor>> InstallableFlake::getCursors(eval_state_t& state) {
  auto eval_cache = open_eval_cache(state, getLockedFlake());

  auto root = eval_cache->get_root();

  std::vector<ref<eval_cache::AttrCursor>> res;

  suggestions_t suggestions;
  auto attrPaths = getActualAttrPaths();

  for (auto& attr_path : attrPaths) {
    debug("trying flake output attribute '%s'", attr_path);

    auto attr = root->find_along_attr_path(AttrPath::parse(state, attr_path));
    if (attr) {
      res.push_back(ref(*attr));
    } else {
      suggestions += attr.get_suggestions();
    }
  }

  if (res.size() == 0) {
    throw Error(suggestions, "flake '%s' does not provide attribute %s", flake_ref,
                show_attr_paths(attrPaths));
  }

  return res;
}

ref<flake::LockedFlake> InstallableFlake::getLockedFlake() const {
  if (!_lockedFlake) {
    flake::LockFlags lockFlagsApplyConfig = lock_flags;
    // FIXME why this side effect?
    lockFlagsApplyConfig.applyNixConfig = true;
    _lockedFlake = make_ref<flake::LockedFlake>(
        lock_flake(flake_settings, *state, flake_ref, lockFlagsApplyConfig));
  }
  // _lockedFlake is now non-null but still just a shared_ptr
  return ref<flake::LockedFlake>(_lockedFlake);
}

flake_ref_t InstallableFlake::nixpkgsFlakeRef() const {
  auto locked_flake = getLockedFlake();

  if (auto nixpkgsInput = locked_flake->lock_file.findInput({"nixpkgs"})) {
    if (auto locked_node = std::dynamic_pointer_cast<const flake::LockedNode>(nixpkgsInput)) {
      debug("using nixpkgs flake '%s'", locked_node->locked_ref);
      return std::move(locked_node->locked_ref);
    }
  }

  return defaultNixpkgsFlakeRef();
}

} // namespace nix
