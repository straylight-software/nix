// straylight::nix::primitives::bench::hash_bench
//
// Microbenchmarks comparing hash implementations:
//   - BLAKE3 (SIMD-accelerated via official C library)
//   - SHA256 (OpenSSL with SHA-NI)
//   - SHA512 (OpenSSL)
//   - SHA1 (OpenSSL)
//   - MD5 (OpenSSL)
//
// Build with -march=znver5 for AVX-512 support on Zen 5
//
// Uses ankerl::nanobench for high-quality microbenchmarking

#define ANKERL_NANOBENCH_IMPLEMENT
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include <nanobench.h>

#include "../hash.h"

namespace hash = straylight::nix::primitives::hash;

// ─────────────────────────────────────────────────────────────────────────────
// Test data generators
// ─────────────────────────────────────────────────────────────────────────────

namespace workloads {

// Generate random data of specified size
std::string generate_random_data(std::size_t size) {
  std::mt19937_64 rng(42);
  std::string data(size, '\0');
  auto* ptr = reinterpret_cast<uint64_t*>(data.data());
  for (std::size_t i = 0; i < size / 8; ++i) {
    ptr[i] = rng();
  }
  return data;
}

// Realistic nix workload: store path strings (short)
std::vector<std::string> generate_store_paths(std::size_t count) {
  std::vector<std::string> paths;
  paths.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    paths.push_back("/nix/store/abc123def456-package-" + std::to_string(i) + ".0.0");
  }
  return paths;
}

// Realistic nix workload: derivation content (medium, ~1KB each)
std::vector<std::string> generate_derivations(std::size_t count) {
  std::vector<std::string> drvs;
  drvs.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    std::string drv = "Derive([";
    for (int j = 0; j < 10; ++j) {
      drv += "(\"out\",\"/nix/store/hash" + std::to_string(i * 10 + j) + "-pkg\",\"\",\"\"),";
    }
    drv += "],[";
    for (int j = 0; j < 20; ++j) {
      drv += "(\"/nix/store/dep" + std::to_string(j) + "-lib\",[\"out\"]),";
    }
    drv += "],[\"/nix/store/src-" + std::to_string(i) + "\"],";
    drv += "\"x86_64-linux\",\"/nix/store/bash/bin/bash\",";
    drv += "[\"--arg1\",\"value1\",\"--arg2\",\"value2\"],";
    drv += "[(\"env\",\"value\")])";
    drvs.push_back(drv);
  }
  return drvs;
}

} // namespace workloads

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Small data (store paths, ~50 bytes)
// ─────────────────────────────────────────────────────────────────────────────

