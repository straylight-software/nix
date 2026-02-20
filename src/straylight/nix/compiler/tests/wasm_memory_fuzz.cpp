/// wasm_memory_fuzz.cpp - Fuzz testing for WASM memory subsystem
///
/// This file provides both:
/// 1. LibFuzzer entry point for continuous fuzzing
/// 2. RapidCheck property-based tests for CI
///
/// Build for fuzzing:
///   clang++ -std=c++23 -g -O1 -fsanitize=fuzzer,address,undefined \
///     -I. wasm_memory_fuzz.cpp -o wasm_memory_fuzz
///
/// Run fuzzer:
///   ./wasm_memory_fuzz -max_len=4096 corpus/

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "straylight/nix/compiler/runtime/wasm_memory.h"

using namespace straylight::nix::compiler::runtime;

// =============================================================================
// Fuzzer operation encoding
// =============================================================================

enum class Op : uint8_t {
  Allocate = 0,
  WriteU8 = 1,
  WriteU32 = 2,
  WriteU64 = 3,
  ReadU8 = 4,
  ReadU32 = 5,
  ReadU64 = 6,
  WriteString = 7,
  ReadString = 8,
  Copy = 9,
  Reset = 10,
  // Add more operations as needed
  MAX_OP
};

// =============================================================================
// Shadow memory for verification
// =============================================================================

/// Shadow memory tracks what we've written so we can verify reads
struct shadow_memory {
  explicit shadow_memory(std::size_t initial_size = 1024 * 1024)
      : data_(initial_size, 0), valid_(initial_size, false) {}

  void write(std::uint32_t offset, const void* src, std::size_t len) {
    ensure_size(offset + len);
    std::memcpy(data_.data() + offset, src, len);
    std::fill(valid_.begin() + offset, valid_.begin() + offset + len, true);
  }

  bool verify(std::uint32_t offset, const void* expected, std::size_t len) const {
    if (offset + len > data_.size())
      return false;
    // Only verify bytes we've written
    for (std::size_t idx = 0; idx < len; ++idx) {
      if (valid_[offset + idx]) {
        if (data_[offset + idx] != static_cast<const uint8_t*>(expected)[idx]) {
          return false;
        }
      }
    }
    return true;
  }

  void reset() { std::fill(valid_.begin(), valid_.end(), false); }

  /// Mark a range of bytes as invalid (for operations we don't track)
  void invalidate(std::uint32_t offset, std::size_t len) {
    ensure_size(offset + len);
    std::fill(valid_.begin() + offset, valid_.begin() + offset + len, false);
  }

private:
  void ensure_size(std::size_t needed) {
    if (needed > data_.size()) {
      data_.resize(needed * 2, 0);
      valid_.resize(needed * 2, false);
    }
  }

  std::vector<uint8_t> data_;
  std::vector<bool> valid_;
};

// =============================================================================
// Fuzz target - exercises memory operations from fuzzer input
// =============================================================================

