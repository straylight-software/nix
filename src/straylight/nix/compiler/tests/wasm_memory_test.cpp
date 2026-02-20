/// wasm_memory_test.cpp - Adversarial tests for the WASM memory subsystem
///
/// These tests verify the critical invariants of the memory system:
/// 1. Handles (offsets) remain valid across memory growth
/// 2. Allocations are properly aligned
/// 3. Bounds checking is correct
/// 4. Memory growth works correctly under pressure
/// 5. Nested operations that trigger growth don't corrupt state

#include <algorithm>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "straylight/nix/compiler/runtime/wasm_memory.h"

using namespace straylight::nix::compiler::runtime;

// =============================================================================
// Basic allocation tests
// =============================================================================

TEST_CASE("wasm_memory: basic allocation", "[memory]") {
  test_memory tm(1, 256, 0x1000); // 1 page, heap at 4KB
  auto& mem = tm.memory();

  SECTION("allocates at heap base") {
    auto off = mem.allocate(16);
    REQUIRE(off.raw() == 0x1000);
  }

  SECTION("successive allocations are contiguous and aligned") {
    auto off1 = mem.allocate(1); // rounds up to 8
    auto off2 = mem.allocate(1); // rounds up to 8
    auto off3 = mem.allocate(16);

    REQUIRE(off1.raw() == 0x1000);
    REQUIRE(off2.raw() == 0x1008); // aligned to 8
    REQUIRE(off3.raw() == 0x1010);
  }

  SECTION("tracks heap usage") {
    REQUIRE(mem.heap_used() == 0);
    mem.allocate(8);
    REQUIRE(mem.heap_used() == 8);
    mem.allocate(24);
    REQUIRE(mem.heap_used() == 32);
  }

  SECTION("reset clears allocations") {
    mem.allocate(100);
    REQUIRE(mem.heap_used() > 0);
    mem.reset_heap();
    REQUIRE(mem.heap_used() == 0);

    // Can allocate again at heap base
    auto off = mem.allocate(8);
    REQUIRE(off.raw() == 0x1000);
  }
}

// =============================================================================
// Memory growth tests
// =============================================================================

TEST_CASE("wasm_memory: automatic growth", "[memory]") {
  test_memory tm(1, 16, 0); // 1 page (64KB), max 16 pages, heap at 0
  auto& mem = tm.memory();

  SECTION("grows when allocation exceeds current size") {
    REQUIRE(tm.pages() == 1);

    // Allocate more than one page
    auto off = mem.allocate(70000); // > 64KB
    REQUIRE(off.raw() == 0);
    REQUIRE(tm.pages() >= 2);
  }

  SECTION("grows incrementally for large allocations") {
    // Allocate 5 pages worth
    auto off = mem.allocate(5 * 65536);
    REQUIRE(off.raw() == 0);
    REQUIRE(tm.pages() >= 5);
  }

  SECTION("throws when max pages exceeded") {
    // Try to allocate more than max (16 pages = 1MB)
    REQUIRE_THROWS_AS(mem.allocate(17 * 65536), memory_exhausted_error);
  }
}

// =============================================================================
// Handle validity across growth - THE CRITICAL TEST
// =============================================================================

TEST_CASE("wasm_memory: handles remain valid after growth", "[memory][critical]") {
  test_memory tm(1, 256, 0); // Start with 1 page
  auto& mem = tm.memory();

  SECTION("data survives growth") {
    // Allocate and write some data
    auto off1 = mem.allocate(8);
    mem.write_u64(off1, 0xDEADBEEFCAFEBABE);

    // Force growth
    auto off2 = mem.allocate(100000); // Forces growth

    // Original data must still be readable via the handle
    REQUIRE(mem.read_u64(off1) == 0xDEADBEEFCAFEBABE);

    // New allocation should work too
    mem.write_u64(off2, 0x1234567890ABCDEF);
    REQUIRE(mem.read_u64(off2) == 0x1234567890ABCDEF);
  }

  SECTION("multiple handles survive growth") {
    std::vector<mem_offset> handles;
    std::vector<std::uint64_t> values;

    // Allocate many small chunks
    for (int idx = 0; idx < 100; ++idx) {
      auto off = mem.allocate(8);
      auto val = static_cast<std::uint64_t>(idx * 12345678901ULL);
      mem.write_u64(off, val);
      handles.push_back(off);
      values.push_back(val);
    }

    // Force growth
    mem.allocate(200000);

    // All handles must still be valid
    for (size_t idx = 0; idx < handles.size(); ++idx) {
      REQUIRE(mem.read_u64(handles[idx]) == values[idx]);
    }
  }
}

// =============================================================================
// Simulated nested operation with growth - THE ADVERSARIAL TEST
// =============================================================================

