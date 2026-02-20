// straylight::nix::primitives::bench::encoding_cmp_bench
//
// Comprehensive comparison benchmarks: straylight encoding.h vs nix/util/base*.h
//
// This benchmark compares:
//   - straylight::base16 vs nix::base16 (hexadecimal encoding)
//   - straylight::base64 vs nix::base64 (RFC 4648)
//   - straylight::nix32 vs nix::base_nix32_t (Nix-specific base32)
//
// Expected speedups:
//   - base16 encode: 2-5x faster (SIMD hex encoding)
//   - base16 decode: 3-8x faster (lookup table + SIMD)
//   - base64 encode/decode: 2-5x faster
//   - nix32 encode/decode: 2-4x faster (optimized lookup tables)
//   - nix32 validation: 10-50x faster (SIMD character validation)
//
// Why straylight encoding is faster:
//   - 256-byte lookup tables for O(1) character mapping
//   - SIMD-accelerated character validation
//   - Pre-computed output sizes (no reallocation)
//   - Optimized bit manipulation

#define ANKERL_NANOBENCH_IMPLEMENT
#include <cstdint>
#include <iostream>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <nanobench.h>

// Straylight primitives
#include "../encoding.h"

namespace encoding = straylight::nix::crypto;

// ─────────────────────────────────────────────────────────────────────────────
// Nix-compatible baseline implementations (from nix/util/base-n.h, base-nix-32.h)
// These are faithful reimplementations of the nix encoding functions
// ─────────────────────────────────────────────────────────────────────────────

namespace nix_impl {

namespace base16 {

constexpr const char* hex_chars = "0123456789abcdef";

std::string encode(std::span<const std::byte> input) {
  std::string result;
  result.reserve(input.size() * 2);
  for (auto b : input) {
    result += hex_chars[static_cast<unsigned char>(b) >> 4];
    result += hex_chars[static_cast<unsigned char>(b) & 0x0F];
  }
  return result;
}

uint8_t hex_char_to_int(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return 0; // error
}

std::string decode(std::string_view s) {
  if (s.size() % 2 != 0)
    throw std::runtime_error("invalid hex string length");

  std::string result;
  result.reserve(s.size() / 2);
  for (std::size_t i = 0; i < s.size(); i += 2) {
    result += static_cast<char>((hex_char_to_int(s[i]) << 4) | hex_char_to_int(s[i + 1]));
  }
  return result;
}

} // namespace base16

namespace base64 {

constexpr const char* b64_chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string encode(std::span<const std::byte> input) {
  std::string result;
  result.reserve(((input.size() + 2) / 3) * 4);

  std::size_t i = 0;
  while (i + 3 <= input.size()) {
    uint32_t triple = (static_cast<uint32_t>(input[i]) << 16) |
                      (static_cast<uint32_t>(input[i + 1]) << 8) |
                      static_cast<uint32_t>(input[i + 2]);
    result += b64_chars[(triple >> 18) & 0x3F];
    result += b64_chars[(triple >> 12) & 0x3F];
    result += b64_chars[(triple >> 6) & 0x3F];
    result += b64_chars[triple & 0x3F];
    i += 3;
  }

  if (i + 1 == input.size()) {
    uint32_t val = static_cast<uint32_t>(input[i]) << 16;
    result += b64_chars[(val >> 18) & 0x3F];
    result += b64_chars[(val >> 12) & 0x3F];
    result += '=';
    result += '=';
  } else if (i + 2 == input.size()) {
    uint32_t val =
        (static_cast<uint32_t>(input[i]) << 16) | (static_cast<uint32_t>(input[i + 1]) << 8);
    result += b64_chars[(val >> 18) & 0x3F];
    result += b64_chars[(val >> 12) & 0x3F];
    result += b64_chars[(val >> 6) & 0x3F];
    result += '=';
  }

  return result;
}

} // namespace base64

namespace nix32 {

// omits e, o, u, t
constexpr const char nix32_chars[33] = "0123456789abcdfghijklmnpqrsvwxyz";

std::string encode(std::span<const std::byte> input) {
  if (input.empty())
    return "";

  // Reverse the input (nix32 is LSB-first)
  std::vector<std::byte> reversed(input.rbegin(), input.rend());

  std::size_t len = ((input.size() * 8) + 4) / 5;
  std::string result(len, '\0');

  std::size_t bit_pos = 0;
  for (std::size_t i = 0; i < len; ++i) {
    std::size_t byte_pos = bit_pos / 8;
    std::size_t bit_offset = bit_pos % 8;

    uint8_t val = 0;
    if (byte_pos < reversed.size()) {
      val = static_cast<uint8_t>(reversed[byte_pos]) >> bit_offset;
      if (byte_pos + 1 < reversed.size() && bit_offset > 3) {
        val |= static_cast<uint8_t>(reversed[byte_pos + 1]) << (8 - bit_offset);
      }
    }
    val &= 0x1F;

    result[len - 1 - i] = nix32_chars[val];
    bit_pos += 5;
  }

  return result;
}

uint8_t nix32_char_to_int(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'd')
    return 10 + (c - 'a');
  if (c == 'f')
    return 14;
  if (c >= 'g' && c <= 'n')
    return 15 + (c - 'g');
  if (c >= 'p' && c <= 's')
    return 23 + (c - 'p');
  if (c == 'v')
    return 27;
  if (c >= 'w' && c <= 'z')
    return 28 + (c - 'w');
  return 0xFF; // invalid
}

std::string decode(std::string_view s) {
  if (s.empty())
    return "";

  std::size_t out_len = (s.size() * 5) / 8;
  std::vector<std::byte> result(out_len, std::byte{0});

  std::size_t bit_pos = 0;
  for (std::size_t i = s.size(); i > 0; --i) {
    uint8_t val = nix32_char_to_int(s[i - 1]);
    if (val == 0xFF)
      throw std::runtime_error("invalid nix32 character");

    std::size_t byte_pos = bit_pos / 8;
    std::size_t bit_offset = bit_pos % 8;

    if (byte_pos < result.size()) {
      result[byte_pos] =
          static_cast<std::byte>(static_cast<uint8_t>(result[byte_pos]) | (val << bit_offset));
      if (byte_pos + 1 < result.size() && bit_offset > 3) {
        result[byte_pos + 1] = static_cast<std::byte>(val >> (8 - bit_offset));
      }
    }
    bit_pos += 5;
  }

  // Reverse the result
  std::reverse(result.begin(), result.end());

  return std::string(reinterpret_cast<const char*>(result.data()), result.size());
}

bool is_valid(std::string_view s) {
  for (char c : s) {
    if (nix32_char_to_int(c) == 0xFF)
      return false;
  }
  return true;
}

} // namespace nix32

} // namespace nix_impl

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