/// Execute a sequence of operations from fuzzer input
/// Returns true if no invariants were violated
bool fuzz_memory_ops(const uint8_t* data, size_t size) {
  if (size < 4)
    return true; // Need at least some input

  // Configuration from first bytes
  uint32_t heap_base = (data[0] % 4) * 0x10000; // 0, 64K, 128K, or 192K
  uint32_t initial_pages = 1 + (data[1] % 4);   // 1-4 pages
  uint32_t max_pages = 16 + (data[2] % 48);     // 16-64 pages

  data += 4;
  size -= 4;

  test_memory tm(initial_pages, max_pages, heap_base);
  auto& mem = tm.memory();
  shadow_memory shadow;

  // Track allocations for later verification
  std::vector<std::pair<mem_offset, std::uint32_t>> allocations;

  size_t pos = 0;
  while (pos < size) {
    if (pos + 1 > size)
      break;

    auto op = static_cast<Op>(data[pos] % static_cast<uint8_t>(Op::MAX_OP));
    pos++;

    switch (op) {
      case Op::Allocate: {
        if (pos + 2 > size)
          break;
        uint32_t alloc_size = data[pos] | (static_cast<uint32_t>(data[pos + 1]) << 8);
        alloc_size = alloc_size % 10000; // Cap at 10KB per allocation
        pos += 2;

        try {
          auto off = mem.allocate(alloc_size);
          allocations.emplace_back(off, alloc_size);
        } catch (const memory_exhausted_error&) {
          // Expected when we run out of memory
        }
        break;
      }

      case Op::WriteU8: {
        if (pos + 2 > size || allocations.empty())
          break;
        size_t idx = data[pos] % allocations.size();
        uint8_t val = data[pos + 1];
        pos += 2;

        auto [off, sz] = allocations[idx];
        if (sz > 0) {
          try {
            mem.write<uint8_t>(off, val);
            shadow.write(off.raw(), &val, 1);
          } catch (const memory_bounds_error&) {
            // Shouldn't happen for valid allocations
            return false;
          }
        }
        break;
      }

      case Op::WriteU32: {
        if (pos + 5 > size || allocations.empty())
          break;
        size_t idx = data[pos] % allocations.size();
        uint32_t val;
        std::memcpy(&val, data + pos + 1, 4);
        pos += 5;

        auto [off, sz] = allocations[idx];
        if (sz >= 4) {
          try {
            mem.write<uint32_t>(off, val);
            shadow.write(off.raw(), &val, 4);
          } catch (const memory_bounds_error&) {
            return false;
          }
        }
        break;
      }

      case Op::WriteU64: {
        if (pos + 9 > size || allocations.empty())
          break;
        size_t idx = data[pos] % allocations.size();
        uint64_t val;
        std::memcpy(&val, data + pos + 1, 8);
        pos += 9;

        auto [off, sz] = allocations[idx];
        if (sz >= 8) {
          try {
            mem.write<uint64_t>(off, val);
            shadow.write(off.raw(), &val, 8);
          } catch (const memory_bounds_error&) {
            return false;
          }
        }
        break;
      }

      case Op::ReadU8: {
        if (pos + 1 > size || allocations.empty())
          break;
        size_t idx = data[pos] % allocations.size();
        pos += 1;

        auto [off, sz] = allocations[idx];
        if (sz > 0) {
          try {
            auto val = mem.read<uint8_t>(off);
            if (!shadow.verify(off.raw(), &val, 1)) {
              return false; // Data corruption!
            }
          } catch (const memory_bounds_error&) {
            return false;
          }
        }
        break;
      }

      case Op::ReadU32: {
        if (pos + 1 > size || allocations.empty())
          break;
        size_t idx = data[pos] % allocations.size();
        pos += 1;

        auto [off, sz] = allocations[idx];
        if (sz >= 4) {
          try {
            auto val = mem.read<uint32_t>(off);
            if (!shadow.verify(off.raw(), &val, 4)) {
              return false;
            }
          } catch (const memory_bounds_error&) {
            return false;
          }
        }
        break;
      }

      case Op::ReadU64: {
        if (pos + 1 > size || allocations.empty())
          break;
        size_t idx = data[pos] % allocations.size();
        pos += 1;

        auto [off, sz] = allocations[idx];
        if (sz >= 8) {
          try {
            auto val = mem.read<uint64_t>(off);
            if (!shadow.verify(off.raw(), &val, 8)) {
              return false;
            }
          } catch (const memory_bounds_error&) {
            return false;
          }
        }
        break;
      }

      case Op::WriteString: {
        if (pos + 2 > size || allocations.empty())
          break;
        size_t idx = data[pos] % allocations.size();
        size_t str_len = data[pos + 1] % 64; // Max 64 char string
        pos += 2;

        if (pos + str_len > size)
          str_len = size - pos;

        auto [off, sz] = allocations[idx];
        if (sz > str_len) {
          try {
            std::string_view str(reinterpret_cast<const char*>(data + pos), str_len);
            // Remove any null bytes from the string
            std::string clean_str;
            for (char c : str) {
              if (c != '\0')
                clean_str += c;
            }
            mem.write_string(off, clean_str);
            // Invalidate shadow for bytes written (string + null terminator)
            shadow.invalidate(off.raw(), clean_str.size() + 1);
          } catch (const memory_bounds_error&) {
            // OK
          }
        }
        pos += str_len;
        break;
      }

      case Op::ReadString: {
        if (pos + 1 > size || allocations.empty())
          break;
        size_t idx = data[pos] % allocations.size();
        pos += 1;

        auto [off, sz] = allocations[idx];
        if (sz > 0) {
          try {
            // Write a null terminator first to ensure valid string
            auto null_pos = off + (sz > 1 ? 1 : 0);
            mem.write<uint8_t>(null_pos, 0);
            // Invalidate the byte we wrote
            shadow.invalidate(null_pos.raw(), 1);
            auto str = mem.read_string(off);
            // Just verify it doesn't crash
            (void)str;
          } catch (const memory_bounds_error&) {
            // OK
          }
        }
        break;
      }

      case Op::Copy: {
        if (pos + 3 > size || allocations.size() < 2)
          break;
        size_t src_idx = data[pos] % allocations.size();
        size_t dst_idx = data[pos + 1] % allocations.size();
        size_t len = data[pos + 2];
        pos += 3;

        auto [src_off, src_sz] = allocations[src_idx];
        auto [dst_off, dst_sz] = allocations[dst_idx];

        len = std::min({len, static_cast<size_t>(src_sz), static_cast<size_t>(dst_sz)});
        if (len > 0) {
          try {
            mem.copy(dst_off, src_off, static_cast<uint32_t>(len));
            // Invalidate destination shadow since we don't track source data
            shadow.invalidate(dst_off.raw(), len);
          } catch (const memory_bounds_error&) {
            // OK
          }
        }
        break;
      }

      case Op::Reset: {
        mem.reset_heap();
        allocations.clear();
        shadow.reset();
        break;
      }

      default:
        break;
    }
  }

  // Final verification: all written data should still be readable
  for (const auto& [off, sz] : allocations) {
    if (sz >= 8) {
      try {
        auto val = mem.read<uint64_t>(off);
        if (!shadow.verify(off.raw(), &val, 8)) {
          return false;
        }
      } catch (const memory_bounds_error&) {
        // Allocation might have been overwritten by reset
      }
    }
  }

  return true;
}

