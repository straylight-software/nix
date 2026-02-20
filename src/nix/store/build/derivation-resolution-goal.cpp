#include "nix/store/build/derivation-resolution-goal.h"

#include <nlohmann/json.hpp>

#include "nix/store/build/worker.h"
#include "nix/util/util.h"

namespace nix {

DerivationResolutionGoal::DerivationResolutionGoal(const store_path_t& drv_path, const derivation_t& drv,
                                                   Worker& worker, BuildMode build_mode)
    : Goal(worker, resolveDerivation()),
      drv_path(drv_path),
      drv{std::make_unique<derivation_t>(drv)},
      build_mode{build_mode} {
  name = fmt("resolving derivation '%s'", worker.store.printStorePath(drv_path));
  trace("created");
}

std::string DerivationResolutionGoal::key() {
  return "dc$" + std::string(drv_path.name()) + "$" + worker.store.printStorePath(drv_path);
}

/**
 * Used for `inputGoals` local variable below
 */
struct value_comparison {
  template <typename T>
  bool operator()(const ref<T>& lhs, const ref<T>& rhs) const {
    return *lhs < *rhs;
  }
};

Goal::Co DerivationResolutionGoal::resolveDerivation() {
  Goals waitees;

  std::map<ref<const SingleDerivedPath>, GoalPtr, value_comparison> inputGoals;

  {
    std::function<void(ref<const SingleDerivedPath>,
                       const DerivedPathMap<string_set_t>::ChildNode&)>
        addWaiteeDerivedPath;

    addWaiteeDerivedPath = [&](ref<const SingleDerivedPath> input_drv,
                               const DerivedPathMap<string_set_t>::ChildNode& input_node) {
      if (!input_node.value.empty()) {
        auto g = worker.makeGoal(
            derived_path_t::Built{
                .drv_path = input_drv,
                .outputs = input_node.value,
            },
            build_mode == bmRepair ? bmRepair : bmNormal);
        inputGoals.insert_or_assign(input_drv, g);
        waitees.insert(std::move(g));
      }
      for (const auto& [output_name, childNode] : input_node.childMap)
        addWaiteeDerivedPath(
            make_ref<SingleDerivedPath>(SingleDerivedPath::Built{input_drv, output_name}),
            childNode);
    };

    for (const auto& [inputDrvPath, input_node] : drv->input_drvs.map) {
      /* Ensure that pure, non-fixed-output derivations don't
         depend on impure derivations. */
      if (experimental_feature_settings.is_enabled(xp_t::impure_derivations) &&
          !drv->type().is_impure() && !drv->type().isFixed()) {
        auto input_drv = worker.eval_store.read_derivation(inputDrvPath);
        if (input_drv.type().is_impure())
          throw Error("pure derivation '%s' depends on impure derivation '%s'",
                      worker.store.printStorePath(drv_path),
                      worker.store.printStorePath(inputDrvPath));
      }

      addWaiteeDerivedPath(makeConstantStorePathRef(inputDrvPath), input_node);
    }
  }

  co_await await(std::move(waitees));

  trace("all inputs realised");

  if (nrFailed != 0) {
    auto msg = fmt("Cannot build '%s'.\n"
                   "Reason: " ANSI_RED "%d %s failed" ANSI_NORMAL ".",
                   magenta_t(worker.store.printStorePath(drv_path)), nrFailed,
                   nrFailed == 1 ? "dependency" : "dependencies");
    msg += show_known_outputs(worker.store, *drv);
    co_return amDone(ecFailed, {build_error_t(build_result_t::Failure::DependencyFailed, msg)});
  }

  /* Gather information necessary for computing the closure and/or
     running the build hook. */

  /* Determine the full set of input paths. */

  /* First, the input derivations. */
  {
    auto& fullDrv = *drv;

    auto drv_type = fullDrv.type();
    bool resolveDrv =
        std::visit(overloaded{[&](const DerivationType::InputAddressed& ia) {
                                /* must resolve if deferred. */
                                return ia.deferred;
                              },
                              [&](const DerivationType::ContentAddressed& ca) {
                                return !fullDrv.input_drvs.map.empty() &&
                                       (ca.fixed
                                            /* Can optionally resolve if fixed, which is good
                                               for avoiding unnecessary rebuilds. */
                                            ? experimental_feature_settings.is_enabled(
                                                  xp_t::ca_derivations)
                                            /* Must resolve if floating and there are any inputs
                                               drvs. */
                                            : true);
                              },
                              [&](const DerivationType::Impure&) { return true; }},
                   drv_type.raw)
        /* no inputs are outputs of dynamic derivations */
        || std::ranges::any_of(fullDrv.input_drvs.map.begin(), fullDrv.input_drvs.map.end(),
                               [](auto& pair) { return !pair.second.childMap.empty(); });

    if (resolveDrv && !fullDrv.input_drvs.map.empty()) {
      experimental_feature_settings.require(xp_t::ca_derivations);

      /* We are be able to resolve this derivation based on the
         now-known results of dependencies. If so, we become a
         stub goal aliasing that resolved derivation goal. */
      std::optional attempt = fullDrv.try_resolve(
          worker.store,
          [&](ref<const SingleDerivedPath> drv_path,
              const std::string& output_name) -> std::optional<store_path_t> {
            auto mEntry = get(inputGoals, drv_path);
            if (!mEntry)
              return std::nullopt;

            auto& buildResult = (*mEntry)->buildResult;
            return std::visit(
                overloaded{
                    [](const build_result_t::Failure&) -> std::optional<store_path_t> {
                      return std::nullopt;
                    },
                    [&](const build_result_t::Success& success) -> std::optional<store_path_t> {
                      auto i = get(success.built_outputs, output_name);
                      if (!i)
                        return std::nullopt;

                      return i->out_path;
                    },
                },
                buildResult.inner);
          });
      if (!attempt) {
        /* TODO (impure derivations-induced tech debt) (see below):
           The above attempt should have found it, but because we manage
           inputDrvOutputs statefully, sometimes it gets out of sync with
           the real source of truth (store). So we query the store
           directly if there's a problem. */
        attempt = fullDrv.try_resolve(worker.store, &worker.eval_store);
      }
      assert(attempt);

      auto pathResolved = write_derivation(worker.store, *attempt, NoRepair, /*read_only =*/true);

      auto msg = fmt("resolved derivation: '%s' -> '%s'", worker.store.printStorePath(drv_path),
                     worker.store.printStorePath(pathResolved));
      act = std::make_unique<activity_t>(
          *logger, lvl_info, act_build_waiting, msg,
          logger_t::fields_t{
              logger_t::field_t{worker.store.printStorePath(drv_path)},
              logger_t::field_t{worker.store.printStorePath(pathResolved)},
          });

      resolvedDrv = std::make_unique<std::pair<store_path_t, basic_derivation_t>>(std::move(pathResolved),
                                                                            *std::move(attempt));
    }
  }

  co_return amDone(ecSuccess, std::nullopt);
}

} // namespace nix
