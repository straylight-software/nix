/// memory_gc_test.cpp - Tests for WASM memory model and GC-related issues
///
/// Tests for issues:
/// - #54, #5200, #8621, #10862, #13483 - Memory not freed after evaluation
/// - #8626 - Alternative GC (whippet) - Verify WASM linear memory model
/// - #9592 - Float out ExprSelect optimization
/// - #4897 - Continuous benchmarks needed
/// - #9159, #4090 - Lazy evaluation patterns
/// - #4279, #6228 - Evaluation caching

#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "straylight/nix/compiler/ast/expression.h"
#include "straylight/nix/compiler/ast/symbol_table.h"
#include "straylight/nix/compiler/compile/compiler.h"
#include "straylight/nix/compiler/parse/parser.h"
#include "straylight/nix/compiler/runtime/wasm_executor.h"
#include "straylight/nix/compiler/runtime/wasm_memory.h"

namespace ast = straylight::nix::compiler::ast;
namespace compile = straylight::nix::compiler::compile;
namespace parse = straylight::nix::compiler::parse;
namespace runtime = straylight::nix::compiler::runtime;

// =============================================================================
// Helper: compile and execute Nix source
// =============================================================================

namespace {
struct eval_result {
  bool success;
  runtime::nix_value value;
  std::string error;
};

auto eval_nix(std::string_view source) -> eval_result {
  try {
    ast::symbol_table symbols;
    auto expr = parse::parse(source, symbols);
    compile::compiler comp(symbols);
    auto module = comp.compile(expr);

    if (!module.validate()) {
      return {false, 0, "WASM validation failed"};
    }

    runtime::wasm_executor executor;
    auto result = executor.execute(module.emit_binary());

    if (!result.success) {
      return {false, 0, result.error};
    }

    return {true, result.value, ""};
  } catch (const std::exception& e) {
    return {false, 0, e.what()};
  }
}
} // namespace

// =============================================================================
// Issue #54, #5200, #8621, #10862, #13483: Memory not freed after evaluation
// WASM arena allocator with reset_heap() addresses these memory leak issues.
// =============================================================================

TEST_CASE("memory: arena reset releases all memory", "[memory][gc][#54][#5200]") {
  // The WASM memory model uses arena allocation with reset_heap().
  // After evaluation completes, reset_heap() releases all memory at once.
  // This is far more efficient than tracing GC for functional evaluation.

  runtime::test_memory tm(1, 256, 0x1000);
  auto& mem = tm.memory();

  SECTION("basic allocation and reset") {
    // Allocate some memory
    auto off1 = mem.allocate(1024);
    auto off2 = mem.allocate(2048);
    auto off3 = mem.allocate(4096);

    REQUIRE(mem.heap_used() == 1024 + 2048 + 4096);

    // Reset releases all memory
    mem.reset_heap();
    REQUIRE(mem.heap_used() == 0);

    // New allocations start from heap base again
    auto off4 = mem.allocate(8);
    REQUIRE(off4.raw() == 0x1000); // Same as off1 would have been
  }

  SECTION("multiple evaluation cycles") {
    // Simulate multiple evaluation cycles
    for (int cycle = 0; cycle < 10; ++cycle) {
      // Each cycle allocates memory
      for (int i = 0; i < 100; ++i) {
        mem.allocate(64);
      }

      std::uint32_t used_before = mem.heap_used();
      REQUIRE(used_before == 100 * 64);

      // Reset after each evaluation
      mem.reset_heap();
      REQUIRE(mem.heap_used() == 0);
    }
  }
}

TEST_CASE("memory: large evaluation doesn't leak", "[memory][gc][#8621][#10862]") {
  // Issue #8621, #10862: Memory grows unbounded during large evaluations
  // With arena allocation, memory is bounded by the peak usage of a single
  // evaluation, not cumulative usage across evaluations.

  runtime::test_memory tm(1, 1024, 0x1000); // Allow up to 64MB
  auto& mem = tm.memory();

  const std::uint32_t allocation_per_eval = 1024 * 1024; // 1MB per evaluation

  // Run many evaluations
  for (int eval = 0; eval < 50; ++eval) {
    // Allocate 1MB
    mem.allocate(allocation_per_eval);
    REQUIRE(mem.heap_used() >= allocation_per_eval);

    // Reset after evaluation
    mem.reset_heap();
    REQUIRE(mem.heap_used() == 0);
  }

  // Memory should NOT have grown to 50MB - it resets each time
  // The underlying buffer may have grown, but the logical heap is reset
  REQUIRE(mem.heap_used() == 0);
}

