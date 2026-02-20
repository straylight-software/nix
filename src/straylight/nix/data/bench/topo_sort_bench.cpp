// straylight::nix::data::topo_sort benchmarks
//
// Benchmarks for topological sort primitive using nanobench.
// Tests realistic workloads from Nix dependency resolution.

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "../topo_sort.h"

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

namespace topo = straylight::nix::data;

namespace {

// Graph structure for benchmarking
struct BenchGraph {
  std::vector<std::string> nodes;
  std::unordered_map<std::string, std::vector<std::string>> deps;
};

// Generate a linear chain: n0 -> n1 -> n2 -> ... -> n(size-1)
BenchGraph generate_linear_chain(std::size_t size) {
  BenchGraph graph;
  graph.nodes.reserve(size);

  for (std::size_t i = 0; i < size; ++i) {
    graph.nodes.push_back("n" + std::to_string(i));
    if (i > 0) {
      graph.deps[graph.nodes[i - 1]].push_back(graph.nodes[i]);
    }
  }

  return graph;
}

// Generate a wide tree: root -> [child1, child2, ..., childN]
BenchGraph generate_wide_tree(std::size_t children) {
  BenchGraph graph;
  graph.nodes.reserve(children + 1);

  graph.nodes.push_back("root");
  for (std::size_t i = 0; i < children; ++i) {
    auto child = "child" + std::to_string(i);
    graph.nodes.push_back(child);
    graph.deps["root"].push_back(child);
  }

  return graph;
}

// Generate a diamond graph with multiple paths
// root -> [mid1, mid2, ..., midN] -> leaf
BenchGraph generate_diamond(std::size_t mid_count) {
  BenchGraph graph;
  graph.nodes.reserve(mid_count + 2);

  graph.nodes.push_back("root");
  graph.nodes.push_back("leaf");

  for (std::size_t i = 0; i < mid_count; ++i) {
    auto mid = "mid" + std::to_string(i);
    graph.nodes.push_back(mid);
    graph.deps["root"].push_back(mid);
    graph.deps[mid].push_back("leaf");
  }

  return graph;
}

// Generate a layered DAG with specified layers and nodes per layer
BenchGraph generate_layered_dag(std::size_t layers, std::size_t nodes_per_layer,
                                std::uint64_t seed = 42) {
  BenchGraph graph;
  std::mt19937_64 rng(seed);

  std::vector<std::vector<std::string>> layer_nodes(layers);

  for (std::size_t layer = 0; layer < layers; ++layer) {
    for (std::size_t i = 0; i < nodes_per_layer; ++i) {
      auto name = "l" + std::to_string(layer) + "n" + std::to_string(i);
      layer_nodes[layer].push_back(name);
      graph.nodes.push_back(name);

      // Connect to some nodes in next layer
      if (layer < layers - 1) {
        std::uniform_int_distribution<std::size_t> dist(1, nodes_per_layer);
        auto num_deps = dist(rng);
        for (std::size_t j = 0; j < num_deps && j < nodes_per_layer; ++j) {
          auto dep_name = "l" + std::to_string(layer + 1) + "n" + std::to_string(j);
          graph.deps[name].push_back(dep_name);
        }
      }
    }
  }

  return graph;
}

// Generate a random DAG (edges only go from lower to higher indices)
BenchGraph generate_random_dag(std::size_t size, double edge_prob, std::uint64_t seed = 42) {
  BenchGraph graph;
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> dist(0.0, 1.0);

  graph.nodes.reserve(size);
  for (std::size_t i = 0; i < size; ++i) {
    graph.nodes.push_back("n" + std::to_string(i));
  }

  for (std::size_t i = 0; i < size; ++i) {
    for (std::size_t j = i + 1; j < size; ++j) {
      if (dist(rng) < edge_prob) {
        graph.deps[graph.nodes[i]].push_back(graph.nodes[j]);
      }
    }
  }

  return graph;
}

// Dependency getter for BenchGraph
auto make_getter(const BenchGraph& graph) {
  return [&graph](const std::string& node) -> const std::vector<std::string>& {
    static const std::vector<std::string> empty{};
    if (auto it = graph.deps.find(node); it != graph.deps.end()) {
      return it->second;
    }
    return empty;
  };
}

} // namespace

