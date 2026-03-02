#include "nix/store/build/derivation-trampoline-goal.h"

#include "nix/store/build/worker.h"
#include "nix/store/derivations.h"

namespace nix {

DerivationTrampolineGoal::DerivationTrampolineGoal(ref<const SingleDerivedPath> drvReq,
                                                   const OutputsSpec& wantedOutputs, Worker& worker,
                                                   BuildMode build_mode)
    : Goal(worker, init()), drvReq(drvReq), wantedOutputs(wantedOutputs), build_mode(build_mode) {
  commonInit();
}

DerivationTrampolineGoal::DerivationTrampolineGoal(const store_path_t& drv_path,
                                                   const OutputsSpec& wantedOutputs,
                                                   const derivation_t& drv, Worker& worker,
                                                   BuildMode build_mode)
    : Goal(worker, haveDerivation(drv_path, drv)),
      drvReq(makeConstantStorePathRef(drv_path)),
      wantedOutputs(wantedOutputs),
      build_mode(build_mode) {
  commonInit();
}

void DerivationTrampolineGoal::commonInit() {
  name =
      fmt("obtaining derivation from '%s' and then building outputs %s",
          drvReq->to_string(worker.store),
          std::visit(overloaded{
                         [&](const OutputsSpec::All) -> std::string { return "* (all of them)"; },
                         [&](const OutputsSpec::Names os) {
                           return concat_strings_sep(", ", quote_strings(os));
                         },
                     },
                     wantedOutputs.raw));
  trace("created outer");

  worker.updateProgress();
}

DerivationTrampolineGoal::~DerivationTrampolineGoal() {}

static store_path_t path_part_of_req(const SingleDerivedPath& req) {
  return std::visit(
      overloaded{
          [&](const SingleDerivedPath::opaque_t& bo) { return bo.path; },
          [&](const SingleDerivedPath::Built& bfd) { return path_part_of_req(*bfd.drv_path); },
      },
      req.raw());
}

std::string DerivationTrampolineGoal::key() {
  return "da$" + std::string(path_part_of_req(*drvReq).name()) + "$" +
         derived_path_t::Built{
             .drv_path = drvReq,
             .outputs = wantedOutputs,
         }
             .to_string(worker.store);
}

Goal::Co DerivationTrampolineGoal::init() {
  trace("need to load derivation from file");

  /* The first thing to do is to make sure that the derivation
     exists.  If it doesn't, it may be built from another derivation,
     or merely substituted. We can make goal to get it and not worry
     about which method it takes to get the derivation. */
  if (auto optDrvPath = [this]() -> std::optional<store_path_t> {
        if (build_mode != bmNormal) {
          return std::nullopt;
        }

        auto drv_path = store_path_t::dummy;
        try {
          drv_path = resolve_derived_path(worker.store, *drvReq);
        } catch (MissingRealisation&) {
          return std::nullopt;
        }
        auto cond = worker.eval_store.isValidPath(drv_path) || worker.store.isValidPath(drv_path);
        return cond ? std::optional{drv_path} : std::nullopt;
      }()) {
    trace(fmt("already have drv '%s' for '%s', can go straight to building",
              worker.store.printStorePath(*optDrvPath), drvReq->to_string(worker.store)));
  } else {
    trace("need to obtain drv we want to build");
    Goals waitees{worker.makeGoal(derived_path_t::fromSingle(*drvReq))};
    co_await await(std::move(waitees));
  }

  trace("outer load and build derivation");

  if (nrFailed != 0) {
    co_return amDone(
        ecFailed, Error("cannot build missing derivation '%s'", drvReq->to_string(worker.store)));
  }

  store_path_t drv_path = resolve_derived_path(worker.store, *drvReq);

  /* `drv_path' should already be a root, but let's be on the safe
     side: if the user forgot to make it a root, we wouldn't want
     things being garbage collected while we're busy. */
  worker.eval_store.addTempRoot(drv_path);

  /* Get the derivation. It is probably in the eval store, but it might be in the main store:

       - Resolved derivation are resolved against main store realisations, and so must be stored
     there.

       - Dynamic derivations are built, and so are found in the main store.
   */
  auto drv = [&] {
    for (auto* drvStore : {&worker.eval_store, &worker.store}) {
      if (drvStore->isValidPath(drv_path)) {
        return drvStore->read_derivation(drv_path);
      }
    }
    assert(false);
  }();

  co_return haveDerivation(std::move(drv_path), std::move(drv));
}

Goal::Co DerivationTrampolineGoal::haveDerivation(store_path_t drv_path, derivation_t drv) {
  trace("have derivation, will kick off derivations goals per wanted output");

  auto resolvedWantedOutputs =
      std::visit(overloaded{
                     [&](const OutputsSpec::Names& names) -> OutputsSpec::Names { return names; },
                     [&](const OutputsSpec::All&) -> OutputsSpec::Names {
                       string_set_t outputs;
                       for (auto& [output_name, _] : drv.outputs) {
                         outputs.insert(output_name);
                       }
                       return outputs;
                     },
                 },
                 wantedOutputs.raw);

  Goals concreteDrvGoals;

  /* Build this step! */

  for (auto& output : resolvedWantedOutputs) {
    auto g = upcast_goal(worker.makeDerivationGoal(drv_path, drv, output, build_mode, false));
    g->preserveException = true;
    /* We will finish with it ourselves, as if we were the derivational goal. */
    concreteDrvGoals.insert(std::move(g));
  }

  // Copy on purpose
  co_await await(Goals(concreteDrvGoals));

  trace("outer build done");

  auto& g = *concreteDrvGoals.begin();
  buildResult = g->buildResult;
  if (auto* successP = buildResult.tryGetSuccess()) {
    for (auto& g2 : concreteDrvGoals) {
      if (auto* successP2 = g2->buildResult.tryGetSuccess()) {
        for (auto&& [x, y] : successP2->built_outputs) {
          successP->built_outputs.insert_or_assign(x, y);
        }
      }
    }
  }

  co_return amDone(g->exit_code, g->ex);
}

} // namespace nix
