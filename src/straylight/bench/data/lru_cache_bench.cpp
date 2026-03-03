// straylight::nix::data::bench::lru_cache_bench
//
// Microbenchmarks for LRU cache operations:
//   - put (insert new key)
//   - put (update existing key)
//   - get (cache hit)
//   - get (cache miss)
//   - eviction overhead
//   - thread-safe variant overhead
//
// Uses ankerl::nanobench for high-quality microbenchmarking

#define ANKERL_NANOBENCH_IMPLEMENT
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <nanobench.h>
#include <straylight/nix/data/lru_cache.h>

namespace lru = straylight::nix::data;

// ─────────────────────────────────────────────────────────────────────────────
// Workload generators
// ─────────────────────────────────────────────────────────────────────────────

namespace workloads {

// Generate unique string keys
std::vector<std::string> generate_string_keys(std::size_t count) {
  std::vector<std::string> keys;
  keys.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    keys.push_back("key_" + std::to_string(i) + "_suffix_data");
  }
  return keys;
}

// Generate integer keys
std::vector<int> generate_int_keys(std::size_t count) {
  std::vector<int> keys;
  keys.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    keys.push_back(static_cast<int>(i));
  }
  return keys;
}

// Generate random access pattern (Zipf-like distribution for cache simulation)
std::vector<std::size_t> generate_zipf_access_pattern(std::size_t num_keys, std::size_t num_ops) {
  std::mt19937_64 rng(42);
  std::vector<std::size_t> pattern;
  pattern.reserve(num_ops);

  // Zipf distribution: lower indices are accessed more frequently
  for (std::size_t i = 0; i < num_ops; ++i) {
    // Simple Zipf approximation: square root biases toward lower indices
    double u = std::uniform_real_distribution<>(0.0, 1.0)(rng);
    std::size_t idx = static_cast<std::size_t>(u * u * num_keys);
    pattern.push_back(idx);
  }
  return pattern;
}

// Generate uniform random access pattern
std::vector<std::size_t> generate_uniform_access_pattern(std::size_t num_keys,
                                                         std::size_t num_ops) {
  std::mt19937_64 rng(42);
  std::uniform_int_distribution<std::size_t> dist(0, num_keys - 1);
  std::vector<std::size_t> pattern;
  pattern.reserve(num_ops);
  for (std::size_t i = 0; i < num_ops; ++i) {
    pattern.push_back(dist(rng));
  }
  return pattern;
}

} // namespace workloads

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Basic operations
// ─────────────────────────────────────────────────────────────────────────────

