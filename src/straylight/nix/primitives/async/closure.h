// straylight::nix::primitives::async::closure
//
// Transitive closure computation for dependency graphs.
// Replaces nix/util/closure.h with a modern, efficient implementation.
//
// Features:
// - Sequential and parallel variants
// - BFS-based traversal with visited tracking
// - Cycle detection (optional)
// - Progress callback support
// - Early termination (e.g., found target item)
//
// Usage:
//   // Sequential closure
//   auto deps = computeClosure(
//       {start_node},
//       [](const T& item) { return get_dependencies(item); }
//   );
//
//   // Parallel closure with executor
//   auto deps = computeClosureAsync(
//       executor,
//       {start_node},
//       [](const T& item) { return get_dependencies(item); }
//   );
//
//   // With early termination
//   auto deps = computeClosure(
//       {start_node},
//       get_deps,
//       ClosureOptions<T>{}.withEarlyTermination([](const T& item) {
//           return item == target;
//       })
//   );

#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "straylight/nix/primitives/async/executor.h"

namespace straylight::nix::primitives::async {

// ─────────────────────────────────────────────────────────────────────────────
// Error types
// ─────────────────────────────────────────────────────────────────────────────

/// Error type for closure computation failures
enum class ClosureErrorKind {
  CycleDetected,
  Cancelled,
  DependencyError,
};

/// Error returned when closure computation fails
struct ClosureError {
  ClosureErrorKind kind;
  std::string message;
  std::optional<std::string> item; // The item that caused the error

