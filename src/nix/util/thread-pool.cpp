#include "nix/util/thread-pool.h"

#include "nix/util/signals.h"
#include "nix/util/util.h"

namespace nix {

thread_pool_t::thread_pool_t(size_t _maxThreads) : max_threads(_maxThreads) {
  if (!max_threads) {
    max_threads = std::thread::hardware_concurrency();
    if (!max_threads) {
      max_threads = 1;
}
  }

  debug("starting pool of %d threads", max_threads - 1);
}

thread_pool_t::~thread_pool_t() {
  shutdown();
}

void thread_pool_t::shutdown() {
  std::vector<std::thread> workers;
  {
    auto state(state_.lock());
    quit = true;
    std::swap(workers, state->workers);
  }

  if (workers.empty()) {
    return;
}

  debug("reaping %d worker threads", workers.size());

  work.notify_all();

  for (auto& thr : workers) {
    thr.join();
}
}

void thread_pool_t::enqueue(work_t t) {
  auto state(state_.lock());
  if (quit) {
    throw ThreadPoolShutDown("cannot enqueue a work item while the thread pool is shutting down");
}
  state->pending.push(std::move(t));
  /* Note: process() also executes items, so count it as a worker. */
  if (state->pending.size() > state->workers.size() + 1 && state->workers.size() + 1 < max_threads) {
    state->workers.emplace_back(&thread_pool_t::do_work, this, false);
}
  work.notify_one();
}

void thread_pool_t::process() {
  state_.lock()->draining = true;

  /* Do work until no more work is pending or active. */
  try {
    do_work(true);

    auto state(state_.lock());

    assert(quit);

    if (state->exception) {
      std::rethrow_exception(state->exception);
}

  } catch (...) {
    /* In the exceptional case, some workers may still be
       active. They may be referencing the stack frame of the
       caller. So wait for them to finish. (~thread_pool_t also does
       this, but it might be destroyed after objects referenced by
       the work item lambdas.) */
    shutdown();
    throw;
  }
}

void thread_pool_t::do_work(bool main_thread) {
  receive_interrupts_t receive_interrupts;

#ifndef _WIN32 // Does Windows need anything similar for async exit handling?
  if (!main_thread) {
    unix::interrupt_check = [&]() { return (bool)quit; };
}
#endif

  bool did_work = false;
  std::exception_ptr exc;

  while (true) {
    work_t w;
    {
      auto state(state_.lock());

      if (did_work) {
        assert(state->active);
        state->active--;

        if (exc) {
          if (!state->exception) {
            state->exception = exc;
            // Tell the other workers to quit.
            quit = true;
            work.notify_all();
          } else {
            /* Print the exception, since we can't
               propagate it. */
            try {
              std::rethrow_exception(exc);
            } catch (const Interrupted&) {
              // The interrupted state may be picked up by multiple
              // workers, which is expected, so we should ignore
              // it silently and let the first one bubble up,
              // rethrown via the original state->exception.
            } catch (const ThreadPoolShutDown&) {
              // Similarly expected.
            } catch (std::exception& e) {
              ignore_exception_except_interrupt();
            }
          }
        }
      }

      /* Wait until a work item is available or we're asked to
         quit. */
      while (true) {
        if (quit) {
          return;
}

        if (!state->pending.empty()) {
          break;
}

        /* If there are no active or pending items, and the
           main thread is running process(), then no new items
           can be added. So exit. */
        if (!state->active && state->draining) {
          quit = true;
          work.notify_all();
          return;
        }

        state.wait(work);
      }

      w = std::move(state->pending.front());
      state->pending.pop();
      state->active++;
    }

    try {
      w();
    } catch (...) {
      exc = std::current_exception();
    }

    did_work = true;
  }
}

} // namespace nix
