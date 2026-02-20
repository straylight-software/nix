// straylight // nix // util // tests
//
// Unit tests for topological sorting

// Catch2 must be included before rapidcheck/catch.h
#include <catch2/catch_test_macros.hpp>

// RapidCheck for property-based testing
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include "nix/util/topo-sort.h"

using namespace nix;

// ─────────────────────────────────────────────────────────────────────────────
// Helper types and functions
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Simple dependency graph represented as adjacency list
using dependency_graph = std::map<std::string, std::set<std::string>>;

// Creates a getChildren function for the given graph
auto make_get_children(const dependency_graph& graph) {
  return [&graph](const std::string& node) -> std::set<std::string> {
    auto it = graph.find(node);
    if (it != graph.end()) {
      return it->second;
    }
    return {};
  };
}

// Verifies that all dependents come before their dependencies in the sorted output.
// Note: nix's topoSort outputs items such that if A depends on B, then A appears
// BEFORE B in the output. This is the typical "build order" - you list what needs
// to be built, and dependencies come after (they get built first when processing
// the list in reverse).
bool verify_ordering(const std::vector<std::string>& sorted, const dependency_graph& graph) {
  std::unordered_map<std::string, size_t> position;
  for (size_t i = 0; i < sorted.size(); ++i) {
    position[sorted[i]] = i;
  }

  for (const auto& [node, dependencies] : graph) {
    auto node_position = position.find(node);
    if (node_position == position.end()) {
      continue; // node not in sorted output
    }

    for (const auto& dependency : dependencies) {
      auto dep_position = position.find(dependency);
      if (dep_position == position.end()) {
        continue; // dependency not in sorted output (external)
      }
      // node (the dependent) must come before dependency (have smaller index)
      if (node_position->second >= dep_position->second) {
        return false;
      }
    }
  }
  return true;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Empty and trivial graph tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("topo sort empty graph", "[topo-sort]") {
  std::set<std::string> items;
  dependency_graph graph;

  auto result = topoSort(items, make_get_children(graph));

  REQUIRE(std::holds_alternative<std::vector<std::string>>(result));
  auto sorted = std::get<std::vector<std::string>>(result);
  REQUIRE(sorted.empty());
}

TEST_CASE("topo sort single node no dependencies", "[topo-sort]") {
  std::set<std::string> items = {"a"};
  dependency_graph graph = {{"a", {}}};

  auto result = topoSort(items, make_get_children(graph));

  REQUIRE(std::holds_alternative<std::vector<std::string>>(result));
  auto sorted = std::get<std::vector<std::string>>(result);
  REQUIRE(sorted.size() == 1);
  REQUIRE(sorted[0] == "a");
}

TEST_CASE("topo sort single node with self-reference ignored", "[topo-sort]") {
  // Self-references are explicitly ignored by the algorithm (i != path check)
  std::set<std::string> items = {"a"};
  dependency_graph graph = {{"a", {"a"}}};

  auto result = topoSort(items, make_get_children(graph));

  REQUIRE(std::holds_alternative<std::vector<std::string>>(result));
  auto sorted = std::get<std::vector<std::string>>(result);
  REQUIRE(sorted.size() == 1);
  REQUIRE(sorted[0] == "a");
}

// ─────────────────────────────────────────────────────────────────────────────
// Linear chain tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("topo sort linear chain", "[topo-sort]") {
  // a -> b -> c -> d (a depends on b, b depends on c, etc.)
  std::set<std::string> items = {"a", "b", "c", "d"};
  dependency_graph graph = {
      {"a", {"b"}},
      {"b", {"c"}},
      {"c", {"d"}},
      {"d", {}},
  };

  auto result = topoSort(items, make_get_children(graph));

  REQUIRE(std::holds_alternative<std::vector<std::string>>(result));
  auto sorted = std::get<std::vector<std::string>>(result);
  REQUIRE(sorted.size() == 4);
  REQUIRE(verify_ordering(sorted, graph));

  // For a linear chain, dependents come before dependencies: a, b, c, d
  REQUIRE(sorted == std::vector<std::string>{"a", "b", "c", "d"});
}

