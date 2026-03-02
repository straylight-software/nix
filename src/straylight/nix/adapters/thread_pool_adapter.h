// straylight::nix::primitives::adapters::thread_pool_adapter
//
// Compatibility shim providing Nix's ThreadPool interface backed by
// straylight::nix::primitives::async::Executor.
//
// This adapter allows incremental migration of Nix code from the legacy
// ThreadPool to the new work-stealing executor without requiring a
// big-bang rewrite.
//
// Key differences from original ThreadPool:
// - Work-stealing instead of FIFO queue (better for unbalanced workloads)
// - Uses taskflow under the hood
// - Built-in Boehm GC thread registration via GcExecutor
//
// Usage (drop-in replacement):
//   // Before:
//   // #include "nix/util/thread-pool.h"
//   // nix::ThreadPool pool(4);
//
//   // After:
//   #include "straylight/nix/adapters/thread_pool_adapter.h"
//   straylight::nix::primitives::adapters::ThreadPool pool(4);
//
// For GC-aware execution:
//   straylight::nix::primitives::adapters::GcThreadPool pool(4);

#pragma once

#include <atomic>
#include <concepts>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

#include "straylight/nix/async/executor.h"
#include "straylight/nix/async/task_graph.h"

namespace straylight::nix::adapters {

// ─────────────────────────────────────────────────────────────────────────────
// ThreadPoolShutDown exception (compatible with nix::ThreadPoolShutDown)
// ─────────────────────────────────────────────────────────────────────────────

/// Exception thrown when enqueueing to a shut down pool.
class ThreadPoolShutDown : public std::runtime_error {
public:
  explicit ThreadPoolShutDown(const char* msg) : std::runtime_error(msg) {}
  explicit ThreadPoolShutDown(const std::string& msg) : std::runtime_error(msg) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// ThreadPool - compatibility shim over Executor
// ─────────────────────────────────────────────────────────────────────────────

/// A ThreadPool compatible adapter backed by straylight's Executor.
///
/// This provides the same interface as nix::ThreadPool:
/// - enqueue(work_t): submit work
/// - process(): run until all work complete
/// - shutdown(): stop accepting work
///
/// Key behavioral differences:
/// 1. Uses work-stealing instead of FIFO (may change execution order)
/// 2. Worker threads are created eagerly, not lazily
/// 3. Exception handling is slightly different (first exception wins)
///
/// For most Nix use cases, these differences should not affect correctness.
template <typename ExecutorType = async::Executor>
class BasicThreadPool {
public:
  using work_t = std::function<void()>;

  /// Create thread pool with specified number of threads.
  /// If maxThreads == 0, uses hardware_concurrency.
  explicit BasicThreadPool(std::size_t maxThreads = 0)
      : executor_(maxThreads == 0 ? std::thread::hardware_concurrency() : maxThreads),
        shutdown_(false),
        draining_(false),
        pending_count_(0),
        active_count_(0) {}

  /// Non-copyable
  BasicThreadPool(const BasicThreadPool&) = delete;
  BasicThreadPool& operator=(const BasicThreadPool&) = delete;

  /// Destructor waits for all work to complete
  ~BasicThreadPool() { shutdown(); }

  /// Enqueue a work item.
  /// Throws ThreadPoolShutDown if the pool is shutting down.
  void enqueue(work_t task) {
    {
      std::lock_guard lock(mutex_);
      if (shutdown_.load(std::memory_order_acquire)) {
        throw ThreadPoolShutDown(
            "cannot enqueue a work item while the thread pool is shutting down");
      }
      pending_count_++;
    }

    executor_.silent_async([this, task = std::move(task)]() mutable {
      {
        std::lock_guard lock(mutex_);
        pending_count_--;
        active_count_++;
      }

      std::exception_ptr exc;
      try {
        task();
      } catch (...) {
        exc = std::current_exception();
      }

      {
        std::lock_guard lock(mutex_);
        active_count_--;

        if (exc) {
          if (!exception_) {
            exception_ = exc;
            // Signal shutdown on first exception
            shutdown_.store(true, std::memory_order_release);
          }
          // Note: Unlike original ThreadPool, we don't print secondary exceptions.
          // The original does this for debugging, but it's not essential behavior.
        }

        // Wake up process() if we're draining and this was the last task
        if (draining_ && pending_count_ == 0 && active_count_ == 0) {
          done_cv_.notify_all();
        }
      }
    });
  }

