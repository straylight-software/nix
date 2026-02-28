#include "nix/store/build/derivation-goal.h"

#include "nix/store/build/derivation-building-goal.h"
#include "nix/store/build/derivation-resolution-goal.h"
#ifndef _WIN32 // TODO enable build hook on Windows
#  include "nix/store/build/derivation-builder.h"
#  include "nix/store/build/hook-instance.h"
#endif
#include <fstream>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "nix/store/build/worker.h"
#include "nix/store/common-protocol-impl.h" // Don't remove is actually needed
#include "nix/store/common-protocol.h"
#include "nix/store/globals.h"
#include "nix/util/compression.h"
#include "nix/util/config-global.h"
#include "nix/util/processes.h"
#include "nix/util/strings.h"
#include "nix/util/util.h"

namespace nix {

DerivationGoal::DerivationGoal(const store_path_t& drv_path, const derivation_t& drv,
                               const OutputName& wantedOutput, Worker& worker, BuildMode build_mode,
                               bool storeDerivation)
    : Goal(worker, haveDerivation(storeDerivation)),
      drv_path(drv_path),
      wantedOutput(wantedOutput),
      drv{std::make_unique<derivation_t>(drv)},
      outputHash{[&] {
        auto output_hashes = static_output_hashes(worker.eval_store, drv);
        if (auto* mOutputHash = get(output_hashes, wantedOutput))
          return *mOutputHash;
        throw Error("derivation '%s' does not have output '%s'",
                    worker.store.printStorePath(drv_path), wantedOutput);
      }()},
      build_mode(build_mode) {
  name = fmt("getting output '%s' from derivation '%s'", wantedOutput,
             worker.store.printStorePath(drv_path));
  trace("created");

  mcExpectedBuilds = std::make_unique<maintain_count_t<uint64_t>>(worker.expectedBuilds);
  worker.updateProgress();
}

std::string DerivationGoal::key() {
  return "db$" + std::string(drv_path.name()) + "$" +
         SingleDerivedPath::Built{
             .drv_path = makeConstantStorePathRef(drv_path),
             .output = wantedOutput,
         }
             .to_string(worker.store);
}

Goal::Co DerivationGoal::haveDerivation(bool storeDerivation) {
  trace("have derivation");

  auto drv_options = [&]() -> derivation_options_t<SingleDerivedPath> {
    try {
      return derivation_options_from_structured_attrs(worker.store, drv->input_drvs, drv->env,
                                                      get(drv->structured_attrs));
    } catch (Error& e) {
      e.add_trace({}, "while parsing derivation '%s'", worker.store.printStorePath(drv_path));
      throw;
    }
  }();

  if (!drv->type().hasKnownOutputPaths())
    experimental_feature_settings.require(xp_t::ca_derivations);

  for (auto& i : drv->outputsAndOptPaths(worker.store))
    if (i.second.second)
      worker.store.addTempRoot(*i.second.second);

  /* We don't yet have any safe way to cache an impure derivation at
     this step. */
  if (drv->type().is_impure()) {
    experimental_feature_settings.require(xp_t::impure_derivations);
  } else {
    /* Check what outputs paths are not already valid. */
    auto checkResult = checkPathValidity();

    /* If they are all valid, then we're done. */
    if (checkResult && checkResult->second == PathStatus::Valid && build_mode == bmNormal) {
      co_return doneSuccess(build_result_t::Success::AlreadyValid, checkResult->first);
    }

    Goals waitees;

    /* We are first going to try to create the invalid output paths
       through substitutes.  If that doesn't work, we'll build
       them. */
    if (settings.use_substitutes && drv_options.substitutesAllowed()) {
      if (!checkResult)
        waitees.insert(
            upcast_goal(worker.makeDrvOutputSubstitutionGoal(DrvOutput{outputHash, wantedOutput})));
      else {
        auto* cap = get_derivation_ca(*drv);
        waitees.insert(upcast_goal(worker.makePathSubstitutionGoal(
            checkResult->first.out_path, build_mode == bmRepair ? Repair : NoRepair,
            cap ? std::optional{*cap} : std::nullopt)));
      }
    }

    co_await await(std::move(waitees));

    trace("all outputs substituted (maybe)");

    assert(!drv->type().is_impure());

    if (nrFailed > 0 && nrFailed > nrNoSubstituters && !settings.try_fallback) {
      co_return doneFailure(build_error_t(
          build_result_t::Failure::TransientFailure,
          "some substitutes for the outputs of derivation '%s' failed (usually happens "
          "due to networking issues); try '--fallback' to build derivation from source ",
          worker.store.printStorePath(drv_path)));
    }

    nrFailed = nrNoSubstituters = 0;

    checkResult = checkPathValidity();

    bool allValid = checkResult && checkResult->second == PathStatus::Valid;

    if (build_mode == bmNormal && allValid) {
      co_return doneSuccess(build_result_t::Success::Substituted, checkResult->first);
    }
    if (build_mode == bmRepair && allValid) {
      co_return repairClosure();
    }
    if (build_mode == bmCheck && !allValid)
      throw Error("some outputs of '%s' are not valid, so checking is not possible",
                  worker.store.printStorePath(drv_path));
  }

  auto resolutionGoal = worker.makeDerivationResolutionGoal(drv_path, *drv, build_mode);
  {
    Goals waitees{resolutionGoal};
    co_await await(std::move(waitees));
  }
  if (nrFailed != 0) {
    co_return doneFailure(
        {build_result_t::Failure::DependencyFailed, "Build failed due to failed dependency"});
  }

  if (resolutionGoal->resolvedDrv) {
    auto& [pathResolved, drvResolved] = *resolutionGoal->resolvedDrv;

    auto resolvedDrvGoal = worker.makeDerivationGoal(pathResolved, drvResolved, wantedOutput,
                                                     build_mode, /*storeDerivation=*/true);
    {
      Goals waitees{resolvedDrvGoal};
      co_await await(std::move(waitees));
    }

    trace("resolved derivation finished");

    auto resolvedResult = resolvedDrvGoal->buildResult;

    // No `std::visit` for coroutines yet
    if (auto* successP = resolvedResult.tryGetSuccess()) {
      auto& success = *successP;
      auto output_hashes = static_output_hashes(worker.eval_store, *drv);
      auto resolvedHashes = static_output_hashes(worker.store, drvResolved);

      auto outputHash = get(output_hashes, wantedOutput);
      auto resolvedHash = get(resolvedHashes, wantedOutput);
      if ((!outputHash) || (!resolvedHash))
        throw Error(
            "derivation '%s' doesn't have expected output '%s' (derivation-goal.cc/resolve)",
            worker.store.printStorePath(drv_path), wantedOutput);

      auto realisation = [&] {
        auto take1 = get(success.built_outputs, wantedOutput);
        if (take1)
          return static_cast<UnkeyedRealisation>(*take1);

        /* The above `get` should work. But stateful tracking of
           outputs in resolvedResult, this can get out of sync with the
           store, which is our actual source of truth. For now we just
           check the store directly if it fails. */
        auto take2 = worker.eval_store.query_realisation(DrvOutput{
            .drvHash = *resolvedHash,
            .output_name = wantedOutput,
        });
        if (take2)
          return *take2;

        throw Error(
            "derivation '%s' doesn't have expected output '%s' (derivation-goal.cc/realisation)",
            worker.store.printStorePath(pathResolved), wantedOutput);
      }();

      if (!drv->type().is_impure()) {
        realisation_t newRealisation{realisation,
                                     {
                                         .drvHash = *outputHash,
                                         .output_name = wantedOutput,
                                     }};
        newRealisation.signatures.clear();
        if (!drv->type().isFixed()) {
          auto& drvStore =
              worker.eval_store.isValidPath(drv_path) ? worker.eval_store : worker.store;
          newRealisation.dependentRealisations =
              drv_output_references(worker.store, *drv, realisation.out_path, &drvStore);
        }
        worker.store.signRealisation(newRealisation);
        worker.store.register_drv_output(newRealisation);
      }

      auto status = success.status;
      if (status == build_result_t::Success::AlreadyValid)
        status = build_result_t::Success::ResolvesToAlreadyValid;

      co_return doneSuccess(status, std::move(realisation));
    } else if (auto* failureP = resolvedResult.tryGetFailure()) {
      co_return doneFailure({
          build_result_t::Failure::DependencyFailed,
          "build of resolved derivation '%s' failed: %s",
          worker.store.printStorePath(pathResolved),
          failureP->errorMsg.empty() ? build_result_t::Failure::status_to_string(failureP->status)
                                     : failureP->errorMsg,
      });
    } else {
      /* This case theoretically shouldn't happen since build_result_t::inner is
         std::variant<Success, Failure>, but it can occur if the variant is in
         a valueless_by_exception state or due to coroutine-related issues.
         Treat it as a failure rather than crashing the daemon. */
      co_return doneFailure({
          build_result_t::Failure::MiscFailure,
          "build of resolved derivation '%s' completed in an unexpected state",
          worker.store.printStorePath(pathResolved),
      });
    }
  }

  /* Give up on substitution for the output we want, actually build this derivation */

  auto g = worker.makeDerivationBuildingGoal(drv_path, *drv, build_mode, storeDerivation);

  /* We will finish with it ourselves, as if we were the derivational goal. */
  g->preserveException = true;

  {
    Goals waitees;
    waitees.insert(g);
    co_await await(std::move(waitees));
  }

  trace("outer build done");

  buildResult = g->buildResult;

  if (auto* successP = buildResult.tryGetSuccess()) {
    auto& success = *successP;
    if (build_mode == bmCheck) {
      /* In checking mode, the builder will not register any outputs.
         So we want to make sure the ones that we wanted to check are
         properly there. */
      success.built_outputs = {{
          wantedOutput,
          {
              assertPathValidity(),
              {
                  .drvHash = outputHash,
                  .output_name = wantedOutput,
              },
          },
      }};
    } else {
      /* Otherwise the builder will give us info for out output, but
         also for other outputs. Filter down to just our output so as
         not to leak info on unrelated things. */
      for (auto it = success.built_outputs.begin(); it != success.built_outputs.end();) {
        if (it->first != wantedOutput) {
          it = success.built_outputs.erase(it);
        } else {
          ++it;
        }
      }

      /* If the wanted output is not in built_outputs (e.g., because it
         was already valid and therefore not re-registered), we need to
         add it ourselves to ensure we return the correct information. */
      if (success.built_outputs.count(wantedOutput) == 0) {
        debug("BUG! wanted output '%s' not in builtOutputs, working around by adding it manually",
              wantedOutput);
        success.built_outputs = {{
            wantedOutput,
            {
                assertPathValidity(),
                {
                    .drvHash = outputHash,
                    .output_name = wantedOutput,
                },
            },
        }};
      }
    }
  }

  co_return amDone(g->exit_code, g->ex);
}

Goal::Co DerivationGoal::repairClosure() {
  assert(!drv->type().is_impure());

  /* If we're repairing, we now know that our own outputs are valid.
     Now check whether the other paths in the outputs closure are
     good.  If not, then start derivation goals for the derivations
     that produced those outputs. */

  /* Get the output closure. */
  auto outputs = [&] {
    for (auto* drvStore : {&worker.eval_store, &worker.store})
      if (drvStore->isValidPath(drv_path))
        return worker.store.queryDerivationOutputMap(drv_path, drvStore);

    OutputPathMap res;
    for (auto& [name, output] : drv->outputsAndOptPaths(worker.store))
      res.insert_or_assign(name, *output.second);
    return res;
  }();

  store_path_set_t outputClosure;
  if (auto* mPath = get(outputs, wantedOutput)) {
    worker.store.computeFSClosure(*mPath, outputClosure);
  }

  /* Filter out our own outputs (which we have already checked). */
  for (auto& i : outputs)
    outputClosure.erase(i.second);

  /* Get all dependencies of this derivation so that we know which
     derivation is responsible for which path in the output
     closure. */
  store_path_set_t inputClosure;

  /* If we're working from an in-memory derivation with no in-store
     `*.drv` file, we cannot do this part. */
  if (worker.store.isValidPath(drv_path))
    worker.store.computeFSClosure(drv_path, inputClosure);

  std::map<store_path_t, store_path_t> outputsToDrv;
  for (auto& i : inputClosure)
    if (i.is_derivation()) {
      auto depOutputs = worker.store.queryPartialDerivationOutputMap(i, &worker.eval_store);
      for (auto& j : depOutputs)
        if (j.second)
          outputsToDrv.insert_or_assign(*j.second, i);
    }

  Goals waitees;

  /* Check each path (slow!). */
  for (auto& i : outputClosure) {
    if (worker.pathContentsGood(i))
      continue;
    printError("found corrupted or missing path '%s' in the output closure of '%s'",
               worker.store.printStorePath(i), worker.store.printStorePath(drv_path));
    auto drvPath2 = outputsToDrv.find(i);
    if (drvPath2 == outputsToDrv.end())
      waitees.insert(upcast_goal(worker.makePathSubstitutionGoal(i, Repair)));
    else
      waitees.insert(worker.makeGoal(
          derived_path_t::Built{
              .drv_path = makeConstantStorePathRef(drvPath2->second),
              .outputs = OutputsSpec::All{},
          },
          bmRepair));
  }

  bool haveWaitees = !waitees.empty();
  co_await await(std::move(waitees));

  if (haveWaitees) {
    trace("closure repaired");
    if (nrFailed > 0)
      throw Error("some paths in the output closure of derivation '%s' could not be repaired",
                  worker.store.printStorePath(drv_path));
  }
  co_return doneSuccess(build_result_t::Success::AlreadyValid, assertPathValidity());
}

std::optional<std::pair<UnkeyedRealisation, PathStatus>> DerivationGoal::checkPathValidity() {
  if (drv->type().is_impure())
    return std::nullopt;

  auto drvOutput = DrvOutput{outputHash, wantedOutput};

  std::optional<UnkeyedRealisation> mRealisation;

  if (auto* mOutput = get(drv->outputs, wantedOutput)) {
    if (auto mPath = mOutput->path(worker.store, drv->name, wantedOutput)) {
      mRealisation = UnkeyedRealisation{
          .out_path = std::move(*mPath),
      };
    }
  } else {
    throw Error("derivation '%s' does not have wanted outputs '%s'",
                worker.store.printStorePath(drv_path), wantedOutput);
  }

  if (experimental_feature_settings.is_enabled(xp_t::ca_derivations)) {
    for (auto* drvStore : {&worker.eval_store, &worker.store}) {
      if (auto real = drvStore->query_realisation(drvOutput)) {
        mRealisation = *real;
        break;
      }
    }
  }

  if (mRealisation) {
    auto& output_path = mRealisation->out_path;
    bool checkHash = build_mode == bmRepair;
    PathStatus status = !worker.store.isValidPath(output_path)               ? PathStatus::Absent
                        : !checkHash || worker.pathContentsGood(output_path) ? PathStatus::Valid
                                                                             : PathStatus::Corrupt;

    if (experimental_feature_settings.is_enabled(xp_t::ca_derivations) &&
        status == PathStatus::Valid) {
      // We know the output because it's a static output of the
      // derivation, and the output path is valid, but we don't have
      // its realisation stored (probably because it has been built
      // without the `ca-derivations` experimental flag).
      worker.store.register_drv_output(realisation_t{
          *mRealisation,
          {
              .drvHash = outputHash,
              .output_name = wantedOutput,
          },
      });
    }

    return {{*mRealisation, status}};
  } else
    return std::nullopt;
}

UnkeyedRealisation DerivationGoal::assertPathValidity() {
  auto checkResult = checkPathValidity();
  if (!(checkResult && checkResult->second == PathStatus::Valid))
    throw Error("some outputs are unexpectedly invalid");
  return checkResult->first;
}

Goal::done_t DerivationGoal::doneSuccess(build_result_t::Success::Status status,
                                         UnkeyedRealisation builtOutput) {
  buildResult.inner = build_result_t::Success{
      .status = status,
      .built_outputs = {{
          wantedOutput,
          {
              std::move(builtOutput),
              DrvOutput{
                  .drvHash = outputHash,
                  .output_name = wantedOutput,
              },
          },
      }},
  };

  logger->result(
      get_cur_activity(), res_build_result,
      nlohmann::json(keyed_build_result_t(
          buildResult, derived_path_t::Built{.drv_path = makeConstantStorePathRef(drv_path),
                                             .outputs = OutputsSpec::All{}})));

  mcExpectedBuilds.reset();

  if (status == build_result_t::Success::Built)
    worker.doneBuilds++;

  worker.updateProgress();

  return amDone(ecSuccess, std::nullopt);
}

Goal::done_t DerivationGoal::doneFailure(build_error_t ex) {
  buildResult.inner = build_result_t::Failure{
      .status = ex.status,
      .errorMsg = fmt("%s", uncolored_t(ex.info().msg_)),
  };

  logger->result(
      get_cur_activity(), res_build_result,
      nlohmann::json(keyed_build_result_t(
          buildResult, derived_path_t::Built{.drv_path = makeConstantStorePathRef(drv_path),
                                             .outputs = OutputsSpec::All{}})));

  mcExpectedBuilds.reset();

  if (ex.status == build_result_t::Failure::TimedOut)
    worker.timedOut = true;
  if (ex.status == build_result_t::Failure::PermanentFailure)
    worker.permanentFailure = true;
  if (ex.status != build_result_t::Failure::DependencyFailed)
    worker.failedBuilds++;

  worker.updateProgress();

  return amDone(ecFailed, {std::move(ex)});
}

} // namespace nix