TEST_CASE("topo sort reverse linear chain", "[topo-sort]") {
  // d -> c -> b -> a (d depends on c, c depends on b, etc.)
  std::set<std::string> items = {"a", "b", "c", "d"};
  dependency_graph graph = {
      {"a", {}},
      {"b", {"a"}},
      {"c", {"b"}},
      {"d", {"c"}},
  };

  auto result = topoSort(items, make_get_children(graph));

  REQUIRE(std::holds_alternative<std::vector<std::string>>(result));
  auto sorted = std::get<std::vector<std::string>>(result);
  REQUIRE(sorted.size() == 4);
  REQUIRE(verify_ordering(sorted, graph));

  // Dependents come before dependencies: d, c, b, a
  REQUIRE(sorted == std::vector<std::string>{"d", "c", "b", "a"});
}

// ─────────────────────────────────────────────────────────────────────────────
// Diamond dependency tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("topo sort diamond dependency", "[topo-sort]") {
  //     a
  //    / \
  //   b   c
  //    \ /
  //     d
  // a depends on b and c, both b and c depend on d
  std::set<std::string> items = {"a", "b", "c", "d"};
  dependency_graph graph = {
      {"a", {"b", "c"}},
      {"b", {"d"}},
      {"c", {"d"}},
      {"d", {}},
  };

  auto result = topoSort(items, make_get_children(graph));

  REQUIRE(std::holds_alternative<std::vector<std::string>>(result));
  auto sorted = std::get<std::vector<std::string>>(result);
  REQUIRE(sorted.size() == 4);
  REQUIRE(verify_ordering(sorted, graph));

  // a must be first (it depends on others), d must be last (nothing depends on it)
  REQUIRE(sorted.front() == "a");
  REQUIRE(sorted.back() == "d");
}

// ─────────────────────────────────────────────────────────────────────────────
// Disconnected graph tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("topo sort disconnected components", "[topo-sort]") {
  // Two separate chains: a -> b and c -> d
  std::set<std::string> items = {"a", "b", "c", "d"};
  dependency_graph graph = {
      {"a", {"b"}},
      {"b", {}},
      {"c", {"d"}},
      {"d", {}},
  };

  auto result = topoSort(items, make_get_children(graph));

  REQUIRE(std::holds_alternative<std::vector<std::string>>(result));
  auto sorted = std::get<std::vector<std::string>>(result);
  REQUIRE(sorted.size() == 4);
  REQUIRE(verify_ordering(sorted, graph));
}

TEST_CASE("topo sort multiple independent nodes", "[topo-sort]") {
  // All nodes are independent with no dependencies
  std::set<std::string> items = {"a", "b", "c", "d", "e"};
  dependency_graph graph = {
      {"a", {}}, {"b", {}}, {"c", {}}, {"d", {}}, {"e", {}},
  };

  auto result = topoSort(items, make_get_children(graph));

  REQUIRE(std::holds_alternative<std::vector<std::string>>(result));
  auto sorted = std::get<std::vector<std::string>>(result);
  REQUIRE(sorted.size() == 5);
  REQUIRE(verify_ordering(sorted, graph));

  // All items should be present
  std::set<std::string> sorted_set(sorted.begin(), sorted.end());
  REQUIRE(sorted_set == items);
}

// ─────────────────────────────────────────────────────────────────────────────
// cycle_t detection tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("topo sort detects simple cycle", "[topo-sort][cycle]") {
  // a -> b -> a (cycle)
  std::set<std::string> items = {"a", "b"};
  dependency_graph graph = {
      {"a", {"b"}},
      {"b", {"a"}},
  };

  auto result = topoSort(items, make_get_children(graph));

  REQUIRE(std::holds_alternative<cycle_t<std::string>>(result));
  auto cycle = std::get<cycle_t<std::string>>(result);
  // The cycle should involve both a and b
  REQUIRE((cycle.path == "a" || cycle.path == "b"));
}