void bench_basic_ops(ankerl::nanobench::Bench& b) {
  constexpr std::size_t cache_size = 1000;
  auto keys = workloads::generate_string_keys(cache_size * 2);

  // Put (insert new keys)
  b.run("put/insert/string", [&] {
    lru::LRUCache<std::string, int> cache(cache_size);
    for (std::size_t i = 0; i < cache_size; ++i) {
      cache.put(keys[i], static_cast<int>(i));
    }
    ankerl::nanobench::doNotOptimizeAway(cache.size());
  });

  // Put (update existing keys)
  b.run("put/update/string", [&] {
    lru::LRUCache<std::string, int> cache(cache_size);
    // Fill cache
    for (std::size_t i = 0; i < cache_size; ++i) {
      cache.put(keys[i], static_cast<int>(i));
    }
    // Update all keys
    for (std::size_t i = 0; i < cache_size; ++i) {
      cache.put(keys[i], static_cast<int>(i * 10));
    }
    ankerl::nanobench::doNotOptimizeAway(cache.size());
  });

  // Get (cache hit)
  lru::LRUCache<std::string, int> filled_cache(cache_size);
  for (std::size_t i = 0; i < cache_size; ++i) {
    filled_cache.put(keys[i], static_cast<int>(i));
  }

  b.run("get/hit/string", [&] {
    for (std::size_t i = 0; i < cache_size; ++i) {
      auto val = filled_cache.get(keys[i]);
      ankerl::nanobench::doNotOptimizeAway(val);
    }
  });

  // Get (cache miss)
  b.run("get/miss/string", [&] {
    for (std::size_t i = cache_size; i < cache_size * 2; ++i) {
      auto val = filled_cache.get(keys[i]);
      ankerl::nanobench::doNotOptimizeAway(val);
    }
  });

  // Contains
  b.run("contains/string", [&] {
    for (std::size_t i = 0; i < cache_size; ++i) {
      auto found = filled_cache.contains(keys[i]);
      ankerl::nanobench::doNotOptimizeAway(found);
    }
  });

  // Peek (no promotion)
  b.run("peek/string", [&] {
    for (std::size_t i = 0; i < cache_size; ++i) {
      auto val = filled_cache.peek(keys[i]);
      ankerl::nanobench::doNotOptimizeAway(val);
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Integer keys (faster hash)
// ─────────────────────────────────────────────────────────────────────────────

void bench_int_keys(ankerl::nanobench::Bench& b) {
  constexpr std::size_t cache_size = 1000;
  auto keys = workloads::generate_int_keys(cache_size * 2);

  b.run("put/insert/int", [&] {
    lru::LRUCache<int, int> cache(cache_size);
    for (std::size_t i = 0; i < cache_size; ++i) {
      cache.put(keys[i], static_cast<int>(i));
    }
    ankerl::nanobench::doNotOptimizeAway(cache.size());
  });

  lru::LRUCache<int, int> filled_cache(cache_size);
  for (std::size_t i = 0; i < cache_size; ++i) {
    filled_cache.put(keys[i], static_cast<int>(i));
  }

  b.run("get/hit/int", [&] {
    for (std::size_t i = 0; i < cache_size; ++i) {
      auto val = filled_cache.get(keys[i]);
      ankerl::nanobench::doNotOptimizeAway(val);
    }
  });

  b.run("get/miss/int", [&] {
    for (std::size_t i = cache_size; i < cache_size * 2; ++i) {
      auto val = filled_cache.get(keys[i]);
      ankerl::nanobench::doNotOptimizeAway(val);
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Eviction overhead
// ─────────────────────────────────────────────────────────────────────────────

void bench_eviction(ankerl::nanobench::Bench& b) {
  constexpr std::size_t cache_size = 100;
  constexpr std::size_t total_inserts = 10000;
  auto keys = workloads::generate_int_keys(total_inserts);

  // Measure steady-state eviction overhead
  b.run("eviction/steady_state", [&] {
    lru::LRUCache<int, int> cache(cache_size);
    for (std::size_t i = 0; i < total_inserts; ++i) {
      cache.put(keys[i], static_cast<int>(i));
    }
    ankerl::nanobench::doNotOptimizeAway(cache.size());
  });

  // With eviction callback
  std::size_t eviction_count = 0;
  b.run("eviction/with_callback", [&] {
    eviction_count = 0;
    lru::LRUCache<int, int> cache(cache_size, [&](const int&, int&) { ++eviction_count; });
    for (std::size_t i = 0; i < total_inserts; ++i) {
      cache.put(keys[i], static_cast<int>(i));
    }
    ankerl::nanobench::doNotOptimizeAway(eviction_count);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Access patterns (cache hit rate simulation)
// ─────────────────────────────────────────────────────────────────────────────

void bench_access_patterns(ankerl::nanobench::Bench& b) {
  constexpr std::size_t cache_size = 100;
  constexpr std::size_t num_keys = 1000;
  constexpr std::size_t num_ops = 10000;

  auto keys = workloads::generate_int_keys(num_keys);
  auto zipf_pattern = workloads::generate_zipf_access_pattern(num_keys, num_ops);
  auto uniform_pattern = workloads::generate_uniform_access_pattern(num_keys, num_ops);

  // Zipf distribution (hot keys accessed frequently)
  b.run("access/zipf", [&] {
    lru::LRUCache<int, int> cache(cache_size);
    std::size_t hits = 0;
    for (auto idx : zipf_pattern) {
      int key = keys[idx];
      auto val = cache.get(key);
      if (val) {
        ++hits;
      } else {
        cache.put(key, key * 10);
      }
    }
    ankerl::nanobench::doNotOptimizeAway(hits);
  });

  // Uniform random (worst case for LRU)
  b.run("access/uniform", [&] {
    lru::LRUCache<int, int> cache(cache_size);
    std::size_t hits = 0;
    for (auto idx : uniform_pattern) {
      int key = keys[idx];
      auto val = cache.get(key);
      if (val) {
        ++hits;
      } else {
        cache.put(key, key * 10);
      }
    }
    ankerl::nanobench::doNotOptimizeAway(hits);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Thread-safe variant overhead
// ─────────────────────────────────────────────────────────────────────────────

void bench_threadsafe(ankerl::nanobench::Bench& b) {
  constexpr std::size_t cache_size = 1000;
  auto keys = workloads::generate_int_keys(cache_size);

  // Non-thread-safe baseline
  b.run("threadsafe/baseline", [&] {
    lru::LRUCache<int, int> cache(cache_size);
    for (std::size_t i = 0; i < cache_size; ++i) {
      cache.put(keys[i], static_cast<int>(i));
    }
    for (std::size_t i = 0; i < cache_size; ++i) {
      auto val = cache.get(keys[i]);
      ankerl::nanobench::doNotOptimizeAway(val);
    }
  });

  // Thread-safe single-threaded (lock overhead)
  b.run("threadsafe/single_thread", [&] {
    lru::LRUCacheSafe<int, int> cache(cache_size);
    for (std::size_t i = 0; i < cache_size; ++i) {
      cache.put(keys[i], static_cast<int>(i));
    }
    for (std::size_t i = 0; i < cache_size; ++i) {
      auto val = cache.get(keys[i]);
      ankerl::nanobench::doNotOptimizeAway(val);
    }
  });

  // Thread-safe with contention
  b.run("threadsafe/4_threads", [&] {
    lru::LRUCacheSafe<int, int> cache(cache_size);

    auto worker = [&](int thread_id) {
      for (std::size_t i = 0; i < cache_size / 4; ++i) {
        int key = static_cast<int>(i + thread_id * 1000);
        cache.put(key, key * 10);
        auto val = cache.get(key);
        ankerl::nanobench::doNotOptimizeAway(val);
      }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
      threads.emplace_back(worker, t);
    }
    for (auto& th : threads) {
      th.join();
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Cache sizes
// ─────────────────────────────────────────────────────────────────────────────

void bench_cache_sizes(ankerl::nanobench::Bench& b) {
  auto keys = workloads::generate_int_keys(100000);
  constexpr std::size_t num_ops = 10000;

  auto run_workload = [&](lru::LRUCache<int, int>& cache) {
    for (std::size_t i = 0; i < num_ops; ++i) {
      cache.put(keys[i % keys.size()], static_cast<int>(i));
      auto val = cache.get(keys[(i + 500) % keys.size()]);
      ankerl::nanobench::doNotOptimizeAway(val);
    }
  };

  b.run("size/10", [&] {
    lru::LRUCache<int, int> cache(10);
    run_workload(cache);
  });

  b.run("size/100", [&] {
    lru::LRUCache<int, int> cache(100);
    run_workload(cache);
  });

  b.run("size/1000", [&] {
    lru::LRUCache<int, int> cache(1000);
    run_workload(cache);
  });

  b.run("size/10000", [&] {
    lru::LRUCache<int, int> cache(10000);
    run_workload(cache);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
  std::cout << "LRU Cache Benchmark\n";
  std::cout << "===================\n\n";

  ankerl::nanobench::Bench b;
  b.title("LRU Cache Operations");
  b.warmup(100);
  b.minEpochIterations(10);
  b.relative(true);

  std::cout << "=== Basic Operations ===\n";
  bench_basic_ops(b);

  std::cout << "\n=== Integer Keys ===\n";
  bench_int_keys(b);

  std::cout << "\n=== Eviction Overhead ===\n";
  bench_eviction(b);

  std::cout << "\n=== Access Patterns ===\n";
  bench_access_patterns(b);

  std::cout << "\n=== Thread-safe Overhead ===\n";
  bench_threadsafe(b);

  std::cout << "\n=== Cache Sizes ===\n";
  bench_cache_sizes(b);

  return 0;
}
