#include "nix/store/build/derivation-building-goal.h"

#include "nix/store/build/derivation-env-desugar.h"
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
#include "nix/store/common-protocol-impl.h"
#include "nix/store/common-protocol.h"
#include "nix/store/globals.h"
#include "nix/store/local-store.h" // TODO remove, along with remaining downcasts
#include "nix/util/compression.h"
#include "nix/util/config-global.h"
#include "nix/util/processes.h"
#include "nix/util/strings.h"
#include "nix/util/util.h"

namespace nix {

DerivationBuildingGoal::DerivationBuildingGoal(const store_path_t& drv_path, const derivation_t& drv,
                                               Worker& worker, BuildMode build_mode,
                                               bool storeDerivation)
    : Goal(worker, gaveUpOnSubstitution(storeDerivation)),
      drv_path(drv_path),
      drv{std::make_unique<derivation_t>(drv)},
      build_mode(build_mode) {
  name = fmt("building derivation '%s'", worker.store.printStorePath(drv_path));
  trace("created");

  /* Prevent the .chroot directory from being
     garbage-collected. (See isActiveTempFile() in gc.cc.) */
  worker.store.addTempRoot(this->drv_path);
}

DerivationBuildingGoal::~DerivationBuildingGoal() {
  /* Careful: we should never ever throw an exception from a
     destructor. */
#ifndef _WIN32 // TODO enable `DerivationBuilder` on Windows
  if (builder)
    builder.reset();
#endif
  try {
    closeLogFile();
  } catch (...) {
    ignore_exception_in_destructor();
  }
}

std::string DerivationBuildingGoal::key() {
  return "dd$" + std::string(drv_path.name()) + "$" + worker.store.printStorePath(drv_path);
}

void DerivationBuildingGoal::kill_child() {
#ifndef _WIN32 // TODO enable build hook on Windows
  hook.reset();
#endif
#ifndef _WIN32 // TODO enable `DerivationBuilder` on Windows
  if (builder && builder->kill_child())
    worker.childTerminated(this);
#endif
}

void DerivationBuildingGoal::timedOut(Error&& ex) {
  kill_child();
  // We're not inside a coroutine, hence we can't use co_return here.
  // Thus we ignore the return value.
  [[maybe_unused]] done_t _ = doneFailure({build_result_t::Failure::TimedOut, std::move(ex)});
}

std::string show_known_outputs(const store_dir_config_t& store, const derivation_t& drv) {
  std::string msg;
  store_path_set_t expected_output_paths;
  for (auto& i : drv.outputsAndOptPaths(store))
    if (i.second.second)
      expected_output_paths.insert(*i.second.second);
  if (!expected_output_paths.empty()) {
    msg += "\nOutput paths:";
    for (auto& p : expected_output_paths)
      msg += fmt("\n  %s", magenta_t(store.printStorePath(p)));
  }
  return msg;
}

static void run_post_build_hook(const store_dir_config_t& store, logger_t& logger,
                                const store_path_t& drv_path, const store_path_set_t& output_paths);

/* At least one of the output paths could not be
   produced using a substitute.  So we have to build instead. */
Goal::Co DerivationBuildingGoal::gaveUpOnSubstitution(bool storeDerivation) {
  Goals waitees;

  /* Copy the input sources from the eval store to the build
     store.

     Note that some inputs might not be in the eval store because they
     are (resolved) derivation outputs in a resolved derivation. */
  if (&worker.eval_store != &worker.store) {
    RealisedPath::Set input_srcs;
    for (auto& i : drv->input_srcs)
      if (worker.eval_store.isValidPath(i))
        input_srcs.insert(i);
    copy_closure(worker.eval_store, worker.store, input_srcs);
  }

  for (auto& i : drv->input_srcs) {
    if (worker.store.isValidPath(i))
      continue;
    if (!settings.use_substitutes)
      throw Error("dependency '%s' of '%s' does not exist, and substitution is disabled",
                  worker.store.printStorePath(i), worker.store.printStorePath(drv_path));
    waitees.insert(upcast_goal(worker.makePathSubstitutionGoal(i)));
  }

  co_await await(std::move(waitees));

  trace("all inputs realised");

  if (nrFailed != 0) {
    auto msg = fmt("Cannot build '%s'.\n"
                   "Reason: " ANSI_RED "%d %s failed" ANSI_NORMAL ".",
                   magenta_t(worker.store.printStorePath(drv_path)), nrFailed,
                   nrFailed == 1 ? "dependency" : "dependencies");
    msg += show_known_outputs(worker.store, *drv);
    co_return doneFailure(build_error_t(build_result_t::Failure::DependencyFailed, msg));
  }

  /* Gather information necessary for computing the closure and/or
     running the build hook. */

  /* Determine the full set of input paths. */

  if (storeDerivation) {
    assert(drv->input_drvs.map.empty());
    /* store_t the resolved derivation, as part of the record of
       what we're actually building */
    write_derivation(worker.store, *drv);
  }

  {
    /* If we get this far, we know no dynamic drvs inputs */

    for (auto& [depDrvPath, depNode] : drv->input_drvs.map) {
      for (auto& output_name : depNode.value) {
        /* Don't need to worry about `inputGoals`, because
           impure derivations are always resolved above. Can
           just use DB. This case only happens in the (older)
           input addressed and fixed output derivation cases. */
        auto outMap = [&] {
          for (auto* drvStore : {&worker.eval_store, &worker.store})
            if (drvStore->isValidPath(depDrvPath))
              return worker.store.queryDerivationOutputMap(depDrvPath, drvStore);
          assert(false);
        }();

        auto outMapPath = outMap.find(output_name);
        if (outMapPath == outMap.end()) {
          throw Error(
              "derivation '%s' requires non-existent output '%s' from input derivation '%s'",
              worker.store.printStorePath(drv_path), output_name,
              worker.store.printStorePath(depDrvPath));
        }

        worker.store.computeFSClosure(outMapPath->second, inputPaths);
      }
    }
  }

  /* Second, the input sources. */
  worker.store.computeFSClosure(drv->input_srcs, inputPaths);

  debug("added input paths %s", worker.store.show_paths(inputPaths));

  /* Okay, try to build.  Note that here we don't wait for a build
     slot to become available, since we don't need one if there is a
     build hook. */
  co_await yield();
  co_return tryToBuild();
}

Goal::Co DerivationBuildingGoal::tryToBuild() {
  auto drv_options = [&] {
    derivation_options_t<SingleDerivedPath> temp;
    try {
      temp = derivation_options_from_structured_attrs(worker.store, drv->input_drvs, drv->env,
                                                      get(drv->structured_attrs));
    } catch (Error& e) {
      e.add_trace({}, "while parsing derivation '%s'", worker.store.printStorePath(drv_path));
      throw;
    }

    auto res = try_resolve(temp,
                           [&](ref<const SingleDerivedPath> drv_path,
                               const std::string& output_name) -> std::optional<store_path_t> {
                             try {
                               return resolve_derived_path(
                                   worker.store, SingleDerivedPath::Built{drv_path, output_name},
                                   &worker.eval_store);
                             } catch (Error&) {
                               return std::nullopt;
                             }
                           });

    /* The derivation must have all of its inputs gotten this point,
       so the resolution will surely succeed.

       (Actually, we shouldn't even enter this goal until we have a
       resolved derivation, or derivation with only input addressed
       transitive inputs, so this should be a no-opt anyways.)
     */
    assert(res);
    return *res;
  }();

  std::map<std::string, InitialOutput> initialOutputs;

  /* Recheck at this point. In particular, whereas before we were
     given this information by the downstream goal, that cannot happen
     anymore if the downstream goal only cares about one output, but
     we care about all outputs. */
  auto output_hashes = static_output_hashes(worker.eval_store, *drv);
  for (auto& [output_name, outputHash] : output_hashes) {
    InitialOutput v{.outputHash = outputHash};

    /* TODO we might want to also allow randomizing the paths
       for regular CA derivations, e.g. for sake of checking
       determinism. */
    if (drv->type().is_impure()) {
      v.known = InitialOutputStatus{
          .path = store_path_t::random(output_path_name(drv->name, output_name)),
          .status = PathStatus::Absent,
      };
    }

    initialOutputs.insert({
        output_name,
        std::move(v),
    });
  }
  checkPathValidity(initialOutputs);

  auto started = [&]() {
    auto msg = fmt(build_mode == bmRepair  ? "repairing outputs of '%s'"
                   : build_mode == bmCheck ? "checking outputs of '%s'"
                                           : "building '%s'",
                   worker.store.printStorePath(drv_path));
#ifndef _WIN32 // TODO enable build hook on Windows
    if (hook)
      msg += fmt(" on '%s'", hook->machine_name);
#endif
    act = std::make_unique<activity_t>(
        *logger, lvl_info, act_build, msg,
        logger_t::fields_t{logger_t::field_t{worker.store.printStorePath(drv_path)},
#ifndef _WIN32 // TODO enable build hook on Windows
                           logger_t::field_t{hook ? hook->machine_name : ""},
#else
                           logger_t::field_t{""},
#endif
                           logger_t::field_t{uint64_t{1}}, logger_t::field_t{uint64_t{1}}});
    mcRunningBuilds = std::make_unique<maintain_count_t<uint64_t>>(worker.runningBuilds);
    worker.updateProgress();
  };

  /**
   * activity_t that denotes waiting for a lock.
   */
  std::unique_ptr<activity_t> actLock;

  /**
   * Locks on (fixed) output paths.
   */
  PathLocks outputLocks;

  bool useHook;

  const ExternalBuilder* external_builder = nullptr;

  while (true) {
    trace("trying to build");

    /* Obtain locks on all output paths, if the paths are known a priori.

       The locks are automatically released when we exit this function or Nix
       crashes.  If we can't acquire the lock, then continue; hopefully some
       other goal can start a build, and if not, the main loop will sleep a few
       seconds and then retry this goal. */
    std::set<std::filesystem::path> lockFiles;
    /* FIXME: Should lock something like the drv itself so we don't build same
       CA drv concurrently */
    if (auto* localStore = dynamic_cast<LocalStore*>(&worker.store)) {
      /* If we aren't a local store, we might need to use the local store as
         a build remote, but that would cause a deadlock. */
      /* FIXME: Make it so we can use ourselves as a build remote even if we
         are the local store (separate locking for building vs scheduling? */
      /* FIXME: find some way to lock for scheduling for the other stores so
         a forking daemon with --store still won't farm out redundant builds.
         */
      for (auto& i : drv->outputsAndOptPaths(worker.store)) {
        if (i.second.second)
          lockFiles.insert(localStore->toRealPath(*i.second.second));
        else
          lockFiles.insert(localStore->toRealPath(drv_path) + "." + i.first);
      }
    }

    if (!outputLocks.lockPaths(lockFiles, "", false)) {
      activity_t act(*logger, lvl_warn, act_build_waiting,
                     fmt("waiting for lock on %s", magenta_t(show_paths(lockFiles))));

      /* Wait then try locking again, repeat until success (returned
         boolean is true). */
      do {
        co_await waitForAWhile();
      } while (!outputLocks.lockPaths(lockFiles, "", false));
    }

    /* Now check again whether the outputs are valid.  This is because
       another process may have started building in parallel.  After
       it has finished and released the locks, we can (and should)
       reuse its results.  (Strictly speaking the first check can be
       omitted, but that would be less efficient.)  Note that since we
       now hold the locks on the output paths, no other process can
       build this derivation, so no further checks are necessary. */
    auto [allValid, validOutputs] = checkPathValidity(initialOutputs);

    if (build_mode != bmCheck && allValid) {
      debug("skipping build of derivation '%s', someone beat us to it",
            worker.store.printStorePath(drv_path));
      outputLocks.setDeletion(true);
      outputLocks.unlock();
      co_return doneSuccess(build_result_t::Success::AlreadyValid, std::move(validOutputs));
    }

    /* If any of the outputs already exist but are not valid, delete
       them. */
    if (auto* localStore = dynamic_cast<local_fs_store*>(&worker.store)) {
      for (auto& [_, status] : initialOutputs) {
        if (!status.known || status.known->isValid())
          continue;
        auto store_path = status.known->path;
        debug("removing invalid path '%s'", worker.store.printStorePath(status.known->path));
        delete_path(localStore->toRealPath(store_path));
      }
    }

    /* Don't do a remote build if the derivation has the attribute
       `preferLocalBuild' set.  Also, check and repair modes are only
       supported for local builds. */
    bool buildLocally =
        (build_mode != bmNormal || drv_options.willBuildLocally(worker.store, *drv)) &&
        settings.max_build_jobs.get() != 0;

    if (buildLocally) {
      useHook = false;
    } else {
      switch (tryBuildHook(initialOutputs, drv_options)) {
        case rpAccept:
          /* yes, it has started doing so.  Wait until we get
             EOF from the hook. */
          useHook = true;
          break;
        case rpPostpone:
          /* Not now; wait until at least one child finishes or
             the wake-up timeout expires. */
          if (!actLock)
            actLock =
                std::make_unique<activity_t>(*logger, lvl_warn, act_build_waiting,
                                             fmt("waiting for a machine to build '%s'",
                                                 magenta_t(worker.store.printStorePath(drv_path))));
          outputLocks.unlock();
          co_await waitForAWhile();
          continue;
        case rpDecline:
          /* We should do it ourselves.

             Now that we've decided we can't / won't do a remote build, check
             that we can in fact build locally. First see if there is an
             external builder for a "semi-local build". If there is, prefer to
             use that. If there is not, then check if we can do a "true" local
             build. */

          external_builder = settings.findExternalDerivationBuilderIfSupported(*drv);

          if (!external_builder && !drv_options.canBuildLocally(worker.store, *drv)) {
            auto msg = fmt(
                "Cannot build '%s'.\n"
                "Reason: " ANSI_RED "required system or feature not available" ANSI_NORMAL "\n"
                "Required system: '%s' with features {%s}\n"
                "Current system: '%s' with features {%s}",
                magenta_t(worker.store.printStorePath(drv_path)), magenta_t(drv->platform),
                concat_strings_sep(", ", drv_options.getRequiredSystemFeatures(*drv)),
                magenta_t(settings.thisSystem),
                concat_strings_sep<string_set_t>(", ", worker.store.store_t::config.systemFeatures));

            // since aarch64-darwin has Rosetta 2, this user can actually run x86_64-darwin on their
            // hardware - we should tell them to run the command to install Darwin 2
            if (drv->platform == "x86_64-darwin" && settings.thisSystem == "aarch64-darwin")
              msg += fmt("\nNote: run `%s` to run programs for x86_64-darwin",
                         magenta_t("/usr/sbin/softwareupdate --install-rosetta && launchctl stop "
                                   "org.nixos.nix-daemon"));

#ifndef _WIN32 // TODO enable `DerivationBuilder` on Windows
            builder.reset();
#endif
            outputLocks.unlock();
            worker.permanentFailure = true;
            co_return doneFailure({build_result_t::Failure::InputRejected, std::move(msg)});
          }
          useHook = false;
          break;
      }
    }
    break;
  }

  actLock.reset();

  if (useHook) {
    buildResult.start_time = time(0); // inexact
    started();
    co_await Suspend{};

#ifndef _WIN32
    assert(hook);
#endif

    trace("hook build done");

    /* Since we got an EOF on the logger pipe, the builder is presumed
       to have terminated.  In fact, the builder could also have
       simply have closed its end of the pipe, so just to be sure,
       kill it. */
    int status =
#ifndef _WIN32 // TODO enable build hook on Windows
        hook->pid.kill();
#else
        0;
#endif

    debug("build hook for '%s' finished", worker.store.printStorePath(drv_path));

    buildResult.timesBuilt++;
    buildResult.stopTime = time(0);

    /* So the child is gone now. */
    worker.childTerminated(this);

    /* Close the read side of the logger pipe. */
#ifndef _WIN32 // TODO enable build hook on Windows
    hook->builder_out.read_side.close();
    hook->fromHook.read_side.close();
#endif

    /* Close the log file. */
    closeLogFile();

    /* Check the exit status. */
    if (!status_ok(status)) {
      auto e = fixupBuilderFailureErrorMessage({build_result_t::Failure::MiscFailure, status, ""});

      outputLocks.unlock();

      /* TODO (once again) support fine-grained error codes, see issue #12641. */

      co_return doneFailure(std::move(e));
    }

    /* Compute the FS closure of the outputs and register them as
       being valid. */
    auto built_outputs =
        /* When using a build hook, the build hook can register the output
           as valid (by doing `nix-store --import').  If so we don't have
           to do anything here.

           We can only early return when the outputs are known a priori. For
           floating content-addressing derivations this isn't the case.

           Aborts if any output is not valid or corrupt, and otherwise
           returns a 'SingleDrvOutputs' structure containing all outputs.
         */
        [&] {
          auto [allValid, validOutputs] = checkPathValidity(initialOutputs);
          if (!allValid)
            throw Error("some outputs are unexpectedly invalid");
          return validOutputs;
        }();

    store_path_set_t output_paths;
    for (auto& [_, output] : built_outputs)
      output_paths.insert(output.out_path);
    run_post_build_hook(worker.store, *logger, drv_path, output_paths);

    /* It is now safe to delete the lock files, since all future
       lockers will see that the output paths are valid; they will
       not create new lock files with the same names as the old
       (unlinked) lock files. */
    outputLocks.setDeletion(true);
    outputLocks.unlock();

    co_return doneSuccess(build_result_t::Success::Built, std::move(built_outputs));
  }

  co_await yield();

  if (!dynamic_cast<LocalStore*>(&worker.store)) {
    throw Error(
        R"(
            Unable to build with a primary store that isn't a local store;
            either pass a different '--store' or enable remote builds.

            For more information check 'man nix.conf' and search for '/machines'.
            )");
  }

#ifdef _WIN32 // TODO enable `DerivationBuilder` on Windows
  throw UnimplementedError("building derivations is not yet implemented on Windows");
#else
  assert(!hook);

  descriptor_t builder_out;

  // Will continue here while waiting for a build user below
  while (true) {
    unsigned int curBuilds = worker.getNrLocalBuilds();
    if (curBuilds >= settings.max_build_jobs) {
      outputLocks.unlock();
      co_await waitForBuildSlot();
      co_return tryToBuild();
    }

    if (!builder) {
      /**
       * Local implementation of these virtual methods, consider
       * this just a record of lambdas.
       */
      struct DerivationBuildingGoalCallbacks : DerivationBuilderCallbacks {
        DerivationBuildingGoal& goal;

        DerivationBuildingGoalCallbacks(DerivationBuildingGoal& goal,
                                        std::unique_ptr<DerivationBuilder>& builder)
            : goal{goal} {}

        ~DerivationBuildingGoalCallbacks() override = default;

        void childTerminated() override { goal.worker.childTerminated(&goal); }

        Path openLogFile() override { return goal.openLogFile(); }

        void closeLogFile() override { goal.closeLogFile(); }
      };

      auto* localStoreP = dynamic_cast<LocalStore*>(&worker.store);
      assert(localStoreP);

      decltype(DerivationBuilderParams::defaultPathsInChroot) defaultPathsInChroot =
          settings.sandboxPaths.get();
      DesugaredEnv desugaredEnv;

      /* Add the closure of store paths to the chroot. */
      store_path_set_t closure;
      for (auto& i : defaultPathsInChroot)
        try {
          if (worker.store.isInStore(i.second.source))
            worker.store.computeFSClosure(worker.store.toStorePath(i.second.source).first, closure);
        } catch (InvalidPath& e) {
        } catch (Error& e) {
          e.add_trace({}, "while processing sandbox path '%s'", i.second.source);
          throw;
        }
      for (auto& i : closure) {
        auto p = worker.store.printStorePath(i);
        defaultPathsInChroot.insert_or_assign(p, ChrootPath{.source = p});
      }

      try {
        desugaredEnv = DesugaredEnv::create(worker.store, *drv, drv_options, inputPaths);
      } catch (build_error_t& e) {
        outputLocks.unlock();
        worker.permanentFailure = true;
        co_return doneFailure(std::move(e));
      }

      DerivationBuilderParams params{
          .drv_path = drv_path,
          .buildResult = buildResult,
          .drv = *drv,
          .drv_options = drv_options,
          .inputPaths = inputPaths,
          .initialOutputs = initialOutputs,
          .build_mode = build_mode,
          .defaultPathsInChroot = std::move(defaultPathsInChroot),
          .systemFeatures = worker.store.config.systemFeatures.get(),
          .desugaredEnv = std::move(desugaredEnv),
          .act = act,
      };

      /* If we have to wait and retry (see below), then `builder` will
         already be created, so we don't need to create it again. */
      builder =
          external_builder
              ? make_external_derivation_builder(
                    *localStoreP, std::make_unique<DerivationBuildingGoalCallbacks>(*this, builder),
                    std::move(params), *external_builder)
              : make_derivation_builder(
                    *localStoreP, std::make_unique<DerivationBuildingGoalCallbacks>(*this, builder),
                    std::move(params));
    }

    if (auto builderOutOpt = builder->start_build()) {
      builder_out = *std::move(builderOutOpt);
    } else {
      if (!actLock)
        actLock =
            std::make_unique<activity_t>(*logger, lvl_warn, act_build_waiting,
                                         fmt("waiting for a free build user ID for '%s'",
                                             magenta_t(worker.store.printStorePath(drv_path))));
      co_await waitForAWhile();
      continue;
    }

    break;
  }

  actLock.reset();

  worker.childStarted(shared_from_this(), {builder_out}, true, true);

  started();
  co_await Suspend{};

  trace("build done");

  SingleDrvOutputs built_outputs;
  try {
    built_outputs = builder->unprepare_build();
  } catch (BuilderFailureError& e) {
    builder.reset();
    outputLocks.unlock();
    co_return doneFailure(fixupBuilderFailureErrorMessage(std::move(e)));
  } catch (build_error_t& e) {
    builder.reset();
    outputLocks.unlock();
// Allow selecting a subset of enum values
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (e.status) {
      case build_result_t::Failure::HashMismatch:
        worker.hashMismatch = true;
        /* See header, the protocols don't know about `HashMismatch`
           yet, so change it to `OutputRejected`, which they expect
           for this case (hash mismatch is a type of output
           rejection). */
        e.status = build_result_t::Failure::OutputRejected;
        break;
      case build_result_t::Failure::not_deterministic_t:
        worker.checkMismatch = true;
        break;
      default:
        /* Other statuses need no adjusting */
        break;
    }
#  pragma GCC diagnostic pop
    co_return doneFailure(std::move(e));
  }
  {
    builder.reset();
    store_path_set_t output_paths;
    /* In the check case we install no store objects, and so
       `built_outputs` is empty. However, per issue #14287, there is
       an expectation that the post-build hook is still executed.
       (This is useful for e.g. logging successful deterministic rebuilds.)

       In order to make that work, in the check case just load the
       (preexisting) infos from scratch, rather than relying on what
       `DerivationBuilder` returned to us. */
    for (auto& [_, output] :
         build_mode == bmCheck ? checkPathValidity(initialOutputs).second : built_outputs) {
      // for sake of `bmRepair`
      worker.markContentsGood(output.out_path);
      output_paths.insert(output.out_path);
    }
    run_post_build_hook(worker.store, *logger, drv_path, output_paths);

    /* It is now safe to delete the lock files, since all future
       lockers will see that the output paths are valid; they will
       not create new lock files with the same names as the old
       (unlinked) lock files. */
    outputLocks.setDeletion(true);
    outputLocks.unlock();
    co_return doneSuccess(build_result_t::Success::Built, std::move(built_outputs));
  }
#endif
}

static void run_post_build_hook(const store_dir_config_t& store, logger_t& logger,
                                const store_path_t& drv_path, const store_path_set_t& output_paths) {
  auto hook = settings.postBuildHook;
  if (hook == "")
    return;

  activity_t act(logger, lvl_talkative, act_post_build_hook,
                 fmt("running post-build-hook '%s'", settings.postBuildHook),
                 logger_t::fields_t{logger_t::field_t{store.printStorePath(drv_path)}});
  push_activity_t pact(act.id_);
  string_map_t hook_environment = get_env();

  hook_environment.emplace("DRV_PATH", store.printStorePath(drv_path));
  hook_environment.emplace("OUT_PATHS",
                           chomp(concat_strings_sep(" ", store.printStorePathSet(output_paths))));
  hook_environment.emplace("NIX_CONFIG", global_config.to_key_value());

  struct log_sink_t : sink_t {
    activity_t& act;
    std::string current_line;

    log_sink_t(activity_t& act) : act(act) {}

    void operator()(std::string_view data) override {
      for (auto c : data) {
        if (c == '\n') {
          flush_line();
        } else {
          current_line += c;
        }
      }
    }

    void flush_line() {
      act.result(res_post_build_log_line, current_line);
      current_line.clear();
    }

    ~log_sink_t() {
      if (current_line != "") {
        current_line += '\n';
        flush_line();
      }
    }
  };

  log_sink_t sink(act);

  run_program2({
      .program = settings.postBuildHook,
      .environment = hook_environment,
      .standard_out = &sink,
      .merge_stderr_to_stdout = true,
  });
}

build_error_t DerivationBuildingGoal::fixupBuilderFailureErrorMessage(BuilderFailureError e) {
  auto msg =
      fmt("Cannot build '%s'.\n"
          "Reason: " ANSI_RED "builder %s" ANSI_NORMAL ".",
          magenta_t(worker.store.printStorePath(drv_path)), status_to_string(e.builderStatus));

  msg += show_known_outputs(worker.store, *drv);

  if (!logger->is_verbose() && !logTail.empty()) {
    msg += fmt("\nLast %d log lines:\n", logTail.size());
    for (auto& line : logTail) {
      msg += "> ";
      msg += line;
      msg += "\n";
    }
    auto nixLogCommand = "nix log";
    // The command is on a separate line for easy copying, such as with triple click.
    // This message will be indented elsewhere, so removing the indentation before the
    // command will not put it at the start of the line unfortunately.
    msg += fmt("For full logs, run:\n  " ANSI_BOLD "%s %s" ANSI_NORMAL, nixLogCommand,
               worker.store.printStorePath(drv_path));
  }

  msg += e.extraMsgAfter;

  return build_error_t{e.status, msg};
}

HookReply
DerivationBuildingGoal::tryBuildHook(const std::map<std::string, InitialOutput>& initialOutputs,
                                     const derivation_options_t<store_path_t>& drv_options) {
#ifdef _WIN32 // TODO enable build hook on Windows
  return rpDecline;
#else
  /* This should use `worker.eval_store`, but per #13179 the build hook
     doesn't work with eval store anyways. */
  if (settings.buildHook.get().empty() || !worker.tryBuildHook ||
      !worker.store.isValidPath(drv_path))
    return rpDecline;

  if (!worker.hook)
    worker.hook = std::make_unique<HookInstance>();

  try {
    /* Send the request to the hook. */
    worker.hook->sink << "try" << (worker.getNrLocalBuilds() < settings.max_build_jobs ? 1 : 0)
                      << drv->platform << worker.store.printStorePath(drv_path)
                      << drv_options.getRequiredSystemFeatures(*drv);
    worker.hook->sink.flush();

    /* Read the first line of input, which should be a word indicating
       whether the hook wishes to perform the build. */
    std::string reply;
    while (true) {
      auto s = [&]() {
        try {
          return read_line(worker.hook->fromHook.read_side.get());
        } catch (Error& e) {
          e.add_trace({}, "while reading the response from the build hook");
          throw;
        }
      }();
      if (handle_json_log_message(s, worker.act, worker.hook->activities, "the build hook", true))
        ;
      else if (s.substr(0, 2) == "# ") {
        reply = s.substr(2);
        break;
      } else {
        s += "\n";
        write_to_stderr(s);
      }
    }

    debug("hook reply is '%1%'", reply);

    if (reply == "decline")
      return rpDecline;
    else if (reply == "decline-permanently") {
      worker.tryBuildHook = false;
      worker.hook = 0;
      return rpDecline;
    } else if (reply == "postpone")
      return rpPostpone;
    else if (reply != "accept")
      throw Error("bad hook reply '%s'", reply);

  } catch (sys_error_t& e) {
    if (e.err_no() == EPIPE) {
      printError("build hook died unexpectedly: %s",
                 chomp(drain_fd(worker.hook->fromHook.read_side.get())));
      worker.hook = 0;
      return rpDecline;
    } else
      throw;
  }

  hook = std::move(worker.hook);

  try {
    hook->machine_name = read_line(hook->fromHook.read_side.get());
  } catch (Error& e) {
    e.add_trace({}, "while reading the machine name from the build hook");
    throw;
  }

  CommonProto::WriteConn conn{hook->sink};

  /* Tell the hook all the inputs that have to be copied to the
     remote system. */
  CommonProto::write(worker.store, conn, inputPaths);

  /* Tell the hooks the missing outputs that have to be copied back
     from the remote system. */
  {
    string_set_t missingOutputs;
    for (auto& [output_name, status] : initialOutputs) {
      // XXX: Does this include known CA outputs?
      if (build_mode != bmCheck && status.known && status.known->isValid())
        continue;
      missingOutputs.insert(output_name);
    }
    CommonProto::write(worker.store, conn, missingOutputs);
  }

  hook->sink = fd_sink_t();
  hook->toHook.write_side.close();

  /* Create the log file and pipe. */
  [[maybe_unused]] Path logFile = openLogFile();

  std::set<muxable_pipe_poll_state_t::comm_channel_t> fds;
  fds.insert(hook->fromHook.read_side.get());
  fds.insert(hook->builder_out.read_side.get());
  worker.childStarted(shared_from_this(), fds, false, false);

  return rpAccept;
#endif
}

Path DerivationBuildingGoal::openLogFile() {
  logSize = 0;

  if (!settings.keepLog)
    return "";

  auto base_name = std::string(base_name_of(worker.store.printStorePath(drv_path)));

  /* Create a log file. */
  Path logDir;
  if (auto localStore = dynamic_cast<LocalStore*>(&worker.store))
    logDir = localStore->config->logDir;
  else
    logDir = settings.nixLogDir;
  Path dir = fmt("%s/%s/%s/", logDir, local_fs_store::drvsLogDir, base_name.substr(0, 2));
  create_dirs(dir);

  Path logFileName = fmt("%s/%s%s", dir, base_name.substr(2), settings.compressLog ? ".bz2" : "");

  fdLogFile = to_descriptor(open(logFileName.c_str(),
                                 O_CREAT | O_WRONLY | O_TRUNC
#ifndef _WIN32
                                     | O_CLOEXEC
#endif
                                 ,
                                 0666));
  if (!fdLogFile)
    throw sys_error_t("creating log file '%1%'", logFileName);

  logFileSink = std::make_shared<fd_sink_t>(fdLogFile.get());

  if (settings.compressLog)
    logSink = std::shared_ptr<compression_sink_t>(make_compression_sink("bzip2", *logFileSink));
  else
    logSink = logFileSink;

  return logFileName;
}

void DerivationBuildingGoal::closeLogFile() {
  auto logSink2 = std::dynamic_pointer_cast<compression_sink_t>(logSink);
  if (logSink2)
    logSink2->finish();
  if (logFileSink)
    logFileSink->flush();
  logSink = logFileSink = 0;
  fdLogFile.close();
}

bool DerivationBuildingGoal::isReadDesc(descriptor_t fd) {
#ifdef _WIN32 // TODO enable build hook on Windows
  return false;
#else
  return (hook && fd == hook->builder_out.read_side.get()) ||
         (builder && fd == builder->builder_out.get());
#endif
}

void DerivationBuildingGoal::handleChildOutput(descriptor_t fd, std::string_view data) {
  // local & `ssh://`-builds are dealt with here.
  auto isWrittenToLog = isReadDesc(fd);
  if (isWrittenToLog) {
    logSize += data.size();
    if (settings.maxLogSize && logSize > settings.maxLogSize) {
      kill_child();
      // We're not inside a coroutine, hence we can't use co_return here.
      // Thus we ignore the return value.
      [[maybe_unused]] done_t _ =
          doneFailure(build_error_t(build_result_t::Failure::LogLimitExceeded,
                                 "%s killed after writing more than %d bytes of log output",
                                 get_name(), settings.maxLogSize));
      return;
    }

    for (auto c : data)
      if (c == '\r')
        currentLogLinePos = 0;
      else if (c == '\n')
        flush_line();
      else {
        if (currentLogLinePos >= currentLogLine.size())
          currentLogLine.resize(currentLogLinePos + 1);
        currentLogLine[currentLogLinePos++] = c;
      }

    if (logSink)
      (*logSink)(data);
  }

#ifndef _WIN32 // TODO enable build hook on Windows
  if (hook && fd == hook->fromHook.read_side.get()) {
    for (auto c : data)
      if (c == '\n') {
        auto json = parse_json_message(currentHookLine, "the derivation builder");
        if (json) {
          auto s = handle_json_log_message(*json, worker.act, hook->activities,
                                           "the derivation builder", true);
          // ensure that logs from a builder using `ssh-ng://` as protocol
          // are also available to `nix log`.
          if (s && !isWrittenToLog && logSink) {
            const auto type = (*json)["type"];
            const auto fields = (*json)["fields"];
            if (type == res_build_log_line) {
              (*logSink)((fields.size() > 0 ? fields[0].get<std::string>() : "") + "\n");
            } else if (type == res_set_phase && !fields.is_null()) {
              const auto phase = fields[0];
              if (!phase.is_null()) {
                // nixpkgs' stdenv produces lines in the log to signal
                // phase changes.
                // We want to get the same lines in case of remote builds.
                // The format is:
                //   @nix { "action": "setPhase", "phase": "$curPhase" }
                const auto logLine =
                    nlohmann::json::object({{"action", "setPhase"}, {"phase", phase}});
                (*logSink)("@nix " +
                           logLine.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) +
                           "\n");
              }
            }
          }
        }
        currentHookLine.clear();
      } else
        currentHookLine += c;
  }
#endif
}

