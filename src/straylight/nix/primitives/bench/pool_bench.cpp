// straylight::nix::primitives::pool benchmarks
//
// Benchmarks for thread-safe resource pool using nanobench.
// Tests acquisition latency, throughput, and contention scenarios.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

#include "../pool.h"

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

namespace pool = straylight::nix::primitives;

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Test resource types
// ─────────────────────────────────────────────────────────────────────────────

// Lightweight resource (minimal construction cost)
struct LightResource {
  int id = 0;
  bool healthy = true;
};

// Heavy resource (simulates expensive construction like DB connection)
struct HeavyResource {
  int id = 0;
  bool healthy = true;
  std::vector<char> buffer;

  HeavyResource() : buffer(4096) {
    // Simulate expensive initialization
    for (auto& c : buffer) {
      c = static_cast<char>(id % 256);
    }
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark helpers
// ─────────────────────────────────────────────────────────────────────────────

template <typename F>
void run_concurrent(std::size_t num_threads, F&& func) {
  std::latch start_latch(num_threads);
  std::vector<std::thread> threads;

  for (std::size_t t = 0; t < num_threads; ++t) {
    threads.emplace_back([&, t]() {
      start_latch.arrive_and_wait();
      func(t);
    });
  }

  for (auto& t : threads) {
    t.join();
  }
}

} // namespace

int main() {
  ankerl::nanobench::Bench bench;
  bench.title("Pool Benchmarks").warmup(100).minEpochIterations(1000).unit("op");

  // ───────────────────────────────────────────────────────────────────────────
  // Single-threaded acquisition benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("pool/get-release/light/single", [&] {
    static pool::Pool<LightResource> p(8);
    auto handle = p.get();
    ankerl::nanobench::doNotOptimizeAway(handle.get());
  });

  bench.run("pool/get-release/heavy/single", [&] {
    static pool::Pool<HeavyResource> p(8);
    auto handle = p.get();
    ankerl::nanobench::doNotOptimizeAway(handle.get());
  });

  bench.run("pool/try_get-release/light/single", [&] {
    static pool::Pool<LightResource> p(8);
    auto handle = p.try_get();
    ankerl::nanobench::doNotOptimizeAway(handle.has_value());
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Pool construction benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("pool/construct/lazy/8", [&] {
    pool::Pool<LightResource> p(8);
    ankerl::nanobench::doNotOptimizeAway(&p);
  });

  bench.run("pool/construct/eager/8", [&] {
    pool::PoolConfig<LightResource> config{
        .max_size = 8,
        .init_mode = pool::PoolInitMode::Eager,
    };
    pool::Pool<LightResource> p(config);
    ankerl::nanobench::doNotOptimizeAway(&p);
  });

  bench.run("pool/construct/lazy/64", [&] {
    pool::Pool<LightResource> p(64);
    ankerl::nanobench::doNotOptimizeAway(&p);
  });

  bench.run("pool/construct/eager/64", [&] {
    pool::PoolConfig<LightResource> config{
        .max_size = 64,
        .init_mode = pool::PoolInitMode::Eager,
    };
    pool::Pool<LightResource> p(config);
    ankerl::nanobench::doNotOptimizeAway(&p);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Reuse vs creation benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  {
    // Pool with resources already available (reuse path)
    pool::PoolConfig<LightResource> config{
        .max_size = 8,
        .init_mode = pool::PoolInitMode::Eager,
    };
    pool::Pool<LightResource> p(config);

    bench.run("pool/get-release/reuse-path", [&] {
      auto handle = p.get();
      ankerl::nanobench::doNotOptimizeAway(handle.get());
    });
  }

  {
    // Pool that creates fresh each time (mark_bad to force creation)
    std::atomic<int> counter{0};
    pool::PoolConfig<LightResource> config{
        .max_size = 1000000, // Very large to avoid blocking
        .factory =
            [&counter]() {
              auto r = std::make_unique<LightResource>();
              r->id = counter.fetch_add(1);
              return r;
            },
    };
    pool::Pool<LightResource> p(config);

    bench.run("pool/get-release/create-path", [&] {
      auto handle = p.get();
      handle.mark_bad(); // Force new creation next time
      ankerl::nanobench::doNotOptimizeAway(handle.get());
    });
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Health check overhead benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  {
    // No health check
    pool::PoolConfig<LightResource> config{
        .max_size = 8,
        .init_mode = pool::PoolInitMode::Eager,
    };
    pool::Pool<LightResource> p(config);

    bench.run("pool/get-release/no-health-check", [&] {
      auto handle = p.get();
      ankerl::nanobench::doNotOptimizeAway(handle.get());
    });
  }

  {
    // Trivial health check
    pool::PoolConfig<LightResource> config{
        .max_size = 8,
        .health_check = [](const LightResource& r) { return r.healthy; },
        .init_mode = pool::PoolInitMode::Eager,
    };
    pool::Pool<LightResource> p(config);

    bench.run("pool/get-release/trivial-health-check", [&] {
      auto handle = p.get();
      ankerl::nanobench::doNotOptimizeAway(handle.get());
    });
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Concurrent throughput benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  // Low contention: more slots than threads
  for (std::size_t threads : {2, 4, 8}) {
    std::string name = "pool/throughput/low-contention/" + std::to_string(threads) + "t";

    bench.run(name, [threads] {
      static pool::Pool<LightResource> p(64);
      static std::atomic<std::size_t> counter{0};

      run_concurrent(threads, [](std::size_t) {
        for (int i = 0; i < 100; ++i) {
          auto handle = p.get();
          counter.fetch_add(1);
        }
      });

      ankerl::nanobench::doNotOptimizeAway(counter.load());
    });
  }

  // High contention: fewer slots than threads
  for (std::size_t threads : {4, 8, 16}) {
    std::string name = "pool/throughput/high-contention/" + std::to_string(threads) + "t";

    bench.run(name, [threads] {
      static pool::Pool<LightResource> p(2);
      static std::atomic<std::size_t> counter{0};

      run_concurrent(threads, [](std::size_t) {
        for (int i = 0; i < 50; ++i) {
          auto handle = p.get();
          counter.fetch_add(1);
        }
      });

      ankerl::nanobench::doNotOptimizeAway(counter.load());
    });
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Latency benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  // Measure acquisition latency with pre-warmed pool
  {
    pool::PoolConfig<LightResource> config{
        .max_size = 1,
        .init_mode = pool::PoolInitMode::Eager,
    };
    pool::Pool<LightResource> p(config);

    bench.run("pool/latency/single-slot-uncontended", [&] {
      auto start = std::chrono::high_resolution_clock::now();
      auto handle = p.get();
      auto end = std::chrono::high_resolution_clock::now();
      ankerl::nanobench::doNotOptimizeAway(end - start);
    });
  }

  // ───────────────────────────────────────────────────────────────────────────
  // try_get failure path benchmark
  // ───────────────────────────────────────────────────────────────────────────

  {
    pool::Pool<LightResource> p(1);
    auto hold = p.get(); // Exhaust the pool

    bench.run("pool/try_get/failure-path", [&] {
      auto result = p.try_get();
      ankerl::nanobench::doNotOptimizeAway(result.has_value());
    });
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Pool size scaling benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  for (std::size_t size : {1, 4, 16, 64, 256}) {
    std::string name = "pool/get-release/size-" + std::to_string(size);

    bench.run(name, [size] {
      static std::vector<std::unique_ptr<pool::Pool<LightResource>>> pools;
      if (pools.size() <= size) {
        pools.resize(size + 1);
      }
      if (!pools[size]) {
        pools[size] = std::make_unique<pool::Pool<LightResource>>(size);
      }

      auto handle = pools[size]->get();
      ankerl::nanobench::doNotOptimizeAway(handle.get());
    });
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Handle operations benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("pool/handle/dereference", [&] {
    static pool::Pool<LightResource> p(8);
    static auto handle = p.get();
    ankerl::nanobench::doNotOptimizeAway(handle->id);
    ankerl::nanobench::doNotOptimizeAway((*handle).id);
    ankerl::nanobench::doNotOptimizeAway(handle.get());
  });

  bench.run("pool/handle/mark_bad", [&] {
    pool::Pool<LightResource> p(1000000);
    auto handle = p.get();
    handle.mark_bad();
    ankerl::nanobench::doNotOptimizeAway(handle.is_bad());
  });

  // ───────────────────────────────────────────────────────────────────────────
  // flush_bad benchmark
  // ───────────────────────────────────────────────────────────────────────────

  {
    pool::PoolConfig<LightResource> config{
        .max_size = 64,
        .health_check = [](const LightResource& r) { return r.healthy; },
        .init_mode = pool::PoolInitMode::Eager,
    };

    bench.run("pool/flush_bad/64-all-healthy", [&] {
      static pool::Pool<LightResource> p(config);
      p.flush_bad();
      ankerl::nanobench::doNotOptimizeAway(p.idle());
    });
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Comparison: raw unique_ptr vs pooled
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("baseline/make_unique<LightResource>", [&] {
    auto ptr = std::make_unique<LightResource>();
    ankerl::nanobench::doNotOptimizeAway(ptr.get());
  });

  bench.run("baseline/make_unique<HeavyResource>", [&] {
    auto ptr = std::make_unique<HeavyResource>();
    ankerl::nanobench::doNotOptimizeAway(ptr.get());
  });

  return 0;
}
