// straylight::nix::util::checked_arithmetic benchmarks
//
// Benchmarks for checked and saturating arithmetic primitives using nanobench.
// Compares against unchecked arithmetic and Nix's Checked class.

#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "../checked_arithmetic.h"

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

namespace arith = straylight::nix::util;

namespace {

// Generate random values for benchmarking
template <typename T>
std::vector<T> generate_random_values(std::size_t count, std::uint64_t seed = 42) {
  std::mt19937_64 rng(seed);

  std::vector<T> values(count);
  if constexpr (std::is_signed_v<T>) {
    std::uniform_int_distribution<T> dist(std::numeric_limits<T>::min() / 4,
                                          std::numeric_limits<T>::max() / 4);
    for (auto& v : values) {
      v = dist(rng);
    }
  } else {
    std::uniform_int_distribution<T> dist(0, std::numeric_limits<T>::max() / 4);
    for (auto& v : values) {
      v = dist(rng);
    }
  }
  return values;
}

// Generate values that will definitely overflow
template <typename T>
std::vector<T> generate_overflow_values(std::size_t count, std::uint64_t seed = 42) {
  std::mt19937_64 rng(seed);

  std::vector<T> values(count);
  if constexpr (std::is_signed_v<T>) {
    std::uniform_int_distribution<T> dist(std::numeric_limits<T>::max() / 2,
                                          std::numeric_limits<T>::max());
    for (auto& v : values) {
      v = dist(rng);
    }
  } else {
    std::uniform_int_distribution<T> dist(std::numeric_limits<T>::max() / 2,
                                          std::numeric_limits<T>::max());
    for (auto& v : values) {
      v = dist(rng);
    }
  }
  return values;
}

} // namespace

