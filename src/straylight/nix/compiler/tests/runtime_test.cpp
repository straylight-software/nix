// straylight // nix-language // tests
//
// Direct tests for runtime functions
//
// Tests the runtime layer in isolation, without going through the full
// compile + execute pipeline. This lets us test edge cases more precisely.

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "straylight/nix/compiler/compile/wasm_types.h"
#include "straylight/nix/compiler/runtime/memory_layout.h"
#include "straylight/nix/compiler/runtime/runtime.h"
#include "straylight/nix/compiler/runtime/wasm_executor.h"

using namespace straylight::nix::compiler::runtime;
using namespace straylight::nix::compiler::compile;
namespace mem = straylight::nix::compiler::memory_layout;

// =============================================================================
// Value creation and inspection
// =============================================================================

TEST_CASE("runtime: value creation", "[runtime][value]") {
  SECTION("null value") {
    auto v = constants::null_value;
    REQUIRE(is_null(v));
    REQUIRE(!is_bool(v));
    REQUIRE(!is_int(v));
    REQUIRE(type_name(v) == "null");
  }

  SECTION("boolean true") {
    auto v = constants::bool_true;
    REQUIRE(is_bool(v));
    REQUIRE(get_bool_value(v) == true);
    REQUIRE(type_name(v) == "bool");
  }

  SECTION("boolean false") {
    auto v = constants::bool_false;
    REQUIRE(is_bool(v));
    REQUIRE(get_bool_value(v) == false);
  }

  SECTION("integer") {
    auto v = make_int(42);
    REQUIRE(is_int(v));
    REQUIRE(get_int_value(v) == 42);
    REQUIRE(type_name(v) == "int");
  }

  SECTION("negative integer") {
    auto v = make_int(-123);
    REQUIRE(is_int(v));
    REQUIRE(get_int_value(v) == -123);
  }

  SECTION("max int32") {
    auto v = make_int(std::numeric_limits<std::int32_t>::max());
    REQUIRE(is_int(v));
    REQUIRE(get_int_value(v) == std::numeric_limits<std::int32_t>::max());
  }

  SECTION("min int32") {
    auto v = make_int(std::numeric_limits<std::int32_t>::min());
    REQUIRE(is_int(v));
    REQUIRE(get_int_value(v) == std::numeric_limits<std::int32_t>::min());
  }

  SECTION("make_bool") {
    REQUIRE(make_bool(true) == constants::bool_true);
    REQUIRE(make_bool(false) == constants::bool_false);
  }
}

// =============================================================================
// Heap allocator
// =============================================================================

TEST_CASE("runtime: heap allocator", "[runtime][heap]") {
  SECTION("basic allocation") {
    heap_allocator heap(mem::HEAP_BASE, mem::DEFAULT_MEMORY_SIZE);
    auto ptr = heap.allocate(100);
    REQUIRE(ptr == mem::HEAP_BASE);
    REQUIRE(ptr % mem::ALIGNMENT == 0);
  }

  SECTION("sequential allocations don't overlap") {
    heap_allocator heap(mem::HEAP_BASE, mem::DEFAULT_MEMORY_SIZE);

    auto p1 = heap.allocate(100);
    auto p2 = heap.allocate(200);
    auto p3 = heap.allocate(50);

    auto s1 = mem::align_up(100);
    auto s2 = mem::align_up(200);

    REQUIRE(p2 >= p1 + s1);
    REQUIRE(p3 >= p2 + s2);
  }

  SECTION("all allocations are aligned") {
    heap_allocator heap(mem::HEAP_BASE, mem::DEFAULT_MEMORY_SIZE);

    for (int idx = 1; idx <= 100; ++idx) {
      auto ptr = heap.allocate(idx);
      REQUIRE(ptr % mem::ALIGNMENT == 0);
    }
  }

  SECTION("bytes_allocated tracks usage") {
    heap_allocator heap(mem::HEAP_BASE, mem::DEFAULT_MEMORY_SIZE);
    REQUIRE(heap.bytes_allocated() == 0);

    heap.allocate(100);
    REQUIRE(heap.bytes_allocated() >= 100);

    heap.allocate(200);
    REQUIRE(heap.bytes_allocated() >= 300);
  }

  SECTION("reset clears allocations") {
    heap_allocator heap(mem::HEAP_BASE, mem::DEFAULT_MEMORY_SIZE);

    heap.allocate(1000);
    heap.allocate(2000);
    REQUIRE(heap.bytes_allocated() >= 3000);

    heap.reset();
    REQUIRE(heap.bytes_allocated() == 0);

    // can allocate again from the beginning
    auto ptr = heap.allocate(100);
    REQUIRE(ptr == mem::HEAP_BASE);
  }

  SECTION("oom throws on exhaustion") {
    heap_allocator heap(mem::HEAP_BASE, mem::HEAP_BASE + 1000); // tiny heap

    REQUIRE_THROWS_AS(heap.allocate(2000), oom_error);
  }

  SECTION("oom throws on cumulative exhaustion") {
    heap_allocator heap(mem::HEAP_BASE, mem::HEAP_BASE + 1000);

    heap.allocate(400);
    heap.allocate(400);
    REQUIRE_THROWS_AS(heap.allocate(400), oom_error);
  }
}

