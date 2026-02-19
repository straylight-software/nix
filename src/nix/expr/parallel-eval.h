#pragma once

#include <functional>
#include <future>
#include <queue>
#include <random>

#include <boost/thread/thread.hpp>

#include "nix/util/environment-variables.h"
#include "nix/util/logging.h"
#include "nix/util/signals.h"
#include "nix/util/sync.h"
#include "nix/util/util.h"

#if NIX_USE_BOEHMGC
#  include <gc.h>
#endif

namespace nix {

// Forward declaration
struct EvalSettings;

struct Executor {
  using work_t = std::function<void()>;

  struct Item {
    std::promise<void> promise;
    work_t work;
  };

  struct State {
    std::multimap<uint64_t, Item> queue;
    std::vector<boost::thread> threads;
  };

  std::atomic_bool quit{false};

  const unsigned int evalCores;

  const bool enabled;

  const std::unique_ptr<interrupt_callback_t> interruptCallback;

  sync_t<State> state_;

  std::condition_variable wakeup;

  static unsigned int getEvalCores(const EvalSettings& eval_settings);

  Executor(const EvalSettings& eval_settings);

  ~Executor();

  void createWorker(State& state);

  void worker();

  std::vector<std::future<void>> spawn(std::vector<std::pair<work_t, uint8_t>>&& items);

  static thread_local bool amWorkerThread;
};

struct FutureVector {
  Executor& executor;

  struct State {
    std::vector<std::future<void>> futures;
  };

  sync_t<State> state_;

  ~FutureVector();

  // FIXME: add a destructor that cancels/waits for all futures.

  void spawn(std::vector<std::pair<Executor::work_t, uint8_t>>&& work);

  void spawn(uint8_t prioPrefix, Executor::work_t&& work) {
    spawn({{std::move(work), prioPrefix}});
  }

  void finishAll();
};

} // namespace nix