void bench_small_data(ankerl::nanobench::Bench& b) {
  auto paths = workloads::generate_store_paths(1000);

  b.run("blake3/small/1000", [&] {
    for (const auto& path : paths) {
      auto h = hash::blake3(path);
      ankerl::nanobench::doNotOptimizeAway(h);
    }
  });

  b.run("sha256/small/1000", [&] {
    for (const auto& path : paths) {
      auto h = hash::sha256(path);
      ankerl::nanobench::doNotOptimizeAway(h);
    }
  });

  b.run("sha512/small/1000", [&] {
    for (const auto& path : paths) {
      auto h = hash::sha512(path);
      ankerl::nanobench::doNotOptimizeAway(h);
    }
  });

  b.run("sha1/small/1000", [&] {
    for (const auto& path : paths) {
      auto h = hash::sha1(path);
      ankerl::nanobench::doNotOptimizeAway(h);
    }
  });

  b.run("md5/small/1000", [&] {
    for (const auto& path : paths) {
      auto h = hash::md5(path);
      ankerl::nanobench::doNotOptimizeAway(h);
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Medium data (derivations, ~1KB each)
// ─────────────────────────────────────────────────────────────────────────────

void bench_medium_data(ankerl::nanobench::Bench& b) {
  auto drvs = workloads::generate_derivations(100);

  b.run("blake3/medium/100", [&] {
    for (const auto& drv : drvs) {
      auto h = hash::blake3(drv);
      ankerl::nanobench::doNotOptimizeAway(h);
    }
  });

  b.run("sha256/medium/100", [&] {
    for (const auto& drv : drvs) {
      auto h = hash::sha256(drv);
      ankerl::nanobench::doNotOptimizeAway(h);
    }
  });

  b.run("sha512/medium/100", [&] {
    for (const auto& drv : drvs) {
      auto h = hash::sha512(drv);
      ankerl::nanobench::doNotOptimizeAway(h);
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Large data (NAR files, 1MB, 10MB, 100MB)
// ─────────────────────────────────────────────────────────────────────────────

void bench_large_data(ankerl::nanobench::Bench& b) {
  auto data_1mb = workloads::generate_random_data(1 * 1024 * 1024);
  auto data_10mb = workloads::generate_random_data(10 * 1024 * 1024);

  // 1MB
  b.run("blake3/1MB", [&] {
    auto h = hash::blake3(data_1mb);
    ankerl::nanobench::doNotOptimizeAway(h);
  });

  b.run("sha256/1MB", [&] {
    auto h = hash::sha256(data_1mb);
    ankerl::nanobench::doNotOptimizeAway(h);
  });

  b.run("sha512/1MB", [&] {
    auto h = hash::sha512(data_1mb);
    ankerl::nanobench::doNotOptimizeAway(h);
  });

  // 10MB
  b.run("blake3/10MB", [&] {
    auto h = hash::blake3(data_10mb);
    ankerl::nanobench::doNotOptimizeAway(h);
  });

  b.run("sha256/10MB", [&] {
    auto h = hash::sha256(data_10mb);
    ankerl::nanobench::doNotOptimizeAway(h);
  });

  b.run("sha512/10MB", [&] {
    auto h = hash::sha512(data_10mb);
    ankerl::nanobench::doNotOptimizeAway(h);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Streaming vs one-shot
// ─────────────────────────────────────────────────────────────────────────────

void bench_streaming(ankerl::nanobench::Bench& b) {
  auto data = workloads::generate_random_data(1 * 1024 * 1024);

  // One-shot
  b.run("blake3/1MB/oneshot", [&] {
    auto h = hash::blake3(data);
    ankerl::nanobench::doNotOptimizeAway(h);
  });

  // Streaming with 64KB chunks
  b.run("blake3/1MB/streaming_64k", [&] {
    hash::Hasher hasher(hash::Algorithm::BLAKE3);
    constexpr std::size_t chunk_size = 64 * 1024;
    for (std::size_t i = 0; i < data.size(); i += chunk_size) {
      std::size_t len = std::min(chunk_size, data.size() - i);
      hasher.update(std::string_view(data).substr(i, len));
    }
    auto h = hasher.finish();
    ankerl::nanobench::doNotOptimizeAway(h);
  });

  // SHA256 comparison
  b.run("sha256/1MB/oneshot", [&] {
    auto h = hash::sha256(data);
    ankerl::nanobench::doNotOptimizeAway(h);
  });

  b.run("sha256/1MB/streaming_64k", [&] {
    hash::Hasher hasher(hash::Algorithm::SHA256);
    constexpr std::size_t chunk_size = 64 * 1024;
    for (std::size_t i = 0; i < data.size(); i += chunk_size) {
      std::size_t len = std::min(chunk_size, data.size() - i);
      hasher.update(std::string_view(data).substr(i, len));
    }
    auto h = hasher.finish();
    ankerl::nanobench::doNotOptimizeAway(h);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Encoding overhead
// ─────────────────────────────────────────────────────────────────────────────

void bench_encoding(ankerl::nanobench::Bench& b) {
  auto h = hash::sha256("test data for encoding benchmark");

  b.run("encode/hex", [&] {
    auto s = h.to_hex();
    ankerl::nanobench::doNotOptimizeAway(s);
  });

  b.run("encode/base64", [&] {
    auto s = h.to_base64();
    ankerl::nanobench::doNotOptimizeAway(s);
  });

  b.run("encode/nix32", [&] {
    auto s = h.to_nix32();
    ankerl::nanobench::doNotOptimizeAway(s);
  });

  b.run("encode/sri", [&] {
    auto s = h.to_sri();
    ankerl::nanobench::doNotOptimizeAway(s);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
  std::cout << "Hash Benchmark: BLAKE3 vs SHA family\n";
  std::cout << "=====================================\n";
#ifdef __AVX512F__
  std::cout << "AVX-512: ENABLED\n";
#else
  std::cout << "AVX-512: disabled\n";
#endif
#ifdef __AVX2__
  std::cout << "AVX2: ENABLED\n";
#endif
#ifdef __SHA__
  std::cout << "SHA-NI: ENABLED\n";
#endif
  std::cout << "\n";

  ankerl::nanobench::Bench b;
  b.title("Hash Operations");
  b.warmup(100);
  b.minEpochIterations(10);
  b.relative(true);

  std::cout << "=== Small Data (~50 bytes, store paths) ===\n";
  bench_small_data(b);

  std::cout << "\n=== Medium Data (~1KB, derivations) ===\n";
  bench_medium_data(b);

  std::cout << "\n=== Large Data (1MB, 10MB) ===\n";
  bench_large_data(b);

  std::cout << "\n=== Streaming vs One-shot ===\n";
  bench_streaming(b);

  std::cout << "\n=== Encoding Overhead ===\n";
  bench_encoding(b);

  return 0;
}
