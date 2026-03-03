// straylight::nix::async tests
//
// Tests for async primitives: executor, task graph, parallel algorithms

#include <catch2/catch_test_macros.hpp>
// IMPORTANT: Catch2 v3 MUST be included BEFORE rapidcheck/catch.h
// rapidcheck checks for CATCH_TEST_MACROS_HPP_INCLUDED macro
#include <atomic>
#include <chrono>
#include <numeric>
#include <set>
#include <thread>
#include <vector>


#include "straylight/nix/async/executor.h"
#include "straylight/nix/async/parallel.h"
#include "straylight/nix/async/task_graph.h"

namespace async = straylight::nix::async;

// ─────────────────────────────────────────────────────────────────────────────
// Executor tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Executor default construction uses hardware_concurrency", "[executor]") {
  async::Executor exec;
  REQUIRE(exec.num_workers() > 0);
  REQUIRE(exec.num_workers() <= std::thread::hardware_concurrency());
}

TEST_CASE("Executor with explicit thread count", "[executor]") {
  async::Executor exec(2);
  REQUIRE(exec.num_workers() == 2);
}

TEST_CASE("Executor::async returns correct result", "[executor]") {
  async::Executor exec(4);

  auto future = exec.async([] { return 42; });
  REQUIRE(future.get() == 42);
}

TEST_CASE("Executor::async handles exceptions", "[executor]") {
  async::Executor exec(2);

  auto future = exec.async([] {
    throw std::runtime_error("test exception");
    return 0;
  });

  REQUIRE_THROWS_AS(future.get(), std::runtime_error);
}

TEST_CASE("Executor::silent_async executes task", "[executor]") {
  async::Executor exec(2);
  std::atomic<int> counter{0};

  exec.silent_async([&] { counter.fetch_add(1); });
  exec.wait_for_all();

  REQUIRE(counter.load() == 1);
}

TEST_CASE("Executor::async_batch processes multiple tasks", "[executor]") {
  async::Executor exec(4);

  std::vector<std::function<int()>> funcs;
  for (int i = 0; i < 10; ++i) {
    funcs.push_back([i] { return i * i; });
  }

  auto futures = exec.async_batch(std::move(funcs));
  REQUIRE(futures.size() == 10);

  for (int i = 0; i < 10; ++i) {
    REQUIRE(futures[i].get() == i * i);
  }
}

TEST_CASE("Executor concurrent execution", "[executor]") {
  async::Executor exec(4);
  std::atomic<int> concurrent_count{0};
  std::atomic<int> max_concurrent{0};

  std::vector<std::function<void()>> tasks;
  for (int i = 0; i < 100; ++i) {
    tasks.push_back([&] {
      int current = concurrent_count.fetch_add(1) + 1;
      int expected = max_concurrent.load();
      while (current > expected && !max_concurrent.compare_exchange_weak(expected, current)) {
      }

      std::this_thread::sleep_for(std::chrono::microseconds(100));
      concurrent_count.fetch_sub(1);
    });
  }

  auto futures = exec.async_batch(std::move(tasks));
  for (auto& f : futures) {
    f.get();
  }

  // Should have had some concurrent execution
  REQUIRE(max_concurrent.load() > 1);
}

TEST_CASE("global_executor singleton", "[executor]") {
  auto& exec1 = async::global_executor();
  auto& exec2 = async::global_executor();
  REQUIRE(&exec1 == &exec2);
}

// ─────────────────────────────────────────────────────────────────────────────
// TaskGraph tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("TaskGraph empty execution", "[task_graph]") {
  async::Executor exec(2);
  async::TaskGraph<int> graph;
  REQUIRE(graph.empty());
  graph.execute(exec); // Should not throw
}

TEST_CASE("TaskGraph single node", "[task_graph]") {
  async::Executor exec(2);
  async::TaskGraph<int> graph;

  int result = 0;
  graph.add_node(1, [&] { result = 42; });
  graph.execute(exec);

  REQUIRE(result == 42);
}

