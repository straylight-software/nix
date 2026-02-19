// straylight::nix::primitives::async::closure benchmarks
//
// Benchmarks comparing sequential vs parallel transitive closure computation.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../async/closure.h"
#include "../async/executor.h"

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

namespace async = straylight::nix::primitives::async;

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Graph generators
// ─────────────────────────────────────────────────────────────────────────────

// Linear chain: 0 -> 1 -> 2 -> ... -> n-1
std::map<int, std::set<int>> makeLinearChain(int n) {
  std::map<int, std::set<int>> edges;
  for (int i = 0; i < n - 1; ++i) {
    edges[i].insert(i + 1);
  }
  return edges;
}

// Binary tree: each node has 0, 1, or 2 children
std::map<int, std::set<int>> makeBinaryTree(int depth) {
  std::map<int, std::set<int>> edges;
  int nextId = 1;
  std::function<void(int, int)> build = [&](int node, int d) {
    if (d >= depth)
      return;
    int left = nextId++;
    int right = nextId++;
    edges[node].insert(left);
    edges[node].insert(right);
    build(left, d + 1);
    build(right, d + 1);
  };
  build(0, 0);
  return edges;
}

// Wide graph: root connects to n children, no further edges
std::map<int, std::set<int>> makeWideGraph(int n) {
  std::map<int, std::set<int>> edges;
  for (int i = 1; i <= n; ++i) {
    edges[0].insert(i);
  }
  return edges;
}

// Diamond lattice: layered graph with connections between adjacent layers
std::map<int, std::set<int>> makeDiamondLattice(int layers, int nodesPerLayer) {
  std::map<int, std::set<int>> edges;
  int id = 0;

  std::vector<std::vector<int>> layerNodes(layers);

  // Create nodes for each layer
  for (int l = 0; l < layers; ++l) {
    for (int n = 0; n < nodesPerLayer; ++n) {
      layerNodes[l].push_back(id++);
    }
  }

  // Connect adjacent layers
  for (int l = 0; l < layers - 1; ++l) {
    for (int from : layerNodes[l]) {
      for (int to : layerNodes[l + 1]) {
        edges[from].insert(to);
      }
    }
  }

  return edges;
}

// Random sparse graph
std::map<int, std::set<int>> makeRandomSparse(int n, double edgeProbability, unsigned seed = 42) {
  std::map<int, std::set<int>> edges;
  std::mt19937 rng(seed);
  std::uniform_real_distribution<> dist(0.0, 1.0);

  for (int i = 0; i < n; ++i) {
    for (int j = i + 1; j < n; ++j) {
      if (dist(rng) < edgeProbability) {
        edges[i].insert(j);
      }
    }
  }
  return edges;
}

// Count total nodes in graph
std::size_t countNodes(const std::map<int, std::set<int>>& edges) {
  std::set<int> all;
  for (const auto& [from, tos] : edges) {
    all.insert(from);
    for (int to : tos) {
      all.insert(to);
    }
  }
  return all.size();
}

// Simulated work for realistic benchmarks
volatile std::uint64_t g_sink = 0;

void doWork(int microseconds) {
  // Busy wait to simulate work
  auto start = std::chrono::high_resolution_clock::now();
  while (true) {
    auto now = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
    if (elapsed >= microseconds)
      break;
    g_sink += 1;
  }
}

} // namespace

