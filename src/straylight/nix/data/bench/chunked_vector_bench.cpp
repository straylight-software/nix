// straylight::nix::data::ChunkedVector benchmarks
//
// Benchmarks comparing ChunkedVector performance against std::vector and std::deque.
// Tests push_back, indexed access, iteration, and stable reference patterns.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <random>
#include <string>
#include <vector>

#include "../chunked_vector.h"

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

namespace chunked_vector = straylight::nix::data;

namespace {

// Generate random indices for access patterns
std::vector<std::size_t> generate_random_indices(std::size_t count, std::size_t max_idx,
                                                 std::uint64_t seed = 42) {
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<std::size_t> dist(0, max_idx - 1);
  std::vector<std::size_t> indices(count);
  for (auto& idx : indices) {
    idx = dist(rng);
  }
  return indices;
}

// Test type sizes
struct Small {
  int value;
};

struct Medium {
  std::int64_t a, b, c, d;
};

struct Large {
  std::int64_t data[16]; // 128 bytes
};

} // namespace

int main() {
  ankerl::nanobench::Bench bench;
  bench.title("ChunkedVector Benchmarks").warmup(100).minEpochIterations(100).unit("op");

  // ───────────────────────────────────────────────────────────────────────────
  // Push Back Benchmarks (small elements)
  // ───────────────────────────────────────────────────────────────────────────

  constexpr std::size_t kSmall = 1000;
  constexpr std::size_t kMedium = 10000;
  constexpr std::size_t kLarge = 100000;

  // std::vector push_back
  bench.run("std::vector/push_back/1K ints", [&] {
    std::vector<int> vec;
    for (std::size_t i = 0; i < kSmall; ++i) {
      vec.push_back(static_cast<int>(i));
    }
    ankerl::nanobench::doNotOptimizeAway(vec);
  });

  bench.run("std::vector/push_back/10K ints", [&] {
    std::vector<int> vec;
    for (std::size_t i = 0; i < kMedium; ++i) {
      vec.push_back(static_cast<int>(i));
    }
    ankerl::nanobench::doNotOptimizeAway(vec);
  });

  bench.run("std::vector/push_back/100K ints", [&] {
    std::vector<int> vec;
    for (std::size_t i = 0; i < kLarge; ++i) {
      vec.push_back(static_cast<int>(i));
    }
    ankerl::nanobench::doNotOptimizeAway(vec);
  });

  // std::deque push_back
  bench.run("std::deque/push_back/1K ints", [&] {
    std::deque<int> deq;
    for (std::size_t i = 0; i < kSmall; ++i) {
      deq.push_back(static_cast<int>(i));
    }
    ankerl::nanobench::doNotOptimizeAway(deq);
  });

  bench.run("std::deque/push_back/10K ints", [&] {
    std::deque<int> deq;
    for (std::size_t i = 0; i < kMedium; ++i) {
      deq.push_back(static_cast<int>(i));
    }
    ankerl::nanobench::doNotOptimizeAway(deq);
  });

  bench.run("std::deque/push_back/100K ints", [&] {
    std::deque<int> deq;
    for (std::size_t i = 0; i < kLarge; ++i) {
      deq.push_back(static_cast<int>(i));
    }
    ankerl::nanobench::doNotOptimizeAway(deq);
  });

  // ChunkedVector push_back
  bench.run("ChunkedVector/push_back/1K ints", [&] {
    chunked_vector::ChunkedVector<int> vec;
    for (std::size_t i = 0; i < kSmall; ++i) {
      vec.push_back(static_cast<int>(i));
    }
    ankerl::nanobench::doNotOptimizeAway(vec);
  });

  bench.run("ChunkedVector/push_back/10K ints", [&] {
    chunked_vector::ChunkedVector<int> vec;
    for (std::size_t i = 0; i < kMedium; ++i) {
      vec.push_back(static_cast<int>(i));
    }
    ankerl::nanobench::doNotOptimizeAway(vec);
  });

  bench.run("ChunkedVector/push_back/100K ints", [&] {
    chunked_vector::ChunkedVector<int> vec;
    for (std::size_t i = 0; i < kLarge; ++i) {
      vec.push_back(static_cast<int>(i));
    }
    ankerl::nanobench::doNotOptimizeAway(vec);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Sequential Access Benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  // Pre-fill containers
  std::vector<int> std_vec(kLarge);
  std::deque<int> std_deq(kLarge);
  chunked_vector::ChunkedVector<int> chunked_vec;

  for (std::size_t i = 0; i < kLarge; ++i) {
    std_vec[i] = static_cast<int>(i);
    std_deq[i] = static_cast<int>(i);
    chunked_vec.push_back(static_cast<int>(i));
  }

  bench.run("std::vector/seq_read/100K", [&] {
    std::int64_t sum = 0;
    for (std::size_t i = 0; i < kLarge; ++i) {
      sum += std_vec[i];
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  bench.run("std::deque/seq_read/100K", [&] {
    std::int64_t sum = 0;
    for (std::size_t i = 0; i < kLarge; ++i) {
      sum += std_deq[i];
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  bench.run("ChunkedVector/seq_read/100K", [&] {
    std::int64_t sum = 0;
    for (std::size_t i = 0; i < kLarge; ++i) {
      sum += chunked_vec[i];
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Iterator-based Sequential Access
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("std::vector/iterator/100K", [&] {
    std::int64_t sum = 0;
    for (auto it = std_vec.begin(); it != std_vec.end(); ++it) {
      sum += *it;
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  bench.run("std::deque/iterator/100K", [&] {
    std::int64_t sum = 0;
    for (auto it = std_deq.begin(); it != std_deq.end(); ++it) {
      sum += *it;
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  bench.run("ChunkedVector/iterator/100K", [&] {
    std::int64_t sum = 0;
    for (auto it = chunked_vec.begin(); it != chunked_vec.end(); ++it) {
      sum += *it;
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Range-based for loop
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("std::vector/range_for/100K", [&] {
    std::int64_t sum = 0;
    for (int v : std_vec) {
      sum += v;
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  bench.run("std::deque/range_for/100K", [&] {
    std::int64_t sum = 0;
    for (int v : std_deq) {
      sum += v;
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  bench.run("ChunkedVector/range_for/100K", [&] {
    std::int64_t sum = 0;
    for (int v : chunked_vec) {
      sum += v;
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Random Access Benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  auto random_indices = generate_random_indices(10000, kLarge);

  bench.run("std::vector/random_read/10K lookups", [&] {
    std::int64_t sum = 0;
    for (std::size_t idx : random_indices) {
      sum += std_vec[idx];
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  bench.run("std::deque/random_read/10K lookups", [&] {
    std::int64_t sum = 0;
    for (std::size_t idx : random_indices) {
      sum += std_deq[idx];
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  bench.run("ChunkedVector/random_read/10K lookups", [&] {
    std::int64_t sum = 0;
    for (std::size_t idx : random_indices) {
      sum += chunked_vec[idx];
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Stable Reference Pattern (Nix use case)
  // Stores pointers while adding more elements
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("ChunkedVector/stable_refs/save_and_grow", [&] {
    chunked_vector::ChunkedVector<int> vec;
    std::vector<int*> refs;
    refs.reserve(1000);

    // Add elements and save references
    for (int i = 0; i < 1000; ++i) {
      vec.push_back(i);
      refs.push_back(&vec[static_cast<std::size_t>(i)]);
    }

    // Add more elements (would invalidate std::vector refs)
    for (int i = 1000; i < 10000; ++i) {
      vec.push_back(i);
    }

    // Access through saved references
    std::int64_t sum = 0;
    for (int* p : refs) {
      sum += *p;
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  // Same pattern with std::deque (also has stable refs)
  bench.run("std::deque/stable_refs/save_and_grow", [&] {
    std::deque<int> deq;
    std::vector<int*> refs;
    refs.reserve(1000);

    for (int i = 0; i < 1000; ++i) {
      deq.push_back(i);
      refs.push_back(&deq[static_cast<std::size_t>(i)]);
    }

    for (int i = 1000; i < 10000; ++i) {
      deq.push_back(i);
    }

    std::int64_t sum = 0;
    for (int* p : refs) {
      sum += *p;
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // add() API (Nix compatibility)
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("ChunkedVector/add/10K elements", [&] {
    chunked_vector::ChunkedVector<int> vec;
    std::size_t last_idx = 0;
    for (int i = 0; i < 10000; ++i) {
      auto [ref, idx] = vec.add(i);
      last_idx = idx;
      ankerl::nanobench::doNotOptimizeAway(ref);
    }
    ankerl::nanobench::doNotOptimizeAway(last_idx);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Larger Element Types
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("std::vector/push_back/10K Large (128B)", [&] {
    std::vector<Large> vec;
    for (std::size_t i = 0; i < kMedium; ++i) {
      Large l{};
      l.data[0] = static_cast<std::int64_t>(i);
      vec.push_back(l);
    }
    ankerl::nanobench::doNotOptimizeAway(vec);
  });

  bench.run("std::deque/push_back/10K Large (128B)", [&] {
    std::deque<Large> deq;
    for (std::size_t i = 0; i < kMedium; ++i) {
      Large l{};
      l.data[0] = static_cast<std::int64_t>(i);
      deq.push_back(l);
    }
    ankerl::nanobench::doNotOptimizeAway(deq);
  });

  bench.run("ChunkedVector/push_back/10K Large (128B)", [&] {
    chunked_vector::ChunkedVector<Large> vec;
    for (std::size_t i = 0; i < kMedium; ++i) {
      Large l{};
      l.data[0] = static_cast<std::int64_t>(i);
      vec.push_back(l);
    }
    ankerl::nanobench::doNotOptimizeAway(vec);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // String Elements (heap allocated, move-only beneficial)
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("std::vector/push_back/10K strings", [&] {
    std::vector<std::string> vec;
    for (std::size_t i = 0; i < kMedium; ++i) {
      vec.push_back("element_" + std::to_string(i));
    }
    ankerl::nanobench::doNotOptimizeAway(vec);
  });

  bench.run("std::deque/push_back/10K strings", [&] {
    std::deque<std::string> deq;
    for (std::size_t i = 0; i < kMedium; ++i) {
      deq.push_back("element_" + std::to_string(i));
    }
    ankerl::nanobench::doNotOptimizeAway(deq);
  });

  bench.run("ChunkedVector/push_back/10K strings", [&] {
    chunked_vector::ChunkedVector<std::string> vec;
    for (std::size_t i = 0; i < kMedium; ++i) {
      vec.push_back("element_" + std::to_string(i));
    }
    ankerl::nanobench::doNotOptimizeAway(vec);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // emplace_back (in-place construction)
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("std::vector/emplace_back/10K strings", [&] {
    std::vector<std::string> vec;
    for (std::size_t i = 0; i < kMedium; ++i) {
      vec.emplace_back(10, 'x');
    }
    ankerl::nanobench::doNotOptimizeAway(vec);
  });

  bench.run("std::deque/emplace_back/10K strings", [&] {
    std::deque<std::string> deq;
    for (std::size_t i = 0; i < kMedium; ++i) {
      deq.emplace_back(10, 'x');
    }
    ankerl::nanobench::doNotOptimizeAway(deq);
  });

  bench.run("ChunkedVector/emplace_back/10K strings", [&] {
    chunked_vector::ChunkedVector<std::string> vec;
    for (std::size_t i = 0; i < kMedium; ++i) {
      vec.emplace_back(10, 'x');
    }
    ankerl::nanobench::doNotOptimizeAway(vec);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // forEach (Nix API compatibility)
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("ChunkedVector/forEach/100K", [&] {
    std::int64_t sum = 0;
    chunked_vec.forEach([&sum](int v) { sum += v; });
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Chunk Size Comparison
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("ChunkedVector<64>/push_back/100K", [&] {
    chunked_vector::ChunkedVector<int, 64> vec;
    for (std::size_t i = 0; i < kLarge; ++i) {
      vec.push_back(static_cast<int>(i));
    }
    ankerl::nanobench::doNotOptimizeAway(vec);
  });

  bench.run("ChunkedVector<256>/push_back/100K", [&] {
    chunked_vector::ChunkedVector<int, 256> vec;
    for (std::size_t i = 0; i < kLarge; ++i) {
      vec.push_back(static_cast<int>(i));
    }
    ankerl::nanobench::doNotOptimizeAway(vec);
  });

  bench.run("ChunkedVector<1024>/push_back/100K", [&] {
    chunked_vector::ChunkedVector<int, 1024> vec;
    for (std::size_t i = 0; i < kLarge; ++i) {
      vec.push_back(static_cast<int>(i));
    }
    ankerl::nanobench::doNotOptimizeAway(vec);
  });

  bench.run("ChunkedVector<8192>/push_back/100K", [&] {
    chunked_vector::ChunkedVector<int, 8192> vec;
    for (std::size_t i = 0; i < kLarge; ++i) {
      vec.push_back(static_cast<int>(i));
    }
    ankerl::nanobench::doNotOptimizeAway(vec);
  });

  bench.run("ChunkedVector<65536>/push_back/100K", [&] {
    chunked_vector::ChunkedVector<int, 65536> vec;
    for (std::size_t i = 0; i < kLarge; ++i) {
      vec.push_back(static_cast<int>(i));
    }
    ankerl::nanobench::doNotOptimizeAway(vec);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Clear and Reuse
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("std::vector/clear_and_refill/10K", [&] {
    std::vector<int> vec;
    for (int iter = 0; iter < 10; ++iter) {
      vec.clear();
      for (std::size_t i = 0; i < kMedium; ++i) {
        vec.push_back(static_cast<int>(i));
      }
    }
    ankerl::nanobench::doNotOptimizeAway(vec);
  });

  bench.run("std::deque/clear_and_refill/10K", [&] {
    std::deque<int> deq;
    for (int iter = 0; iter < 10; ++iter) {
      deq.clear();
      for (std::size_t i = 0; i < kMedium; ++i) {
        deq.push_back(static_cast<int>(i));
      }
    }
    ankerl::nanobench::doNotOptimizeAway(deq);
  });

  bench.run("ChunkedVector/clear_and_refill/10K", [&] {
    chunked_vector::ChunkedVector<int> vec;
    for (int iter = 0; iter < 10; ++iter) {
      vec.clear();
      for (std::size_t i = 0; i < kMedium; ++i) {
        vec.push_back(static_cast<int>(i));
      }
    }
    ankerl::nanobench::doNotOptimizeAway(vec);
  });

  return 0;
}