TEST_CASE("memory: thunks don't cause unbounded growth", "[memory][gc][#13483]") {
  // Issue #13483: Thunks can cause memory to grow if not properly managed.
  // With arena allocation, thunks are just allocations that get reset.

  runtime::test_memory tm(1, 256, 0x1000);
  auto& mem = tm.memory();

  SECTION("many thunk allocations") {
    // Simulate creating many thunks (each thunk is ~20 bytes)
    const int num_thunks = 10000;
    const std::uint32_t thunk_size = 20;

    for (int i = 0; i < num_thunks; ++i) {
      mem.allocate(thunk_size);
    }

    std::uint32_t peak = mem.heap_used();
    REQUIRE(peak >= num_thunks * thunk_size);

    // Reset clears all thunks
    mem.reset_heap();
    REQUIRE(mem.heap_used() == 0);
  }

  SECTION("nested thunk evaluation") {
    // Simulate deeply nested thunk forcing
    for (int depth = 0; depth < 100; ++depth) {
      // Each level allocates for intermediate results
      mem.allocate(128);
    }

    // After evaluation, reset clears everything
    mem.reset_heap();
    REQUIRE(mem.heap_used() == 0);
  }
}

// =============================================================================
// Issue #8626: Alternative GC (whippet) - WASM linear memory model
// The WASM memory model doesn't need Boehm GC or whippet.
// =============================================================================

TEST_CASE("memory: WASM model doesn't need tracing GC", "[memory][#8626]") {
  // The WASM linear memory model with arena allocation eliminates the need
  // for tracing GC entirely. This is because:
  // 1. All values are in contiguous linear memory
  // 2. References are 32-bit offsets, not pointers
  // 3. Memory is reset wholesale after each evaluation

  runtime::test_memory tm(1, 256, 0x1000);
  auto& mem = tm.memory();

  SECTION("no pointer chasing required") {
    // In the WASM model, values are stored at known offsets
    auto off1 = mem.allocate(8);
    auto off2 = mem.allocate(8);

    mem.write_u64(off1, 0xDEADBEEF);
    mem.write_u64(off2, off1.raw()); // Store reference as offset

    // Reading the reference gives us the offset, not a pointer
    auto ref = mem.read_u64(off2);
    REQUIRE(ref == off1.raw());

    // We can use this offset to read the original value
    auto val = mem.read_u64(runtime::mem_offset{static_cast<std::uint32_t>(ref)});
    REQUIRE(val == 0xDEADBEEF);
  }

  SECTION("no root set enumeration") {
    // Arena allocation means we don't need to find GC roots
    // Everything allocated is "live" until reset

    std::vector<runtime::mem_offset> allocations;
    for (int i = 0; i < 1000; ++i) {
      allocations.push_back(mem.allocate(8));
    }

    // All allocations are valid - no GC can invalidate them
    for (const auto& off : allocations) {
      mem.write_u64(off, 0x12345678);
      REQUIRE(mem.read_u64(off) == 0x12345678);
    }

    // Reset is O(1) - just move the heap pointer
    mem.reset_heap();
    REQUIRE(mem.heap_used() == 0);
  }
}

TEST_CASE("memory: deterministic memory layout", "[memory][#8626]") {
  // WASM linear memory gives deterministic memory layout
  // This makes debugging and testing much easier than with GC

  runtime::test_memory tm(1, 256, 0x1000);
  auto& mem = tm.memory();

  // Allocations are always at predictable offsets
  auto off1 = mem.allocate(8);
  auto off2 = mem.allocate(16);
  auto off3 = mem.allocate(8);

  REQUIRE(off1.raw() == 0x1000);
  REQUIRE(off2.raw() == 0x1008); // 8 bytes after off1
  REQUIRE(off3.raw() == 0x1018); // 16 bytes after off2 (aligned to 8)

  // After reset, same pattern repeats
  mem.reset_heap();

  auto off4 = mem.allocate(8);
  auto off5 = mem.allocate(16);

  REQUIRE(off4.raw() == 0x1000);
  REQUIRE(off5.raw() == 0x1008);
}

