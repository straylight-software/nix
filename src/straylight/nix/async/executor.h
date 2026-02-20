// straylight::nix::async::executor
//
// Work-stealing thread pool executor built on taskflow.
//
// Features:
// - Work-stealing scheduler (better than FIFO for unbalanced workloads)
// - Boehm GC integration via WorkerInterface
// - Configurable thread count
// - Exception propagation
//
// Usage:
//   Executor exec(4);  // 4 worker threads
//   exec.async([] { do_work(); });
//   exec.wait_for_all();
//
// For Boehm GC environments (NIX_USE_BOEHMGC):
//   GcExecutor exec(4);  // Registers worker threads with GC

#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <taskflow/taskflow.hpp>

// Optional Boehm GC support
#if defined(NIX_USE_BOEHMGC) && NIX_USE_BOEHMGC
#  include <gc.h>
#  define STRAYLIGHT_HAS_BOEHM_GC 1
#else
#  define STRAYLIGHT_HAS_BOEHM_GC 0
#endif

namespace straylight::nix::async {

// ─────────────────────────────────────────────────────────────────────────────
// Worker interface for Boehm GC integration
// ─────────────────────────────────────────────────────────────────────────────

#if STRAYLIGHT_HAS_BOEHM_GC

/// Worker interface that registers/unregisters threads with Boehm GC.
/// This is critical for parallel Nix evaluation - GC must know about all threads.
class GcWorkerInterface : public tf::WorkerInterface {
public:
  void scheduler_prologue(tf::Worker& worker) override {
    GC_stack_base sb;
    GC_get_stack_base(&sb);
    GC_register_my_thread(&sb);
  }

  void scheduler_epilogue(tf::Worker& worker, std::exception_ptr ptr) override {
    GC_unregister_my_thread();
  }
};

#endif // STRAYLIGHT_HAS_BOEHM_GC

// ─────────────────────────────────────────────────────────────────────────────
// Executor - work-stealing thread pool
// ─────────────────────────────────────────────────────────────────────────────

/// Work-stealing executor backed by taskflow.
/// For GC-aware execution, use GcExecutor instead.
class Executor {
public:
  /// Create executor with specified number of threads.
  /// If num_threads == 0, uses hardware_concurrency.
  explicit Executor(std::size_t num_threads = 0)
      : executor_(num_threads == 0 ? std::thread::hardware_concurrency() : num_threads) {}

  /// Non-copyable, non-movable (contains std::atomic and complex state)
  Executor(const Executor&) = delete;
  Executor& operator=(const Executor&) = delete;
  Executor(Executor&&) = delete;
  Executor& operator=(Executor&&) = delete;

  ~Executor() {
    // Wait for all pending work to complete
    executor_.wait_for_all();
  }

  /// Number of worker threads
  [[nodiscard]] std::size_t num_workers() const noexcept { return executor_.num_workers(); }

  /// Number of tasks currently in the queue
  [[nodiscard]] std::size_t num_topologies() const noexcept { return executor_.num_topologies(); }

  /// Submit a task and get a future for the result.
  template <typename F>
  [[nodiscard]] auto async(F&& func) -> std::future<std::invoke_result_t<F>> {
    return executor_.async(std::forward<F>(func));
  }

  /// Submit a task without caring about the result.
  template <typename F>
  void silent_async(F&& func) {
    executor_.silent_async(std::forward<F>(func));
  }

  /// Submit multiple tasks and get futures for all results.
  template <typename F>
  [[nodiscard]] auto async_batch(std::vector<F>&& funcs)
      -> std::vector<std::future<std::invoke_result_t<F>>> {
    std::vector<std::future<std::invoke_result_t<F>>> futures;
    futures.reserve(funcs.size());
    for (auto& f : funcs) {
      futures.push_back(executor_.async(std::move(f)));
    }
    return futures;
  }

  /// Run a taskflow graph (for DAG execution).
  std::future<void> run(tf::Taskflow& taskflow) { return executor_.run(taskflow); }

  /// Run a taskflow graph and wait for completion.
  void run_and_wait(tf::Taskflow& taskflow) { executor_.run(taskflow).wait(); }

  /// Wait for all submitted tasks to complete.
  void wait_for_all() { executor_.wait_for_all(); }

  /// Access the underlying tf::Executor (for advanced use).
  [[nodiscard]] tf::Executor& underlying() noexcept { return executor_; }
  [[nodiscard]] const tf::Executor& underlying() const noexcept { return executor_; }

protected:
  // Protected constructor for derived classes that want custom worker interface
  explicit Executor(std::size_t num_threads, std::unique_ptr<tf::WorkerInterface> worker_interface)
      : executor_(num_threads == 0 ? std::thread::hardware_concurrency() : num_threads,
                  std::move(worker_interface)) {}

  tf::Executor executor_;
};

// ─────────────────────────────────────────────────────────────────────────────
// GcExecutor - executor with Boehm GC integration
// ─────────────────────────────────────────────────────────────────────────────

#if STRAYLIGHT_HAS_BOEHM_GC

/// Executor that registers all worker threads with Boehm GC.
/// Use this for parallel Nix evaluation where GC must track all threads.
class GcExecutor : public Executor {
public:
  explicit GcExecutor(std::size_t num_threads = 0)
      : Executor(num_threads == 0 ? std::thread::hardware_concurrency() : num_threads,
                 std::make_unique<GcWorkerInterface>()) {}
};

#else

// When Boehm GC is not available, GcExecutor is just an alias
using GcExecutor = Executor;

#endif // STRAYLIGHT_HAS_BOEHM_GC

// ─────────────────────────────────────────────────────────────────────────────
// Global executor (optional singleton pattern)
// ─────────────────────────────────────────────────────────────────────────────

/// Get a shared global executor instance.
/// Thread-safe initialization (C++11 magic statics).
/// Uses hardware_concurrency threads.
[[nodiscard]] inline Executor& global_executor() {
  static Executor instance;
  return instance;
}

#if STRAYLIGHT_HAS_BOEHM_GC
/// Get a shared global GC-aware executor instance.
[[nodiscard]] inline GcExecutor& global_gc_executor() {
  static GcExecutor instance;
  return instance;
}
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Utility: run multiple functions in parallel and collect results
// ─────────────────────────────────────────────────────────────────────────────

/// Execute functions in parallel and return their results.
/// Blocks until all functions complete.
template <typename F>
[[nodiscard]] auto parallel_invoke(Executor& exec, std::vector<F>&& funcs)
    -> std::vector<std::invoke_result_t<F>> {
  using R = std::invoke_result_t<F>;

  auto futures = exec.async_batch(std::move(funcs));

  std::vector<R> results;
  results.reserve(futures.size());
  for (auto& f : futures) {
    results.push_back(f.get());
  }
  return results;
}

/// Execute functions that return void in parallel.
/// Blocks until all complete. Propagates first exception.
template <typename F>
  requires std::is_void_v<std::invoke_result_t<F>>
void parallel_invoke(Executor& exec, std::vector<F>&& funcs) {
  auto futures = exec.async_batch(std::move(funcs));

  std::exception_ptr first_exception;
  for (auto& f : futures) {
    try {
      f.get();
    } catch (...) {
      if (!first_exception) {
        first_exception = std::current_exception();
      }
    }
  }
  if (first_exception) {
    std::rethrow_exception(first_exception);
  }
}

} // namespace straylight::nix::async