  [[nodiscard]] bool isCycle() const noexcept { return kind == ClosureErrorKind::CycleDetected; }
  [[nodiscard]] bool isCancelled() const noexcept { return kind == ClosureErrorKind::Cancelled; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Result types
// ─────────────────────────────────────────────────────────────────────────────

/// Result of a closure computation
template <typename T>
struct ClosureResult {
  std::unordered_set<T> items;
  bool earlyTerminated = false;
  std::optional<T> terminatingItem; // Item that triggered early termination
};

// ─────────────────────────────────────────────────────────────────────────────
// Options for closure computation
// ─────────────────────────────────────────────────────────────────────────────

/// Configuration options for closure computation
template <typename T>
struct ClosureOptions {
  /// Progress callback: called for each newly discovered item
  /// If it returns false, computation is cancelled
  std::function<bool(const T&, std::size_t current_size)> onProgress;

  /// Early termination predicate: if returns true, stop and return current result
  std::function<bool(const T&)> earlyTerminate;

  /// Whether to detect cycles (adds overhead)
  bool detectCycles = false;

  /// Maximum depth to traverse (0 = unlimited)
  std::size_t maxDepth = 0;

  /// Maximum items to collect (0 = unlimited)
  std::size_t maxItems = 0;

  // Builder-style setters
  ClosureOptions& withProgress(std::function<bool(const T&, std::size_t)> cb) {
    onProgress = std::move(cb);
    return *this;
  }

  ClosureOptions& withEarlyTermination(std::function<bool(const T&)> pred) {
    earlyTerminate = std::move(pred);
    return *this;
  }

  ClosureOptions& withCycleDetection(bool enable = true) {
    detectCycles = enable;
    return *this;
  }

  ClosureOptions& withMaxDepth(std::size_t depth) {
    maxDepth = depth;
    return *this;
  }

  ClosureOptions& withMaxItems(std::size_t count) {
    maxItems = count;
    return *this;
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// Concepts
// ─────────────────────────────────────────────────────────────────────────────

/// Concept for types that can be used as closure items
template <typename T>
concept ClosureItem = std::equality_comparable<T> && std::movable<T> && requires(const T& t) {
  { std::hash<T>{}(t) } -> std::convertible_to<std::size_t>;
};

/// Concept for synchronous dependency functions
template <typename F, typename T>
concept DependencyFunction = requires(F f, const T& item) {
  { f(item) } -> std::convertible_to<std::set<T>>;
};

/// Concept for async dependency functions
template <typename F, typename T>
concept AsyncDependencyFunction = requires(F f, const T& item) {
  { f(item) } -> std::same_as<std::future<std::set<T>>>;
};

// ─────────────────────────────────────────────────────────────────────────────
// Sequential closure computation
// ─────────────────────────────────────────────────────────────────────────────

/// Compute the transitive closure of a set of items using BFS traversal.
///
/// @tparam T          The item type (must be hashable and equality comparable)
/// @param startItems  Initial items to start the traversal
/// @param getDeps     Function returning the dependencies of an item
/// @param options     Configuration options
/// @return            Result containing all transitively reachable items, or error
template <ClosureItem T, DependencyFunction<T> F>
[[nodiscard]] auto computeClosure(const std::set<T>& startItems, F&& getDeps,
                                  ClosureOptions<T> options = {})
    -> std::expected<ClosureResult<T>, ClosureError> {
  ClosureResult<T> result;

  if (startItems.empty()) {
    return result;
  }

  // BFS queue with (item, depth) pairs
  std::queue<std::pair<T, std::size_t>> queue;

  // Initialize with starting items
  for (const auto& item : startItems) {
    if (result.items.insert(item).second) {
      queue.emplace(item, 0);

      // Check early termination
      if (options.earlyTerminate && options.earlyTerminate(item)) {
        result.earlyTerminated = true;
        result.terminatingItem = item;
        return result;
      }

      // Check max items
      if (options.maxItems > 0 && result.items.size() >= options.maxItems) {
        return result;
      }

      // Progress callback
      if (options.onProgress && !options.onProgress(item, result.items.size())) {
        return std::unexpected(
            ClosureError{ClosureErrorKind::Cancelled, "cancelled by progress callback", {}});
      }
    }
  }

  // Track path for cycle detection (only if enabled)
  std::unordered_set<T> currentPath;

  // BFS traversal
  while (!queue.empty()) {
    auto [current, depth] = std::move(queue.front());
    queue.pop();

    // Check depth limit
    if (options.maxDepth > 0 && depth >= options.maxDepth) {
      continue;
    }

    // Get dependencies
    std::set<T> deps;
    try {
      deps = getDeps(current);
    } catch (const std::exception& e) {
      return std::unexpected(ClosureError{
          ClosureErrorKind::DependencyError,
          std::string("error getting dependencies: ") + e.what(),
          std::nullopt,
      });
    }

    // Cycle detection (if enabled)
    if (options.detectCycles) {
      currentPath.insert(current);
    }

    for (const auto& dep : deps) {
      // Cycle detection
      if (options.detectCycles && currentPath.contains(dep)) {
        return std::unexpected(ClosureError{
            ClosureErrorKind::CycleDetected,
            "cycle detected in closure",
            std::nullopt,
        });
      }

      // Skip if already visited
      if (!result.items.insert(dep).second) {
        continue;
      }

      // Early termination
      if (options.earlyTerminate && options.earlyTerminate(dep)) {
        result.earlyTerminated = true;
        result.terminatingItem = dep;
        return result;
      }

      // Check max items
      if (options.maxItems > 0 && result.items.size() >= options.maxItems) {
        return result;
      }

      // Progress callback
      if (options.onProgress && !options.onProgress(dep, result.items.size())) {
        return std::unexpected(
            ClosureError{ClosureErrorKind::Cancelled, "cancelled by progress callback", {}});
      }

      queue.emplace(dep, depth + 1);
    }

    if (options.detectCycles) {
      currentPath.erase(current);
    }
  }

  return result;
}

/// Get just the set of items (throws on error)
template <ClosureItem T, DependencyFunction<T> F>
[[nodiscard]] std::unordered_set<T> computeClosureSet(const std::set<T>& startItems, F&& getDeps,
                                                      ClosureOptions<T> options = {}) {
  auto result = computeClosure(startItems, std::forward<F>(getDeps), std::move(options));
  if (!result) {
    throw std::runtime_error(result.error().message);
  }
  return std::move(result->items);
}

// ─────────────────────────────────────────────────────────────────────────────
// Parallel closure computation
// ─────────────────────────────────────────────────────────────────────────────

/// Compute the transitive closure in parallel using an executor.
///
/// Dependencies are fetched in parallel where possible. The algorithm
/// processes items level-by-level, fetching all dependencies for the
/// current frontier in parallel before moving to the next level.
///
/// @tparam T          The item type (must be hashable and equality comparable)
/// @param exec        Executor for parallel execution
/// @param startItems  Initial items to start the traversal
/// @param getDeps     Function returning the dependencies of an item (thread-safe)
/// @param options     Configuration options
/// @return            Result containing all transitively reachable items, or error
template <ClosureItem T, DependencyFunction<T> F>
[[nodiscard]] auto computeClosureAsync(Executor& exec, const std::set<T>& startItems, F&& getDeps,
                                       ClosureOptions<T> options = {})
    -> std::expected<ClosureResult<T>, ClosureError> {
  ClosureResult<T> result;

  if (startItems.empty()) {
    return result;
  }

  // Thread-safe state
  struct SharedState {
    std::mutex mutex;
    std::unordered_set<T> visited;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> earlyTerminated{false};
    std::optional<T> terminatingItem;
    std::exception_ptr exception;
    std::size_t depth = 0;
  };

  auto state = std::make_shared<SharedState>();

  // Initialize with starting items
  std::vector<T> currentFrontier;
  {
    std::lock_guard lock(state->mutex);
    for (const auto& item : startItems) {
      if (state->visited.insert(item).second) {
        currentFrontier.push_back(item);

        // Check early termination
        if (options.earlyTerminate && options.earlyTerminate(item)) {
          result.items = std::move(state->visited);
          result.earlyTerminated = true;
          result.terminatingItem = item;
          return result;
        }

        // Check max items
        if (options.maxItems > 0 && state->visited.size() >= options.maxItems) {
          result.items = std::move(state->visited);
          return result;
        }

        // Progress callback
        if (options.onProgress && !options.onProgress(item, state->visited.size())) {
          return std::unexpected(
              ClosureError{ClosureErrorKind::Cancelled, "cancelled by progress callback", {}});
        }
      }
    }
  }

  // Process level-by-level for better parallelism
  while (!currentFrontier.empty()) {
    // Check depth limit
    if (options.maxDepth > 0 && state->depth >= options.maxDepth) {
      break;
    }
    state->depth++;

    // Fetch dependencies for all items in current frontier in parallel
    std::vector<std::future<std::pair<T, std::set<T>>>> futures;
    futures.reserve(currentFrontier.size());

    for (const auto& item : currentFrontier) {
      futures.push_back(exec.async([&getDeps, item, state]() -> std::pair<T, std::set<T>> {
        if (state->cancelled.load()) {
          return {item, {}};
        }
        try {
          return {item, getDeps(item)};
        } catch (...) {
          std::lock_guard lock(state->mutex);
          if (!state->exception) {
            state->exception = std::current_exception();
          }
          state->cancelled = true;
          return {item, {}};
        }
      }));
    }

    // Collect results and build next frontier
    std::vector<T> nextFrontier;

    for (auto& future : futures) {
      auto [item, deps] = future.get();

      if (state->cancelled.load()) {
        continue;
      }

      std::lock_guard lock(state->mutex);

      for (const auto& dep : deps) {
        // Skip if already visited
        if (!state->visited.insert(dep).second) {
          continue;
        }

        // Early termination
        if (options.earlyTerminate && options.earlyTerminate(dep)) {
          state->earlyTerminated = true;
          state->terminatingItem = dep;
          state->cancelled = true;
          break;
        }

        // Check max items
        if (options.maxItems > 0 && state->visited.size() >= options.maxItems) {
          state->cancelled = true;
          break;
        }

        // Progress callback
        if (options.onProgress && !options.onProgress(dep, state->visited.size())) {
          state->cancelled = true;
          break;
        }

        nextFrontier.push_back(dep);
      }
    }

    // Check for exceptions
    if (state->exception) {
      try {
        std::rethrow_exception(state->exception);
      } catch (const std::exception& e) {
        return std::unexpected(ClosureError{
            ClosureErrorKind::DependencyError,
            std::string("error getting dependencies: ") + e.what(),
            std::nullopt,
        });
      }
    }

    // Check for early termination
    if (state->earlyTerminated.load()) {
      result.items = std::move(state->visited);
      result.earlyTerminated = true;
      result.terminatingItem = state->terminatingItem;
      return result;
    }

    // Check for cancellation
    if (state->cancelled.load()) {
      // If cancelled due to max items, return partial result
      if (options.maxItems > 0 && state->visited.size() >= options.maxItems) {
        result.items = std::move(state->visited);
        return result;
      }
      // Otherwise, it was a progress callback cancellation
      return std::unexpected(
          ClosureError{ClosureErrorKind::Cancelled, "cancelled by progress callback", {}});
    }

    currentFrontier = std::move(nextFrontier);
  }

  result.items = std::move(state->visited);
  return result;
}

/// Get just the set of items (throws on error)
template <ClosureItem T, DependencyFunction<T> F>
[[nodiscard]] std::unordered_set<T>
computeClosureSetAsync(Executor& exec, const std::set<T>& startItems, F&& getDeps,
                       ClosureOptions<T> options = {}) {
  auto result = computeClosureAsync(exec, startItems, std::forward<F>(getDeps), std::move(options));
  if (!result) {
    throw std::runtime_error(result.error().message);
  }
  return std::move(result->items);
}

// ─────────────────────────────────────────────────────────────────────────────
// Convenience overloads using global executor
// ─────────────────────────────────────────────────────────────────────────────

template <ClosureItem T, DependencyFunction<T> F>
[[nodiscard]] auto computeClosureAsync(const std::set<T>& startItems, F&& getDeps,
                                       ClosureOptions<T> options = {})
    -> std::expected<ClosureResult<T>, ClosureError> {
  return computeClosureAsync(global_executor(), startItems, std::forward<F>(getDeps),
                             std::move(options));
}

template <ClosureItem T, DependencyFunction<T> F>
[[nodiscard]] std::unordered_set<T>
computeClosureSetAsync(const std::set<T>& startItems, F&& getDeps, ClosureOptions<T> options = {}) {
  return computeClosureSetAsync(global_executor(), startItems, std::forward<F>(getDeps),
                                std::move(options));
}

} // namespace straylight::nix::primitives::async
