#include "nix/store/build/derivation-trampoline-goal.h"
#include "nix/store/build/substitution-goal.h"
#include "nix/store/build/worker.h"
#include "nix/store/derivations.h"
#include "nix/store/local-store.h"
#include "nix/util/strings.h"

namespace nix {

void store_t::build_paths(const std::vector<derived_path_t>& reqs, BuildMode build_mode,
                       std::shared_ptr<store_t> eval_store) {
  Worker worker(*this, eval_store ? *eval_store : *this);

  Goals goals;
  for (auto& br : reqs)
    goals.insert(worker.makeGoal(br, build_mode));

  worker.run(goals);

  string_set_t failed;
  std::optional<Error> ex;
  for (auto& i : goals) {
    if (i->ex) {
      if (ex)
        logError(i->ex->info());
      else
        ex = std::move(i->ex);
    }
    if (i->exit_code != Goal::ecSuccess) {
      if (auto i2 = dynamic_cast<DerivationTrampolineGoal*>(i.get()))
        failed.insert(i2->drvReq->to_string(*this));
      else if (auto i2 = dynamic_cast<PathSubstitutionGoal*>(i.get()))
        failed.insert(printStorePath(i2->store_path));
    }
  }

  if (failed.size() == 1 && ex) {
    ex->with_exit_status(worker.failingExitStatus());
    throw std::move(*ex);
  } else if (!failed.empty()) {
    if (ex)
      logError(ex->info());
    throw Error(worker.failingExitStatus(), "build of %s failed",
                concat_strings_sep(", ", quote_strings(failed)));
  }
}

std::vector<keyed_build_result_t> store_t::build_paths_with_results(const std::vector<derived_path_t>& reqs,
                                                           BuildMode build_mode,
                                                           std::shared_ptr<store_t> eval_store) {
  Worker worker(*this, eval_store ? *eval_store : *this);

  Goals goals;
  std::vector<std::pair<const derived_path_t&, GoalPtr>> state;

  for (const auto& req : reqs) {
    auto goal = worker.makeGoal(req, build_mode);
    goals.insert(goal);
    state.push_back({req, goal});
  }

  worker.run(goals);

  std::vector<keyed_build_result_t> results;
  results.reserve(state.size());

  for (auto& [req, goalPtr] : state)
    results.emplace_back(keyed_build_result_t{
        goalPtr->buildResult,
        /* .path = */ req,
    });

  return results;
}

build_result_t store_t::buildDerivation(const store_path_t& drv_path, const basic_derivation_t& drv,
                                   BuildMode build_mode) {
  Worker worker(*this, *this);
  auto goal = worker.makeDerivationTrampolineGoal(drv_path, OutputsSpec::All{}, drv, build_mode);

  try {
    worker.run(Goals{goal});
    return goal->buildResult;
  } catch (Error& e) {
    return build_result_t{.inner{build_result_t::Failure{
        .status = build_result_t::Failure::MiscFailure,
        .errorMsg = e.msg(),
    }}};
  };
}

void store_t::ensure_path(const store_path_t& path) {
  /* If the path is already valid, we're done. */
  if (isValidPath(path))
    return;

  Worker worker(*this, *this);
  GoalPtr goal = worker.makePathSubstitutionGoal(path);
  Goals goals = {goal};

  worker.run(goals);

  if (goal->exit_code != Goal::ecSuccess) {
    if (goal->ex) {
      goal->ex->with_exit_status(worker.failingExitStatus());
      throw std::move(*goal->ex);
    } else
      throw Error(worker.failingExitStatus(), "path '%s' does not exist and cannot be created",
                  printStorePath(path));
  }
}

void store_t::repairPath(const store_path_t& path) {
  Worker worker(*this, *this);
  GoalPtr goal = worker.makePathSubstitutionGoal(path, Repair);
  Goals goals = {goal};

  worker.run(goals);

  if (goal->exit_code != Goal::ecSuccess) {
    /* Since substituting the path didn't work, if we have a valid
       deriver, then rebuild the deriver. */
    auto info = queryPathInfo(path);
    if (info->deriver && isValidPath(*info->deriver)) {
      goals.clear();
      goals.insert(worker.makeGoal(
          derived_path_t::Built{
              .drv_path = makeConstantStorePathRef(*info->deriver),
              // FIXME: Should just build the specific output we need.
              .outputs = OutputsSpec::All{},
          },
          bmRepair));
      worker.run(goals);
    } else
      throw Error(worker.failingExitStatus(), "cannot repair path '%s'", printStorePath(path));
  }
}

} // namespace nix