std::span<const std::byte> to_byte_span(const std::vector<uint8_t>& data) {
  return std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size());
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Base16 (Hex) encode
// ─────────────────────────────────────────────────────────────────────────────

void bench_base16_encode(ankerl::nanobench::Bench& b) {
  auto data_32 = generate_random_data(32);         // SHA256 hash
  auto data_20 = generate_random_data(20);         // Store path hash (160 bits)
  auto data_1k = generate_random_data(1024);       // Small file
  auto data_64k = generate_random_data(64 * 1024); // NAR chunk

  // 32 bytes (SHA256 hash - most common)
  b.run("nix/base16/encode/32B", [&] {
    auto result = nix_impl::base16::encode(to_byte_span(data_32));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("straylight/base16/encode/32B", [&] {
    auto result = encoding::base16::encode(data_32);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // 20 bytes (store path hash)
  b.run("nix/base16/encode/20B", [&] {
    auto result = nix_impl::base16::encode(to_byte_span(data_20));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("straylight/base16/encode/20B", [&] {
    auto result = encoding::base16::encode(data_20);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // 1KB
  b.run("nix/base16/encode/1KB", [&] {
    auto result = nix_impl::base16::encode(to_byte_span(data_1k));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("straylight/base16/encode/1KB", [&] {
    auto result = encoding::base16::encode(data_1k);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // 64KB
  b.run("nix/base16/encode/64KB", [&] {
    auto result = nix_impl::base16::encode(to_byte_span(data_64k));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("straylight/base16/encode/64KB", [&] {
    auto result = encoding::base16::encode(data_64k);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // Expected: straylight 2-5x faster
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Base16 (Hex) decode
// ─────────────────────────────────────────────────────────────────────────────

void bench_base16_decode(ankerl::nanobench::Bench& b) {
  auto data_32 = generate_random_data(32);
  auto data_20 = generate_random_data(20);

  auto hex_32 = encoding::base16::encode(data_32);
  auto hex_20 = encoding::base16::encode(data_20);

  b.run("nix/base16/decode/32B", [&] {
    auto result = nix_impl::base16::decode(hex_32);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("straylight/base16/decode/32B", [&] {
    auto result = encoding::base16::decode(hex_32);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("nix/base16/decode/20B", [&] {
    auto result = nix_impl::base16::decode(hex_20);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("straylight/base16/decode/20B", [&] {
    auto result = encoding::base16::decode(hex_20);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // Expected: straylight 3-8x faster
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Base64 encode
// ─────────────────────────────────────────────────────────────────────────────

void bench_base64_encode(ankerl::nanobench::Bench& b) {
  auto data_32 = generate_random_data(32); // SHA256 hash
  auto data_64 = generate_random_data(64); // SHA512 hash
  auto data_1k = generate_random_data(1024);
  auto data_64k = generate_random_data(64 * 1024);

  b.run("nix/base64/encode/32B", [&] {
    auto result = nix_impl::base64::encode(to_byte_span(data_32));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("straylight/base64/encode/32B", [&] {
    auto result = encoding::base64::encode(data_32);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("nix/base64/encode/64B", [&] {
    auto result = nix_impl::base64::encode(to_byte_span(data_64));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("straylight/base64/encode/64B", [&] {
    auto result = encoding::base64::encode(data_64);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("nix/base64/encode/1KB", [&] {
    auto result = nix_impl::base64::encode(to_byte_span(data_1k));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("straylight/base64/encode/1KB", [&] {
    auto result = encoding::base64::encode(data_1k);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("nix/base64/encode/64KB", [&] {
    auto result = nix_impl::base64::encode(to_byte_span(data_64k));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("straylight/base64/encode/64KB", [&] {
    auto result = encoding::base64::encode(data_64k);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // Expected: straylight 2-5x faster
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Nix32 encode (most important for store paths)
// ─────────────────────────────────────────────────────────────────────────────

void bench_nix32_encode(ankerl::nanobench::Bench& b) {
  auto data_32 = generate_random_data(32); // SHA256 hash
  auto data_20 = generate_random_data(20); // Store path hash (160 bits)

  b.run("nix/nix32/encode/32B", [&] {
    auto result = nix_impl::nix32::encode(to_byte_span(data_32));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("straylight/nix32/encode/32B", [&] {
    auto result = encoding::nix32::encode(data_32);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("nix/nix32/encode/20B", [&] {
    auto result = nix_impl::nix32::encode(to_byte_span(data_20));
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("straylight/nix32/encode/20B", [&] {
    auto result = encoding::nix32::encode(data_20);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // Expected: straylight 2-4x faster
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Nix32 decode
// ─────────────────────────────────────────────────────────────────────────────

void bench_nix32_decode(ankerl::nanobench::Bench& b) {
  auto data_32 = generate_random_data(32);
  auto data_20 = generate_random_data(20);

  auto nix32_32 = encoding::nix32::encode(data_32);
  auto nix32_20 = encoding::nix32::encode(data_20);

  b.run("nix/nix32/decode/32B", [&] {
    auto result = nix_impl::nix32::decode(nix32_32);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("straylight/nix32/decode/32B", [&] {
    auto result = encoding::nix32::decode(nix32_32);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("nix/nix32/decode/20B", [&] {
    auto result = nix_impl::nix32::decode(nix32_20);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("straylight/nix32/decode/20B", [&] {
    auto result = encoding::nix32::decode(nix32_20);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // Expected: straylight 2-4x faster
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Nix32 validation (SIMD-accelerated)
// Important for NAR reference scanning
// ─────────────────────────────────────────────────────────────────────────────

void bench_nix32_validation(ankerl::nanobench::Bench& b) {
  // Valid nix32 string (store path hash, 32 chars)
  std::string valid_32 = "0123456789abcdfghijklmnpqrsvwxyz";

  // Valid 64 chars (exactly fits AVX-512)
  std::string valid_64 = valid_32 + valid_32;

  // Valid 1KB (bulk SIMD testing)
  std::string valid_1k;
  valid_1k.reserve(1024);
  while (valid_1k.size() < 1024) {
    valid_1k += valid_32;
  }
  valid_1k.resize(1024);

  // Invalid string (contains 'e' which is not in nix32 alphabet)
  std::string invalid = "hello"; // contains 'e' and 'o'

  b.run("nix/nix32/is_valid/32_chars", [&] {
    auto result = nix_impl::nix32::is_valid(valid_32);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("straylight/nix32/is_valid/32_chars", [&] {
    auto result = encoding::nix32::is_valid(valid_32);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("nix/nix32/is_valid/64_chars", [&] {
    auto result = nix_impl::nix32::is_valid(valid_64);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("straylight/nix32/is_valid/64_chars", [&] {
    auto result = encoding::nix32::is_valid(valid_64);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("nix/nix32/is_valid/1KB", [&] {
    auto result = nix_impl::nix32::is_valid(valid_1k);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("straylight/nix32/is_valid/1KB", [&] {
    auto result = encoding::nix32::is_valid(valid_1k);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // Invalid (early exit)
  b.run("nix/nix32/is_valid/invalid", [&] {
    auto result = nix_impl::nix32::is_valid(invalid);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  b.run("straylight/nix32/is_valid/invalid", [&] {
    auto result = encoding::nix32::is_valid(invalid);
    ankerl::nanobench::doNotOptimizeAway(result);
  });

  // Expected: straylight 10-50x faster (SIMD validation)
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark: Batch encoding (realistic store path workflow)
// ─────────────────────────────────────────────────────────────────────────────

void bench_batch_encoding(ankerl::nanobench::Bench& b) {
  // Generate 1000 random 20-byte hashes (store path hashes)
  std::vector<std::vector<uint8_t>> hashes;
  hashes.reserve(1000);
  for (int i = 0; i < 1000; ++i) {
    hashes.push_back(generate_random_data(20, 42 + i));
  }

  b.run("nix/nix32/batch_encode/1000", [&] {
    std::vector<std::string> results;
    results.reserve(1000);
    for (const auto& hash : hashes) {
      results.push_back(nix_impl::nix32::encode(to_byte_span(hash)));
    }
    ankerl::nanobench::doNotOptimizeAway(results);
  });

  b.run("straylight/nix32/batch_encode/1000", [&] {
    std::vector<std::string> results;
    results.reserve(1000);
    for (const auto& hash : hashes) {
      results.push_back(encoding::nix32::encode(hash));
    }
    ankerl::nanobench::doNotOptimizeAway(results);
  });

  // Expected: straylight 2-4x faster in aggregate
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
  std::cout << "Encoding Comparison: nix/util/base*.h vs straylight encoding.h\n";
  std::cout << "================================================================\n";
#ifdef __AVX512F__
  std::cout << "AVX-512: ENABLED (optimal validation performance)\n";
#else
  std::cout << "AVX-512: disabled\n";
#endif
#ifdef __AVX2__
  std::cout << "AVX2: ENABLED\n";
#endif
  std::cout << "\n";
  std::cout << "Expected speedups:\n";
  std::cout << "  - base16 encode: 2-5x\n";
  std::cout << "  - base16 decode: 3-8x\n";
  std::cout << "  - base64 encode: 2-5x\n";
  std::cout << "  - nix32 encode/decode: 2-4x\n";
  std::cout << "  - nix32 validation: 10-50x (SIMD)\n";
  std::cout << "\n";

  ankerl::nanobench::Bench b;
  b.title("Encoding Comparison").warmup(100).minEpochIterations(1000).unit("op");
  b.relative(true);

  std::cout << "=== Base16 Encode ===\n";
  bench_base16_encode(b);

  std::cout << "\n=== Base16 Decode ===\n";
  bench_base16_decode(b);

  std::cout << "\n=== Base64 Encode ===\n";
  bench_base64_encode(b);

  std::cout << "\n=== Nix32 Encode ===\n";
  bench_nix32_encode(b);

  std::cout << "\n=== Nix32 Decode ===\n";
  bench_nix32_decode(b);

  std::cout << "\n=== Nix32 Validation (SIMD) ===\n";
  bench_nix32_validation(b);

  std::cout << "\n=== Batch Encoding ===\n";
  bench_batch_encoding(b);

  return 0;
}
