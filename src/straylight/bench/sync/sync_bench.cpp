// straylight::nix::sync::sync benchmarks
//
// Benchmarks for synchronized value primitives using nanobench.
// Tests locking overhead, contention patterns, and reader-writer performance.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include <straylight/nix/sync/sync.h>

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

namespace prims = straylight::nix::sync;

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark helpers
// ─────────────────────────────────────────────────────────────────────────────

// Run parallel workload and return total operations
template <typename F>
std::uint64_t run_parallel(std::size_t num_threads, std::size_t ops_per_thread, F&& func) {
  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  std::latch start_latch(static_cast<std::ptrdiff_t>(num_threads));

  for (std::size_t i = 0; i < num_threads; ++i) {
    threads.emplace_back([&, ops_per_thread] {
      start_latch.arrive_and_wait();
      for (std::size_t j = 0; j < ops_per_thread; ++j) {
        func();
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  return static_cast<std::uint64_t>(num_threads * ops_per_thread);
}

} // namespace

int main() {
  ankerl::nanobench::Bench bench;
  bench.title("Sync Benchmarks").warmup(100).minEpochIterations(1000).unit("op");

  // ───────────────────────────────────────────────────────────────────────────
  // Single-threaded lock/unlock overhead
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("sync/lock_unlock/int (single thread)", [&] {
    prims::Sync<int> s(42);
    auto lock = s.lock();
    ankerl::nanobench::doNotOptimizeAway(*lock);
  });

  bench.run("sync/with_lock/int (single thread)", [&] {
    prims::Sync<int> s(42);
    s.with_lock([](int& v) { ankerl::nanobench::doNotOptimizeAway(v); });
  });

  bench.run("sync/try_lock/int (single thread)", [&] {
    prims::Sync<int> s(42);
    if (auto lock = s.try_lock()) {
      ankerl::nanobench::doNotOptimizeAway(**lock);
    }
  });

  bench.run("sync/read_lock/int (single thread)", [&] {
    prims::Sync<int> s(42);
    auto lock = s.read_lock();
    ankerl::nanobench::doNotOptimizeAway(*lock);
  });

  // Baseline: raw mutex
  bench.run("baseline/std::mutex lock_unlock", [&] {
    std::mutex mtx;
    int value = 42;
    {
      std::unique_lock lock(mtx);
      ankerl::nanobench::doNotOptimizeAway(value);
    }
  });

  // ───────────────────────────────────────────────────────────────────────────
  // SharedSync reader-writer performance
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("shared_sync/read_lock/int (single thread)", [&] {
    prims::SharedSync<int> s(42);
    auto lock = s.read_lock();
    ankerl::nanobench::doNotOptimizeAway(*lock);
  });

  bench.run("shared_sync/write_lock/int (single thread)", [&] {
    prims::SharedSync<int> s(42);
    auto lock = s.lock();
    ankerl::nanobench::doNotOptimizeAway(*lock);
  });

  // Baseline: raw shared_mutex
  bench.run("baseline/std::shared_mutex shared_lock", [&] {
    std::shared_mutex mtx;
    int value = 42;
    {
      std::shared_lock lock(mtx);
      ankerl::nanobench::doNotOptimizeAway(value);
    }
  });

  bench.run("baseline/std::shared_mutex unique_lock", [&] {
    std::shared_mutex mtx;
    int value = 42;
    {
      std::unique_lock lock(mtx);
      ankerl::nanobench::doNotOptimizeAway(value);
    }
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Larger data types
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("sync/lock_unlock/vector<int> (single thread)", [&] {
    prims::Sync<std::vector<int>> s(std::in_place, 100, 42);
    auto lock = s.lock();
    ankerl::nanobench::doNotOptimizeAway(lock->size());
  });

  bench.run("sync/lock_unlock/map<string,int> (single thread)", [&] {
    prims::Sync<std::map<std::string, int>> s;
    auto lock = s.lock();
    ankerl::nanobench::doNotOptimizeAway(lock->size());
  });

  bench.run("sync/with_lock/vector push_back", [&] {
    prims::Sync<std::vector<int>> s;
    s.with_lock([](std::vector<int>& v) {
      v.push_back(42);
      if (v.size() > 1000) {
        v.clear();
      }
    });
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Copy operations
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("sync/copy/string (short)", [&] {
    prims::Sync<std::string> s("hello");
    auto copy = s.copy();
    ankerl::nanobench::doNotOptimizeAway(copy);
  });

  std::string long_string(1000, 'x');
  bench.run("sync/copy/string (1KB)", [&] {
    prims::Sync<std::string> s(long_string);
    auto copy = s.copy();
    ankerl::nanobench::doNotOptimizeAway(copy);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Concurrent benchmarks - low contention
  // ───────────────────────────────────────────────────────────────────────────

  // Setup persistent synchronized values for concurrent tests
  prims::Sync<int> shared_counter(0);
  prims::SharedSync<int> shared_read_counter(0);

  constexpr std::size_t concurrent_threads = 4;
  constexpr std::size_t ops_per_thread = 10000;

  bench.run("sync/concurrent_increment/4 threads", [&] {
    run_parallel(concurrent_threads, ops_per_thread,
                 [&] { shared_counter.with_lock([](int& c) { ++c; }); });
  });

  bench.run("shared_sync/concurrent_read/4 threads", [&] {
    run_parallel(concurrent_threads, ops_per_thread, [&] {
      auto lock = shared_read_counter.read_lock();
      ankerl::nanobench::doNotOptimizeAway(*lock);
    });
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Reader-writer ratio benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  // 90% reads, 10% writes - typical for caches
  prims::SharedSync<int> rw_data(42);
  std::atomic<std::size_t> op_counter{0};

  bench.run("shared_sync/90%_read_10%_write/4 threads", [&] {
    run_parallel(concurrent_threads, ops_per_thread, [&] {
      auto op = op_counter.fetch_add(1);
      if (op % 10 == 0) {
        // Write
        auto lock = rw_data.lock();
        ++(*lock);
      } else {
        // Read
        auto lock = rw_data.read_lock();
        ankerl::nanobench::doNotOptimizeAway(*lock);
      }
    });
  });

  // 50% reads, 50% writes
  op_counter.store(0);
  bench.run("shared_sync/50%_read_50%_write/4 threads", [&] {
    run_parallel(concurrent_threads, ops_per_thread, [&] {
      auto op = op_counter.fetch_add(1);
      if (op % 2 == 0) {
        // Write
        auto lock = rw_data.lock();
        ++(*lock);
      } else {
        // Read
        auto lock = rw_data.read_lock();
        ankerl::nanobench::doNotOptimizeAway(*lock);
      }
    });
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Contention scaling benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  // Test with increasing thread counts
  for (std::size_t num_threads : {1, 2, 4, 8}) {
    prims::Sync<int> counter(0);
    std::string name = "sync/contention/" + std::to_string(num_threads) + " threads";

    bench.run(name, [&, num_threads] {
      run_parallel(num_threads, ops_per_thread / num_threads,
                   [&] { counter.with_lock([](int& c) { ++c; }); });
    });
  }

  // SharedSync read scaling (should scale better)
  for (std::size_t num_threads : {1, 2, 4, 8}) {
    prims::SharedSync<int> data(42);
    std::string name = "shared_sync/read_scale/" + std::to_string(num_threads) + " threads";

    bench.run(name, [&, num_threads] {
      run_parallel(num_threads, ops_per_thread / num_threads, [&] {
        auto lock = data.read_lock();
        ankerl::nanobench::doNotOptimizeAway(*lock);
      });
    });
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Real-world workload simulations
  // ───────────────────────────────────────────────────────────────────────────

  // Cache lookup pattern: lookup, miss -> insert
  bench.run("sync/cache_pattern/map lookup+insert", [&] {
    prims::Sync<std::map<int, int>> cache;
    for (int i = 0; i < 100; ++i) {
      cache.with_lock([i](std::map<int, int>& m) {
        auto it = m.find(i);
        if (it == m.end()) {
          m[i] = i * 2;
        }
      });
    }
  });

  // Counters pattern: multiple counters in a struct
  struct Counters {
    std::uint64_t requests = 0;
    std::uint64_t errors = 0;
    std::uint64_t bytes = 0;
  };

  prims::Sync<Counters> counters;
  bench.run("sync/counters_pattern/increment", [&] {
    counters.with_lock([](Counters& c) {
      ++c.requests;
      c.bytes += 1024;
    });
  });

  // ───────────────────────────────────────────────────────────────────────────
  // SyncWithCV benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("sync_with_cv/lock", [&] {
    prims::SyncWithCV<int> data(42);
    auto lock = data.lock();
    ankerl::nanobench::doNotOptimizeAway(*lock);
  });

  bench.run("sync_with_cv/notify_one", [&] {
    prims::SyncWithCV<int> data(42);
    data.with_lock_notify_one([](int& v) { ++v; });
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Comparison: Sync vs raw mutex+value
  // ───────────────────────────────────────────────────────────────────────────

  // Raw mutex pattern
  struct RawProtected {
    std::mutex mutex;
    int value = 0;
  };

  RawProtected raw;
  bench.run("baseline/raw_mutex+value/increment", [&] {
    std::lock_guard lock(raw.mutex);
    ++raw.value;
    ankerl::nanobench::doNotOptimizeAway(raw.value);
  });

  // Sync pattern
  prims::Sync<int> sync_value(0);
  bench.run("sync/increment", [&] {
    sync_value.with_lock([](int& v) {
      ++v;
      ankerl::nanobench::doNotOptimizeAway(v);
    });
  });

  return 0;
}
