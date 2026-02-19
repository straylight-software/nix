// straylight::nix::primitives::topo_sort tests
//
// Property-based testing with rapidcheck for topological sort primitive.
// Tests Kahn's algorithm implementation with cycle detection.

// clang-format off
// Catch2 MUST be included before rapidcheck/catch.h for v3 compatibility
#include <catch2/catch_test_macros.hpp>
// clang-format on

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include "../topo_sort.h"

namespace topo = straylight::nix::primitives;

// ─────────────────────────────────────────────────────────────────────────────
// Helper types and functions
// ─────────────────────────────────────────────────────────────────────────────

using Graph = std::unordered_map<std::string, std::vector<std::string>>;

auto make_dep_getter(const Graph& graph) {
  return [&graph](const std::string& node) -> const std::vector<std::string>& {
    static const std::vector<std::string> empty{};
    if (auto it = graph.find(node); it != graph.end()) {
      return it->second;
    }
    return empty;
  };
}

// Check that a sorted order is valid (all dependencies come before dependents)
bool is_valid_topo_order(const std::vector<std::string>& sorted, const Graph& deps) {
  std::unordered_set<std::string> seen;
  for (const auto& node : sorted) {
    if (auto it = deps.find(node); it != deps.end()) {
      for (const auto& dep : it->second) {
        if (seen.find(dep) == seen.end() && deps.find(dep) != deps.end()) {
          return false;
        }
      }
    }
    seen.insert(node);
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Basic functionality tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("topoSort empty input", "[topo_sort]") {
  std::vector<std::string> items;
  auto result = topo::topoSort(items, [](const std::string&) {
    return std::vector<std::string>{};
  });
  REQUIRE(result.has_value());
  REQUIRE(result->empty());
}

TEST_CASE("topoSort single item no deps", "[topo_sort]") {
  std::vector<std::string> items = {"a"};
  auto result = topo::topoSort(items, [](const std::string&) {
    return std::vector<std::string>{};
  });
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 1);
  REQUIRE((*result)[0] == "a");
}

TEST_CASE("topoSort linear chain", "[topo_sort]") {
  std::vector<std::string> items = {"a", "b", "c", "d"};
  Graph deps = {{"a", {"b"}}, {"b", {"c"}}, {"c", {"d"}}};
  auto result = topo::topoSort(items, make_dep_getter(deps));
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 4);
  REQUIRE(is_valid_topo_order(*result, deps));
}

TEST_CASE("topoSort diamond dependency", "[topo_sort]") {
  std::vector<std::string> items = {"a", "b", "c", "d"};
  Graph deps = {{"a", {"b", "c"}}, {"b", {"d"}}, {"c", {"d"}}};
  auto result = topo::topoSort(items, make_dep_getter(deps));
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 4);
  REQUIRE(is_valid_topo_order(*result, deps));
  REQUIRE((*result)[0] == "d");
  REQUIRE((*result)[3] == "a");
}

TEST_CASE("topoSort independent items", "[topo_sort]") {
  std::vector<std::string> items = {"a", "b", "c"};
  auto result = topo::topoSort(items, [](const std::string&) {
    return std::vector<std::string>{};
  });
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 3);
  std::set<std::string> result_set(result->begin(), result->end());
  REQUIRE(result_set == std::set<std::string>{"a", "b", "c"});
}

TEST_CASE("topoSort ignores external dependencies", "[topo_sort]") {
  std::vector<std::string> items = {"a", "b"};
  Graph deps = {{"a", {"b", "x"}}};
  auto result = topo::topoSort(items, make_dep_getter(deps));
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 2);
  REQUIRE((*result)[0] == "b");
  REQUIRE((*result)[1] == "a");
}

TEST_CASE("topoSort ignores self-dependencies", "[topo_sort]") {
  std::vector<std::string> items = {"a", "b"};
  Graph deps = {{"a", {"a", "b"}}};
  auto result = topo::topoSort(items, make_dep_getter(deps));
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 2);
  REQUIRE((*result)[0] == "b");
  REQUIRE((*result)[1] == "a");
}

