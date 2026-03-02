#include "nix/cmd/installable-attr-path.h"

#include <queue>
#include <regex>

#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/cmd/common-eval-args.h"
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

InstallableAttrPath::InstallableAttrPath(ref<eval_state_t> state, SourceExprCommand& cmd,
                                         value_t* v, const std::string& attr_path,
                                         ExtendedOutputsSpec extendedOutputsSpec)
    : InstallableValue(state),
      cmd(cmd),
      v(alloc_root_value(v)),
      attr_path(attr_path),
      extendedOutputsSpec(std::move(extendedOutputsSpec)) {}

std::pair<value_t*, pos_idx_t> InstallableAttrPath::toValue(eval_state_t& state) {
  auto [v_res, pos] = find_along_attr_path(state, attr_path, *cmd.getAutoArgs(state), **v);
  state.forceValue(*v_res, pos);
  return {v_res, pos};
}

DerivedPathsWithInfo InstallableAttrPath::to_derived_paths() {
  auto [v, pos] = toValue(*state);

  if (std::optional derivedPathWithInfo = trySinglePathToDerivedPaths(
          *v, pos, fmt("while evaluating the attribute '%s'", attr_path))) {
    return {*derivedPathWithInfo};
  }

  bindings_t& auto_args = *cmd.getAutoArgs(*state);

  PackageInfos package_infos;
  get_derivations(*state, *v, "", auto_args, package_infos, false);

  // Backward compatibility hack: group results by drvPath. This
  // helps keep .all output together.
  std::map<store_path_t, OutputsSpec> byDrvPath;

  for (auto& package_info : package_infos) {
    auto drv_path = package_info.queryDrvPath();
    if (!drv_path) {
      throw Error("'%s' is not a derivation", what());
    }

    auto newOutputs =
        std::visit(overloaded{
                       [&](const ExtendedOutputsSpec::Default& d) -> OutputsSpec {
                         string_set_t outputsToInstall;
                         for (auto& output : package_info.queryOutputs(false, true)) {
                           outputsToInstall.insert(output.first);
                         }
                         if (outputsToInstall.empty()) {
                           outputsToInstall.insert("out");
                         }
                         return OutputsSpec::Names{std::move(outputsToInstall)};
                       },
                       [&](const ExtendedOutputsSpec::explicit_t& e) -> OutputsSpec { return e; },
                   },
                   extendedOutputsSpec.raw);

    auto [iter, didInsert] = byDrvPath.emplace(*drv_path, newOutputs);

    if (!didInsert) {
      iter->second = iter->second.union_(newOutputs);
    }
  }

  DerivedPathsWithInfo res;
  for (auto& [drv_path, outputs] : byDrvPath) {
    state->waitForPath(drv_path);
    res.push_back({
        .path =
            derived_path_t::Built{
                .drv_path = makeConstantStorePathRef(drv_path),
                .outputs = outputs,
            },
        .info = make_ref<ExtraPathInfoValue>(ExtraPathInfoValue::value_t{
            .extendedOutputsSpec = outputs,
            /* FIXME: reconsider backwards compatibility above
               so we can fill in this info. */
        }),
    });
  }

  return res;
}

InstallableAttrPath InstallableAttrPath::parse(ref<eval_state_t> state, SourceExprCommand& cmd,
                                               value_t* v, std::string_view prefix,
                                               ExtendedOutputsSpec extendedOutputsSpec) {
  return {
      state, cmd, v, prefix == "." ? "" : std::string{prefix}, std::move(extendedOutputsSpec),
  };
}

} // namespace nix
