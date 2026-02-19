// straylight::nix::primitives::encoding benchmarks
//
// Benchmarks for base16/base64/nix32 encoding primitives using nanobench.
// Tests realistic workloads from Nix store operations.

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "../encoding.h"

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

namespace encoding = straylight::nix::primitives::encoding;

namespace {

// Generate random binary data
std::vector<uint8_t> generate_random_data(std::size_t size, uint64_t seed = 42) {
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<uint8_t> dist(0, 255);
  std::vector<uint8_t> data(size);
  for (auto& b : data) {
    b = dist(rng);
  }
  return data;
}

} // namespace

int main() {
  ankerl::nanobench::Bench bench;
  bench.title("Encoding Benchmarks").warmup(100).minEpochIterations(1000).unit("op");

  // ───────────────────────────────────────────────────────────────────────────
  // Test data sizes relevant to Nix
  // ───────────────────────────────────────────────────────────────────────────

  // SHA256 hash (32 bytes) - most common
  auto data_32 = generate_random_data(32);

  // SHA512 hash (64 bytes)
  auto data_64 = generate_random_data(64);

  // Truncated store path hash (20 bytes, 160 bits)
  auto data_20 = generate_random_data(20);

  // Medium content (1KB - small file hash, NAR header)
  auto data_1k = generate_random_data(1024);

  // Large content (64KB - NAR chunk)
  auto data_64k = generate_random_data(64 * 1024);

  // Pre-encode for decode benchmarks
  auto hex_32 = encoding::base16::encode(data_32);
  auto b64_32 = encoding::base64::encode(data_32);
  auto nix32_32 = encoding::nix32::encode(data_32);

  auto hex_20 = encoding::base16::encode(data_20);
  auto nix32_20 = encoding::nix32::encode(data_20);

  // ───────────────────────────────────────────────────────────────────────────
  // Base16 (Hex) Benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("base16/encode/32B (SHA256)", [&] {
    auto result = encoding::base16::encode(data_32);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("base16/encode/20B (store path)", [&] {
    auto result = encoding::base16::encode(data_20);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("base16/encode/1KB", [&] {
    auto result = encoding::base16::encode(data_1k);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("base16/encode/64KB", [&] {
    auto result = encoding::base16::encode(data_64k);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("base16/decode/32B", [&] {
    auto result = encoding::base16::decode(hex_32);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("base16/decode/20B", [&] {
    auto result = encoding::base16::decode(hex_20);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Base64 Benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("base64/encode/32B (SHA256)", [&] {
    auto result = encoding::base64::encode(data_32);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("base64/encode/64B (SHA512)", [&] {
    auto result = encoding::base64::encode(data_64);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("base64/encode/1KB", [&] {
    auto result = encoding::base64::encode(data_1k);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("base64/encode/64KB", [&] {
    auto result = encoding::base64::encode(data_64k);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("base64/decode/32B", [&] {
    auto result = encoding::base64::decode(b64_32);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Nix32 Benchmarks (most important for store paths)
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("nix32/encode/32B (SHA256)", [&] {
    auto result = encoding::nix32::encode(data_32);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("nix32/encode/20B (store path)", [&] {
    auto result = encoding::nix32::encode(data_20);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("nix32/decode/32B", [&] {
    auto result = encoding::nix32::decode(nix32_32);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("nix32/decode/20B (store path)", [&] {
    auto result = encoding::nix32::decode(nix32_20);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // Validation Benchmarks (used for reference scanning in NAR)
  // ───────────────────────────────────────────────────────────────────────────

  // Generate valid and invalid strings for testing
  std::string valid_nix32 = "0123456789abcdfghijklmnpqrsvwxyz";
  std::string invalid_nix32 = "hello"; // contains 'e' and 'o'

  // Large valid string (1KB) for SIMD testing
  std::string valid_nix32_1k;
  valid_nix32_1k.reserve(1024);
  while (valid_nix32_1k.size() < 1024) {
    valid_nix32_1k += valid_nix32;
  }
  valid_nix32_1k.resize(1024);

  // 64-byte string (exactly fits AVX-512)
  std::string valid_nix32_64 = valid_nix32 + valid_nix32;

  bench.run("nix32/is_valid/32 chars (valid)", [&] {
    auto result = encoding::nix32::is_valid(valid_nix32);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("nix32/is_valid/64 chars (AVX-512)", [&] {
    auto result = encoding::nix32::is_valid(valid_nix32_64);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("nix32/is_valid/1KB (SIMD bulk)", [&] {
    auto result = encoding::nix32::is_valid(valid_nix32_1k);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("nix32/is_valid/5 chars (invalid)", [&] {
    auto result = encoding::nix32::is_valid(invalid_nix32);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  bench.run("nix32/lookup_reverse", [&] {
    // Simulate scanning: lookup each character
    uint8_t sum = 0;
    for (char c : valid_nix32) {
      sum += encoding::nix32::lookup_reverse(c);
    }
    ankerl::nanobench::doNotOptimizeAway(sum);
  });

  return 0;
}