int main() {
  ankerl::nanobench::Bench bench;
  bench.title("Checked Arithmetic Benchmarks").warmup(100).minEpochIterations(10000).unit("op");

  constexpr std::size_t N = 1000;

  // ───────────────────────────────────────────────────────────────────────────
  // Generate test data
  // ───────────────────────────────────────────────────────────────────────────

  auto safe_i32 = generate_random_values<std::int32_t>(N);
  auto safe_i64 = generate_random_values<std::int64_t>(N);
  auto safe_u64 = generate_random_values<std::uint64_t>(N);
  auto safe_size = generate_random_values<std::size_t>(N);

  auto overflow_i32 = generate_overflow_values<std::int32_t>(N);
  auto overflow_i64 = generate_overflow_values<std::int64_t>(N);
  auto overflow_u64 = generate_overflow_values<std::uint64_t>(N);

  // ───────────────────────────────────────────────────────────────────────────
  // Baseline: Unchecked arithmetic
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("unchecked/add/i32", [&] {
    std::int32_t sum = 0;
    for (std::size_t i = 0; i < N - 1; ++i) {
      sum += safe_i32[i] + safe_i32[i + 1];
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  bench.run("unchecked/add/i64", [&] {
    std::int64_t sum = 0;
    for (std::size_t i = 0; i < N - 1; ++i) {
      sum += safe_i64[i] + safe_i64[i + 1];
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  bench.run("unchecked/mul/i32", [&] {
    std::int32_t product = 1;
    for (std::size_t i = 0; i < 100; ++i) {
      product = (product * safe_i32[i]) % 1000000007;
    }
    ankerl::nanobench::doNotOptimizeAway(product);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Checked addition (no overflow path)
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("checked_add/i32/safe", [&] {
    std::int32_t sum = 0;
    for (std::size_t i = 0; i < N - 1; ++i) {
      auto result = arith::checked_add(safe_i32[i], safe_i32[i + 1]);
      if (result) {
        sum += *result;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  bench.run("checked_add/i64/safe", [&] {
    std::int64_t sum = 0;
    for (std::size_t i = 0; i < N - 1; ++i) {
      auto result = arith::checked_add(safe_i64[i], safe_i64[i + 1]);
      if (result) {
        sum += *result;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  bench.run("checked_add/u64/safe", [&] {
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < N - 1; ++i) {
      auto result = arith::checked_add(safe_u64[i], safe_u64[i + 1]);
      if (result) {
        sum += *result;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  bench.run("checked_add/size_t/safe", [&] {
    std::size_t sum = 0;
    for (std::size_t i = 0; i < N - 1; ++i) {
      auto result = arith::checked_add(safe_size[i], safe_size[i + 1]);
      if (result) {
        sum += *result;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Checked addition (overflow path)
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("checked_add/i32/overflow", [&] {
    std::size_t overflow_count = 0;
    for (std::size_t i = 0; i < N - 1; ++i) {
      auto result = arith::checked_add(overflow_i32[i], overflow_i32[i + 1]);
      if (!result) {
        ++overflow_count;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(overflow_count);
  });

  bench.run("checked_add/i64/overflow", [&] {
    std::size_t overflow_count = 0;
    for (std::size_t i = 0; i < N - 1; ++i) {
      auto result = arith::checked_add(overflow_i64[i], overflow_i64[i + 1]);
      if (!result) {
        ++overflow_count;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(overflow_count);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Checked subtraction
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("checked_sub/i32/safe", [&] {
    std::int32_t diff = 0;
    for (std::size_t i = 0; i < N - 1; ++i) {
      auto result = arith::checked_sub(safe_i32[i], safe_i32[i + 1]);
      if (result) {
        diff += *result;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(diff);
  });

  bench.run("checked_sub/u64/safe", [&] {
    std::uint64_t diff = 0;
    for (std::size_t i = 0; i < N - 1; ++i) {
      // Use larger - smaller to avoid underflow
      auto a = std::max(safe_u64[i], safe_u64[i + 1]);
      auto b = std::min(safe_u64[i], safe_u64[i + 1]);
      auto result = arith::checked_sub(a, b);
      if (result) {
        diff += *result;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(diff);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Checked multiplication
  // ───────────────────────────────────────────────────────────────────────────

  // Use small values to avoid overflow in most cases
  std::vector<std::int32_t> small_i32(N);
  std::vector<std::int64_t> small_i64(N);
  std::mt19937_64 rng(123);
  std::uniform_int_distribution<std::int32_t> small_dist(-1000, 1000);
  for (std::size_t i = 0; i < N; ++i) {
    small_i32[i] = small_dist(rng);
    small_i64[i] = small_dist(rng);
  }

  bench.run("checked_mul/i32/safe", [&] {
    std::int32_t product = 0;
    for (std::size_t i = 0; i < N - 1; ++i) {
      auto result = arith::checked_mul(small_i32[i], small_i32[i + 1]);
      if (result) {
        product += *result;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(product);
  });

  bench.run("checked_mul/i64/safe", [&] {
    std::int64_t product = 0;
    for (std::size_t i = 0; i < N - 1; ++i) {
      auto result = arith::checked_mul(small_i64[i], small_i64[i + 1]);
      if (result) {
        product += *result;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(product);
  });

  bench.run("checked_mul/i32/overflow", [&] {
    std::size_t overflow_count = 0;
    for (std::size_t i = 0; i < N - 1; ++i) {
      auto result = arith::checked_mul(overflow_i32[i], overflow_i32[i + 1]);
      if (!result) {
        ++overflow_count;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(overflow_count);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Saturating addition
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("saturating_add/i32/safe", [&] {
    std::int32_t sum = 0;
    for (std::size_t i = 0; i < N - 1; ++i) {
      sum = arith::saturating_add(sum, arith::saturating_add(safe_i32[i], safe_i32[i + 1]));
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  bench.run("saturating_add/i64/safe", [&] {
    std::int64_t sum = 0;
    for (std::size_t i = 0; i < N - 1; ++i) {
      sum = arith::saturating_add(sum, arith::saturating_add(safe_i64[i], safe_i64[i + 1]));
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  bench.run("saturating_add/i32/overflow", [&] {
    std::int32_t sum = 0;
    for (std::size_t i = 0; i < N - 1; ++i) {
      sum = arith::saturating_add(sum, arith::saturating_add(overflow_i32[i], overflow_i32[i + 1]));
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Saturating subtraction
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("saturating_sub/u64/underflow", [&] {
    std::uint64_t result = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t i = 0; i < N; ++i) {
      // This will frequently underflow and saturate to 0
      result = arith::saturating_sub(result, safe_u64[i]);
      result = arith::saturating_add(result, std::uint64_t{1});
    }
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Saturating multiplication
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("saturating_mul/i32/safe", [&] {
    std::int32_t product = 0;
    for (std::size_t i = 0; i < N - 1; ++i) {
      product += arith::saturating_mul(small_i32[i], small_i32[i + 1]);
    }
    ankerl::nanobench::doNotOptimizeAway(product);
  });

  bench.run("saturating_mul/i32/overflow", [&] {
    std::int32_t sum = 0;
    for (std::size_t i = 0; i < N - 1; ++i) {
      sum += arith::saturating_mul(overflow_i32[i], overflow_i32[i + 1]);
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // size_t specific benchmarks (memory allocation safety)
  // ───────────────────────────────────────────────────────────────────────────

  // Typical allocation sizes
  std::vector<std::size_t> alloc_counts(N);
  std::vector<std::size_t> alloc_sizes(N);
  std::uniform_int_distribution<std::size_t> count_dist(1, 10000);
  std::uniform_int_distribution<std::size_t> size_dist(1, 1024);
  for (std::size_t i = 0; i < N; ++i) {
    alloc_counts[i] = count_dist(rng);
    alloc_sizes[i] = size_dist(rng);
  }

  bench.run("checked_mul/size_t/allocation", [&] {
    std::size_t total = 0;
    for (std::size_t i = 0; i < N; ++i) {
      auto result = arith::checked_mul(alloc_counts[i], alloc_sizes[i]);
      if (result) {
        total += *result;
      }
    }
    ankerl::nanobench::doNotOptimizeAway(total);
  });

  bench.run("saturating_mul/size_t/allocation", [&] {
    std::size_t total = 0;
    for (std::size_t i = 0; i < N; ++i) {
      total += arith::saturating_mul(alloc_counts[i], alloc_sizes[i]);
    }
    ankerl::nanobench::doNotOptimizeAway(total);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Single operation latency (not throughput)
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("checked_add/single/i64", [&] {
    volatile std::int64_t a = 12345678901234LL;
    volatile std::int64_t b = 98765432109876LL;
    auto result = arith::checked_add(a, b);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("checked_mul/single/i64", [&] {
    volatile std::int64_t a = 123456789LL;
    volatile std::int64_t b = 987654321LL;
    auto result = arith::checked_mul(a, b);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("saturating_add/single/i64", [&] {
    volatile std::int64_t a = std::numeric_limits<std::int64_t>::max() - 100;
    volatile std::int64_t b = 200;
    auto result = arith::saturating_add(a, b);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  return 0;
}
