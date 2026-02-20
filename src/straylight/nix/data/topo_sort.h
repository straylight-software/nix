// straylight::nix::primitives::topo_sort
//
// Modern C++23 topological sort primitive using Kahn's algorithm (BFS-based).
// Provides O(V+E) complexity with cycle detection and detailed error reporting.
//
// This replaces nix/util/topo-sort.h with:
// - BFS-based Kahn's algorithm (instead of DFS)
// - std::expected return type for error handling
// - Detailed cycle error reporting with all nodes in the cycle
// - Optional parallel topological sort for independent nodes

#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <expected>
#include <functional>
#include <queue>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace straylight::nix::data {

// ─────────────────────────────────────────────────────────────────────────────
// Error types
// ─────────────────────────────────────────────────────────────────────────────

/// Error returned when a cycle is detected in the dependency graph.
/// Contains all nodes that form the cycle for detailed error reporting.
template <typename T>
struct CycleError {
  /// Nodes forming the cycle (in cycle order, may not include all cycle nodes
  /// but guaranteed to include at least one node from the cycle)
  std::vector<T> cycle_nodes;

  /// Human-readable error message
  [[nodiscard]] auto message() const -> std::string {
    if (cycle_nodes.empty()) {
      return "Cycle detected in dependency graph";
    }

    std::string msg = "Cycle detected in dependency graph involving: ";
    for (std::size_t i = 0; i < cycle_nodes.size(); ++i) {
      if (i > 0) {
        msg += " -> ";
      }
      if constexpr (requires(const T& t) { std::to_string(t); }) {
        msg += std::to_string(cycle_nodes[i]);
      } else if constexpr (requires(const T& t) {
                             { t } -> std::convertible_to<std::string_view>;
                           }) {
        msg += std::string(cycle_nodes[i]);
      } else {
        msg += "[node]";
      }
    }
    return msg;
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// Concepts
// ─────────────────────────────────────────────────────────────────────────────

/// Concept for dependency getter callable
template <typename F, typename T>
concept DependencyGetter = std::invocable<F, const T&> && requires(F f, const T& item) {
  { std::invoke(f, item) } -> std::ranges::range;
  requires std::convertible_to<std::ranges::range_value_t<std::invoke_result_t<F, const T&>>, T>;
};

// ─────────────────────────────────────────────────────────────────────────────
// Main topoSort function (Kahn's algorithm)
// ─────────────────────────────────────────────────────────────────────────────

/// Topologically sort items based on their dependencies using Kahn's algorithm.
///
/// @tparam T The item type (must be hashable and equality comparable)
/// @tparam GetDeps Callable that returns the dependencies of an item
///
/// @param items Range of items to sort
/// @param get_deps Function (const T&) -> Range<T> returning dependencies
///
/// @return Sorted vector with dependencies before dependents, or CycleError
///
/// Time complexity: O(V + E) where V = items, E = total dependencies
/// Space complexity: O(V + E) for the in-degree map and adjacency tracking
///
/// Example:
/// ```cpp
/// std::vector<std::string> packages = {"a", "b", "c"};
/// auto deps = [](const std::string& pkg) {
///     if (pkg == "a") return std::vector{"b", "c"};
///     if (pkg == "b") return std::vector{"c"};
///     return std::vector<std::string>{};
/// };
/// auto result = topoSort(packages, deps);
/// // result.value() == {"c", "b", "a"}
/// ```
template <std::ranges::range Range, DependencyGetter<std::ranges::range_value_t<Range>> GetDeps>
  requires std::equality_comparable<std::ranges::range_value_t<Range>>
[[nodiscard]] auto topoSort(Range&& items, GetDeps&& get_deps)
    -> std::expected<std::vector<std::ranges::range_value_t<Range>>,
                     CycleError<std::ranges::range_value_t<Range>>> {
  using T = std::ranges::range_value_t<Range>;

  // Build item set for filtering dependencies
  std::unordered_set<T> item_set;
  for (const auto& item : items) {
    item_set.insert(item);
  }

  if (item_set.empty()) {
    return std::vector<T>{};
  }

  // Compute in-degrees and reverse adjacency (who depends on whom)
  std::unordered_map<T, std::size_t> in_degree;
  std::unordered_map<T, std::vector<T>> dependents; // item -> items that depend on it

  // Initialize all items with zero in-degree
  for (const auto& item : item_set) {
    in_degree[item] = 0;
  }

  // Build the graph
  for (const auto& item : item_set) {
    auto deps = std::invoke(get_deps, item);
    for (const auto& dep : deps) {
      // Only consider dependencies within our item set
      if (dep != item && item_set.contains(dep)) {
        in_degree[item]++;
        dependents[dep].push_back(item);
      }
    }
  }

  // Initialize queue with zero in-degree nodes (no dependencies)
  std::queue<T> ready;
  for (const auto& [item, degree] : in_degree) {
    if (degree == 0) {
      ready.push(item);
    }
  }

  // Kahn's algorithm: process nodes in topological order
  std::vector<T> sorted;
  sorted.reserve(item_set.size());

  while (!ready.empty()) {
    T current = ready.front();
    ready.pop();
    sorted.push_back(current);

    // Decrease in-degree for all dependents
    if (auto it = dependents.find(current); it != dependents.end()) {
      for (const auto& dependent : it->second) {
        if (--in_degree[dependent] == 0) {
          ready.push(dependent);
        }
      }
    }
  }

  // Check for cycle: if we haven't processed all items, there's a cycle
  if (sorted.size() != item_set.size()) {
    // Find nodes still with non-zero in-degree (they form the cycle)
    std::vector<T> cycle_nodes;
    for (const auto& [item, degree] : in_degree) {
      if (degree > 0) {
        cycle_nodes.push_back(item);
      }
    }
    return std::unexpected(CycleError<T>{std::move(cycle_nodes)});
  }

  return sorted;
}

// ─────────────────────────────────────────────────────────────────────────────
// Parallel topological sort (returns levels of independent nodes)
// ─────────────────────────────────────────────────────────────────────────────

/// Result of parallel topological sort: nodes grouped by level.
/// Nodes within the same level have no dependencies on each other and can be
/// processed in parallel.
template <typename T>
using TopoLevels = std::vector<std::vector<T>>;

/// Topologically sort items and group by parallelization level.
///
/// Returns levels where all items in each level can be processed in parallel,
/// as they have no dependencies on each other - only on items in previous levels.
///
/// @param items Range of items to sort
/// @param get_deps Function returning dependencies of an item
///
/// @return Vector of levels, or CycleError if cycle detected
///
/// Example:
/// ```cpp
/// // Given: a->b, a->c, b->d, c->d
/// auto result = topoSortParallel(items, deps);
/// // result.value() == {{d}, {b, c}, {a}}
/// // Level 0: d (no deps)
/// // Level 1: b, c (only depend on d, can run in parallel)
/// // Level 2: a (depends on b, c)
/// ```
template <std::ranges::range Range, DependencyGetter<std::ranges::range_value_t<Range>> GetDeps>
  requires std::equality_comparable<std::ranges::range_value_t<Range>>
[[nodiscard]] auto topoSortParallel(Range&& items, GetDeps&& get_deps)
    -> std::expected<TopoLevels<std::ranges::range_value_t<Range>>,
                     CycleError<std::ranges::range_value_t<Range>>> {
  using T = std::ranges::range_value_t<Range>;

  // Build item set
  std::unordered_set<T> item_set;
  for (const auto& item : items) {
    item_set.insert(item);
  }

  if (item_set.empty()) {
    return TopoLevels<T>{};
  }

  // Compute in-degrees and reverse adjacency
  std::unordered_map<T, std::size_t> in_degree;
  std::unordered_map<T, std::vector<T>> dependents;

  for (const auto& item : item_set) {
    in_degree[item] = 0;
  }

  for (const auto& item : item_set) {
    auto deps = std::invoke(get_deps, item);
    for (const auto& dep : deps) {
      if (dep != item && item_set.contains(dep)) {
        in_degree[item]++;
        dependents[dep].push_back(item);
      }
    }
  }

  // Collect initial ready nodes (level 0)
  std::vector<T> current_level;
  for (const auto& [item, degree] : in_degree) {
    if (degree == 0) {
      current_level.push_back(item);
    }
  }

  TopoLevels<T> levels;
  std::size_t processed = 0;

  while (!current_level.empty()) {
    processed += current_level.size();
    levels.push_back(std::move(current_level));
    current_level = {};

    // Find next level: decrease in-degrees and collect newly ready nodes
    for (const auto& item : levels.back()) {
      if (auto it = dependents.find(item); it != dependents.end()) {
        for (const auto& dependent : it->second) {
          if (--in_degree[dependent] == 0) {
            current_level.push_back(dependent);
          }
        }
      }
    }
  }

  // Check for cycle
  if (processed != item_set.size()) {
    std::vector<T> cycle_nodes;
    for (const auto& [item, degree] : in_degree) {
      if (degree > 0) {
        cycle_nodes.push_back(item);
      }
    }
    return std::unexpected(CycleError<T>{std::move(cycle_nodes)});
  }

  return levels;
}

// ─────────────────────────────────────────────────────────────────────────────
// Utility: flatten parallel levels to linear order
// ─────────────────────────────────────────────────────────────────────────────

/// Flatten parallel levels into a single sorted vector.
template <typename T>
[[nodiscard]] auto flattenLevels(const TopoLevels<T>& levels) -> std::vector<T> {
  std::size_t total = 0;
  for (const auto& level : levels) {
    total += level.size();
  }

  std::vector<T> result;
  result.reserve(total);

  for (const auto& level : levels) {
    for (const auto& item : level) {
      result.push_back(item);
    }
  }

  return result;
}

} // namespace straylight::nix::data
