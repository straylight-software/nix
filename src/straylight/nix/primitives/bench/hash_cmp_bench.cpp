// straylight::nix::primitives::bench::hash_cmp_bench
//
// Comprehensive comparison benchmarks: straylight hash.h (BLAKE3) vs nix hash_string
//
// This benchmark compares:
//   - BLAKE3 (AVX-512/AVX2 accelerated) vs OpenSSL SHA256
//   - Streaming vs one-shot hashing
//   - Various data sizes (store paths to NAR files)
//
// Expected speedups (BLAKE3 vs SHA256):
//   - Small data (<1KB): 2-5x faster
//   - Medium data (1-100KB): 5-15x faster
//   - Large data (>1MB): 10-30x faster (with AVX-512)
//
// Why BLAKE3 is faster than SHA256:
//   - Tree-based structure allows parallelization
//   - AVX-512 processes 16 blocks at once
//   - Optimized for modern CPUs (no SHA-NI required)
//   - Smaller state size (64 bytes vs 256 bytes)
//
// Note: This benchmark compares AGAINST SHA256 (the nix default), not against
// the nix implementation directly. Nix uses the same OpenSSL SHA256.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include <nanobench.h>

// OpenSSL for comparison
#include <openssl/md5.h>
#include <openssl/sha.h>

// Straylight primitives
#include "../hash.h"

namespace hash = straylight::nix::primitives::hash;

namespace {

// Generate random data of specified size
std::string generate_random_data(std::size_t size, uint64_t seed = 42) {
  std::mt19937_64 rng(seed);
  std::string data(size, '\0');
  auto* ptr = reinterpret_cast<uint64_t*>(data.data());
  for (std::size_t i = 0; i < size / 8; ++i) {
    ptr[i] = rng();
  }
  return data;
}

// Generate realistic nix store paths (short strings, ~60 bytes)
std::vector<std::string> generate_store_paths(std::size_t count) {
  std::vector<std::string> paths;
  paths.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    paths.push_back("/nix/store/3b4p38fk6hgwaf2p6jcci5k93b9cnxc8-package-" + std::to_string(i) +
                    ".0.0");
  }
  return paths;
}

// Generate derivation content (medium size, ~1KB each)
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
    drvs.push_back(std::move(drv));
  }
  return drvs;
}

// OpenSSL SHA256 one-shot (simulating nix::hash_string)
void openssl_sha256(const std::string& data, unsigned char out[32]) {
  SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), out);
}

// OpenSSL SHA512 one-shot
void openssl_sha512(const std::string& data, unsigned char out[64]) {
  SHA512(reinterpret_cast<const unsigned char*>(data.data()), data.size(), out);
}