// =============================================================================
// Runtime context
// =============================================================================

TEST_CASE("runtime: context memory operations", "[runtime][context]") {
  runtime_context ctx(mem::DEFAULT_MEMORY_SIZE);

  SECTION("read/write i32") {
    ctx.write_i32(100, 0x12345678);
    REQUIRE(ctx.read_i32(100) == 0x12345678);
  }

  SECTION("read/write i64") {
    ctx.write_i64(100, 0x123456789ABCDEF0LL);
    REQUIRE(ctx.read_i64(100) == 0x123456789ABCDEF0LL);
  }

  SECTION("read/write value") {
    auto v = make_int(42);
    ctx.write_value(100, v);
    REQUIRE(ctx.read_value(100) == v);
  }

  SECTION("read/write negative i32") {
    ctx.write_i32(100, -12345);
    REQUIRE(ctx.read_i32(100) == -12345);
  }

  SECTION("read string") {
    // Write a null-terminated string
    const char* str = "hello";
    for (size_t idx = 0; idx <= strlen(str); ++idx) {
      ctx.memory[100 + idx] = static_cast<std::uint8_t>(str[idx]);
    }
    auto result = ctx.read_string(100);
    REQUIRE(result == "hello");
  }

  SECTION("out of bounds read throws") {
    REQUIRE_THROWS_AS(ctx.read_bytes(mem::DEFAULT_MEMORY_SIZE + 100, 10), runtime_error);
  }

  SECTION("out of bounds write throws") {
    std::uint8_t data[10] = {0};
    REQUIRE_THROWS_AS(ctx.write_bytes(mem::DEFAULT_MEMORY_SIZE + 100, data), runtime_error);
  }
}

// =============================================================================
// Arithmetic operations
// =============================================================================

TEST_CASE("runtime: rt_add", "[runtime][arithmetic]") {
  runtime_context ctx;

  SECTION("add two positive integers") {
    auto a = make_int(10);
    auto b = make_int(20);
    auto result = rt_add(ctx, a, b, 0, 0);
    REQUIRE(is_int(result));
    REQUIRE(get_int_value(result) == 30);
  }

  SECTION("add positive and negative") {
    auto a = make_int(10);
    auto b = make_int(-3);
    auto result = rt_add(ctx, a, b, 0, 0);
    REQUIRE(get_int_value(result) == 7);
  }

  SECTION("add two negatives") {
    auto a = make_int(-10);
    auto b = make_int(-20);
    auto result = rt_add(ctx, a, b, 0, 0);
    REQUIRE(get_int_value(result) == -30);
  }

  SECTION("add zero") {
    auto a = make_int(42);
    auto b = make_int(0);
    auto result = rt_add(ctx, a, b, 0, 0);
    REQUIRE(get_int_value(result) == 42);
  }

  SECTION("type error on non-numeric") {
    auto a = make_int(1);
    auto b = constants::bool_true;
    REQUIRE_THROWS_AS(rt_add(ctx, a, b, 1, 1), type_error);
  }
}

TEST_CASE("runtime: rt_sub", "[runtime][arithmetic]") {
  runtime_context ctx;

  SECTION("subtract") {
    auto a = make_int(30);
    auto b = make_int(10);
    auto result = rt_sub(ctx, a, b, 0, 0);
    REQUIRE(get_int_value(result) == 20);
  }

  SECTION("subtract to negative") {
    auto a = make_int(10);
    auto b = make_int(30);
    auto result = rt_sub(ctx, a, b, 0, 0);
    REQUIRE(get_int_value(result) == -20);
  }
}