void DerivationBuildingGoal::handle_eof(descriptor_t fd) {
  if (!currentLogLine.empty())
    flush_line();
  worker.wakeUp(shared_from_this());
}

void DerivationBuildingGoal::flush_line() {
  if (handle_json_log_message(currentLogLine, *act, builderActivities, "the derivation builder",
                              false))
    ;

  else {
    logTail.push_back(currentLogLine);
    if (logTail.size() > settings.logLines)
      logTail.pop_front();

    act->result(res_build_log_line, currentLogLine);
  }

  currentLogLine = "";
  currentLogLinePos = 0;
}

std::map<std::string, std::optional<store_path_t>>
DerivationBuildingGoal::queryPartialDerivationOutputMap() {
  assert(!drv->type().is_impure());

  for (auto* drvStore : {&worker.eval_store, &worker.store})
    if (drvStore->isValidPath(drv_path))
      return worker.store.queryPartialDerivationOutputMap(drv_path, drvStore);

  /* In-memory derivation will naturally fall back on this case, where
     we do best-effort with static information. */
  std::map<std::string, std::optional<store_path_t>> res;
  for (auto& [name, output] : drv->outputs)
    res.insert_or_assign(name, output.path(worker.store, drv->name, name));
  return res;
}

std::pair<bool, SingleDrvOutputs>
DerivationBuildingGoal::checkPathValidity(std::map<std::string, InitialOutput>& initialOutputs) {
  if (drv->type().is_impure())
    return {false, {}};

  bool checkHash = build_mode == bmRepair;
  SingleDrvOutputs validOutputs;

  for (auto& i : queryPartialDerivationOutputMap()) {
    auto initialOutput = get(initialOutputs, i.first);
    if (!initialOutput)
      // this is an invalid output, gets caught with (!wantedOutputsLeft.empty())
      continue;
    auto& info = *initialOutput;
    if (i.second) {
      auto output_path = *i.second;
      info.known = {
          .path = output_path,
          .status = !worker.store.isValidPath(output_path)               ? PathStatus::Absent
                    : !checkHash || worker.pathContentsGood(output_path) ? PathStatus::Valid
                                                                         : PathStatus::Corrupt,
      };
    }
    auto drvOutput = DrvOutput{info.outputHash, i.first};
    if (experimental_feature_settings.is_enabled(xp_t::ca_derivations)) {
      if (auto real = worker.store.query_realisation(drvOutput)) {
        auto output_path = real->out_path;
        info.known = {
            .path = output_path,
            .status = !worker.store.isValidPath(output_path)               ? PathStatus::Absent
                      : !checkHash || worker.pathContentsGood(output_path) ? PathStatus::Valid
                                                                           : PathStatus::Corrupt,
        };
      } else if (info.known && info.known->isValid()) {
        // We know the output because it's a static output of the
        // derivation, and the output path is valid, but we don't have
        // its realisation stored (probably because it has been built
        // without the `ca-derivations` experimental flag).
        worker.store.register_drv_output(realisation_t{
            {
                .out_path = info.known->path,
            },
            drvOutput,
        });
      }
    }
    if (info.known && info.known->isValid())
      validOutputs.emplace(i.first, realisation_t{
                                        {
                                            .out_path = info.known->path,
                                        },
                                        drvOutput,
                                    });
  }

  bool allValid = true;
  for (auto& [_, status] : initialOutputs) {
    if (!status.known || !status.known->isValid()) {
      allValid = false;
      break;
    }
  }

  return {allValid, validOutputs};
}