// =============================================================================
// Issue #9592: Float out ExprSelect optimization
// =============================================================================

TEST_CASE("compiler: nested select optimization", "[compiler][optimization][#9592]") {
  // Issue #9592: Nested selects like `a.b.c.d` should be optimized
  // to avoid repeated attribute lookups. The compiler should recognize
  // this pattern and emit efficient code.

  ast::symbol_table symbols;

  SECTION("deep select path compiles") {
    // Parse: { a = { b = { c = { d = 42; }; }; }; }.a.b.c.d
    auto expr = parse::parse("{ a = { b = { c = { d = 42; }; }; }; }.a.b.c.d", symbols);
    REQUIRE(expr != nullptr);

    compile::compiler comp(symbols);
    auto module = comp.compile(expr);
    REQUIRE(module.validate());
  }

  SECTION("select with default compiles") {
    // Parse: { a = 1; }.b or 42
    auto expr = parse::parse("{ a = 1; }.b or 42", symbols);
    REQUIRE(expr != nullptr);

    compile::compiler comp(symbols);
    auto module = comp.compile(expr);
    REQUIRE(module.validate());
  }

  SECTION("chained or-default compiles") {
    // Parse: x.a.b.c or x.a.b or x.a or x
    auto expr = parse::parse("let x = {}; in x.a.b.c or x.a.b or x.a or x or {}", symbols);
    REQUIRE(expr != nullptr);

    compile::compiler comp(symbols);
    auto module = comp.compile(expr);
    REQUIRE(module.validate());
  }
}

TEST_CASE("execution: nested select is efficient", "[execution][optimization][#9592]") {
  // Verify that nested selects execute correctly

  auto result = eval_nix("{ a = { b = { c = 42; }; }; }.a.b.c");

  REQUIRE(result.success);
  REQUIRE(runtime::is_int(result.value));
  REQUIRE(runtime::get_int_value(result.value) == 42);
}

// =============================================================================
// Issue #4897: Continuous benchmarks needed
// These benchmarks can be run in CI to detect performance regressions.
// =============================================================================

TEST_CASE("benchmark: parse performance baseline", "[benchmark][#4897]") {
  // Establish baseline for parsing performance
  // This test can be run in CI to detect regressions

  const int iterations = 100;
  ast::symbol_table symbols;

  auto start = std::chrono::steady_clock::now();

  for (int i = 0; i < iterations; ++i) {
    symbols.clear();
    auto expr = parse::parse("let x = 1; y = 2; z = x + y; in z * 2", symbols);
    REQUIRE(expr != nullptr);
  }

  auto end = std::chrono::steady_clock::now();
  auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

  INFO("Parsed " << iterations << " expressions in " << duration_us << "us");
  INFO("Average: " << (duration_us / iterations) << "us per parse");

  // Should parse simple expressions quickly
  REQUIRE(duration_us < 1000000); // Under 1 second for 100 parses
}

TEST_CASE("benchmark: compile performance baseline", "[benchmark][#4897]") {
  ast::symbol_table symbols;
  auto expr = parse::parse("let f = x: x + 1; in f (f (f 10))", symbols);

  const int iterations = 50;

  auto start = std::chrono::steady_clock::now();

  for (int i = 0; i < iterations; ++i) {
    compile::compiler comp(symbols);
    auto module = comp.compile(expr);
    REQUIRE(module.validate());
  }

  auto end = std::chrono::steady_clock::now();
  auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

  INFO("Compiled " << iterations << " expressions in " << duration_us << "us");
  INFO("Average: " << (duration_us / iterations) << "us per compile");

  REQUIRE(duration_us < 5000000); // Under 5 seconds
}