int main() {
  ankerl::nanobench::Bench bench;
  bench.title("Transitive Closure Benchmarks").warmup(5).minEpochIterations(5).unit("closure");

  const std::size_t hwThreads = std::thread::hardware_concurrency();
  const std::size_t testThreads = std::min(hwThreads, std::size_t{8});

  // ───────────────────────────────────────────────────────────────────────────
  // Linear chain benchmarks (worst case for parallelism - no parallelism possible)
  // ───────────────────────────────────────────────────────────────────────────

  {
    auto edges = makeLinearChain(100);
    std::set<int> start{0};
    auto getDeps = [&](int n) { return edges[n]; };

    bench.run("Linear chain 100 (sequential)", [&] {
      auto result = async::computeClosure(start, getDeps);
      ankerl::nanobench::doNotOptimizeAway(result);
    });

    async::Executor exec(testThreads);
    bench.run("Linear chain 100 (parallel)", [&] {
      auto result = async::computeClosureAsync(exec, start, getDeps);
      ankerl::nanobench::doNotOptimizeAway(result);
    });
  }

  {
    auto edges = makeLinearChain(1000);
    std::set<int> start{0};
    auto getDeps = [&](int n) { return edges[n]; };

    bench.run("Linear chain 1K (sequential)", [&] {
      auto result = async::computeClosure(start, getDeps);
      ankerl::nanobench::doNotOptimizeAway(result);
    });

    async::Executor exec(testThreads);
    bench.run("Linear chain 1K (parallel)", [&] {
      auto result = async::computeClosureAsync(exec, start, getDeps);
      ankerl::nanobench::doNotOptimizeAway(result);
    });
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Wide graph benchmarks (best case for parallelism - all deps fetched at once)
  // ───────────────────────────────────────────────────────────────────────────

  {
    auto edges = makeWideGraph(100);
    std::set<int> start{0};
    auto getDeps = [&](int n) { return edges[n]; };

    bench.run("Wide graph 100 (sequential)", [&] {
      auto result = async::computeClosure(start, getDeps);
      ankerl::nanobench::doNotOptimizeAway(result);
    });

    async::Executor exec(testThreads);
    bench.run("Wide graph 100 (parallel)", [&] {
      auto result = async::computeClosureAsync(exec, start, getDeps);
      ankerl::nanobench::doNotOptimizeAway(result);
    });
  }

  {
    auto edges = makeWideGraph(1000);
    std::set<int> start{0};
    auto getDeps = [&](int n) { return edges[n]; };

    bench.run("Wide graph 1K (sequential)", [&] {
      auto result = async::computeClosure(start, getDeps);
      ankerl::nanobench::doNotOptimizeAway(result);
    });

    async::Executor exec(testThreads);
    bench.run("Wide graph 1K (parallel)", [&] {
      auto result = async::computeClosureAsync(exec, start, getDeps);
      ankerl::nanobench::doNotOptimizeAway(result);
    });
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Binary tree benchmarks (balanced parallelism)
  // ───────────────────────────────────────────────────────────────────────────

  {
    auto edges = makeBinaryTree(8); // 2^8 - 1 = 255 nodes
    std::size_t nodeCount = countNodes(edges);
    std::set<int> start{0};
    auto getDeps = [&](int n) { return edges[n]; };

    bench.run("Binary tree depth 8 (" + std::to_string(nodeCount) + " nodes) (sequential)", [&] {
      auto result = async::computeClosure(start, getDeps);
      ankerl::nanobench::doNotOptimizeAway(result);
    });

    async::Executor exec(testThreads);
    bench.run("Binary tree depth 8 (" + std::to_string(nodeCount) + " nodes) (parallel)", [&] {
      auto result = async::computeClosureAsync(exec, start, getDeps);
      ankerl::nanobench::doNotOptimizeAway(result);
    });
  }

  {
    auto edges = makeBinaryTree(12); // 2^12 - 1 = 4095 nodes
    std::size_t nodeCount = countNodes(edges);
    std::set<int> start{0};
    auto getDeps = [&](int n) { return edges[n]; };

    bench.run("Binary tree depth 12 (" + std::to_string(nodeCount) + " nodes) (sequential)", [&] {
      auto result = async::computeClosure(start, getDeps);
      ankerl::nanobench::doNotOptimizeAway(result);
    });

    async::Executor exec(testThreads);
    bench.run("Binary tree depth 12 (" + std::to_string(nodeCount) + " nodes) (parallel)", [&] {
      auto result = async::computeClosureAsync(exec, start, getDeps);
      ankerl::nanobench::doNotOptimizeAway(result);
    });
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Diamond lattice benchmarks (dense connectivity)
  // ───────────────────────────────────────────────────────────────────────────

  {
    auto edges = makeDiamondLattice(5, 10); // 5 layers, 10 nodes each = 50 nodes
    std::size_t nodeCount = countNodes(edges);
    std::set<int> start{0};
    auto getDeps = [&](int n) { return edges[n]; };

    bench.run("Lattice 5x10 (" + std::to_string(nodeCount) + " nodes) (sequential)", [&] {
      auto result = async::computeClosure(start, getDeps);
      ankerl::nanobench::doNotOptimizeAway(result);
    });

    async::Executor exec(testThreads);
    bench.run("Lattice 5x10 (" + std::to_string(nodeCount) + " nodes) (parallel)", [&] {
      auto result = async::computeClosureAsync(exec, start, getDeps);
      ankerl::nanobench::doNotOptimizeAway(result);
    });
  }

  {
    auto edges = makeDiamondLattice(10, 20); // 10 layers, 20 nodes each = 200 nodes
    std::size_t nodeCount = countNodes(edges);
    std::set<int> start{0};
    auto getDeps = [&](int n) { return edges[n]; };

    bench.run("Lattice 10x20 (" + std::to_string(nodeCount) + " nodes) (sequential)", [&] {
      auto result = async::computeClosure(start, getDeps);
      ankerl::nanobench::doNotOptimizeAway(result);
    });

    async::Executor exec(testThreads);
    bench.run("Lattice 10x20 (" + std::to_string(nodeCount) + " nodes) (parallel)", [&] {
      auto result = async::computeClosureAsync(exec, start, getDeps);
      ankerl::nanobench::doNotOptimizeAway(result);
    });
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Random sparse graph benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  {
    auto edges = makeRandomSparse(500, 0.01); // 500 nodes, ~1% edge probability
    std::size_t nodeCount = countNodes(edges);
    std::set<int> start{0};
    auto getDeps = [&](int n) { return edges[n]; };

    bench.run("Random sparse 500 (1% edges) (sequential)", [&] {
      auto result = async::computeClosure(start, getDeps);
      ankerl::nanobench::doNotOptimizeAway(result);
    });

    async::Executor exec(testThreads);
    bench.run("Random sparse 500 (1% edges) (parallel)", [&] {
      auto result = async::computeClosureAsync(exec, start, getDeps);
      ankerl::nanobench::doNotOptimizeAway(result);
    });
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Simulated I/O latency benchmarks (where parallelism really helps)
  // ───────────────────────────────────────────────────────────────────────────

  {
    auto edges = makeWideGraph(20); // 20 children
    std::set<int> start{0};

    // Simulate 100us I/O latency per dependency fetch
    auto getDepsSlow = [&](int n) {
      doWork(100); // 100 microseconds
      return edges[n];
    };

    bench.run("Wide 20 + 100us latency (sequential)", [&] {
      auto result = async::computeClosure(start, getDepsSlow);
      ankerl::nanobench::doNotOptimizeAway(result);
    });

    async::Executor exec(testThreads);
    bench.run("Wide 20 + 100us latency (parallel)", [&] {
      auto result = async::computeClosureAsync(exec, start, getDepsSlow);
      ankerl::nanobench::doNotOptimizeAway(result);
    });
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Scalability benchmarks (varying thread count)
  // ───────────────────────────────────────────────────────────────────────────

  {
    auto edges = makeBinaryTree(10); // ~1K nodes
    std::size_t nodeCount = countNodes(edges);
    std::set<int> start{0};
    auto getDeps = [&](int n) { return edges[n]; };

    for (std::size_t threads : {1UL, 2UL, 4UL, 8UL, 16UL}) {
      if (threads > hwThreads)
        continue;

      async::Executor exec(threads);
      std::string name = "Binary tree " + std::to_string(nodeCount) + " nodes (" +
                         std::to_string(threads) + " threads)";
      bench.run(name.c_str(), [&] {
        auto result = async::computeClosureAsync(exec, start, getDeps);
        ankerl::nanobench::doNotOptimizeAway(result);
      });
    }
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Early termination benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  {
    auto edges = makeLinearChain(10000);
    std::set<int> start{0};
    auto getDeps = [&](int n) { return edges[n]; };

    async::ClosureOptions<int> optionsEarly;
    optionsEarly.withEarlyTermination([](int n) { return n == 100; });

    bench.run("Linear 10K early terminate at 100 (sequential)", [&] {
      auto result = async::computeClosure(start, getDeps, optionsEarly);
      ankerl::nanobench::doNotOptimizeAway(result);
    });

    async::Executor exec(testThreads);
    bench.run("Linear 10K early terminate at 100 (parallel)", [&] {
      auto result = async::computeClosureAsync(exec, start, getDeps, optionsEarly);
      ankerl::nanobench::doNotOptimizeAway(result);
    });

    bench.run("Linear 10K full traversal (sequential)", [&] {
      auto result = async::computeClosure(start, getDeps);
      ankerl::nanobench::doNotOptimizeAway(result);
    });

    bench.run("Linear 10K full traversal (parallel)", [&] {
      auto result = async::computeClosureAsync(exec, start, getDeps);
      ankerl::nanobench::doNotOptimizeAway(result);
    });
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Max items limit benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  {
    auto edges = makeBinaryTree(14); // ~16K nodes
    std::set<int> start{0};
    auto getDeps = [&](int n) { return edges[n]; };

    async::ClosureOptions<int> optionsLimit;
    optionsLimit.withMaxItems(1000);

    bench.run("Binary tree 16K limit 1K (sequential)", [&] {
      auto result = async::computeClosure(start, getDeps, optionsLimit);
      ankerl::nanobench::doNotOptimizeAway(result);
    });

    async::Executor exec(testThreads);
    bench.run("Binary tree 16K limit 1K (parallel)", [&] {
      auto result = async::computeClosureAsync(exec, start, getDeps, optionsLimit);
      ankerl::nanobench::doNotOptimizeAway(result);
    });
  }

  return 0;
}