  /// Execute work items until the queue is empty.
  /// Propagates the first exception thrown by any work item.
  void process() {
    {
      std::lock_guard lock(mutex_);
      draining_ = true;
    }

    // Wait for all pending work to complete
    {
      std::unique_lock lock(mutex_);
      done_cv_.wait(lock, [this] {
        return (pending_count_ == 0 && active_count_ == 0) ||
               shutdown_.load(std::memory_order_acquire);
      });
    }

    // Wait for executor to finish any in-flight tasks
    executor_.wait_for_all();

    // Check for exceptions
    std::exception_ptr exc;
    {
      std::lock_guard lock(mutex_);
      exc = exception_;
      // Mark as shutdown after process completes
      shutdown_.store(true, std::memory_order_release);
    }

    if (exc) {
      std::rethrow_exception(exc);
    }
  }

  /// Shut down all worker threads.
  /// Active work items are finished, pending items may be discarded.
  void shutdown() {
    {
      std::lock_guard lock(mutex_);
      shutdown_.store(true, std::memory_order_release);
      done_cv_.notify_all();
    }
    executor_.wait_for_all();
  }

  /// Number of worker threads
  [[nodiscard]] std::size_t num_workers() const noexcept { return executor_.num_workers(); }

  /// Access the underlying executor (for advanced use)
  [[nodiscard]] ExecutorType& executor() noexcept { return executor_; }
  [[nodiscard]] const ExecutorType& executor() const noexcept { return executor_; }

private:
  ExecutorType executor_;
  std::atomic<bool> shutdown_;
  bool draining_;

  mutable std::mutex mutex_;
  std::condition_variable done_cv_;

  std::size_t pending_count_;
  std::size_t active_count_;
  std::exception_ptr exception_;
};

/// Standard ThreadPool using the default Executor
using ThreadPool = BasicThreadPool<async::Executor>;

/// GC-aware ThreadPool using GcExecutor
/// Use this when processing Nix values that may trigger GC.
using GcThreadPool = BasicThreadPool<async::GcExecutor>;

// ─────────────────────────────────────────────────────────────────────────────
// processGraph - compatibility wrapper
// ─────────────────────────────────────────────────────────────────────────────

/// Process in parallel a set of items of type T that have a partial
/// ordering between them. Drop-in replacement for nix::processGraph.
///
/// This is a thin wrapper that delegates to straylight's process_graph.
///
/// @param nodes          Initial set of nodes to process
/// @param getEdges       Function returning dependencies for a node
/// @param processNode    Function to process a node
/// @param discoverNodes  If true, new nodes from getEdges are added to graph
/// @param maxThreads     Number of threads (0 = hardware_concurrency)
template <typename T>
  requires std::totally_ordered<T>
void processGraph(const std::set<T>& nodes, std::function<std::set<T>(const T&)> getEdges,
                  std::function<void(const T&)> processNode, bool discoverNodes = false,
                  std::size_t maxThreads = 0) {
  if (nodes.empty()) {
    return;
  }

  // Create a temporary executor with the specified thread count
  async::Executor exec(maxThreads);

  // Delegate to the new process_graph implementation
  async::process_graph<T>(nodes, std::move(getEdges), std::move(processNode), exec, discoverNodes);
}

/// GC-aware version of processGraph.
/// Use when processing Nix store paths or other GC-managed objects.
template <typename T>
  requires std::totally_ordered<T>
void processGraphGc(const std::set<T>& nodes, std::function<std::set<T>(const T&)> getEdges,
                    std::function<void(const T&)> processNode, bool discoverNodes = false,
                    std::size_t maxThreads = 0) {
  if (nodes.empty()) {
    return;
  }

  async::GcExecutor exec(maxThreads);
  async::process_graph<T>(nodes, std::move(getEdges), std::move(processNode), exec, discoverNodes);
}

// ─────────────────────────────────────────────────────────────────────────────
// Namespace alias for easier migration
// ─────────────────────────────────────────────────────────────────────────────

} // namespace straylight::nix::adapters

// Optional: Allow using the adapter in the nix namespace for minimal changes
// Uncomment if you want to use `nix::ThreadPool` directly:
//
// namespace nix {
//   using ThreadPool = straylight::nix::primitives::adapters::ThreadPool;
//   using ThreadPoolShutDown = straylight::nix::primitives::adapters::ThreadPoolShutDown;
//
//   template <typename T>
//   void processGraph(const std::set<T>& nodes,
//                     std::function<std::set<T>(const T&)> getEdges,
//                     std::function<void(const T&)> processNode,
//                     bool discoverNodes = false,
//                     std::size_t maxThreads = 0) {
//     straylight::nix::primitives::adapters::processGraph<T>(
//         nodes, std::move(getEdges), std::move(processNode), discoverNodes, maxThreads);
//   }
// }