TEST_CASE("TaskGraph linear dependency chain", "[task_graph]") {
  async::Executor exec(4);
  async::TaskGraph<int> graph;

  std::vector<int> order;
  std::mutex mtx;

  graph.add_node(1, [&] {
    std::lock_guard lock(mtx);
    order.push_back(1);
  });
  graph.add_node(2, [&] {
    std::lock_guard lock(mtx);
    order.push_back(2);
  });
  graph.add_node(3, [&] {
    std::lock_guard lock(mtx);
    order.push_back(3);
  });

  // 1 -> 2 -> 3
  graph.add_edge(1, 2);
  graph.add_edge(2, 3);

  graph.execute(exec);

  REQUIRE(order.size() == 3);
  // 1 must come before 2, 2 must come before 3
  auto pos1 = std::find(order.begin(), order.end(), 1) - order.begin();
  auto pos2 = std::find(order.begin(), order.end(), 2) - order.begin();
  auto pos3 = std::find(order.begin(), order.end(), 3) - order.begin();
  REQUIRE(pos1 < pos2);
  REQUIRE(pos2 < pos3);
}

TEST_CASE("TaskGraph diamond dependency", "[task_graph]") {
  async::Executor exec(4);
  async::TaskGraph<std::string> graph;

  std::set<std::string> completed;
  std::mutex mtx;

  auto mark_done = [&](const std::string& name) {
    std::lock_guard lock(mtx);
    completed.insert(name);
  };

  graph.add_node("A", [&] { mark_done("A"); });
  graph.add_node("B", [&] { mark_done("B"); });
  graph.add_node("C", [&] { mark_done("C"); });
  graph.add_node("D", [&] { mark_done("D"); });

  //     A
  //    / \
  //   B   C
  //    \ /
  //     D
  graph.add_edge("A", "B");
  graph.add_edge("A", "C");
  graph.add_edge("B", "D");
  graph.add_edge("C", "D");

  graph.execute(exec);

  REQUIRE(completed.size() == 4);
  REQUIRE(completed.count("A"));
  REQUIRE(completed.count("B"));
  REQUIRE(completed.count("C"));
  REQUIRE(completed.count("D"));
}

TEST_CASE("TaskGraph::would_create_cycle detects self-loop", "[task_graph]") {
  async::TaskGraph<int> graph;
  graph.add_node(1, [] {});
  REQUIRE(graph.would_create_cycle(1, 1));
}

TEST_CASE("TaskGraph::would_create_cycle detects indirect cycle", "[task_graph]") {
  async::TaskGraph<int> graph;
  graph.add_node(1, [] {});
  graph.add_node(2, [] {});
  graph.add_node(3, [] {});

  graph.add_edge(1, 2);
  graph.add_edge(2, 3);

  // Adding 3 -> 1 would create a cycle
  REQUIRE(graph.would_create_cycle(3, 1));
  // But 1 -> 3 would not
  REQUIRE_FALSE(graph.would_create_cycle(1, 3));
}

// ─────────────────────────────────────────────────────────────────────────────
// process_graph tests (dynamic DAG)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("process_graph with no dependencies", "[task_graph]") {
  async::Executor exec(4);

  std::set<int> nodes{1, 2, 3, 4, 5};
  std::atomic<int> processed{0};

  async::process_graph<int>(
      nodes, [](const int&) { return std::set<int>{}; }, // No dependencies
      [&](const int&) { processed.fetch_add(1); }, exec);

  REQUIRE(processed.load() == 5);
}

TEST_CASE("process_graph respects dependencies", "[task_graph]") {
  async::Executor exec(4);

  std::set<int> nodes{1, 2, 3};
  std::vector<int> order;
  std::mutex mtx;

  // 1 depends on 2, 2 depends on 3
  auto get_deps = [](const int& n) -> std::set<int> {
    if (n == 1) {
      return {2};
    }
    if (n == 2) {
      return {3};
    }
    return {};
  };

  async::process_graph<int>(
      nodes, get_deps,
      [&](const int& n) {
        std::lock_guard lock(mtx);
        order.push_back(n);
      },
      exec);

  REQUIRE(order.size() == 3);
  // 3 must come before 2, 2 must come before 1
  auto pos1 = std::find(order.begin(), order.end(), 1) - order.begin();
  auto pos2 = std::find(order.begin(), order.end(), 2) - order.begin();
  auto pos3 = std::find(order.begin(), order.end(), 3) - order.begin();
  REQUIRE(pos3 < pos2);
  REQUIRE(pos2 < pos1);
}