Goal::done_t DerivationBuildingGoal::doneSuccess(build_result_t::Success::Status status,
                                                 SingleDrvOutputs built_outputs) {
  buildResult.inner = build_result_t::Success{
      .status = status,
      .built_outputs = std::move(built_outputs),
  };

  logger->result(act ? act->id_ : get_cur_activity(), res_build_result,
                 nlohmann::json(keyed_build_result_t(
                     buildResult, derived_path_t::Built{.drv_path = makeConstantStorePathRef(drv_path),
                                                     .outputs = OutputsSpec::All{}})));

  mcRunningBuilds.reset();

  if (status == build_result_t::Success::Built)
    worker.doneBuilds++;

  worker.updateProgress();

  return amDone(ecSuccess, std::nullopt);
}

Goal::done_t DerivationBuildingGoal::doneFailure(build_error_t ex) {
  buildResult.inner = build_result_t::Failure{
      .status = ex.status,
      .errorMsg = fmt("%s", uncolored_t(ex.info().msg)),
  };

  logger->result(act ? act->id_ : get_cur_activity(), res_build_result,
                 nlohmann::json(keyed_build_result_t(
                     buildResult, derived_path_t::Built{.drv_path = makeConstantStorePathRef(drv_path),
                                                     .outputs = OutputsSpec::All{}})));

  mcRunningBuilds.reset();

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