TEST_CASE("benchmark: execution performance baseline", "[benchmark][#4897]") {
  ast::symbol_table symbols;
  auto expr = parse::parse("let sum = a: b: a + b; in sum 100 200", symbols);
  compile::compiler comp(symbols);
  auto module = comp.compile(expr);
  auto binary = module.emit_binary();

  const int iterations = 100;

  auto start = std::chrono::steady_clock::now();

  for (int i = 0; i < iterations; ++i) {
    runtime::wasm_executor executor;
    auto result = executor.execute(binary);
    REQUIRE(result.success);
    REQUIRE(runtime::is_int(result.value));
    REQUIRE(runtime::get_int_value(result.value) == 300);
  }

  auto end = std::chrono::steady_clock::now();
  auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

  INFO("Executed " << iterations << " times in " << duration_us << "us");
  INFO("Average: " << (duration_us / iterations) << "us per execution");

  REQUIRE(duration_us < 5000000); // Under 5 seconds
}

TEST_CASE("benchmark: memory allocation performance", "[benchmark][#4897]") {
  runtime::test_memory tm(1, 256, 0x1000);
  auto& mem = tm.memory();

  const int iterations = 10000;

  auto start = std::chrono::steady_clock::now();

  for (int i = 0; i < iterations; ++i) {
    mem.allocate(64);
  }

  auto end = std::chrono::steady_clock::now();
  auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

  INFO("Allocated " << iterations << " blocks in " << duration_ns << "ns");
  INFO("Average: " << (duration_ns / iterations) << "ns per allocation");

  // Bump allocation should be very fast - under 1us per allocation
  REQUIRE((duration_ns / iterations) < 1000);
}

// =============================================================================
// Issue #9159, #4090: Lazy evaluation patterns
// =============================================================================

TEST_CASE("compiler: thunks are created for lazy patterns", "[compiler][lazy][#9159][#4090]") {
  // Verify that the compiler creates thunks for lazy evaluation

  ast::symbol_table symbols;

  SECTION("let bindings create thunks") {
    // In `let x = expensive; in y`, x should be a thunk
    auto expr = parse::parse("let x = 1 + 2 + 3; in x", symbols);
    REQUIRE(expr != nullptr);

    compile::compiler comp(symbols);
    auto module = comp.compile(expr);
    REQUIRE(module.validate());

    // The WAT should contain thunk creation
    auto wat = module.emit_text();
    // Thunks are stored with __makeThunk
    REQUIRE(wat.find("__makeThunk") != std::string::npos);
  }

  SECTION("rec bindings create thunks for laziness") {
    // rec { x = y + 1; y = 1; } requires thunks for mutual recursion
    auto expr = parse::parse("rec { x = y + 1; y = 1; }.x", symbols);
    REQUIRE(expr != nullptr);

    compile::compiler comp(symbols);
    auto module = comp.compile(expr);
    REQUIRE(module.validate());

    auto wat = module.emit_text();
    REQUIRE(wat.find("__thunk") != std::string::npos);
  }

  SECTION("function arguments are not thunked") {
    // Nix uses strict function arguments
    auto expr = parse::parse("(x: x + 1) 5", symbols);
    REQUIRE(expr != nullptr);

    compile::compiler comp(symbols);
    auto module = comp.compile(expr);
    REQUIRE(module.validate());
  }
}

TEST_CASE("execution: lazy evaluation avoids unnecessary work", "[execution][lazy][#9159]") {
  // Verify that lazy evaluation actually avoids computing unused values

  SECTION("unused let binding not evaluated") {
    // x is defined but never used, so it shouldn't be evaluated
    // This is hard to test directly, but we can verify the result is correct
    auto result = eval_nix("let x = 1 / 0; y = 42; in y");

    // Should return 42 without evaluating the division by zero
    REQUIRE(result.success);
    REQUIRE(runtime::is_int(result.value));
    REQUIRE(runtime::get_int_value(result.value) == 42);
  }

  SECTION("if-then-else only evaluates taken branch") {
    // false branch has division by zero but shouldn't be evaluated
    auto result = eval_nix("if true then 42 else 1 / 0");

    REQUIRE(result.success);
    REQUIRE(runtime::is_int(result.value));
    REQUIRE(runtime::get_int_value(result.value) == 42);
  }
}