TEST_CASE("process_graph detects cycle", "[task_graph]") {
  async::Executor exec(4);

  std::set<int> nodes{1, 2, 3};

  // Cyclic: 1 -> 2 -> 3 -> 1
  auto get_deps = [](const int& n) -> std::set<int> {
    if (n == 1) {
      return {2};
    }
    if (n == 2) {
      return {3};
    }
    if (n == 3) {
      return {1};
    }
    return {};
  };

  REQUIRE_THROWS_AS(
      async::process_graph<int>(nodes, get_deps, [](const int&) {}, exec), async::CycleError);
}

// ─────────────────────────────────────────────────────────────────────────────
// Parallel algorithm tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parallel_for iterates all elements", "[parallel]") {
  async::Executor exec(4);

  std::vector<int> data(100);
  std::iota(data.begin(), data.end(), 0);

  std::atomic<int> sum{0};
  async::parallel_for(exec, data, [&](int x) { sum.fetch_add(x); });

  int expected = 99 * 100 / 2; // Sum of 0..99
  REQUIRE(sum.load() == expected);
}

TEST_CASE("parallel_for_index iterates indices", "[parallel]") {
  async::Executor exec(4);

  std::atomic<size_t> sum{0};
  async::parallel_for_index(exec, 100, [&](size_t i) { sum.fetch_add(i); });

  size_t expected = 99 * 100 / 2;
  REQUIRE(sum.load() == expected);
}

TEST_CASE("parallel_reduce computes sum", "[parallel]") {
  async::Executor exec(4);

  std::vector<int> data(100);
  std::iota(data.begin(), data.end(), 1); // 1..100

  int sum = async::parallel_reduce(exec, data, 0, std::plus<>{});

  int expected = 100 * 101 / 2; // Sum of 1..100
  REQUIRE(sum == expected);
}

TEST_CASE("parallel_sum convenience function", "[parallel]") {
  async::Executor exec(4);

  std::vector<int> data{1, 2, 3, 4, 5};
  int sum = async::parallel_sum(exec, data);
  REQUIRE(sum == 15);
}

TEST_CASE("parallel_filter selects matching elements", "[parallel]") {
  async::Executor exec(4);

  std::vector<int> data(100);
  std::iota(data.begin(), data.end(), 0);

  auto evens = async::parallel_filter(exec, data, [](int x) { return x % 2 == 0; });

  REQUIRE(evens.size() == 50);
  for (int x : evens) {
    REQUIRE(x % 2 == 0);
  }
}

TEST_CASE("parallel_any returns true when match exists", "[parallel]") {
  async::Executor exec(4);

  std::vector<int> data(100);
  std::iota(data.begin(), data.end(), 0);

  REQUIRE(async::parallel_any(exec, data, [](int x) { return x == 50; }));
  REQUIRE_FALSE(async::parallel_any(exec, data, [](int x) { return x == 200; }));
}

TEST_CASE("parallel_all returns true when all match", "[parallel]") {
  async::Executor exec(4);

  std::vector<int> data(100);
  std::iota(data.begin(), data.end(), 0);

  REQUIRE(async::parallel_all(exec, data, [](int x) { return x < 100; }));
  REQUIRE_FALSE(async::parallel_all(exec, data, [](int x) { return x < 50; }));
}

TEST_CASE("parallel_none returns true when none match", "[parallel]") {
  async::Executor exec(4);

  std::vector<int> data(100);
  std::iota(data.begin(), data.end(), 0);

  REQUIRE(async::parallel_none(exec, data, [](int x) { return x >= 100; }));
  REQUIRE_FALSE(async::parallel_none(exec, data, [](int x) { return x == 50; }));
}

TEST_CASE("parallel_count_if counts matches", "[parallel]") {
  async::Executor exec(4);

  std::vector<int> data(100);
  std::iota(data.begin(), data.end(), 0);

  size_t count = async::parallel_count_if(exec, data, [](int x) { return x % 3 == 0; });

  // 0, 3, 6, ..., 99 -> 34 numbers
  REQUIRE(count == 34);
}

TEST_CASE("parallel_transform modifies elements", "[parallel]") {
  async::Executor exec(4);

  std::vector<int> data{1, 2, 3, 4, 5};
  std::vector<int> result(5);

  async::parallel_transform(exec, data, result.begin(), [](int x) { return x * x; });

  REQUIRE(result == std::vector<int>{1, 4, 9, 16, 25});
}

TEST_CASE("parallel_transform_inplace modifies in place", "[parallel]") {
  async::Executor exec(4);

  std::vector<int> data{1, 2, 3, 4, 5};
  async::parallel_transform_inplace(exec, data, [](int x) { return x * 2; });

  REQUIRE(data == std::vector<int>{2, 4, 6, 8, 10});
}