// ─────────────────────────────────────────────────────────────────────────────
// Cycle detection tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("topoSort detects simple cycle", "[topo_sort][cycle]") {
  std::vector<std::string> items = {"a", "b"};
  Graph deps = {{"a", {"b"}}, {"b", {"a"}}};
  auto result = topo::topoSort(items, make_dep_getter(deps));
  REQUIRE_FALSE(result.has_value());
  REQUIRE_FALSE(result.error().cycle_nodes.empty());
}

TEST_CASE("topoSort detects 3-node cycle", "[topo_sort][cycle]") {
  std::vector<std::string> items = {"a", "b", "c"};
  Graph deps = {{"a", {"b"}}, {"b", {"c"}}, {"c", {"a"}}};
  auto result = topo::topoSort(items, make_dep_getter(deps));
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().cycle_nodes.size() == 3);
}

TEST_CASE("topoSort detects partial cycle", "[topo_sort][cycle]") {
  std::vector<std::string> items = {"a", "b", "c", "d"};
  Graph deps = {{"d", {"a"}}, {"a", {"b"}}, {"b", {"c"}}, {"c", {"a"}}};
  auto result = topo::topoSort(items, make_dep_getter(deps));
  REQUIRE_FALSE(result.has_value());
  std::set<std::string> cycle_set(result.error().cycle_nodes.begin(),
                                  result.error().cycle_nodes.end());
  REQUIRE(cycle_set.contains("a"));
  REQUIRE(cycle_set.contains("b"));
  REQUIRE(cycle_set.contains("c"));
}

TEST_CASE("CycleError message generation", "[topo_sort][cycle]") {
  topo::CycleError<std::string> error{{"a", "b", "c"}};
  auto msg = error.message();
  REQUIRE(msg.find("Cycle") != std::string::npos);
  REQUIRE(msg.find("a") != std::string::npos);
}

TEST_CASE("CycleError message with int type", "[topo_sort][cycle]") {
  topo::CycleError<int> error{{1, 2, 3}};
  auto msg = error.message();
  REQUIRE(msg.find("Cycle") != std::string::npos);
  REQUIRE(msg.find("1") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Parallel topological sort tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("topoSortParallel empty input", "[topo_sort][parallel]") {
  std::vector<std::string> items;
  auto result = topo::topoSortParallel(items, [](const std::string&) {
    return std::vector<std::string>{};
  });
  REQUIRE(result.has_value());
  REQUIRE(result->empty());
}

TEST_CASE("topoSortParallel independent items in one level", "[topo_sort][parallel]") {
  std::vector<std::string> items = {"a", "b", "c"};
  auto result = topo::topoSortParallel(items, [](const std::string&) {
    return std::vector<std::string>{};
  });
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 1);
  REQUIRE((*result)[0].size() == 3);
}

TEST_CASE("topoSortParallel linear chain", "[topo_sort][parallel]") {
  std::vector<std::string> items = {"a", "b", "c", "d"};
  Graph deps = {{"a", {"b"}}, {"b", {"c"}}, {"c", {"d"}}};
  auto result = topo::topoSortParallel(items, make_dep_getter(deps));
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 4);
  REQUIRE((*result)[0][0] == "d");
  REQUIRE((*result)[3][0] == "a");
}

TEST_CASE("topoSortParallel diamond", "[topo_sort][parallel]") {
  std::vector<std::string> items = {"a", "b", "c", "d"};
  Graph deps = {{"a", {"b", "c"}}, {"b", {"d"}}, {"c", {"d"}}};
  auto result = topo::topoSortParallel(items, make_dep_getter(deps));
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 3);
  REQUIRE((*result)[0].size() == 1);
  REQUIRE((*result)[1].size() == 2);
  REQUIRE((*result)[2].size() == 1);
}