TEST_CASE("wasm_memory: nested operations with growth", "[memory][adversarial]") {
  test_memory tm(1, 256, 0x1000);
  auto& mem = tm.memory();

  // Simulate the problematic pattern:
  // 1. Outer function allocates and writes data
  // 2. Calls inner function that allocates (potentially growing memory)
  // 3. Outer function continues using its offset (not cached pointer!)

  SECTION("outer offset valid after inner allocation triggers growth") {
    // Outer: allocate
    auto outer_off = mem.allocate(64);
    mem.write_u64(outer_off, 0xAAAAAAAAAAAAAAAA);

    // Inner: allocate huge amount (triggers growth)
    auto inner_off = mem.allocate(200000);
    mem.write_u64(inner_off, 0xBBBBBBBBBBBBBBBB);

    // Outer: continue using its offset
    // THIS IS THE BUG WE'RE TESTING: if outer had cached a raw pointer,
    // it would be invalid now. But using the offset works.
    REQUIRE(mem.read_u64(outer_off) == 0xAAAAAAAAAAAAAAAA);

    // Verify inner data too
    REQUIRE(mem.read_u64(inner_off) == 0xBBBBBBBBBBBBBBBB);
  }

  SECTION("deeply nested allocations all remain valid") {
    std::vector<std::pair<mem_offset, std::uint64_t>> stack;

    // Simulate 10 levels of nesting, each allocating and triggering growth
    for (int depth = 0; depth < 10; ++depth) {
      auto off = mem.allocate(8);
      auto val = static_cast<std::uint64_t>(depth * 0x1111111111111111ULL);
      mem.write_u64(off, val);
      stack.emplace_back(off, val);

      // Trigger growth at each level
      mem.allocate(70000);
    }

    // Unwind: all offsets must still be valid
    while (!stack.empty()) {
      auto [off, expected] = stack.back();
      stack.pop_back();
      REQUIRE(mem.read_u64(off) == expected);
    }
  }
}

// =============================================================================
// Bounds checking
// =============================================================================

TEST_CASE("wasm_memory: bounds checking", "[memory]") {
  test_memory tm(1, 1, 0); // 1 page, cannot grow
  auto& mem = tm.memory();

  SECTION("read past end throws") {
    REQUIRE_THROWS_AS(mem.read_u64(mem_offset{65530}), memory_bounds_error);
  }

  SECTION("write past end throws") {
    REQUIRE_THROWS_AS(mem.write_u64(mem_offset{65530}, 0), memory_bounds_error);
  }

  SECTION("allocation past max throws") {
    REQUIRE_THROWS_AS(mem.allocate(70000), memory_exhausted_error);
  }

  SECTION("exact boundary works") {
    // Write at the last valid position
    auto off = mem_offset{65536 - 8};
    mem.write_u64(off, 0x12345678);
    REQUIRE(mem.read_u64(off) == 0x12345678);
  }
}

// =============================================================================
// String operations
// =============================================================================

TEST_CASE("wasm_memory: string operations", "[memory]") {
  test_memory tm(1, 256, 0x1000);
  auto& mem = tm.memory();

  SECTION("write and read string") {
    auto off = mem.allocate(32);
    mem.write_string(off, "hello world");
    REQUIRE(mem.read_string(off) == "hello world");
  }

  SECTION("empty string") {
    auto off = mem.allocate(8);
    mem.write_string(off, "");
    REQUIRE(mem.read_string(off) == "");
  }

  SECTION("string with embedded data before") {
    auto off1 = mem.allocate(16);
    auto off2 = mem.allocate(16);

    mem.write_string(off1, "first");
    mem.write_string(off2, "second");

    REQUIRE(mem.read_string(off1) == "first");
    REQUIRE(mem.read_string(off2) == "second");
  }
}

// =============================================================================
// Copy operations
// =============================================================================

TEST_CASE("wasm_memory: copy operations", "[memory]") {
  test_memory tm(1, 256, 0x1000);
  auto& mem = tm.memory();

  SECTION("basic copy") {
    auto src = mem.allocate(16);
    auto dst = mem.allocate(16);

    mem.write_u64(src, 0xAAAAAAAAAAAAAAAA);
    mem.write_u64(src + 8, 0xBBBBBBBBBBBBBBBB);

    mem.copy(dst, src, 16);

    REQUIRE(mem.read_u64(dst) == 0xAAAAAAAAAAAAAAAA);
    REQUIRE(mem.read_u64(dst + 8) == 0xBBBBBBBBBBBBBBBB);
  }

  SECTION("overlapping copy forward") {
    auto off = mem.allocate(24);
    mem.write_u64(off, 0x1111111111111111);
    mem.write_u64(off + 8, 0x2222222222222222);
    mem.write_u64(off + 16, 0x3333333333333333);

    // Copy overlapping forward
    mem.copy(off + 8, off, 16);

    REQUIRE(mem.read_u64(off) == 0x1111111111111111);      // unchanged
    REQUIRE(mem.read_u64(off + 8) == 0x1111111111111111);  // copied from off
    REQUIRE(mem.read_u64(off + 16) == 0x2222222222222222); // copied from off+8
  }
}