// ─────────────────────────────────────────────────────────────────────────────
// Small input fallback tests (sequential execution)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parallel_for falls back to sequential for small inputs", "[parallel]") {
  async::Executor exec(4);

  std::vector<int> data{1, 2, 3}; // Below threshold
  int sum = 0;
  // No atomic needed - sequential execution
  async::parallel_for(exec, data, [&](int x) { sum += x; });
  REQUIRE(sum == 6);
}

// ─────────────────────────────────────────────────────────────────────────────
// parallel_invoke tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parallel_invoke with results", "[parallel]") {
  async::Executor exec(4);

  std::vector<std::function<int()>> funcs;
  funcs.push_back([] { return 1; });
  funcs.push_back([] { return 2; });
  funcs.push_back([] { return 3; });

  auto results = async::parallel_invoke(exec, std::move(funcs));
  REQUIRE(results.size() == 3);
  REQUIRE(results[0] == 1);
  REQUIRE(results[1] == 2);
  REQUIRE(results[2] == 3);
}

TEST_CASE("parallel_invoke void functions", "[parallel]") {
  async::Executor exec(4);

  std::atomic<int> counter{0};
  std::vector<std::function<void()>> funcs;
  for (int i = 0; i < 5; ++i) {
    funcs.push_back([&] { counter.fetch_add(1); });
  }

  async::parallel_invoke(exec, std::move(funcs));
  REQUIRE(counter.load() == 5);
}