// =============================================================================
// LibFuzzer entry point
// =============================================================================

#ifdef FUZZING_BUILD_MODE_LIBFUZZER

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (!fuzz_memory_ops(data, size)) {
    __builtin_trap(); // Signal invariant violation
  }
  return 0;
}

#endif

// =============================================================================
// RapidCheck property tests (for CI without fuzzer)
// =============================================================================

#ifndef FUZZING_BUILD_MODE_LIBFUZZER

#  include <rapidcheck.h>

#  include <catch2/catch_test_macros.hpp>

TEST_CASE("wasm_memory: property - allocations never overlap", "[memory][property]") {
  rc::check("allocations are disjoint", []() {
    test_memory tm(1, 64, 0x1000);
    auto& mem = tm.memory();

    auto sizes = *rc::gen::container<std::vector<uint32_t>>(rc::gen::inRange(1u, 1000u));

    std::vector<std::pair<uint32_t, uint32_t>> ranges; // (start, end)

    for (auto size : sizes) {
      try {
        auto off = mem.allocate(size);
        uint32_t aligned_size = (size + 7) & ~7u;
        ranges.emplace_back(off.raw(), off.raw() + aligned_size);
      } catch (const memory_exhausted_error&) {
        break;
      }
    }

    // Check no overlaps
    for (size_t idx = 0; idx < ranges.size(); ++idx) {
      for (size_t jdx = idx + 1; jdx < ranges.size(); ++jdx) {
        auto [s1, e1] = ranges[idx];
        auto [s2, e2] = ranges[jdx];
        RC_ASSERT(e1 <= s2 || e2 <= s1);
      }
    }
  });
}

TEST_CASE("wasm_memory: property - data survives growth", "[memory][property]") {
  rc::check("written data readable after growth", []() {
    test_memory tm(1, 256, 0x1000);
    auto& mem = tm.memory();

    // Write some data
    auto off = mem.allocate(8);
    auto val = *rc::gen::arbitrary<uint64_t>();
    mem.write_u64(off, val);

    // Force growth
    auto grow_size = *rc::gen::inRange(65536u, 500000u);
    try {
      mem.allocate(grow_size);
    } catch (const memory_exhausted_error&) {
      // OK
    }

    // Data must survive
    RC_ASSERT(mem.read_u64(off) == val);
  });
}

TEST_CASE("wasm_memory: property - alignment always correct", "[memory][property]") {
  rc::check("all allocations 8-byte aligned", []() {
    test_memory tm(1, 64, 0); // heap at 0 for easy checking
    auto& mem = tm.memory();

    auto sizes = *rc::gen::container<std::vector<uint32_t>>(rc::gen::inRange(0u, 500u));

    for (auto size : sizes) {
      try {
        auto off = mem.allocate(size);
        RC_ASSERT(off.raw() % 8 == 0);
      } catch (const memory_exhausted_error&) {
        break;
      }
    }
  });
}

TEST_CASE("wasm_memory: property - heap usage monotonic", "[memory][property]") {
  rc::check("heap usage only increases (until reset)", []() {
    test_memory tm(1, 64, 0x1000);
    auto& mem = tm.memory();

    uint32_t prev_usage = 0;
    auto sizes = *rc::gen::container<std::vector<uint32_t>>(rc::gen::inRange(1u, 200u));

    for (auto size : sizes) {
      try {
        mem.allocate(size);
        auto usage = mem.heap_used();
        RC_ASSERT(usage >= prev_usage);
        prev_usage = usage;
      } catch (const memory_exhausted_error&) {
        break;
      }
    }
  });
}

TEST_CASE("wasm_memory: property - bounds always checked", "[memory][property]") {
  rc::check("out of bounds access throws", []() {
    test_memory tm(1, 1, 0); // 1 page, can't grow
    auto& mem = tm.memory();

    auto offset = *rc::gen::inRange(65536u, 100000u); // Beyond memory
    RC_ASSERT_THROWS_AS(mem.read_u64(mem_offset{offset}), memory_bounds_error);
  });
}

TEST_CASE("wasm_memory: property - fuzz operations", "[memory][property][fuzz]") {
  rc::check("random operations don't corrupt data", []() {
    // Generate random bytes for fuzzing
    auto input = *rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>());
    if (input.size() < 4) {
      // Need at least 4 bytes for configuration
      return;
    }

    RC_ASSERT(fuzz_memory_ops(input.data(), input.size()));
  });
}

TEST_CASE("wasm_memory: extended fuzz", "[memory][fuzz][stress]") {
  // Run more iterations with larger inputs
  for (int idx = 0; idx < 100; ++idx) {
    rc::check("extended fuzz testing", []() {
      auto input = *rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>());
      if (input.size() < 4)
        return;

      RC_ASSERT(fuzz_memory_ops(input.data(), input.size()));
    });
  }
}

#endif // !FUZZING_BUILD_MODE_LIBFUZZER