TEST_CASE("runtime: rt_mul", "[runtime][arithmetic]") {
  runtime_context ctx;

  SECTION("multiply") {
    auto a = make_int(6);
    auto b = make_int(7);
    auto result = rt_mul(ctx, a, b, 0, 0);
    REQUIRE(get_int_value(result) == 42);
  }

  SECTION("multiply by zero") {
    auto a = make_int(12345);
    auto b = make_int(0);
    auto result = rt_mul(ctx, a, b, 0, 0);
    REQUIRE(get_int_value(result) == 0);
  }

  SECTION("multiply negatives") {
    auto a = make_int(-3);
    auto b = make_int(-4);
    auto result = rt_mul(ctx, a, b, 0, 0);
    REQUIRE(get_int_value(result) == 12);
  }
}

TEST_CASE("runtime: rt_div", "[runtime][arithmetic]") {
  runtime_context ctx;

  SECTION("divide evenly") {
    auto a = make_int(42);
    auto b = make_int(6);
    auto result = rt_div(ctx, a, b, 0, 0);
    REQUIRE(get_int_value(result) == 7);
  }

  SECTION("divide with truncation") {
    auto a = make_int(7);
    auto b = make_int(2);
    auto result = rt_div(ctx, a, b, 0, 0);
    REQUIRE(get_int_value(result) == 3); // floor division
  }

  SECTION("divide by zero throws") {
    auto a = make_int(42);
    auto b = make_int(0);
    REQUIRE_THROWS_AS(rt_div(ctx, a, b, 1, 1), runtime_error);
  }

  SECTION("negative division - floor semantics") {
    // Nix uses floor division (rounds toward negative infinity)
    auto a = make_int(-7);
    auto b = make_int(2);
    auto result = rt_div(ctx, a, b, 0, 0);
    REQUIRE(get_int_value(result) == -4); // floor(-3.5) = -4
  }
}

TEST_CASE("runtime: rt_negate", "[runtime][arithmetic]") {
  runtime_context ctx;

  SECTION("negate positive") {
    auto v = make_int(42);
    auto result = rt_negate(ctx, v);
    REQUIRE(get_int_value(result) == -42);
  }

  SECTION("negate negative") {
    auto v = make_int(-42);
    auto result = rt_negate(ctx, v);
    REQUIRE(get_int_value(result) == 42);
  }

  SECTION("negate zero") {
    auto v = make_int(0);
    auto result = rt_negate(ctx, v);
    REQUIRE(get_int_value(result) == 0);
  }

  SECTION("negate non-numeric throws") {
    auto v = constants::bool_true;
    REQUIRE_THROWS_AS(rt_negate(ctx, v), type_error);
  }
}

// =============================================================================
// Comparison operations
// =============================================================================

TEST_CASE("runtime: rt_less_than", "[runtime][comparison]") {
  runtime_context ctx;

  SECTION("less than true") {
    auto a = make_int(1);
    auto b = make_int(2);
    auto result = rt_less_than(ctx, a, b);
    REQUIRE(result == constants::bool_true);
  }

  SECTION("less than false") {
    auto a = make_int(2);
    auto b = make_int(1);
    auto result = rt_less_than(ctx, a, b);
    REQUIRE(result == constants::bool_false);
  }

  SECTION("equal is not less") {
    auto a = make_int(5);
    auto b = make_int(5);
    auto result = rt_less_than(ctx, a, b);
    REQUIRE(result == constants::bool_false);
  }
}

TEST_CASE("runtime: rt_eq", "[runtime][comparison]") {
  runtime_context ctx;

  SECTION("integers equal") {
    auto a = make_int(42);
    auto b = make_int(42);
    REQUIRE(rt_eq(ctx, a, b) == constants::bool_true);
  }

  SECTION("integers not equal") {
    auto a = make_int(1);
    auto b = make_int(2);
    REQUIRE(rt_eq(ctx, a, b) == constants::bool_false);
  }

  SECTION("booleans equal") {
    REQUIRE(rt_eq(ctx, constants::bool_true, constants::bool_true) == constants::bool_true);
    REQUIRE(rt_eq(ctx, constants::bool_false, constants::bool_false) == constants::bool_true);
  }

  SECTION("booleans not equal") {
    REQUIRE(rt_eq(ctx, constants::bool_true, constants::bool_false) == constants::bool_false);
  }

  SECTION("null equals null") {
    REQUIRE(rt_eq(ctx, constants::null_value, constants::null_value) == constants::bool_true);
  }

  SECTION("different types not equal") {
    auto i = make_int(1);
    auto b = constants::bool_true;
    REQUIRE(rt_eq(ctx, i, b) == constants::bool_false);
  }
}