int main() {
  ankerl::nanobench::Bench bench;
  bench.title("Topological Sort Benchmarks").warmup(100).minEpochIterations(100).unit("op");

  // ───────────────────────────────────────────────────────────────────────────
  // Linear chain benchmarks (worst case for parallel levels)
  // ───────────────────────────────────────────────────────────────────────────

  auto linear_10 = generate_linear_chain(10);
  auto linear_100 = generate_linear_chain(100);
  auto linear_1000 = generate_linear_chain(1000);

  bench.run("linear/10 nodes", [&] {
    auto result = topo::topoSort(linear_10.nodes, make_getter(linear_10));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("linear/100 nodes", [&] {
    auto result = topo::topoSort(linear_100.nodes, make_getter(linear_100));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("linear/1000 nodes", [&] {
    auto result = topo::topoSort(linear_1000.nodes, make_getter(linear_1000));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Wide tree benchmarks (best case for parallel - all children independent)
  // ───────────────────────────────────────────────────────────────────────────

  auto wide_100 = generate_wide_tree(100);
  auto wide_1000 = generate_wide_tree(1000);

  bench.run("wide/100 children", [&] {
    auto result = topo::topoSort(wide_100.nodes, make_getter(wide_100));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("wide/1000 children", [&] {
    auto result = topo::topoSort(wide_1000.nodes, make_getter(wide_1000));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Diamond benchmarks (common in package dependencies)
  // ───────────────────────────────────────────────────────────────────────────

  auto diamond_10 = generate_diamond(10);
  auto diamond_100 = generate_diamond(100);

  bench.run("diamond/10 paths", [&] {
    auto result = topo::topoSort(diamond_10.nodes, make_getter(diamond_10));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("diamond/100 paths", [&] {
    auto result = topo::topoSort(diamond_100.nodes, make_getter(diamond_100));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Layered DAG benchmarks (realistic package dependency structure)
  // ───────────────────────────────────────────────────────────────────────────

  auto layered_5x20 = generate_layered_dag(5, 20);     // 100 nodes
  auto layered_10x50 = generate_layered_dag(10, 50);   // 500 nodes
  auto layered_10x100 = generate_layered_dag(10, 100); // 1000 nodes

  bench.run("layered/5x20 (100 nodes)", [&] {
    auto result = topo::topoSort(layered_5x20.nodes, make_getter(layered_5x20));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("layered/10x50 (500 nodes)", [&] {
    auto result = topo::topoSort(layered_10x50.nodes, make_getter(layered_10x50));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("layered/10x100 (1000 nodes)", [&] {
    auto result = topo::topoSort(layered_10x100.nodes, make_getter(layered_10x100));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Random DAG benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  auto random_100_sparse = generate_random_dag(100, 0.05);   // ~250 edges
  auto random_100_dense = generate_random_dag(100, 0.20);    // ~1000 edges
  auto random_500_sparse = generate_random_dag(500, 0.02);   // ~2500 edges
  auto random_1000_sparse = generate_random_dag(1000, 0.01); // ~5000 edges

  bench.run("random/100 sparse", [&] {
    auto result = topo::topoSort(random_100_sparse.nodes, make_getter(random_100_sparse));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("random/100 dense", [&] {
    auto result = topo::topoSort(random_100_dense.nodes, make_getter(random_100_dense));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("random/500 sparse", [&] {
    auto result = topo::topoSort(random_500_sparse.nodes, make_getter(random_500_sparse));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("random/1000 sparse", [&] {
    auto result = topo::topoSort(random_1000_sparse.nodes, make_getter(random_1000_sparse));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Parallel sort benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("parallel/wide 100 children", [&] {
    auto result = topo::topoSortParallel(wide_100.nodes, make_getter(wide_100));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("parallel/layered 5x20", [&] {
    auto result = topo::topoSortParallel(layered_5x20.nodes, make_getter(layered_5x20));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("parallel/layered 10x50", [&] {
    auto result = topo::topoSortParallel(layered_10x50.nodes, make_getter(layered_10x50));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("parallel/random 500 sparse", [&] {
    auto result = topo::topoSortParallel(random_500_sparse.nodes, make_getter(random_500_sparse));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Integer key benchmarks (avoid string hashing overhead)
  // ───────────────────────────────────────────────────────────────────────────

  // Linear chain with integers
  std::vector<int> int_linear_1000;
  int_linear_1000.reserve(1000);
  for (int i = 0; i < 1000; ++i) {
    int_linear_1000.push_back(i);
  }

  bench.run("int/linear 1000", [&] {
    auto result = topo::topoSort(int_linear_1000, [](int i) {
      if (i < 999) {
        return std::vector<int>{i + 1};
      }
      return std::vector<int>{};
    });
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // Random DAG with integers
  std::mt19937_64 rng(42);
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  std::unordered_map<int, std::vector<int>> int_deps;

  for (int i = 0; i < 1000; ++i) {
    for (int j = i + 1; j < 1000; ++j) {
      if (dist(rng) < 0.01) {
        int_deps[i].push_back(j);
      }
    }
  }

  bench.run("int/random 1000 sparse", [&] {
    auto result = topo::topoSort(int_linear_1000, [&int_deps](int i) {
      if (auto it = int_deps.find(i); it != int_deps.end()) {
        return it->second;
      }
      return std::vector<int>{};
    });
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Real-world-ish: Nixpkgs-like dependency structure
  // ───────────────────────────────────────────────────────────────────────────

  // Simulate a typical build closure:
  // - ~100 packages
  // - Average 5 dependencies each
  // - Some packages (stdenv, glibc) are dependencies of many
  auto nixpkgs_like = generate_layered_dag(10, 10, 12345);
  // Add some common dependencies
  for (std::size_t i = 0; i < 5; ++i) {
    for (std::size_t j = 5; j < 10; ++j) {
      nixpkgs_like.deps["l" + std::to_string(i) + "n0"].push_back("l" + std::to_string(j) + "n0");
    }
  }

  bench.run("nixpkgs-like/100 packages", [&] {
    auto result = topo::topoSort(nixpkgs_like.nodes, make_getter(nixpkgs_like));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("nixpkgs-like/100 packages (parallel)", [&] {
    auto result = topo::topoSortParallel(nixpkgs_like.nodes, make_getter(nixpkgs_like));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  return 0;
}
