// straylight::nix::async benchmarks
//
// Benchmarks for async primitives using nanobench.
// Tests executor, task graph, and parallel algorithms performance.

#include <atomic>
#include <cstdint>
#include <functional>
#include <numeric>
#include <random>
#include <set>
#include <vector>

#include <straylight/nix/async/async/executor.h>
#include <straylight/nix/async/async/parallel.h>
#include <straylight/nix/async/async/task_graph.h>

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

namespace async = straylight::nix::async;

namespace {

// Simple work function that does some computation
volatile uint64_t g_sink = 0;

void do_trivial_work() {
  g_sink = 42;
}

void do_light_work() {
  uint64_t sum = 0;
  for (int i = 0; i < 100; ++i) {
    sum += i * i;
  }
  g_sink = sum;
}

void do_medium_work() {
  uint64_t sum = 0;
  for (int i = 0; i < 10000; ++i) {
    sum += i * i;
  }
  g_sink = sum;
}

void do_heavy_work() {
  std::uint64_t sum = 0;
  for (std::int64_t i = 0; i < 1000000; ++i) {
    sum += static_cast<std::uint64_t>(i * i);
  }
  g_sink = sum;
}

} // namespace

int main() {
  ankerl::nanobench::Bench bench;
  bench.title("Async Primitives Benchmarks").warmup(10).minEpochIterations(10).unit("op");

  // Get thread count
  const size_t hw_threads = std::thread::hardware_concurrency();
  const size_t test_threads = std::min(hw_threads, size_t{8});

  // ───────────────────────────────────────────────────────────────────────────
  // Executor creation benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("Executor create/destroy (2 threads)", [] {
    async::Executor exec(2);
    ankerl::nanobench::doNotOptimizeAway(&exec);
  });

  bench.run("Executor create/destroy (4 threads)", [] {
    async::Executor exec(4);
    ankerl::nanobench::doNotOptimizeAway(&exec);
  });

  bench.run("Executor create/destroy (8 threads)", [] {
    async::Executor exec(8);
    ankerl::nanobench::doNotOptimizeAway(&exec);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Task submission benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  {
    async::Executor exec(test_threads);

    bench.run("async() + get() trivial task", [&] {
      auto future = exec.async(do_trivial_work);
      future.get();
    });

    bench.run("async() + get() light task", [&] {
      auto future = exec.async(do_light_work);
      future.get();
    });

    bench.run("async() + get() medium task", [&] {
      auto future = exec.async(do_medium_work);
      future.get();
    });

    bench.run("silent_async() trivial task", [&] { exec.silent_async(do_trivial_work); });
    exec.wait_for_all();

    // Batch submission
    bench.run("async_batch() 100 trivial tasks", [&] {
      std::vector<std::function<void()>> tasks(100, do_trivial_work);
      auto futures = exec.async_batch(std::move(tasks));
      for (auto& f : futures) {
        f.get();
      }
    });

    bench.run("async_batch() 100 light tasks", [&] {
      std::vector<std::function<void()>> tasks(100, do_light_work);
      auto futures = exec.async_batch(std::move(tasks));
      for (auto& f : futures) {
        f.get();
      }
    });

    bench.run("async_batch() 1000 trivial tasks", [&] {
      std::vector<std::function<void()>> tasks(1000, do_trivial_work);
      auto futures = exec.async_batch(std::move(tasks));
      for (auto& f : futures) {
        f.get();
      }
    });
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Parallel algorithms benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  {
    async::Executor exec(test_threads);

    // Test data
    std::vector<int> small_data(100);
    std::vector<int> medium_data(10000);
    std::vector<int> large_data(1000000);
    std::iota(small_data.begin(), small_data.end(), 0);
    std::iota(medium_data.begin(), medium_data.end(), 0);
    std::iota(large_data.begin(), large_data.end(), 0);

    // parallel_for benchmarks
    bench.run("parallel_for 100 elements (trivial work)", [&] {
      std::atomic<int> sum{0};
      async::parallel_for(exec, small_data, [&](int x) { sum.fetch_add(x); });
      ankerl::nanobench::doNotOptimizeAway(sum.load());
    });

    bench.run("parallel_for 10K elements (trivial work)", [&] {
      std::atomic<int> sum{0};
      async::parallel_for(exec, medium_data, [&](int x) { sum.fetch_add(x); });
      ankerl::nanobench::doNotOptimizeAway(sum.load());
    });

    bench.run("parallel_for 1M elements (trivial work)", [&] {
      std::atomic<long long> sum{0};
      async::parallel_for(exec, large_data, [&](int x) { sum.fetch_add(x); });
      ankerl::nanobench::doNotOptimizeAway(sum.load());
    });

    // Sequential comparison
    bench.run("std::for_each 1M elements (trivial work)", [&] {
      long long sum = 0;
      std::for_each(large_data.begin(), large_data.end(), [&](int x) { sum += x; });
      ankerl::nanobench::doNotOptimizeAway(sum);
    });

    // parallel_reduce benchmarks
    bench.run("parallel_reduce 10K elements", [&] {
      int sum = async::parallel_reduce(exec, medium_data, 0, std::plus<>{});
      ankerl::nanobench::doNotOptimizeAway(sum);
    });

    bench.run("parallel_reduce 1M elements", [&] {
      long long sum = async::parallel_reduce(exec, large_data, 0LL, std::plus<>{});
      ankerl::nanobench::doNotOptimizeAway(sum);
    });

    bench.run("std::accumulate 1M elements", [&] {
      long long sum = std::accumulate(large_data.begin(), large_data.end(), 0LL);
      ankerl::nanobench::doNotOptimizeAway(sum);
    });

    // parallel_filter benchmarks
    bench.run("parallel_filter 10K (50% match)", [&] {
      auto result = async::parallel_filter(exec, medium_data, [](int x) { return x % 2 == 0; });
      ankerl::nanobench::doNotOptimizeAway(result);
    });

    bench.run("parallel_filter 1M (50% match)", [&] {
      auto result = async::parallel_filter(exec, large_data, [](int x) { return x % 2 == 0; });
      ankerl::nanobench::doNotOptimizeAway(result);
    });

    // parallel_count_if benchmarks
    bench.run("parallel_count_if 1M elements", [&] {
      auto count = async::parallel_count_if(exec, large_data, [](int x) { return x % 3 == 0; });
      ankerl::nanobench::doNotOptimizeAway(count);
    });

    bench.run("std::count_if 1M elements", [&] {
      auto count =
          std::count_if(large_data.begin(), large_data.end(), [](int x) { return x % 3 == 0; });
      ankerl::nanobench::doNotOptimizeAway(count);
    });

    // parallel_any benchmarks
    bench.run("parallel_any 1M (early match)", [&] {
      bool found = async::parallel_any(exec, large_data, [](int x) { return x == 50; });
      ankerl::nanobench::doNotOptimizeAway(found);
    });

    bench.run("parallel_any 1M (late match)", [&] {
      bool found = async::parallel_any(exec, large_data, [](int x) { return x == 999999; });
      ankerl::nanobench::doNotOptimizeAway(found);
    });

    bench.run("parallel_any 1M (no match)", [&] {
      bool found = async::parallel_any(exec, large_data, [](int x) { return x < 0; });
      ankerl::nanobench::doNotOptimizeAway(found);
    });
  }

  // ───────────────────────────────────────────────────────────────────────────
  // TaskGraph benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  {
    async::Executor exec(test_threads);

    // Linear chain
    bench.run("TaskGraph linear chain 10 nodes", [&] {
      async::TaskGraph<int> graph;
      for (int i = 0; i < 10; ++i) {
        graph.add_node(i, do_light_work);
        if (i > 0) {
          graph.add_edge(i - 1, i);
        }
      }
      graph.execute(exec);
    });

    bench.run("TaskGraph linear chain 100 nodes", [&] {
      async::TaskGraph<int> graph;
      for (int i = 0; i < 100; ++i) {
        graph.add_node(i, do_trivial_work);
        if (i > 0) {
          graph.add_edge(i - 1, i);
        }
      }
      graph.execute(exec);
    });

    // Wide graph (all independent)
    bench.run("TaskGraph 100 independent nodes", [&] {
      async::TaskGraph<int> graph;
      for (int i = 0; i < 100; ++i) {
        graph.add_node(i, do_light_work);
      }
      graph.execute(exec);
    });

    bench.run("TaskGraph 1000 independent nodes", [&] {
      async::TaskGraph<int> graph;
      for (int i = 0; i < 1000; ++i) {
        graph.add_node(i, do_trivial_work);
      }
      graph.execute(exec);
    });

    // Diamond pattern (fan-out / fan-in)
    bench.run("TaskGraph diamond 100 nodes", [&] {
      async::TaskGraph<int> graph;
      // Root node
      graph.add_node(0, do_trivial_work);
      // Middle layer (fan-out)
      for (int i = 1; i <= 98; ++i) {
        graph.add_node(i, do_trivial_work);
        graph.add_edge(0, i);
      }
      // Final node (fan-in)
      graph.add_node(99, do_trivial_work);
      for (int i = 1; i <= 98; ++i) {
        graph.add_edge(i, 99);
      }
      graph.execute(exec);
    });
  }

  // ───────────────────────────────────────────────────────────────────────────
  // process_graph benchmarks (dynamic DAG)
  // ───────────────────────────────────────────────────────────────────────────

  {
    async::Executor exec(test_threads);

    // No dependencies
    bench.run("process_graph 100 nodes (no deps)", [&] {
      std::set<int> nodes;
      for (int i = 0; i < 100; ++i) {
        nodes.insert(i);
      }
      async::process_graph<int>(
          nodes, [](const int&) { return std::set<int>{}; }, [](const int&) { do_light_work(); },
          exec);
    });

    // Linear dependencies (worst case - no parallelism)
    bench.run("process_graph 100 linear deps", [&] {
      std::set<int> nodes;
      for (int i = 0; i < 100; ++i) {
        nodes.insert(i);
      }
      async::process_graph<int>(
          nodes,
          [](const int& n) -> std::set<int> {
            if (n > 0) {
              return {n - 1};
            }
            return {};
          },
          [](const int&) { do_trivial_work(); }, exec);
    });

    // Two-level tree (root + children)
    bench.run("process_graph 100 nodes (2-level tree)", [&] {
      std::set<int> nodes;
      for (int i = 0; i < 100; ++i) {
        nodes.insert(i);
      }
      async::process_graph<int>(
          nodes,
          [](const int& n) -> std::set<int> {
            if (n > 0) {
              return {0}; // All depend on root
            }
            return {};
          },
          [](const int&) { do_light_work(); }, exec);
    });
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Scalability benchmarks (varying thread count)
  // ───────────────────────────────────────────────────────────────────────────

  std::vector<int> scale_data(100000);
  std::iota(scale_data.begin(), scale_data.end(), 0);

  for (size_t threads : {1, 2, 4, 8}) {
    if (threads > hw_threads) {
      continue;
    }

    async::Executor exec(threads);
    std::string name = "parallel_reduce 100K (" + std::to_string(threads) + " threads)";
    bench.run(name.c_str(), [&] {
      int sum = async::parallel_reduce(exec, scale_data, 0, std::plus<>{});
      ankerl::nanobench::doNotOptimizeAway(sum);
    });
  }

  return 0;
}