// =============================================================================
// Boolean operations
// =============================================================================

TEST_CASE("runtime: rt_not", "[runtime][boolean]") {
  runtime_context ctx;

  SECTION("not true") {
    auto result = rt_not(ctx, constants::bool_true);
    REQUIRE(result == constants::bool_false);
  }

  SECTION("not false") {
    auto result = rt_not(ctx, constants::bool_false);
    REQUIRE(result == constants::bool_true);
  }

  SECTION("not non-boolean throws") {
    REQUIRE_THROWS_AS(rt_not(ctx, make_int(1)), type_error);
  }
}

// =============================================================================
// Collection operations
// =============================================================================

TEST_CASE("runtime: rt_make_list", "[runtime][collection]") {
  runtime_context ctx;

  SECTION("empty list") {
    auto result = rt_make_list(ctx, 0, 0);
    REQUIRE(is_list(result));
  }

  SECTION("list with elements") {
    // Write elements to memory first
    std::uint32_t offset = mem::HEAP_BASE;
    ctx.write_value(offset, make_int(1));
    ctx.write_value(offset + 8, make_int(2));
    ctx.write_value(offset + 16, make_int(3));

    auto result = rt_make_list(ctx, offset, 3);
    REQUIRE(is_list(result));
  }
}

TEST_CASE("runtime: rt_concat lists", "[runtime][collection]") {
  runtime_context ctx;

  // Create two lists in memory
  auto list1_ptr = ctx.allocate(mem::list_size(2));
  ctx.write_i32(list1_ptr + mem::LIST_COUNT_OFFSET, 2);
  ctx.write_value(list1_ptr + mem::LIST_ELEMENTS_OFFSET, make_int(1));
  ctx.write_value(list1_ptr + mem::LIST_ELEMENTS_OFFSET + 8, make_int(2));

  auto list2_ptr = ctx.allocate(mem::list_size(2));
  ctx.write_i32(list2_ptr + mem::LIST_COUNT_OFFSET, 2);
  ctx.write_value(list2_ptr + mem::LIST_ELEMENTS_OFFSET, make_int(3));
  ctx.write_value(list2_ptr + mem::LIST_ELEMENTS_OFFSET + 8, make_int(4));

  auto list1 = make_value(value_tag::list, list1_ptr);
  auto list2 = make_value(value_tag::list, list2_ptr);

  auto result = rt_concat(ctx, list1, list2);
  REQUIRE(is_list(result));

  // Check the result has 4 elements
  auto result_ptr = get_payload(result);
  auto count = ctx.read_u32(result_ptr + mem::LIST_COUNT_OFFSET);
  REQUIRE(count == 4);
}

// =============================================================================
// Error handling
// =============================================================================

TEST_CASE("runtime: error types", "[runtime][error]") {
  SECTION("runtime_error with position") {
    runtime_error err("test error", 10, 20);
    REQUIRE(err.line == 10);
    REQUIRE(err.column == 20);
    std::string msg = err.what();
    REQUIRE(msg.find("test error") != std::string::npos);
    REQUIRE(msg.find("line 10") != std::string::npos);
    REQUIRE(msg.find("column 20") != std::string::npos);
  }

  SECTION("runtime_error without position") {
    runtime_error err("test error");
    REQUIRE(err.line == 0);
    REQUIRE(err.column == 0);
  }

  SECTION("type_error is runtime_error") {
    type_error err("type error message", 5, 10);
    const runtime_error& base = err;
    REQUIRE(base.line == 5);
  }

  SECTION("attr_error is runtime_error") {
    attr_error err("attr error message", 1, 2);
    const runtime_error& base = err;
    REQUIRE(base.line == 1);
  }

  SECTION("oom_error is runtime_error") {
    oom_error err("out of memory");
    const runtime_error& base = err;
    std::string msg = base.what();
    REQUIRE(msg.find("out of memory") != std::string::npos);
  }
}