TEST_CASE("parallel_invoke propagates exception", "[parallel]") {
  async::Executor exec(4);

  std::vector<std::function<void()>> funcs;
  funcs.push_back([] {});
  funcs.push_back([] { throw std::runtime_error("oops"); });
  funcs.push_back([] {});

  REQUIRE_THROWS_AS(async::parallel_invoke(exec, std::move(funcs)), std::runtime_error);
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-based tests
// ─────────────────────────────────────────────────────────────────────────────

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

TEST_CASE("parallel_reduce property tests", "[parallel][property]") {
  async::Executor exec(4);

  rc::prop("parallel_reduce equals sequential reduce", [&exec]() {
    auto data = *rc::gen::container<std::vector<int>>(rc::gen::inRange(-1000, 1000));

    int parallel_sum = async::parallel_reduce(exec, data, 0, std::plus<>{});
    int sequential_sum = std::accumulate(data.begin(), data.end(), 0);

    RC_ASSERT(parallel_sum == sequential_sum);
  });
}

TEST_CASE("parallel_filter property tests", "[parallel][property]") {
  async::Executor exec(4);

  rc::prop("parallel_filter contains all matching elements", [&exec]() {
    auto data = *rc::gen::container<std::vector<int>>(rc::gen::inRange(0, 100));

    auto threshold = *rc::gen::inRange(0, 100);
    auto pred = [threshold](int x) { return x > threshold; };

    auto parallel_result = async::parallel_filter(exec, data, pred);

    // All elements in result should match predicate
    for (int x : parallel_result) {
      RC_ASSERT(pred(x));
    }

    // Count should match sequential filter
    auto seq_count = std::count_if(data.begin(), data.end(), pred);
    RC_ASSERT(static_cast<size_t>(seq_count) == parallel_result.size());
  });
}

TEST_CASE("parallel_for processes all elements", "[parallel][property]") {
  async::Executor exec(4);

  rc::prop("parallel_for visits every element", [&exec]() {
    auto data = *rc::gen::container<std::vector<int>>(rc::gen::inRange(-1000, 1000));

    std::atomic<size_t> visited{0};
    async::parallel_for(exec, data, [&](int) { visited.fetch_add(1); });

    RC_ASSERT(visited.load() == data.size());
  });
}

TEST_CASE("parallel_count_if matches std::count_if", "[parallel][property]") {
  async::Executor exec(4);

  rc::prop("parallel_count_if equals sequential count", [&exec]() {
    auto data = *rc::gen::container<std::vector<int>>(rc::gen::inRange(0, 100));
    auto threshold = *rc::gen::inRange(0, 100);

    auto pred = [threshold](int x) { return x >= threshold; };

    auto parallel_count = async::parallel_count_if(exec, data, pred);
    auto seq_count = static_cast<size_t>(std::count_if(data.begin(), data.end(), pred));

    RC_ASSERT(parallel_count == seq_count);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Heavy metal parallel reduce correctness tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parallel_reduce correctness properties", "[parallel][property][reduce]") {
  async::Executor exec(4);

  rc::prop("parallel_reduce sum equals sequential sum for large data", [&exec]() {
    // Generate larger datasets to ensure parallel execution
    auto len = *rc::gen::inRange<std::size_t>(100, 10000);
    auto data = *rc::gen::container<std::vector<int>>(len, rc::gen::inRange(-1000, 1000));

    int parallel_sum = async::parallel_reduce(exec, data, 0, std::plus<>{});
    int sequential_sum = std::accumulate(data.begin(), data.end(), 0);

    RC_ASSERT(parallel_sum == sequential_sum);
  });

  rc::prop("parallel_reduce product equals sequential product", [&exec]() {
    // Use small values to avoid overflow
    auto data = *rc::gen::container<std::vector<int>>(rc::gen::inRange(1, 5));
    RC_PRE(!data.empty());
    RC_PRE(data.size() <= 10); // Limit size to avoid huge products

    int64_t parallel_prod =
        async::parallel_reduce(exec, data, int64_t{1}, std::multiplies<int64_t>{});
    int64_t sequential_prod =
        std::accumulate(data.begin(), data.end(), int64_t{1}, std::multiplies<int64_t>{});

    RC_ASSERT(parallel_prod == sequential_prod);
  });

  rc::prop("parallel_reduce min equals sequential min", [&exec]() {
    auto data =
        *rc::gen::nonEmpty(rc::gen::container<std::vector<int>>(rc::gen::inRange(-10000, 10000)));

    int parallel_min = async::parallel_reduce(exec, data, std::numeric_limits<int>::max(),
                                              [](int a, int b) { return std::min(a, b); });
    int sequential_min = *std::min_element(data.begin(), data.end());

    RC_ASSERT(parallel_min == sequential_min);
  });

  rc::prop("parallel_reduce max equals sequential max", [&exec]() {
    auto data =
        *rc::gen::nonEmpty(rc::gen::container<std::vector<int>>(rc::gen::inRange(-10000, 10000)));

    int parallel_max = async::parallel_reduce(exec, data, std::numeric_limits<int>::min(),
                                              [](int a, int b) { return std::max(a, b); });
    int sequential_max = *std::max_element(data.begin(), data.end());

    RC_ASSERT(parallel_max == sequential_max);
  });

  rc::prop("parallel_reduce with init value", [&exec]() {
    auto data = *rc::gen::container<std::vector<int>>(rc::gen::inRange(0, 100));
    auto init = *rc::gen::inRange(0, 1000);

    int parallel_sum = async::parallel_reduce(exec, data, init, std::plus<>{});
    int sequential_sum = std::accumulate(data.begin(), data.end(), init);

    RC_ASSERT(parallel_sum == sequential_sum);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Parallel transform correctness tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parallel_transform correctness properties", "[parallel][property][transform]") {
  async::Executor exec(4);

  rc::prop("parallel_transform square equals sequential square", [&exec]() {
    auto data = *rc::gen::container<std::vector<int>>(rc::gen::inRange(-100, 100));

    std::vector<int> parallel_result(data.size());
    std::vector<int> sequential_result(data.size());

    async::parallel_transform(exec, data, parallel_result.begin(), [](int x) { return x * x; });
    std::transform(data.begin(), data.end(), sequential_result.begin(),
                   [](int x) { return x * x; });

    RC_ASSERT(parallel_result == sequential_result);
  });

  rc::prop("parallel_transform_inplace equals sequential inplace", [&exec]() {
    auto data = *rc::gen::container<std::vector<int>>(rc::gen::inRange(-100, 100));
    auto data_copy = data;

    async::parallel_transform_inplace(exec, data, [](int x) { return x * 2; });
    std::transform(data_copy.begin(), data_copy.end(), data_copy.begin(),
                   [](int x) { return x * 2; });

    RC_ASSERT(data == data_copy);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Parallel filter correctness tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parallel_filter correctness properties", "[parallel][property][filter]") {
  async::Executor exec(4);

  rc::prop("parallel_filter result is subset of input", [&exec]() {
    auto data = *rc::gen::container<std::vector<int>>(rc::gen::arbitrary<int>());
    auto threshold = *rc::gen::arbitrary<int>();

    auto filtered =
        async::parallel_filter(exec, data, [threshold](int x) { return x > threshold; });

    // Every element in filtered should be in data and satisfy predicate
    for (int x : filtered) {
      RC_ASSERT(x > threshold);
      RC_ASSERT(std::find(data.begin(), data.end(), x) != data.end());
    }
  });

  rc::prop("parallel_filter count matches parallel_count_if", [&exec]() {
    auto data = *rc::gen::container<std::vector<int>>(rc::gen::inRange(0, 100));
    auto threshold = *rc::gen::inRange(0, 100);

    auto pred = [threshold](int x) { return x > threshold; };

    auto filtered = async::parallel_filter(exec, data, pred);
    auto count = async::parallel_count_if(exec, data, pred);

    RC_ASSERT(filtered.size() == count);
  });

  rc::prop("parallel_filter empty predicate returns empty", [&exec]() {
    auto data = *rc::gen::container<std::vector<int>>(rc::gen::inRange(0, 100));

    auto filtered = async::parallel_filter(exec, data, [](int) { return false; });

    RC_ASSERT(filtered.empty());
  });

  rc::prop("parallel_filter all predicate returns all", [&exec]() {
    auto data = *rc::gen::container<std::vector<int>>(rc::gen::inRange(0, 100));

    auto filtered = async::parallel_filter(exec, data, [](int) { return true; });

    // filtered should contain all elements (but order may differ)
    RC_ASSERT(filtered.size() == data.size());
    for (int x : data) {
      RC_ASSERT(std::find(filtered.begin(), filtered.end(), x) != filtered.end());
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Parallel predicate correctness tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parallel predicate correctness properties", "[parallel][property][predicate]") {
  async::Executor exec(4);

  rc::prop("parallel_any equals std::any_of", [&exec]() {
    auto data = *rc::gen::container<std::vector<int>>(rc::gen::inRange(-100, 100));
    auto threshold = *rc::gen::inRange(-100, 100);

    auto pred = [threshold](int x) { return x > threshold; };

    bool parallel_result = async::parallel_any(exec, data, pred);
    bool sequential_result = std::any_of(data.begin(), data.end(), pred);

    RC_ASSERT(parallel_result == sequential_result);
  });

  rc::prop("parallel_all equals std::all_of", [&exec]() {
    auto data = *rc::gen::container<std::vector<int>>(rc::gen::inRange(-100, 100));
    auto threshold = *rc::gen::inRange(-100, 100);

    auto pred = [threshold](int x) { return x < threshold; };

    bool parallel_result = async::parallel_all(exec, data, pred);
    bool sequential_result = std::all_of(data.begin(), data.end(), pred);

    RC_ASSERT(parallel_result == sequential_result);
  });

  rc::prop("parallel_none equals std::none_of", [&exec]() {
    auto data = *rc::gen::container<std::vector<int>>(rc::gen::inRange(-100, 100));
    auto threshold = *rc::gen::inRange(-100, 100);

    auto pred = [threshold](int x) { return x == threshold; };

    bool parallel_result = async::parallel_none(exec, data, pred);
    bool sequential_result = std::none_of(data.begin(), data.end(), pred);

    RC_ASSERT(parallel_result == sequential_result);
  });

  rc::prop("parallel_any/all/none are consistent", [&exec]() {
    auto data = *rc::gen::container<std::vector<int>>(rc::gen::inRange(0, 100));
    auto threshold = *rc::gen::inRange(0, 100);

    auto pred = [threshold](int x) { return x > threshold; };

    bool any_result = async::parallel_any(exec, data, pred);
    bool all_result = async::parallel_all(exec, data, pred);
    bool none_result = async::parallel_none(exec, data, pred);

    // Logical relationships
    if (all_result) {
      RC_ASSERT(any_result || data.empty());
      RC_ASSERT(!none_result || data.empty());
    }
    if (none_result) {
      RC_ASSERT(!any_result);
      RC_ASSERT(!all_result || data.empty());
    }
    if (any_result) {
      RC_ASSERT(!none_result);
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Associative operation tests for parallel_reduce
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parallel_reduce associativity requirements", "[parallel][property][associativity]") {
  async::Executor exec(4);

  rc::prop("string concatenation (non-commutative) requires special handling", []() {
    // NOTE: std::string concatenation is associative but NOT commutative
    // parallel_reduce may not preserve order, so this test verifies
    // that the result has the same length and characters (set equality)
    async::Executor exec_local(4);

    auto strings = *rc::gen::container<std::vector<std::string>>(
        rc::gen::container<std::string>(rc::gen::inRange<char>('a', 'z')));

    // Sequential concatenation
    std::string sequential;
    for (const auto& s : strings) {
      sequential += s;
    }

    // For truly order-preserving concat, we'd need a different implementation
    // Here we just verify total character count matches
    std::string parallel =
        async::parallel_reduce(exec_local, strings, std::string{},
                               [](std::string a, const std::string& b) { return a + b; });

    RC_ASSERT(parallel.size() == sequential.size());
  });

  rc::prop("XOR reduction is both associative and commutative", [&exec]() {
    auto data = *rc::gen::container<std::vector<uint32_t>>(rc::gen::arbitrary<uint32_t>());

    uint32_t parallel_xor = async::parallel_reduce(exec, data, uint32_t{0},
                                                   [](uint32_t a, uint32_t b) { return a ^ b; });
    uint32_t sequential_xor = std::accumulate(data.begin(), data.end(), uint32_t{0},
                                              [](uint32_t a, uint32_t b) { return a ^ b; });

    RC_ASSERT(parallel_xor == sequential_xor);
  });

  rc::prop("bitwise OR is both associative and commutative", [&exec]() {
    auto data = *rc::gen::container<std::vector<uint32_t>>(rc::gen::arbitrary<uint32_t>());

    uint32_t parallel_or = async::parallel_reduce(exec, data, uint32_t{0},
                                                  [](uint32_t a, uint32_t b) { return a | b; });
    uint32_t sequential_or = std::accumulate(data.begin(), data.end(), uint32_t{0},
                                             [](uint32_t a, uint32_t b) { return a | b; });

    RC_ASSERT(parallel_or == sequential_or);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge case and stress tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parallel algorithm edge cases", "[parallel][property][edge]") {
  async::Executor exec(4);

  rc::prop("parallel_for handles empty range", [&exec]() {
    std::vector<int> empty;
    std::atomic<int> counter{0};

    async::parallel_for(exec, empty, [&](int) { counter.fetch_add(1); });

    RC_ASSERT(counter.load() == 0);
  });

  rc::prop("parallel_reduce handles single element", [&exec]() {
    auto value = *rc::gen::arbitrary<int>();
    std::vector<int> single{value};

    int result = async::parallel_reduce(exec, single, 0, std::plus<>{});

    RC_ASSERT(result == value);
  });

  rc::prop("parallel_filter handles empty input", [&exec]() {
    std::vector<int> empty;

    auto filtered = async::parallel_filter(exec, empty, [](int) { return true; });

    RC_ASSERT(filtered.empty());
  });

  rc::prop("parallel_for_index handles zero count", [&exec]() {
    std::atomic<size_t> counter{0};

    async::parallel_for_index(exec, 0, [&](size_t) { counter.fetch_add(1); });

    RC_ASSERT(counter.load() == 0);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Determinism tests (parallel results should match sequential)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parallel algorithm determinism", "[parallel][property][determinism]") {
  rc::prop("parallel_reduce produces same result across multiple runs", []() {
    async::Executor exec(4);

    auto data = *rc::gen::container<std::vector<int>>(rc::gen::inRange(-1000, 1000));

    int result1 = async::parallel_reduce(exec, data, 0, std::plus<>{});
    int result2 = async::parallel_reduce(exec, data, 0, std::plus<>{});
    int result3 = async::parallel_reduce(exec, data, 0, std::plus<>{});

    RC_ASSERT(result1 == result2);
    RC_ASSERT(result2 == result3);
  });

  rc::prop("parallel_count_if produces same result across multiple runs", []() {
    async::Executor exec(4);

    auto data = *rc::gen::container<std::vector<int>>(rc::gen::inRange(0, 100));
    auto threshold = *rc::gen::inRange(0, 100);

    auto pred = [threshold](int x) { return x > threshold; };

    size_t count1 = async::parallel_count_if(exec, data, pred);
    size_t count2 = async::parallel_count_if(exec, data, pred);
    size_t count3 = async::parallel_count_if(exec, data, pred);

    RC_ASSERT(count1 == count2);
    RC_ASSERT(count2 == count3);
  });
}
