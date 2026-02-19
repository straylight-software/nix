#include "nix/store/build/worker.h"

#include "nix/store/build/derivation-building-goal.h"
#include "nix/store/build/derivation-goal.h"
#include "nix/store/build/derivation-resolution-goal.h"
#include "nix/store/build/derivation-trampoline-goal.h"
#include "nix/store/build/drv-output-substitution-goal.h"
#include "nix/store/build/substitution-goal.h"
#include "nix/store/local-store.h"
#include "nix/store/machines.h"
#ifndef _WIN32 // TODO Enable building on Windows
#  include "nix/store/build/hook-instance.h"
#endif
#include "nix/store/globals.h"
#include "nix/util/signals.h"

namespace nix {

Worker::Worker(Store& store, Store& eval_store)
    : act(*logger, act_realise),
      actDerivations(*logger, act_builds),
      actSubstitutions(*logger, act_copy_paths),
      store(store),
      eval_store(eval_store) {
  nrLocalBuilds = 0;
  nrSubstitutions = 0;
  lastWokenUp = steady_time_point::min();
  permanentFailure = false;
  timedOut = false;
  hashMismatch = false;
  checkMismatch = false;
}

Worker::~Worker() {
  /* Explicitly get rid of all strong pointers now.  After this all
     goals that refer to this worker should be gone.  (Otherwise we
     are in trouble, since goals may call childTerminated() etc. in
     their destructors). */
  topGoals.clear();

  assert(expectedSubstitutions == 0);
  assert(expectedDownloadSize == 0);
  assert(expectedNarSize == 0);
}

template <class G, typename... Args>
std::shared_ptr<G> Worker::initGoalIfNeeded(std::weak_ptr<G>& goal_weak, Args&&... args) {
  if (auto goal = goal_weak.lock())
    return goal;

  auto goal = std::make_shared<G>(args...);
  goal_weak = goal;
  wakeUp(goal);
  return goal;
}

std::shared_ptr<DerivationTrampolineGoal>
Worker::makeDerivationTrampolineGoal(ref<const SingleDerivedPath> drvReq,
                                     const OutputsSpec& wantedOutputs, BuildMode build_mode) {
  return initGoalIfNeeded(derivationTrampolineGoals.ensureSlot(*drvReq).value[wantedOutputs],
                          drvReq, wantedOutputs, *this, build_mode);
}

std::shared_ptr<DerivationTrampolineGoal>
Worker::makeDerivationTrampolineGoal(const StorePath& drv_path, const OutputsSpec& wantedOutputs,
                                     const Derivation& drv, BuildMode build_mode) {
  return initGoalIfNeeded(
      derivationTrampolineGoals.ensureSlot(DerivedPath::opaque_t{drv_path}).value[wantedOutputs],
      drv_path, wantedOutputs, drv, *this, build_mode);
}

std::shared_ptr<DerivationGoal> Worker::makeDerivationGoal(const StorePath& drv_path,
                                                           const Derivation& drv,
                                                           const OutputName& wantedOutput,
                                                           BuildMode build_mode,
                                                           bool storeDerivation) {
  return initGoalIfNeeded(derivationGoals[drv_path][wantedOutput], drv_path, drv, wantedOutput, *this,
                          build_mode, storeDerivation);
}

std::shared_ptr<DerivationResolutionGoal>
Worker::makeDerivationResolutionGoal(const StorePath& drv_path, const Derivation& drv,
                                     BuildMode build_mode) {
  return initGoalIfNeeded(derivationResolutionGoals[drv_path], drv_path, drv, *this, build_mode);
}

std::shared_ptr<DerivationBuildingGoal> Worker::makeDerivationBuildingGoal(const StorePath& drv_path,
                                                                           const Derivation& drv,
                                                                           BuildMode build_mode,
                                                                           bool storeDerivation) {
  return initGoalIfNeeded(derivationBuildingGoals[drv_path], drv_path, drv, *this, build_mode,
                          storeDerivation);
}

std::shared_ptr<PathSubstitutionGoal>
Worker::makePathSubstitutionGoal(const StorePath& path, RepairFlag repair,
                                 std::optional<ContentAddress> ca) {
  return initGoalIfNeeded(substitutionGoals[path], path, *this, repair, ca);
}

std::shared_ptr<DrvOutputSubstitutionGoal>
Worker::makeDrvOutputSubstitutionGoal(const DrvOutput& id) {
  return initGoalIfNeeded(drvOutputSubstitutionGoals[id], id, *this);
}

GoalPtr Worker::makeGoal(const DerivedPath& req, BuildMode build_mode) {
  return std::visit(overloaded{
                        [&](const DerivedPath::Built& bfd) -> GoalPtr {
                          return makeDerivationTrampolineGoal(bfd.drv_path, bfd.outputs, build_mode);
                        },
                        [&](const DerivedPath::opaque_t& bo) -> GoalPtr {
                          return makePathSubstitutionGoal(
                              bo.path, build_mode == bmRepair ? Repair : NoRepair);
                        },
                    },
                    req.raw());
}

/**
 * This function is polymorphic (both via type parameters and
 * overloading) and recursive in order to work on a various types of
 * trees
 *
 * @return Whether the tree node we are processing is not empty / should
 * be kept alive. In the case of this overloading the node in question
 * is the leaf, the weak reference itself. If the weak reference points
 * to the goal we are looking for, our caller can delete it. In the
 * inductive case where the node is an interior node, we'll likewise
 * return whether the interior node is non-empty. If it is empty
 * (because we just deleted its last child), then our caller can
 * likewise delete it.
 */
template <typename G>
static bool remove_goal(std::shared_ptr<G> goal, std::weak_ptr<G>& gp) {
  return gp.lock() != goal;
}

template <typename K, typename G, typename Inner>
static bool remove_goal(std::shared_ptr<G> goal, std::map<K, Inner>& goalMap) {
  /* !!! inefficient */
  for (auto i = goalMap.begin(); i != goalMap.end();) {
    if (!remove_goal(goal, i->second))
      i = goalMap.erase(i);
    else
      ++i;
  }
  return !goalMap.empty();
}

template <typename G>
static bool
remove_goal(std::shared_ptr<G> goal,
           typename DerivedPathMap<std::map<OutputsSpec, std::weak_ptr<G>>>::ChildNode& node) {
  return remove_goal(goal, node.value) || remove_goal(goal, node.childMap);
}

void Worker::remove_goal(GoalPtr goal) {
  if (auto drvGoal = std::dynamic_pointer_cast<DerivationTrampolineGoal>(goal))
    nix::remove_goal(drvGoal, derivationTrampolineGoals.map);
  else if (auto drvGoal = std::dynamic_pointer_cast<DerivationGoal>(goal))
    nix::remove_goal(drvGoal, derivationGoals);
  else if (auto drvResolutionGoal = std::dynamic_pointer_cast<DerivationResolutionGoal>(goal))
    nix::remove_goal(drvResolutionGoal, derivationResolutionGoals);
  else if (auto drvBuildingGoal = std::dynamic_pointer_cast<DerivationBuildingGoal>(goal))
    nix::remove_goal(drvBuildingGoal, derivationBuildingGoals);
  else if (auto subGoal = std::dynamic_pointer_cast<PathSubstitutionGoal>(goal))
    nix::remove_goal(subGoal, substitutionGoals);
  else if (auto subGoal = std::dynamic_pointer_cast<DrvOutputSubstitutionGoal>(goal))
    nix::remove_goal(subGoal, drvOutputSubstitutionGoals);
  else
    assert(false);

  if (topGoals.find(goal) != topGoals.end()) {
    topGoals.erase(goal);
    /* If a top-level goal failed, then kill all other goals
       (unless keep_going was set). */
    if (goal->exit_code == Goal::ecFailed && !settings.keep_going)
      topGoals.clear();
  }

  /* Wake up goals waiting for any goal to finish. */
  for (auto& i : waitingForAnyGoal) {
    GoalPtr goal = i.lock();
    if (goal)
      wakeUp(goal);
  }

  waitingForAnyGoal.clear();
}

void Worker::wakeUp(GoalPtr goal) {
  goal->trace("woken up");
  add_to_weak_goals(awake, goal);
}

size_t Worker::getNrLocalBuilds() {
  return nrLocalBuilds;
}

size_t Worker::getNrSubstitutions() {
  return nrSubstitutions;
}

void Worker::childStarted(GoalPtr goal, const std::set<muxable_pipe_poll_state_t::comm_channel_t>& channels,
                          bool inBuildSlot, bool respectTimeouts) {
  Child child;
  child.goal = goal;
  child.goal2 = goal.get();
  child.channels = channels;
  child.timeStarted = child.last_output = steady_time_point::clock::now();
  child.inBuildSlot = inBuildSlot;
  child.respectTimeouts = respectTimeouts;
  children.emplace_back(child);
  if (inBuildSlot) {
    switch (goal->jobCategory()) {
      case JobCategory::Substitution:
        nrSubstitutions++;
        break;
      case JobCategory::Build:
        nrLocalBuilds++;
        break;
      case JobCategory::Administration:
        /* Intentionally not limited, see docs */
        break;
      default:
        unreachable();
    }
  }
}

void Worker::childTerminated(Goal* goal, bool wakeSleepers) {
  auto i = std::find_if(children.begin(), children.end(),
                        [&](const Child& child) { return child.goal2 == goal; });
  if (i == children.end())
    return;

  if (i->inBuildSlot) {
    switch (goal->jobCategory()) {
      case JobCategory::Substitution:
        assert(nrSubstitutions > 0);
        nrSubstitutions--;
        break;
      case JobCategory::Build:
        assert(nrLocalBuilds > 0);
        nrLocalBuilds--;
        break;
      case JobCategory::Administration:
        /* Intentionally not limited, see docs */
        break;
      default:
        unreachable();
    }
  }

  children.erase(i);

  if (wakeSleepers) {
    /* Wake up goals waiting for a build slot. */
    for (auto& j : wantingToBuild) {
      GoalPtr goal = j.lock();
      if (goal)
        wakeUp(goal);
    }

    wantingToBuild.clear();
  }
}

void Worker::waitForBuildSlot(GoalPtr goal) {
  goal->trace("wait for build slot");
  bool isSubstitutionGoal = goal->jobCategory() == JobCategory::Substitution;
  if ((!isSubstitutionGoal && getNrLocalBuilds() < settings.max_build_jobs) ||
      (isSubstitutionGoal && getNrSubstitutions() < settings.maxSubstitutionJobs))
    wakeUp(goal); /* we can do it right away */
  else
    add_to_weak_goals(wantingToBuild, goal);
}

void Worker::waitForAnyGoal(GoalPtr goal) {
  debug("wait for any goal");
  add_to_weak_goals(waitingForAnyGoal, goal);
}

void Worker::waitForAWhile(GoalPtr goal) {
  debug("wait for a while");
  add_to_weak_goals(waitingForAWhile, goal);
}

void Worker::run(const Goals& _topGoals) {
  std::vector<nix::DerivedPath> topPaths;

  for (auto& i : _topGoals) {
    topGoals.insert(i);
    if (auto goal = dynamic_cast<DerivationTrampolineGoal*>(i.get())) {
      topPaths.push_back(DerivedPath::Built{
          .drv_path = goal->drvReq,
          .outputs = goal->wantedOutputs,
      });
    } else if (auto goal = dynamic_cast<PathSubstitutionGoal*>(i.get())) {
      topPaths.push_back(DerivedPath::opaque_t{goal->store_path});
    }
  }

  /* Call query_missing() to efficiently query substitutes. */
  store.query_missing(topPaths);

  debug("entered goal loop");

  while (1) {
    check_interrupt();

    // TODO GC interface?
    if (auto localStore = dynamic_cast<LocalStore*>(&store))
      localStore->autoGC(false);

    /* Call every wake goal (in the ordering established by
       CompareGoalPtrs). */
    while (!awake.empty() && !topGoals.empty()) {
      Goals awake2;
      for (auto& i : awake) {
        GoalPtr goal = i.lock();
        if (goal)
          awake2.insert(goal);
      }
      awake.clear();
      for (auto& goal : awake2) {
        check_interrupt();
        goal->work();
        if (topGoals.empty())
          break; // stuff may have been cancelled
      }
    }

    if (topGoals.empty())
      break;

    /* Wait for input. */
    if (!children.empty() || !waitingForAWhile.empty())
      waitForInput();
    else if (awake.empty() && 0U == settings.max_build_jobs) {
      if (get_machines().empty())
        throw Error(
            "Unable to start any build; either increase '--max-jobs' or enable remote builds.\n"
            "\n"
            "For more information run 'man nix.conf' and search for '/machines'.");
      else
        throw Error("Unable to start any build; remote machines may not have all required system "
                    "features.\n"
                    "\n"
                    "For more information run 'man nix.conf' and search for '/machines'.");
    } else
      assert(!awake.empty());
  }

  /* If --keep-going is not set, it's possible that the main goal
     exited while some of its subgoals were still active.  But if
     --keep-going *is* set, then they must all be finished now. */
  assert(!settings.keep_going || awake.empty());
  assert(!settings.keep_going || wantingToBuild.empty());
  assert(!settings.keep_going || children.empty());
}

void Worker::waitForInput() {
  printMsg(lvl_vomit, "waiting for children");

  /* Process output from the file descriptors attached to the
     children, namely log output and output path creation commands.
     We also use this to detect child termination: if we get EOF on
     the logger pipe of a build, we assume that the builder has
     terminated. */

  bool useTimeout = false;
  long timeout = 0;
  auto before = steady_time_point::clock::now();

  /* If we're monitoring for silence on stdout/stderr, or if there
     is a build timeout, then wait for input until the first
     deadline for any child. */
  auto nearest = steady_time_point::max(); // nearest deadline
  if (settings.minFree.get() != 0)
    // Periodicallty wake up to see if we need to run the garbage collector.
    nearest = before + std::chrono::seconds(10);
  for (auto& i : children) {
    if (!i.respectTimeouts)
      continue;
    if (0 != settings.max_silent_time)
      nearest = std::min(nearest, i.last_output + std::chrono::seconds(settings.max_silent_time));
    if (0 != settings.buildTimeout)
      nearest = std::min(nearest, i.timeStarted + std::chrono::seconds(settings.buildTimeout));
  }
  if (nearest != steady_time_point::max()) {
    timeout = std::max(
        1L, (long)std::chrono::duration_cast<std::chrono::seconds>(nearest - before).count());
    useTimeout = true;
  }

  /* If we are polling goals that are waiting for a lock, then wake
     up after a few seconds at most. */
  if (!waitingForAWhile.empty()) {
    useTimeout = true;
    if (lastWokenUp == steady_time_point::min() || lastWokenUp > before)
      lastWokenUp = before;
    timeout = std::max(1L, (long)std::chrono::duration_cast<std::chrono::seconds>(
                               lastWokenUp + std::chrono::seconds(settings.pollInterval) - before)
                               .count());
  } else
    lastWokenUp = steady_time_point::min();

  if (useTimeout)
    vomit("sleeping %d seconds", timeout);

  muxable_pipe_poll_state_t state;

#ifndef _WIN32
  /* use select() to wait for the input side of any logger pipe to
     become `available'.  Note that `available' (i.e., non-blocking)
     includes EOF. */
  for (auto& i : children) {
    for (auto& j : i.channels) {
      state.poll_status.push_back((struct pollfd){.fd = j, .events = POLLIN});
      state.fd_to_poll_status[j] = state.poll_status.size() - 1;
    }
  }
#endif

  state.poll(
#ifdef _WIN32
      ioport.get(),
#endif
      useTimeout ? (std::optional{timeout * 1000}) : std::nullopt);

  auto after = steady_time_point::clock::now();

  /* Process all available file descriptors. FIXME: this is
     O(children * fds). */
  decltype(children)::iterator i;
  for (auto j = children.begin(); j != children.end(); j = i) {
    i = std::next(j);

    check_interrupt();

    GoalPtr goal = j->goal.lock();
    assert(goal);

    state.iterate(
        j->channels,
        [&](descriptor_t k, std::string_view data) {
          printMsg(lvl_vomit, "%1%: read %2% bytes", goal->get_name(), data.size());
          j->last_output = after;
          goal->handleChildOutput(k, data);
        },
        [&](descriptor_t k) {
          debug("%1%: got EOF", goal->get_name());
          goal->handle_eof(k);
        });

    if (goal->exit_code == Goal::ecBusy && 0 != settings.max_silent_time && j->respectTimeouts &&
        after - j->last_output >= std::chrono::seconds(settings.max_silent_time)) {
      goal->timedOut(Error("%1% timed out after %2% seconds of silence", goal->get_name(),
                           settings.max_silent_time));
    }

    else if (goal->exit_code == Goal::ecBusy && 0 != settings.buildTimeout && j->respectTimeouts &&
             after - j->timeStarted >= std::chrono::seconds(settings.buildTimeout)) {
      goal->timedOut(
          Error("%1% timed out after %2% seconds", goal->get_name(), settings.buildTimeout));
    }
  }

  if (!waitingForAWhile.empty() &&
      lastWokenUp + std::chrono::seconds(settings.pollInterval) <= after) {
    lastWokenUp = after;
    for (auto& i : waitingForAWhile) {
      GoalPtr goal = i.lock();
      if (goal)
        wakeUp(goal);
    }
    waitingForAWhile.clear();
  }
}

unsigned int Worker::failingExitStatus() {
  // See API docs in header for explanation
  unsigned int mask = 0;
  bool buildFailure = permanentFailure || timedOut || hashMismatch;
  if (buildFailure)
    mask |= 0x04; // 100
  if (timedOut)
    mask |= 0x01; // 101
  if (hashMismatch)
    mask |= 0x02; // 102
  if (checkMismatch) {
    mask |= 0x08; // 104
  }

  if (mask)
    mask |= 0x60;
  return mask ? mask : 1;
}

bool Worker::pathContentsGood(const StorePath& path) {
  auto i = pathContentsGoodCache.find(path);
  if (i != pathContentsGoodCache.end())
    return i->second;
  printInfo("checking path '%s'...", store.printStorePath(path));
  auto info = store.queryPathInfo(path);
  bool res = false;
  if (auto accessor = store.getFSAccessor(path, /*require_valid_path=*/false)) {
    auto current =
        hash_path({ref{accessor}}, file_ingestion_method_t::nix_archive, info->nar_hash.algo()).first;
    Hash nullHash(hash_algorithm_t::SHA256);
    res = info->nar_hash == nullHash || info->nar_hash == current;
  }
  pathContentsGoodCache.insert_or_assign(path, res);
  if (!res)
    printError("path '%s' is corrupted or missing!", store.printStorePath(path));
  return res;
}

void Worker::markContentsGood(const StorePath& path) {
  pathContentsGoodCache.insert_or_assign(path, true);
}

GoalPtr upcast_goal(std::shared_ptr<PathSubstitutionGoal> subGoal) {
  return subGoal;
}

GoalPtr upcast_goal(std::shared_ptr<DrvOutputSubstitutionGoal> subGoal) {
  return subGoal;
}

GoalPtr upcast_goal(std::shared_ptr<DerivationGoal> subGoal) {
  return subGoal;
}

} // namespace nix