TEST_CASE("topo sort detects three node cycle", "[topo-sort][cycle]") {
  // a -> b -> c -> a (cycle)
  std::set<std::string> items = {"a", "b", "c"};
  dependency_graph graph = {
      {"a", {"b"}},
      {"b", {"c"}},
      {"c", {"a"}},
  };

  auto result = topoSort(items, make_get_children(graph));

  REQUIRE(std::holds_alternative<cycle_t<std::string>>(result));
}

TEST_CASE("topo sort detects cycle with tail", "[topo-sort][cycle]") {
  // d -> a -> b -> c -> a (a, b, c form cycle, d leads into it)
  std::set<std::string> items = {"a", "b", "c", "d"};
  dependency_graph graph = {
      {"a", {"b"}},
      {"b", {"c"}},
      {"c", {"a"}},
      {"d", {"a"}},
  };

  auto result = topoSort(items, make_get_children(graph));

  REQUIRE(std::holds_alternative<cycle_t<std::string>>(result));
}

TEST_CASE("topo sort cycle provides path and parent info", "[topo-sort][cycle]") {
  // Simple cycle for predictable results
  std::set<std::string> items = {"x", "y"};
  dependency_graph graph = {
      {"x", {"y"}},
      {"y", {"x"}},
  };

  auto result = topoSort(items, make_get_children(graph));

  REQUIRE(std::holds_alternative<cycle_t<std::string>>(result));
  auto cycle = std::get<cycle_t<std::string>>(result);

  // The cycle should have path and parent that are both in the item set
  REQUIRE(items.count(cycle.path) == 1);
  REQUIRE(items.count(cycle.parent) == 1);
  // path and parent should be different (the edge that closes the cycle)
  REQUIRE(cycle.path != cycle.parent);
}

// ─────────────────────────────────────────────────────────────────────────────
// External dependencies tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("topo sort ignores dependencies not in item set", "[topo-sort]") {
  // a depends on external, which is not in items
  std::set<std::string> items = {"a", "b"};
  dependency_graph graph = {
      {"a", {"b", "external"}},
      {"b", {}},
  };

  auto result = topoSort(items, make_get_children(graph));

  REQUIRE(std::holds_alternative<std::vector<std::string>>(result));
  auto sorted = std::get<std::vector<std::string>>(result);
  REQUIRE(sorted.size() == 2);
  REQUIRE(verify_ordering(sorted, graph));
}

// ─────────────────────────────────────────────────────────────────────────────
// Complex graph tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("topo sort complex dependency graph", "[topo-sort]") {
  //       a
  //      /|\
  //     b c d
  //     |/| |
  //     e f g
  //      \|/
  //       h
  std::set<std::string> items = {"a", "b", "c", "d", "e", "f", "g", "h"};
  dependency_graph graph = {
      {"a", {"b", "c", "d"}}, {"b", {"e"}}, {"c", {"e", "f"}}, {"d", {"g"}},
      {"e", {"h"}},           {"f", {"h"}}, {"g", {"h"}},      {"h", {}},
  };

  auto result = topoSort(items, make_get_children(graph));

  REQUIRE(std::holds_alternative<std::vector<std::string>>(result));
  auto sorted = std::get<std::vector<std::string>>(result);
  REQUIRE(sorted.size() == 8);
  REQUIRE(verify_ordering(sorted, graph));

  // a must be first (depends on others), h must be last (nothing depends on it)
  REQUIRE(sorted.front() == "a");
  REQUIRE(sorted.back() == "h");
}