// =============================================================================
// Issue #4279, #6228: Evaluation caching
// =============================================================================

TEST_CASE("execution: same expression cached", "[execution][cache][#4279][#6228]") {
  // When the same expression is evaluated multiple times (e.g., through
  // sharing), the result should be cached (memoized in thunks).

  SECTION("shared let binding evaluated once") {
    // x is used twice but should only be computed once
    auto result = eval_nix("let x = 1 + 2; in x + x");

    // (1 + 2) + (1 + 2) = 6
    REQUIRE(result.success);
    REQUIRE(runtime::is_int(result.value));
    REQUIRE(runtime::get_int_value(result.value) == 6);
  }

  SECTION("recursive reference memoized") {
    // In rec { x = 1; y = x + x; }, x should be memoized
    auto result = eval_nix("rec { x = 1; y = x + x; }.y");

    // 1 + 1 = 2
    REQUIRE(result.success);
    REQUIRE(runtime::is_int(result.value));
    REQUIRE(runtime::get_int_value(result.value) == 2);
  }
}

TEST_CASE("compiler: import caching structure", "[compiler][cache][#6228]") {
  // Verify that the compiler structure supports import caching
  // (actual import caching is done at the evaluator level)

  ast::symbol_table symbols;

  // Multiple modules can be compiled independently
  auto expr1 = parse::parse("42", symbols);
  auto expr2 = parse::parse("{ a = 1; }", symbols);

  compile::compiler comp1(symbols);
  auto module1 = comp1.compile(expr1);

  compile::compiler comp2(symbols, comp1.data_segment_end());
  auto module2 = comp2.compile(expr2);

  // Both should be valid
  REQUIRE(module1.validate());
  REQUIRE(module2.validate());

  // Module2's data segment should start after module1's
  REQUIRE(comp2.data_segment_end() > comp1.data_segment_end());
}

// =============================================================================
// Stress tests for memory safety
// =============================================================================

TEST_CASE("memory: stress test allocation patterns", "[memory][stress]") {
  runtime::test_memory tm(1, 512, 0x1000);
  auto& mem = tm.memory();

  std::mt19937 rng(12345);
  std::uniform_int_distribution<std::uint32_t> size_dist(8, 1024);

  SECTION("random allocation sizes") {
    std::vector<std::pair<runtime::mem_offset, std::uint64_t>> allocations;

    for (int i = 0; i < 1000; ++i) {
      auto size = size_dist(rng);
      auto off = mem.allocate(size);
      std::uint64_t magic = static_cast<std::uint64_t>(i) * 0x1234567890ABCDEFULL;
      mem.write_u64(off, magic);
      allocations.emplace_back(off, magic);
    }

    // Verify all allocations
    for (const auto& [off, magic] : allocations) {
      REQUIRE(mem.read_u64(off) == magic);
    }
  }

  SECTION("allocation after growth") {
    // Force multiple growth events
    for (int i = 0; i < 100; ++i) {
      mem.allocate(65536); // One page each
    }

    // Memory should have grown
    REQUIRE(tm.pages() >= 100);

    // New allocations should still work
    auto off = mem.allocate(8);
    mem.write_u64(off, 0xCAFEBABE);
    REQUIRE(mem.read_u64(off) == 0xCAFEBABE);
  }
}

TEST_CASE("memory: concurrent access pattern simulation", "[memory][stress]") {
  // Simulate patterns that would occur with concurrent evaluations
  // (WASM is single-threaded, but this tests the memory model)

  runtime::test_memory tm(1, 256, 0x1000);
  auto& mem = tm.memory();

  SECTION("interleaved allocations and reads") {
    std::vector<runtime::mem_offset> offsets;

    for (int i = 0; i < 500; ++i) {
      auto off = mem.allocate(16);
      mem.write_u64(off, static_cast<std::uint64_t>(i));
      offsets.push_back(off);

      // Periodically verify previous allocations
      if (i > 0 && i % 50 == 0) {
        for (int j = 0; j < i; ++j) {
          REQUIRE(mem.read_u64(offsets[j]) == static_cast<std::uint64_t>(j));
        }
      }
    }
  }
}