// OpenSSL MD5 one-shot (legacy)
void openssl_md5(const std::string& data, unsigned char out[16]) {
  MD5(reinterpret_cast<const unsigned char*>(data.data()), data.size(), out);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Small data (store paths, ~60 bytes)
// This is the most common case in nix
// ─────────────────────────────────────────────────────────────────────────────

void bench_small_data(ankerl::nanobench::Bench& b) {
  auto paths = generate_store_paths(1000);

  // SHA256 (nix default)
  b.run("openssl/sha256/small/1000", [&] {
    unsigned char out[32];
    for (const auto& path : paths) {
      openssl_sha256(path, out);
      ankerl::nanobench::doNotOptimizeAway(out);
    }
  });

  b.run("straylight/sha256/small/1000", [&] {
    for (const auto& path : paths) {
      auto h = hash::sha256(path);
      ankerl::nanobench::doNotOptimizeAway(h);
    }
  });

  // BLAKE3 (our new default)
  b.run("straylight/blake3/small/1000", [&] {
    for (const auto& path : paths) {
      auto h = hash::blake3(path);
      ankerl::nanobench::doNotOptimizeAway(h);
    }
  });

  // Expected: BLAKE3 ~2-5x faster than SHA256 on small data
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Medium data (derivations, ~1KB each)
// Common for derivation hashing
// ─────────────────────────────────────────────────────────────────────────────

void bench_medium_data(ankerl::nanobench::Bench& b) {
  auto drvs = generate_derivations(100);

  b.run("openssl/sha256/medium/100", [&] {
    unsigned char out[32];
    for (const auto& drv : drvs) {
      openssl_sha256(drv, out);
      ankerl::nanobench::doNotOptimizeAway(out);
    }
  });

  b.run("straylight/sha256/medium/100", [&] {
    for (const auto& drv : drvs) {
      auto h = hash::sha256(drv);
      ankerl::nanobench::doNotOptimizeAway(h);
    }
  });

  b.run("straylight/blake3/medium/100", [&] {
    for (const auto& drv : drvs) {
      auto h = hash::blake3(drv);
      ankerl::nanobench::doNotOptimizeAway(h);
    }
  });

  // Expected: BLAKE3 ~5-10x faster than SHA256
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Large data (NAR files, 1MB-100MB)
// Important for source hashing
// ─────────────────────────────────────────────────────────────────────────────

void bench_large_data(ankerl::nanobench::Bench& b) {
  auto data_1mb = generate_random_data(1 * 1024 * 1024);
  auto data_10mb = generate_random_data(10 * 1024 * 1024);

  // 1MB
  b.run("openssl/sha256/1MB", [&] {
    unsigned char out[32];
    openssl_sha256(data_1mb, out);
    ankerl::nanobench::doNotOptimizeAway(out);
  });

  b.run("straylight/sha256/1MB", [&] {
    auto h = hash::sha256(data_1mb);
    ankerl::nanobench::doNotOptimizeAway(h);
  });

  b.run("straylight/blake3/1MB", [&] {
    auto h = hash::blake3(data_1mb);
    ankerl::nanobench::doNotOptimizeAway(h);
  });

  // 10MB
  b.run("openssl/sha256/10MB", [&] {
    unsigned char out[32];
    openssl_sha256(data_10mb, out);
    ankerl::nanobench::doNotOptimizeAway(out);
  });

  b.run("straylight/sha256/10MB", [&] {
    auto h = hash::sha256(data_10mb);
    ankerl::nanobench::doNotOptimizeAway(h);
  });

  b.run("straylight/blake3/10MB", [&] {
    auto h = hash::blake3(data_10mb);
    ankerl::nanobench::doNotOptimizeAway(h);
  });

  // Expected: BLAKE3 ~10-30x faster than SHA256 on large data (with AVX-512)
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Streaming vs one-shot
// Important for processing files without loading fully into memory
// ─────────────────────────────────────────────────────────────────────────────

void bench_streaming(ankerl::nanobench::Bench& b) {
  auto data = generate_random_data(1 * 1024 * 1024);
  constexpr std::size_t chunk_size = 64 * 1024; // 64KB chunks

  // One-shot BLAKE3
  b.run("straylight/blake3/1MB/oneshot", [&] {
    auto h = hash::blake3(data);
    ankerl::nanobench::doNotOptimizeAway(h);
  });

  // Streaming BLAKE3 (64KB chunks)
  b.run("straylight/blake3/1MB/streaming_64k", [&] {
    hash::Hasher hasher(hash::Algorithm::BLAKE3);
    for (std::size_t i = 0; i < data.size(); i += chunk_size) {
      std::size_t len = std::min(chunk_size, data.size() - i);
      hasher.update(std::string_view(data).substr(i, len));
    }
    auto h = hasher.finish();
    ankerl::nanobench::doNotOptimizeAway(h);
  });

  // OpenSSL SHA256 streaming (for comparison)
  b.run("openssl/sha256/1MB/streaming_64k", [&] {
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    for (std::size_t i = 0; i < data.size(); i += chunk_size) {
      std::size_t len = std::min(chunk_size, data.size() - i);
      SHA256_Update(&ctx, data.data() + i, len);
    }
    unsigned char out[32];
    SHA256_Final(out, &ctx);
    ankerl::nanobench::doNotOptimizeAway(out);
  });

  // Straylight SHA256 streaming
  b.run("straylight/sha256/1MB/streaming_64k", [&] {
    hash::Hasher hasher(hash::Algorithm::SHA256);
    for (std::size_t i = 0; i < data.size(); i += chunk_size) {
      std::size_t len = std::min(chunk_size, data.size() - i);
      hasher.update(std::string_view(data).substr(i, len));
    }
    auto h = hasher.finish();
    ankerl::nanobench::doNotOptimizeAway(h);
  });

  // Expected: streaming should be close to one-shot performance
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: All algorithms comparison
// Shows relative performance of all supported algorithms
// ─────────────────────────────────────────────────────────────────────────────

void bench_all_algorithms(ankerl::nanobench::Bench& b) {
  auto data = generate_random_data(100 * 1024); // 100KB

  b.run("straylight/md5/100KB", [&] {
    auto h = hash::md5(data);
    ankerl::nanobench::doNotOptimizeAway(h);
  });

  b.run("straylight/sha1/100KB", [&] {
    auto h = hash::sha1(data);
    ankerl::nanobench::doNotOptimizeAway(h);
  });

  b.run("straylight/sha256/100KB", [&] {
    auto h = hash::sha256(data);
    ankerl::nanobench::doNotOptimizeAway(h);
  });

  b.run("straylight/sha512/100KB", [&] {
    auto h = hash::sha512(data);
    ankerl::nanobench::doNotOptimizeAway(h);
  });

  b.run("straylight/blake3/100KB", [&] {
    auto h = hash::blake3(data);
    ankerl::nanobench::doNotOptimizeAway(h);
  });

  // Expected order (fastest to slowest): BLAKE3 >> SHA512 > SHA256 > SHA1 > MD5
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Encoding overhead
// Shows cost of hash output encoding (hex, base64, nix32)
// ─────────────────────────────────────────────────────────────────────────────

void bench_encoding_overhead(ankerl::nanobench::Bench& b) {
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

  // hash + encode (realistic use case)
  auto test_data = "some content to hash";

  b.run("sha256+hex", [&] {
    auto h = hash::sha256(test_data);
    auto s = h.to_hex();
    ankerl::nanobench::doNotOptimizeAway(s);
  });

  b.run("blake3+hex", [&] {
    auto h = hash::blake3(test_data);
    auto s = h.to_hex();
    ankerl::nanobench::doNotOptimizeAway(s);
  });

  b.run("sha256+nix32", [&] {
    auto h = hash::sha256(test_data);
    auto s = h.to_nix32();
    ankerl::nanobench::doNotOptimizeAway(s);
  });

  b.run("blake3+nix32", [&] {
    auto h = hash::blake3(test_data);
    auto s = h.to_nix32();
    ankerl::nanobench::doNotOptimizeAway(s);
  });

  // Expected: encoding is negligible compared to hashing
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Store path hash (real-world use case)
// SHA256 compressed to 160 bits, encoded in nix32
// ─────────────────────────────────────────────────────────────────────────────

void bench_store_path_hash(ankerl::nanobench::Bench& b) {
  auto paths = generate_store_paths(1000);

  // Full store path hash workflow
  b.run("store_path_hash/workflow/1000", [&] {
    for (const auto& path : paths) {
      auto h = hash::store_path_hash(path);
      auto s = h.to_nix32();
      ankerl::nanobench::doNotOptimizeAway(s);
    }
  });

  // Compare to raw SHA256 + nix32 (without compression)
  b.run("sha256+nix32/1000", [&] {
    for (const auto& path : paths) {
      auto h = hash::sha256(path);
      auto s = h.to_nix32();
      ankerl::nanobench::doNotOptimizeAway(s);
    }
  });

  // BLAKE3 alternative
  b.run("blake3+nix32/1000", [&] {
    for (const auto& path : paths) {
      auto h = hash::blake3(path);
      auto s = h.to_nix32();
      ankerl::nanobench::doNotOptimizeAway(s);
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
  std::cout << "Hash Comparison: OpenSSL SHA256 vs straylight hash.h (BLAKE3)\n";
  std::cout << "================================================================\n";
#ifdef __AVX512F__
  std::cout << "AVX-512: ENABLED (optimal BLAKE3 performance)\n";
#else
  std::cout << "AVX-512: disabled (BLAKE3 will use AVX2 fallback)\n";
#endif
#ifdef __AVX2__
  std::cout << "AVX2: ENABLED\n";
#endif
#ifdef __SHA__
  std::cout << "SHA-NI: ENABLED (accelerates SHA256)\n";
#endif
  std::cout << "\n";
  std::cout << "Expected speedups (BLAKE3 vs SHA256):\n";
  std::cout << "  - Small (<1KB): 2-5x\n";
  std::cout << "  - Medium (1-100KB): 5-15x\n";
  std::cout << "  - Large (>1MB): 10-30x (with AVX-512)\n";
  std::cout << "\n";

  ankerl::nanobench::Bench b;
  b.title("Hash Comparison");
  b.warmup(100);
  b.minEpochIterations(10);
  b.relative(true);

  std::cout << "=== Small Data (~60 bytes, store paths) ===\n";
  bench_small_data(b);

  std::cout << "\n=== Medium Data (~1KB, derivations) ===\n";
  bench_medium_data(b);

  std::cout << "\n=== Large Data (1MB, 10MB) ===\n";
  bench_large_data(b);

  std::cout << "\n=== Streaming vs One-shot ===\n";
  bench_streaming(b);

  std::cout << "\n=== All Algorithms Comparison (100KB) ===\n";
  bench_all_algorithms(b);

  std::cout << "\n=== Encoding Overhead ===\n";
  bench_encoding_overhead(b);

  std::cout << "\n=== Store Path Hash Workflow ===\n";
  bench_store_path_hash(b);

  return 0;
}