// ─────────────────────────────────────────────────────────────────────────────
// Integer key tests (to verify template works with different types)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("topo sort with integer keys", "[topo-sort]") {
  std::set<int> items = {1, 2, 3, 4};
  std::map<int, std::set<int>> graph = {
      {1, {2, 3}},
      {2, {4}},
      {3, {4}},
      {4, {}},
  };

  auto get_children = [&graph](const int& node) -> std::set<int> {
    auto it = graph.find(node);
    return it != graph.end() ? it->second : std::set<int>{};
  };

  auto result = topoSort(items, get_children);

  REQUIRE(std::holds_alternative<std::vector<int>>(result));
  auto sorted = std::get<std::vector<int>>(result);
  REQUIRE(sorted.size() == 4);

  // Verify ordering: dependents come before dependencies
  std::unordered_map<int, size_t> position;
  for (size_t i = 0; i < sorted.size(); ++i) {
    position[sorted[i]] = i;
  }

  // 1 should come before 2 and 3, which should come before 4
  REQUIRE(position[1] < position[2]);
  REQUIRE(position[1] < position[3]);
  REQUIRE(position[2] < position[4]);
  REQUIRE(position[3] < position[4]);
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-based tests with RapidCheck
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("topo sort property tests", "[topo-sort][property]") {
  rc::prop("sorted output contains exactly the input items (for acyclic graphs)", []() {
    // Generate a DAG by only allowing edges from lower to higher numbered nodes
    auto node_count = *rc::gen::inRange(0, 20);
    std::set<std::string> items;
    dependency_graph graph;

    for (int i = 0; i < node_count; ++i) {
      std::string node = "node_" + std::to_string(i);
      items.insert(node);
      graph[node] = {};

      // Only add edges to nodes with higher indices (ensures acyclic)
      for (int j = i + 1; j < node_count; ++j) {
        if (*rc::gen::inRange(0, 3) == 0) { // 33% chance of edge
          graph[node].insert("node_" + std::to_string(j));
        }
      }
    }

    auto result = topoSort(items, make_get_children(graph));

    RC_ASSERT(std::holds_alternative<std::vector<std::string>>(result));
    auto sorted = std::get<std::vector<std::string>>(result);

    std::set<std::string> sorted_set(sorted.begin(), sorted.end());
    RC_ASSERT(sorted_set == items);
  });

  rc::prop("all dependencies come before dependents (for acyclic graphs)", []() {
    // Generate a DAG
    auto node_count = *rc::gen::inRange(1, 15);
    std::set<std::string> items;
    dependency_graph graph;

    for (int i = 0; i < node_count; ++i) {
      std::string node = "n" + std::to_string(i);
      items.insert(node);
      graph[node] = {};

      // Only add edges to nodes with higher indices (ensures acyclic)
      for (int j = i + 1; j < node_count; ++j) {
        if (*rc::gen::inRange(0, 2) == 0) { // 50% chance of edge
          graph[node].insert("n" + std::to_string(j));
        }
      }
    }

    auto result = topoSort(items, make_get_children(graph));

    RC_ASSERT(std::holds_alternative<std::vector<std::string>>(result));
    auto sorted = std::get<std::vector<std::string>>(result);
    RC_ASSERT(verify_ordering(sorted, graph));
  });

  rc::prop("cycles are always detected", []() {
    // Generate a graph that definitely has a cycle
    auto cycle_size = *rc::gen::inRange(2, 10);
    std::set<std::string> items;
    dependency_graph graph;

    // Create a cycle: 0 -> 1 -> 2 -> ... -> (n-1) -> 0
    for (int i = 0; i < cycle_size; ++i) {
      std::string node = "c" + std::to_string(i);
      std::string next = "c" + std::to_string((i + 1) % cycle_size);
      items.insert(node);
      graph[node] = {next};
    }

    auto result = topoSort(items, make_get_children(graph));

    RC_ASSERT(std::holds_alternative<cycle_t<std::string>>(result));
    auto cycle = std::get<cycle_t<std::string>>(result);
    RC_ASSERT(items.count(cycle.path) == 1);
    RC_ASSERT(items.count(cycle.parent) == 1);
  });

  rc::prop("output preserves relative order when no dependencies exist", []() {
    // With no dependencies, traversal order depends on set iteration order
    auto node_count = *rc::gen::inRange(0, 20);
    std::set<std::string> items;
    dependency_graph graph;

    for (int i = 0; i < node_count; ++i) {
      std::string node = "ind_" + std::to_string(i);
      items.insert(node);
      graph[node] = {}; // No dependencies
    }

    auto result = topoSort(items, make_get_children(graph));

    RC_ASSERT(std::holds_alternative<std::vector<std::string>>(result));
    auto sorted = std::get<std::vector<std::string>>(result);
    RC_ASSERT(sorted.size() == items.size());
  });
}
