#pragma once
///@file

#include <future>
#include <set>

#include "nix/util/sync.h"

using std::set;

namespace nix {

template <typename T>
using get_edges_async_t = std::function<void(const T&, std::function<void(std::promise<set<T>>&)>)>;

template <typename T>
auto compute_closure(const set<T> start_elts, set<T>& res, get_edges_async_t<T> get_edges_async)
    -> void {
  struct state_t {
    size_t pending = 0;
    set<T>& res;
    std::exception_ptr exc;
  };

  sync_t<state_t> state_sync(state_t{0, res, {}});

  std::condition_variable done;

  auto enqueue = [&](this auto& enqueue, const T& current) -> void {
    {
      auto state(state_sync.lock());
      if (state->exc) {
        return;
      }
      if (!state->res.insert(current).second) {
        return;
      }
      state->pending++;
    }

    get_edges_async(current, [&](std::promise<set<T>>& prom) {
      try {
        auto children = prom.get_future().get();
        for (auto& child : children) {
          enqueue(child);
        }
        {
          auto state(state_sync.lock());
          assert(state->pending);
          if (!--state->pending) {
            done.notify_one();
          }
        }
      } catch (...) {
        auto state(state_sync.lock());
        if (!state->exc) {
          state->exc = std::current_exception();
        }
        assert(state->pending);
        if (!--state->pending) {
          done.notify_one();
        }
      };
    });
  };

  for (auto& start_elt : start_elts) {
    enqueue(start_elt);
  }

  {
    auto state(state_sync.lock());
    while (state->pending) {
      state.wait(done);
    }
    if (state->exc) {
      std::rethrow_exception(state->exc);
    }
  }
}

} // namespace nix
