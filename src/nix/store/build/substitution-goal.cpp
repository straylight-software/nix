#include "nix/store/build/substitution-goal.h"

#include <coroutine>

#include <nlohmann/json.hpp>

#include "nix/store/build/worker.h"
#include "nix/store/globals.h"
#include "nix/store/nar-info.h"
#include "nix/store/store-open.h"
#include "nix/util/finally.h"
#include "nix/util/signals.h"

namespace nix {

PathSubstitutionGoal::PathSubstitutionGoal(const store_path_t& store_path, Worker& worker,
                                           RepairFlag repair, std::optional<content_address_t> ca)
    : Goal(worker, init()), store_path(store_path), repair(repair), ca(ca) {
  name = fmt("substitution of '%s'", worker.store.printStorePath(this->store_path));
  trace("created");
  maintainExpectedSubstitutions =
      std::make_unique<maintain_count_t<uint64_t>>(worker.expectedSubstitutions);
}

PathSubstitutionGoal::~PathSubstitutionGoal() {
  cleanup();
}

Goal::done_t PathSubstitutionGoal::doneSuccess(build_result_t::Success::Status status) {
  buildResult.inner = build_result_t::Success{
      .status = status,
  };

  logger->result(
      get_cur_activity(), res_build_result,
      nlohmann::json(keyed_build_result_t(buildResult, derived_path_t::opaque_t{store_path})));

  return amDone(ecSuccess);
}

Goal::done_t PathSubstitutionGoal::doneFailure(ExitCode result,
                                               build_result_t::Failure::Status status,
                                               std::string errorMsg) {
  debug(errorMsg);
  buildResult.inner = build_result_t::Failure{
      .status = status,
      .errorMsg = std::move(errorMsg),
  };

  logger->result(
      get_cur_activity(), res_build_result,
      nlohmann::json(keyed_build_result_t(buildResult, derived_path_t::opaque_t{store_path})));

  return amDone(result);
}

Goal::Co PathSubstitutionGoal::init() {
  trace("init");

  worker.store.addTempRoot(store_path);

  /* If the path already exists we're done. */
  if (!repair && worker.store.isValidPath(store_path)) {
    co_return doneSuccess(build_result_t::Success::AlreadyValid);
  }

  if (settings.readOnlyMode)
    throw Error("cannot substitute path '%s' - no write access to the Nix store",
                worker.store.printStorePath(store_path));

  auto subs = settings.use_substitutes ? get_default_substituters() : std::list<ref<store_t>>();

  bool substituterFailed = false;
  std::optional<Error> lastStoresException = std::nullopt;

  for (const auto& sub : subs) {
    trace("trying next substituter");
    if (lastStoresException.has_value()) {
      logError(lastStoresException->info());
      lastStoresException.reset();
    }

    cleanup();

    /* The path the substituter refers to the path as. This will be
     * different when the stores have different names. */
    std::optional<store_path_t> subPath;

    /* Path info returned by the substituter's query info operation. */
    std::shared_ptr<const valid_path_info_t> info;

    if (ca) {
      subPath = sub->makeFixedOutputPathFromCA(std::string{store_path.name()},
                                               ContentAddressWithReferences::withoutRefs(*ca));
      if (sub->store_dir == worker.store.store_dir)
        assert(subPath == store_path);
    } else if (sub->store_dir != worker.store.store_dir) {
      continue;
    }

    try {
      // FIXME: make async
      info = sub->queryPathInfo(subPath ? *subPath : store_path);
    } catch (InvalidPath& e) {
      continue;
    } catch (SubstituterDisabled& e) {
      continue;
    } catch (Error& e) {
      lastStoresException = std::make_optional(std::move(e));
      continue;
    }

    if (info->path != store_path) {
      if (info->isContentAddressed(*sub) && info->references.empty()) {
        auto info2 = std::make_shared<valid_path_info_t>(*info);
        info2->path = store_path;
        info = info2;
      } else {
        printError("asked '%s' for '%s' but got '%s'", sub->config.getHumanReadableURI(),
                   worker.store.printStorePath(store_path), sub->printStorePath(info->path));
        continue;
      }
    }

    /* Update the total expected download size. */
    auto narInfo = std::dynamic_pointer_cast<const nar_info_t>(info);

    maintainExpectedNar =
        std::make_unique<maintain_count_t<uint64_t>>(worker.expectedNarSize, info->nar_size);

    maintainExpectedDownload = narInfo && narInfo->file_size
                                   ? std::make_unique<maintain_count_t<uint64_t>>(
                                         worker.expectedDownloadSize, narInfo->file_size)
                                   : nullptr;

    worker.updateProgress();

    /* Bail out early if this substituter lacks a valid
       signature. LocalStore::add_to_store() also checks for this, but
       only after we've downloaded the path. */
    if (!sub->config.isTrusted && worker.store.pathInfoIsUntrusted(*info)) {
      warn("ignoring substitute for '%s' from '%s', as it's not signed by any of the keys in "
           "'trusted-public-keys'",
           worker.store.printStorePath(store_path), sub->config.getHumanReadableURI());
      continue;
    }

    Goals waitees;

    /* To maintain the closure invariant, we first have to realise the
       paths referenced by this one. */
    for (auto& i : info->references)
      if (i != store_path) /* ignore self-references */
        waitees.insert(worker.makePathSubstitutionGoal(i));

    co_await await(std::move(waitees));

    // FIXME: consider returning boolean instead of passing in reference
    bool out = false; // is mutated by tryToRun
    co_await tryToRun(subPath ? *subPath : store_path, sub, info, out);
    substituterFailed = substituterFailed || out;
  }

  /* None left.  Terminate this goal and let someone else deal
     with it. */

  if (substituterFailed) {
    worker.failedSubstitutions++;
    worker.updateProgress();
  }
  if (lastStoresException.has_value()) {
    if (!settings.try_fallback) {
      throw *lastStoresException;
    } else
      logError(lastStoresException->info());
  }

  /* Hack: don't indicate failure if there were no substituters.
     In that case the calling derivation should just do a
     build. */
  co_return doneFailure(substituterFailed ? ecFailed : ecNoSubstituters,
                        build_result_t::Failure::NoSubstituters,
                        fmt("path '%s' is required, but there is no substituter that can build it",
                            worker.store.printStorePath(store_path)));
}

Goal::Co PathSubstitutionGoal::tryToRun(store_path_t subPath, nix::ref<store_t> sub,
                                        std::shared_ptr<const valid_path_info_t> info,
                                        bool& substituterFailed) {
  trace("all references realised");

  if (nrFailed > 0) {
    co_return doneFailure(nrNoSubstituters > 0 ? ecNoSubstituters : ecFailed,
                          build_result_t::Failure::DependencyFailed,
                          fmt("some references of path '%s' could not be realised",
                              worker.store.printStorePath(store_path)));
  }

  for (auto& i : info->references)
    /* ignore self-references */
    if (i != store_path) {
      if (!worker.store.isValidPath(i)) {
        throw Error("reference '%s' of path '%s' is not a valid path",
                    worker.store.printStorePath(i), worker.store.printStorePath(store_path));
      }
    }

  co_await yield();

  trace("trying to run");

  /* Make sure that we are allowed to start a substitution.  Note that even
     if maxSubstitutionJobs == 0, we still allow a substituter to run. This
     prevents infinite waiting. */
  while (worker.getNrSubstitutions() >= std::max(1U, (unsigned int)settings.maxSubstitutionJobs)) {
    co_await waitForBuildSlot();
  }

  auto maintainRunningSubstitutions =
      std::make_unique<maintain_count_t<uint64_t>>(worker.runningSubstitutions);
  worker.updateProgress();

#ifndef _WIN32
  outPipe.create();
#else
  outPipe.createAsyncPipe(worker.ioport.get());
#endif

  auto promise = std::promise<void>();

  thr = std::thread([this, &promise, &subPath, &sub]() {
    try {
      receive_interrupts_t receive_interrupts;

      /* Wake up the worker loop when we're done. */
      finally_t updateStats([this]() { outPipe.write_side.close(); });

      logger_t::fields_t fields;
      fields.push_back(logger_t::field_t(worker.store.printStorePath(store_path)));
      fields.push_back(logger_t::field_t(sub->config.getHumanReadableURI()));
      activity_t act(*logger, act_substitute, fields);
      push_activity_t pact(act.id_);

      copy_store_path(*sub, worker.store, subPath, repair,
                      sub->config.isTrusted ? NoCheckSigs : CheckSigs);

      promise.set_value();
    } catch (...) {
      promise.set_exception(std::current_exception());
    }
  });

  worker.childStarted(shared_from_this(),
                      {
#ifndef _WIN32
                          outPipe.read_side.get()
#else
                          &outPipe
#endif
                      },
                      true, false);

  co_await Suspend{};

  trace("substitute finished");

  thr.join();
  worker.childTerminated(this);

  try {
    promise.get_future().get();
  } catch (std::exception& e) {
    /* Cause the parent build to fail unless --fallback is given,
       or the substitute has disappeared. The latter case behaves
       the same as the substitute never having existed in the
       first place. */
    try {
      throw;
    } catch (SubstituteGone& sg) {
      /* Missing NARs are expected when they've been garbage collected.
         This is not a failure, so log as a warning instead of an error. */
      logWarning({.msg = sg.info().msg});
    } catch (...) {
      printError(e.what());
      substituterFailed = true;
    }

    co_return Return{};
  }

  worker.markContentsGood(store_path);

  printMsg(lvl_chatty, "substitution of path '%s' succeeded",
           worker.store.printStorePath(store_path));

  maintainRunningSubstitutions.reset();

  maintainExpectedSubstitutions.reset();
  worker.doneSubstitutions++;

  if (maintainExpectedDownload) {
    auto file_size = maintainExpectedDownload->delta;
    maintainExpectedDownload.reset();
    worker.doneDownloadSize += file_size;
  }

  assert(maintainExpectedNar);
  worker.doneNarSize += maintainExpectedNar->delta;
  maintainExpectedNar.reset();

  worker.updateProgress();

  co_return doneSuccess(build_result_t::Success::Substituted);
}

void PathSubstitutionGoal::handle_eof(descriptor_t fd) {
  worker.wakeUp(shared_from_this());
}

void PathSubstitutionGoal::cleanup() {
  try {
    if (thr.joinable()) {
      // FIXME: signal worker thread to quit.
      thr.join();
      worker.childTerminated(this);
    }

    outPipe.close();
  } catch (...) {
    ignore_exception_in_destructor();
  }
}

} // namespace nix