// =============================================================================
// Alignment tests
// =============================================================================

TEST_CASE("wasm_memory: allocation alignment", "[memory]") {
  test_memory tm(1, 256, 0x1000);
  auto& mem = tm.memory();

  SECTION("all allocations are 8-byte aligned") {
    for (int size = 1; size <= 100; ++size) {
      auto off = mem.allocate(static_cast<std::uint32_t>(size));
      REQUIRE((off.raw() % 8) == 0);
    }
  }

  SECTION("odd sizes round up correctly") {
    mem.reset_heap();
    auto off1 = mem.allocate(1);
    auto off2 = mem.allocate(1);
    REQUIRE(off2.raw() - off1.raw() == 8); // 1 byte rounds to 8
  }
}

// =============================================================================
// Stress test with random operations
// =============================================================================

TEST_CASE("wasm_memory: stress test", "[memory][stress]") {
  test_memory tm(1, 256, 0x1000);
  auto& mem = tm.memory();

  std::mt19937 rng(42); // Fixed seed for reproducibility
  std::uniform_int_distribution<std::uint32_t> size_dist(1, 1000);
  std::uniform_int_distribution<std::uint64_t> value_dist;

  std::vector<std::pair<mem_offset, std::uint64_t>> allocated;

  SECTION("random allocations and verifications") {
    // Do 1000 allocations
    for (int idx = 0; idx < 1000; ++idx) {
      auto size = size_dist(rng);
      auto off = mem.allocate(size);
      auto val = value_dist(rng);

      // Write a value at the start of the allocation
      mem.write_u64(off, val);
      allocated.emplace_back(off, val);
    }

    // Verify all allocations still readable
    for (const auto& [off, val] : allocated) {
      REQUIRE(mem.read_u64(off) == val);
    }
  }

  SECTION("interleaved allocations and reads") {
    for (int idx = 0; idx < 500; ++idx) {
      auto size = size_dist(rng);
      auto off = mem.allocate(size);
      auto val = value_dist(rng);
      mem.write_u64(off, val);
      allocated.emplace_back(off, val);

      // Randomly verify an existing allocation
      if (!allocated.empty()) {
        auto idx = rng() % allocated.size();
        REQUIRE(mem.read_u64(allocated[idx].first) == allocated[idx].second);
      }
    }
  }
}

// =============================================================================
// Regression test for the original bug
// =============================================================================

TEST_CASE("wasm_memory: regression - simulated genericClosure pattern", "[memory][regression]") {
  // This simulates the exact pattern that caused the original bug:
  // 1. Runtime function reads data from memory
  // 2. Runtime function allocates (thunks, lists, etc.)
  // 3. The allocation triggers memory growth
  // 4. Original read data must still be valid

  test_memory tm(2, 256, 0x20000); // 2 pages (128KB), heap at 128KB (same as real layout)
  auto& mem = tm.memory();

  // Simulate: data section has an attrset at offset 0x100
  auto attrset_off = mem_offset{0x100};
  mem.write_u32(attrset_off, 2);                 // count = 2
  mem.write_u32(attrset_off + 4, 0x200);         // key1 offset
  mem.write_u64(attrset_off + 8, 0x1234567890);  // value1
  mem.write_u32(attrset_off + 16, 0x210);        // key2 offset
  mem.write_u64(attrset_off + 20, 0xABCDEF0123); // value2

  // Simulate: genericClosure reads the attrset
  auto count = mem.read_u32(attrset_off);
  REQUIRE(count == 2);

  // Simulate: genericClosure allocates thunks (triggers growth)
  for (int idx = 0; idx < 100; ++idx) {
    mem.allocate(20); // THUNK_SIZE
  }

  // Simulate: genericClosure allocates result list (may trigger more growth)
  mem.allocate(100000);

  // THE CRITICAL CHECK: can we still read the original attrset data?
  // If we were caching raw pointers, this would fail after growth.
  REQUIRE(mem.read_u32(attrset_off) == 2);
  REQUIRE(mem.read_u32(attrset_off + 4) == 0x200);
  REQUIRE(mem.read_u64(attrset_off + 8) == 0x1234567890);
  REQUIRE(mem.read_u32(attrset_off + 16) == 0x210);
  REQUIRE(mem.read_u64(attrset_off + 20) == 0xABCDEF0123);
}

// =============================================================================
// Adversarial: pointer-caching pattern that would be unsafe
// =============================================================================