TEST_CASE("topoSortParallel detects cycle", "[topo_sort][parallel][cycle]") {
  std::vector<std::string> items = {"a", "b"};
  Graph deps = {{"a", {"b"}}, {"b", {"a"}}};
  auto result = topo::topoSortParallel(items, make_dep_getter(deps));
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("flattenLevels", "[topo_sort][parallel]") {
  topo::TopoLevels<std::string> levels = {{"d"}, {"b", "c"}, {"a"}};
  auto flat = topo::flattenLevels(levels);
  REQUIRE(flat.size() == 4);
  REQUIRE(flat[0] == "d");
  REQUIRE(flat[3] == "a");
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-based tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("topoSort output contains all input items", "[topo_sort][property]") {
  rc::prop("sorted output contains exactly the input items", []() {
    auto n = *rc::gen::inRange<std::size_t>(1, 20);
    std::vector<std::string> items;
    for (std::size_t i = 0; i < n; ++i) {
      items.push_back("item" + std::to_string(i));
    }
    auto result = topo::topoSort(items, [](const std::string&) {
      return std::vector<std::string>{};
    });
    RC_ASSERT(result.has_value());
    std::set<std::string> input_set(items.begin(), items.end());
    std::set<std::string> output_set(result->begin(), result->end());
    RC_ASSERT(input_set == output_set);
  });
}

TEST_CASE("topoSort dependencies come before dependents", "[topo_sort][property]") {
  rc::prop("all dependencies appear before their dependents", []() {
    auto n = *rc::gen::inRange<std::size_t>(2, 10);
    std::vector<std::string> items;
    for (std::size_t i = 0; i < n; ++i) {
      items.push_back("node" + std::to_string(i));
    }
    std::unordered_map<std::string, std::vector<std::string>> deps;
    for (std::size_t i = 0; i < n - 1; ++i) {
      auto num_deps = *rc::gen::inRange<std::size_t>(0, n - i - 1);
      for (std::size_t j = 0; j < num_deps; ++j) {
        auto dep_idx = *rc::gen::inRange<std::size_t>(i + 1, n);
        deps[items[i]].push_back(items[dep_idx]);
      }
    }
    auto result = topo::topoSort(items, [&deps](const std::string& item) {
      if (auto it = deps.find(item); it != deps.end()) {
        return it->second;
      }
      return std::vector<std::string>{};
    });
    RC_ASSERT(result.has_value());
    RC_ASSERT(is_valid_topo_order(*result, deps));
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Integer type tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("topoSort with integer items", "[topo_sort]") {
  std::vector<int> items = {1, 2, 3, 4};
  std::unordered_map<int, std::vector<int>> deps = {{1, {2, 3}}, {2, {4}}, {3, {4}}};
  auto result = topo::topoSort(items, [&deps](int item) {
    if (auto it = deps.find(item); it != deps.end()) {
      return it->second;
    }
    return std::vector<int>{};
  });
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 4);
  REQUIRE((*result)[0] == 4);
  REQUIRE((*result)[3] == 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge cases and stress tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("topoSort large graph", "[topo_sort][stress]") {
  constexpr std::size_t N = 1000;
  std::vector<std::size_t> items;
  items.reserve(N);
  for (std::size_t i = 0; i < N; ++i) {
    items.push_back(i);
  }
  auto result = topo::topoSort(items, [](std::size_t item) {
    if (item < 999) {
      return std::vector<std::size_t>{item + 1};
    }
    return std::vector<std::size_t>{};
  });
  REQUIRE(result.has_value());
  REQUIRE(result->size() == N);
  REQUIRE((*result)[0] == 999);
  REQUIRE((*result)[N - 1] == 0);
}

TEST_CASE("topoSort duplicate items in input", "[topo_sort]") {
  std::vector<std::string> items = {"a", "b", "a", "c", "b"};
  auto result = topo::topoSort(items, [](const std::string&) {
    return std::vector<std::string>{};
  });
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 3);
  std::set<std::string> result_set(result->begin(), result->end());
  REQUIRE(result_set == std::set<std::string>{"a", "b", "c"});
}
