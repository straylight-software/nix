// straylight::nix::async::parallel
//
// Parallel algorithms built on taskflow.
//
// Features:
// - parallel_for: iterate over ranges in parallel
// - parallel_for_index: index-based parallel iteration
// - parallel_transform: parallel map operation
// - parallel_reduce: parallel reduction
// - parallel_filter: parallel filtering
//
// Usage:
//   std::vector<int> data = {1, 2, 3, 4, 5};
//   parallel_for(exec, data, [](int& x) { x *= 2; });
//
//   int sum = parallel_reduce(exec, data, 0, std::plus<>{});

#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <functional>
#include <iterator>
#include <mutex>
#include <numeric>
#include <ranges>
#include <type_traits>
#include <utility>
#include <vector>

#include <taskflow/algorithm/for_each.hpp>
#include <taskflow/algorithm/reduce.hpp>
#include <taskflow/algorithm/transform.hpp>
#include <taskflow/taskflow.hpp>

#include "executor.h"

namespace straylight::nix::async {

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

/// Default chunk size for parallel operations.
/// Smaller chunks = better load balancing, larger chunks = less overhead.
inline constexpr std::size_t kDefaultChunkSize = 1;

/// Minimum elements to justify parallel execution.
/// Below this threshold, sequential execution is used.
inline constexpr std::size_t kParallelThreshold = 16;

// ─────────────────────────────────────────────────────────────────────────────
// parallel_for - iterate over ranges
// ─────────────────────────────────────────────────────────────────────────────

/// Execute a function for each element in a range, in parallel.
/// Elements are processed in unspecified order.
template <std::ranges::range R, typename F>
  requires std::invocable<F, std::ranges::range_reference_t<R>>
void parallel_for(Executor& exec, R&& range, F&& func, std::size_t chunk_size = kDefaultChunkSize) {
  auto begin = std::ranges::begin(range);
  auto end = std::ranges::end(range);

  if (begin == end)
    return;

  auto size = std::ranges::distance(begin, end);
  if (static_cast<std::size_t>(size) < kParallelThreshold) {
    // Sequential fallback for small ranges
    std::for_each(begin, end, std::forward<F>(func));
    return;
  }

  tf::Taskflow taskflow;
  taskflow.for_each(begin, end, std::forward<F>(func));
  exec.run_and_wait(taskflow);
}

/// Execute a function for indices in [0, count), in parallel.
template <typename F>
  requires std::invocable<F, std::size_t>
void parallel_for_index(Executor& exec, std::size_t count, F&& func,
                        std::size_t chunk_size = kDefaultChunkSize) {
  if (count == 0)
    return;

  if (count < kParallelThreshold) {
    // Sequential fallback
    for (std::size_t i = 0; i < count; ++i) {
      func(i);
    }
    return;
  }

  tf::Taskflow taskflow;
  taskflow.for_each_index(std::size_t{0}, count, std::size_t{1}, std::forward<F>(func));
  exec.run_and_wait(taskflow);
}

// ─────────────────────────────────────────────────────────────────────────────
// parallel_transform - parallel map operation
// ─────────────────────────────────────────────────────────────────────────────

/// Transform elements from input range to output range in parallel.
/// Output range must have enough space.
template <std::ranges::input_range R, typename O, typename F>
  requires std::invocable<F, std::ranges::range_reference_t<R>>
void parallel_transform(Executor& exec, R&& input, O output, F&& func) {
  auto begin = std::ranges::begin(input);
  auto end = std::ranges::end(input);

  if (begin == end)
    return;

  auto size = std::ranges::distance(begin, end);
  if (static_cast<std::size_t>(size) < kParallelThreshold) {
    // Sequential fallback
    std::transform(begin, end, output, std::forward<F>(func));
    return;
  }

  tf::Taskflow taskflow;
  taskflow.transform(begin, end, output, std::forward<F>(func));
  exec.run_and_wait(taskflow);
}

/// Transform elements in-place in parallel.
template <std::ranges::range R, typename F>
  requires std::invocable<F, std::ranges::range_reference_t<R>>
void parallel_transform_inplace(Executor& exec, R&& range, F&& func) {
  auto begin = std::ranges::begin(range);
  auto end = std::ranges::end(range);

  if (begin == end)
    return;

  auto size = std::ranges::distance(begin, end);
  if (static_cast<std::size_t>(size) < kParallelThreshold) {
    // Sequential fallback
    std::transform(begin, end, begin, std::forward<F>(func));
    return;
  }

  tf::Taskflow taskflow;
  taskflow.transform(begin, end, begin, std::forward<F>(func));
  exec.run_and_wait(taskflow);
}

// ─────────────────────────────────────────────────────────────────────────────
// parallel_reduce - parallel reduction
// ─────────────────────────────────────────────────────────────────────────────

/// Reduce a range to a single value in parallel.
/// The binary operation must be associative (order of operations is unspecified).
template <std::ranges::range R, typename T, typename BinaryOp>
  requires std::invocable<BinaryOp, T, std::ranges::range_reference_t<R>>
[[nodiscard]] T parallel_reduce(Executor& exec, R&& range, T init, BinaryOp&& op) {
  auto begin = std::ranges::begin(range);
  auto end = std::ranges::end(range);

  if (begin == end)
    return init;

  auto size = std::ranges::distance(begin, end);
  if (static_cast<std::size_t>(size) < kParallelThreshold) {
    // Sequential fallback
    return std::accumulate(begin, end, std::move(init), std::forward<BinaryOp>(op));
  }

  tf::Taskflow taskflow;
  T result = init;
  taskflow.reduce(begin, end, result, std::forward<BinaryOp>(op));
  exec.run_and_wait(taskflow);
  return result;
}

/// Parallel sum of a range.
template <std::ranges::range R>
[[nodiscard]] auto parallel_sum(Executor& exec, R&& range) {
  using T = std::ranges::range_value_t<R>;
  return parallel_reduce(exec, std::forward<R>(range), T{}, std::plus<>{});
}

// ─────────────────────────────────────────────────────────────────────────────
// parallel_filter - parallel filtering (returns new container)
// ─────────────────────────────────────────────────────────────────────────────

/// Filter elements matching a predicate in parallel.
/// Returns a new vector containing matching elements.
template <std::ranges::range R, typename Pred>
  requires std::predicate<Pred, std::ranges::range_reference_t<R>>
[[nodiscard]] auto parallel_filter(Executor& exec, R&& range, Pred&& pred)
    -> std::vector<std::ranges::range_value_t<R>> {
  using T = std::ranges::range_value_t<R>;

  auto begin = std::ranges::begin(range);
  auto end = std::ranges::end(range);

  if (begin == end)
    return {};

  auto size = std::ranges::distance(begin, end);

  if (static_cast<std::size_t>(size) < kParallelThreshold) {
    // Sequential fallback
    std::vector<T> result;
    std::copy_if(begin, end, std::back_inserter(result), std::forward<Pred>(pred));
    return result;
  }

  // Parallel filter: each worker collects matching elements into thread-local vectors
  // then merge at the end
  std::mutex mutex;
  std::vector<T> result;
  result.reserve(static_cast<std::size_t>(size) / 2); // Estimate 50% match rate

  parallel_for(exec, std::forward<R>(range), [&](const auto& elem) {
    if (pred(elem)) {
      std::lock_guard lock(mutex);
      result.push_back(elem);
    }
  });

  return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// parallel_any / parallel_all / parallel_none - parallel predicates
// ─────────────────────────────────────────────────────────────────────────────

/// Check if any element satisfies the predicate (parallel, short-circuiting).
template <std::ranges::range R, typename Pred>
  requires std::predicate<Pred, std::ranges::range_reference_t<R>>
[[nodiscard]] bool parallel_any(Executor& exec, R&& range, Pred&& pred) {
  auto begin = std::ranges::begin(range);
  auto end = std::ranges::end(range);

  if (begin == end)
    return false;

  auto size = std::ranges::distance(begin, end);
  if (static_cast<std::size_t>(size) < kParallelThreshold) {
    return std::any_of(begin, end, std::forward<Pred>(pred));
  }

  std::atomic<bool> found{false};

  parallel_for(exec, std::forward<R>(range), [&](const auto& elem) {
    if (!found.load(std::memory_order_relaxed) && pred(elem)) {
      found.store(true, std::memory_order_relaxed);
    }
  });

  return found.load();
}

/// Check if all elements satisfy the predicate.
template <std::ranges::range R, typename Pred>
  requires std::predicate<Pred, std::ranges::range_reference_t<R>>
[[nodiscard]] bool parallel_all(Executor& exec, R&& range, Pred&& pred) {
  return !parallel_any(exec, std::forward<R>(range),
                       [&pred](const auto& elem) { return !pred(elem); });
}

/// Check if no element satisfies the predicate.
template <std::ranges::range R, typename Pred>
  requires std::predicate<Pred, std::ranges::range_reference_t<R>>
[[nodiscard]] bool parallel_none(Executor& exec, R&& range, Pred&& pred) {
  return !parallel_any(exec, std::forward<R>(range), std::forward<Pred>(pred));
}

// ─────────────────────────────────────────────────────────────────────────────
// parallel_count - count matching elements
// ─────────────────────────────────────────────────────────────────────────────

/// Count elements matching a predicate in parallel.
template <std::ranges::range R, typename Pred>
  requires std::predicate<Pred, std::ranges::range_reference_t<R>>
[[nodiscard]] std::size_t parallel_count_if(Executor& exec, R&& range, Pred&& pred) {
  auto begin = std::ranges::begin(range);
  auto end = std::ranges::end(range);

  if (begin == end)
    return 0;

  auto size = std::ranges::distance(begin, end);
  if (static_cast<std::size_t>(size) < kParallelThreshold) {
    return static_cast<std::size_t>(std::count_if(begin, end, std::forward<Pred>(pred)));
  }

  std::atomic<std::size_t> count{0};

  parallel_for(exec, std::forward<R>(range), [&](const auto& elem) {
    if (pred(elem)) {
      count.fetch_add(1, std::memory_order_relaxed);
    }
  });

  return count.load();
}

// ─────────────────────────────────────────────────────────────────────────────
// Convenience overloads using global executor
// ─────────────────────────────────────────────────────────────────────────────

template <std::ranges::range R, typename F>
  requires std::invocable<F, std::ranges::range_reference_t<R>>
void parallel_for(R&& range, F&& func) {
  parallel_for(global_executor(), std::forward<R>(range), std::forward<F>(func));
}

template <typename F>
  requires std::invocable<F, std::size_t>
void parallel_for_index(std::size_t count, F&& func) {
  parallel_for_index(global_executor(), count, std::forward<F>(func));
}

template <std::ranges::range R, typename T, typename BinaryOp>
[[nodiscard]] T parallel_reduce(R&& range, T init, BinaryOp&& op) {
  return parallel_reduce(global_executor(), std::forward<R>(range), std::move(init),
                         std::forward<BinaryOp>(op));
}

template <std::ranges::range R>
[[nodiscard]] auto parallel_sum(R&& range) {
  return parallel_sum(global_executor(), std::forward<R>(range));
}

template <std::ranges::range R, typename Pred>
  requires std::predicate<Pred, std::ranges::range_reference_t<R>>
[[nodiscard]] auto parallel_filter(R&& range, Pred&& pred) {
  return parallel_filter(global_executor(), std::forward<R>(range), std::forward<Pred>(pred));
}

template <std::ranges::range R, typename Pred>
  requires std::predicate<Pred, std::ranges::range_reference_t<R>>
[[nodiscard]] bool parallel_any(R&& range, Pred&& pred) {
  return parallel_any(global_executor(), std::forward<R>(range), std::forward<Pred>(pred));
}

template <std::ranges::range R, typename Pred>
  requires std::predicate<Pred, std::ranges::range_reference_t<R>>
[[nodiscard]] bool parallel_all(R&& range, Pred&& pred) {
  return parallel_all(global_executor(), std::forward<R>(range), std::forward<Pred>(pred));
}

template <std::ranges::range R, typename Pred>
  requires std::predicate<Pred, std::ranges::range_reference_t<R>>
[[nodiscard]] bool parallel_none(R&& range, Pred&& pred) {
  return parallel_none(global_executor(), std::forward<R>(range), std::forward<Pred>(pred));
}

template <std::ranges::range R, typename Pred>
  requires std::predicate<Pred, std::ranges::range_reference_t<R>>
[[nodiscard]] std::size_t parallel_count_if(R&& range, Pred&& pred) {
  return parallel_count_if(global_executor(), std::forward<R>(range), std::forward<Pred>(pred));
}

} // namespace straylight::nix::async