TEST_CASE("wasm_memory: mem_ptr must not be cached", "[memory][adversarial]") {
  test_memory tm(1, 256, 0x1000);
  auto& mem = tm.memory();

  // This test demonstrates the CORRECT pattern vs INCORRECT pattern
  // The INCORRECT pattern would cache mem_ptr across an allocation

  auto off1 = mem.allocate(8);
  mem.write_u64(off1, 0xDEADBEEF);

  // CORRECT pattern: always use offset, get fresh pointer at point of use
  {
    // Allocate (may grow memory)
    mem.allocate(100000);

    // Get fresh pointer now
    auto val = mem.read_u64(off1);
    REQUIRE(val == 0xDEADBEEF);
  }

  // INCORRECT pattern (commented out as demonstration):
  // {
  //   auto ptr = mem.ptr<std::uint64_t>(off1);  // get pointer
  //   mem.allocate(100000);                      // grow memory - ptr now dangling!
  //   auto val = *ptr;                           // USE AFTER FREE!
  // }
}

// =============================================================================
// Adversarial: growth at exact page boundary
// =============================================================================

TEST_CASE("wasm_memory: allocation at exact page boundary", "[memory][adversarial]") {
  test_memory tm(1, 256, 0); // heap at 0
  auto& mem = tm.memory();

  // Fill up to near page boundary
  auto used = mem.allocate(65536 - 16); // leave 16 bytes
  REQUIRE(tm.pages() == 1);

  // Write some data before boundary
  auto before = mem.allocate(8);
  mem.write_u64(before, 0xAAAAAAAAAAAAAAAA);
  REQUIRE(tm.pages() == 1);

  // This allocation should trigger growth
  auto after = mem.allocate(16);
  REQUIRE(tm.pages() >= 2);

  // Data before boundary must survive
  REQUIRE(mem.read_u64(before) == 0xAAAAAAAAAAAAAAAA);

  // New allocation should work
  mem.write_u64(after, 0xBBBBBBBBBBBBBBBB);
  REQUIRE(mem.read_u64(after) == 0xBBBBBBBBBBBBBBBB);
}

// =============================================================================
// Adversarial: maximum stress - grow memory many times
// =============================================================================

TEST_CASE("wasm_memory: repeated growth stress", "[memory][adversarial][stress]") {
  test_memory tm(1, 256, 0);
  auto& mem = tm.memory();

  std::vector<std::pair<mem_offset, std::uint64_t>> allocations;

  // Force 100 memory growths
  for (int idx = 0; idx < 100; ++idx) {
    // Allocate enough to trigger growth
    auto off = mem.allocate(65536);
    auto val = static_cast<std::uint64_t>(idx * 0x0101010101010101ULL);
    mem.write_u64(off, val);
    allocations.emplace_back(off, val);
  }

  // All allocations must be valid
  for (const auto& [off, expected] : allocations) {
    REQUIRE(mem.read_u64(off) == expected);
  }

  REQUIRE(tm.pages() >= 100);
}

// =============================================================================
// Adversarial: zero-size allocation
// =============================================================================

TEST_CASE("wasm_memory: zero size allocation", "[memory][adversarial]") {
  test_memory tm(1, 256, 0x1000);
  auto& mem = tm.memory();

  // Zero-size allocations return the current heap pointer without advancing
  // This is valid - they're just "markers" with no storage
  auto off1 = mem.allocate(0);
  auto off2 = mem.allocate(0);

  // Both point to same location (no storage consumed)
  REQUIRE(off1.raw() == off2.raw());
  REQUIRE(off1.raw() == 0x1000);

  // A real allocation still works
  auto off3 = mem.allocate(8);
  REQUIRE(off3.raw() == 0x1000);

  // And now zero-size points further
  auto off4 = mem.allocate(0);
  REQUIRE(off4.raw() == 0x1008);
}

// =============================================================================
// Adversarial: read/write at allocation boundaries
// =============================================================================

TEST_CASE("wasm_memory: read/write at allocation edge", "[memory][adversarial]") {
  test_memory tm(1, 256, 0x1000);
  auto& mem = tm.memory();

  // Allocate exactly 8 bytes
  auto off = mem.allocate(8);

  // Write fills the entire allocation
  mem.write_u64(off, 0x123456789ABCDEF0);
  REQUIRE(mem.read_u64(off) == 0x123456789ABCDEF0);

  // Allocate again - should not overlap
  auto off2 = mem.allocate(8);
  mem.write_u64(off2, 0xFEDCBA9876543210);

  // First allocation should be unchanged
  REQUIRE(mem.read_u64(off) == 0x123456789ABCDEF0);
  REQUIRE(mem.read_u64(off2) == 0xFEDCBA9876543210);
}
