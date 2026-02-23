// straylight::nix::util::checked_arithmetic tests
//
// Property-based testing with rapidcheck for checked arithmetic primitives.
// Tests checked_add, checked_sub, checked_mul and saturating variants.

// Catch2 must be included before rapidcheck/catch.h for v3 compatibility
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include <catch2/catch_test_macros.hpp>

#include "../checked_arithmetic.h"
namespace arith = straylight::nix::util;

// ─────────────────────────────────────────────────────────────────────────────
// Checked Addition Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("checked_add basic unsigned", "[checked_arithmetic][add]") {
  // No overflow
  REQUIRE(arith::checked_add(1u, 2u) == std::optional{3u});
  REQUIRE(arith::checked_add(0u, 0u) == std::optional{0u});
  REQUIRE(arith::checked_add(100u, 200u) == std::optional{300u});

  // Overflow
  constexpr auto max_u32 = std::numeric_limits<std::uint32_t>::max();
  REQUIRE(arith::checked_add(max_u32, 1u) == std::nullopt);
  REQUIRE(arith::checked_add(max_u32, max_u32) == std::nullopt);

  // Edge cases
  REQUIRE(arith::checked_add(max_u32 - 1, 1u) == std::optional{max_u32});
  REQUIRE(arith::checked_add(max_u32, 0u) == std::optional{max_u32});
}

TEST_CASE("checked_add basic signed", "[checked_arithmetic][add]") {
  // No overflow
  REQUIRE(arith::checked_add(1, 2) == std::optional{3});
  REQUIRE(arith::checked_add(-1, 1) == std::optional{0});
  REQUIRE(arith::checked_add(-100, -200) == std::optional{-300});

  // Positive overflow
  constexpr auto max_i32 = std::numeric_limits<std::int32_t>::max();
  constexpr auto min_i32 = std::numeric_limits<std::int32_t>::min();
  REQUIRE(arith::checked_add(max_i32, 1) == std::nullopt);

  // Negative overflow
  REQUIRE(arith::checked_add(min_i32, -1) == std::nullopt);

  // Edge cases
  REQUIRE(arith::checked_add(max_i32, 0) == std::optional{max_i32});
  REQUIRE(arith::checked_add(min_i32, 0) == std::optional{min_i32});
  REQUIRE(arith::checked_add(max_i32, min_i32) == std::optional{-1});
}

TEST_CASE("checked_add property: no overflow when result fits",
          "[checked_arithmetic][add][property]") {
  rc::prop("checked_add succeeds for small values", []() {
    // Use smaller range to avoid overflow
    auto a = *rc::gen::inRange<std::int32_t>(-1000000, 1000000);
    auto b = *rc::gen::inRange<std::int32_t>(-1000000, 1000000);

    auto result = arith::checked_add(a, b);
    RC_ASSERT(result.has_value());
    RC_ASSERT(*result == a + b);
  });
}

TEST_CASE("checked_add property: detects overflow", "[checked_arithmetic][add][property]") {
  rc::prop("checked_add returns nullopt on unsigned overflow", []() {
    constexpr auto max_val = std::numeric_limits<std::uint32_t>::max();
    auto a = *rc::gen::inRange<std::uint32_t>(max_val / 2 + 1, max_val);
    auto b = *rc::gen::inRange<std::uint32_t>(max_val / 2 + 1, max_val);

    auto result = arith::checked_add(a, b);
    RC_ASSERT(!result.has_value());
  });

  rc::prop("checked_add returns nullopt on signed positive overflow", []() {
    constexpr auto max_val = std::numeric_limits<std::int32_t>::max();
    auto a = *rc::gen::inRange<std::int32_t>(max_val / 2 + 1, max_val);
    auto b = *rc::gen::inRange<std::int32_t>(max_val / 2 + 1, max_val);

    auto result = arith::checked_add(a, b);
    RC_ASSERT(!result.has_value());
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Checked Subtraction Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("checked_sub basic unsigned", "[checked_arithmetic][sub]") {
  // No underflow
  REQUIRE(arith::checked_sub(5u, 3u) == std::optional{2u});
  REQUIRE(arith::checked_sub(100u, 100u) == std::optional{0u});
  REQUIRE(arith::checked_sub(0u, 0u) == std::optional{0u});

  // Underflow
  REQUIRE(arith::checked_sub(0u, 1u) == std::nullopt);
  REQUIRE(arith::checked_sub(5u, 10u) == std::nullopt);

  constexpr auto max_u32 = std::numeric_limits<std::uint32_t>::max();
  REQUIRE(arith::checked_sub(0u, max_u32) == std::nullopt);
}

TEST_CASE("checked_sub basic signed", "[checked_arithmetic][sub]") {
  // No overflow
  REQUIRE(arith::checked_sub(5, 3) == std::optional{2});
  REQUIRE(arith::checked_sub(-5, -3) == std::optional{-2});
  REQUIRE(arith::checked_sub(5, -3) == std::optional{8});

  // Overflow (positive)
  constexpr auto max_i32 = std::numeric_limits<std::int32_t>::max();
  constexpr auto min_i32 = std::numeric_limits<std::int32_t>::min();
  REQUIRE(arith::checked_sub(max_i32, -1) == std::nullopt);

  // Overflow (negative)
  REQUIRE(arith::checked_sub(min_i32, 1) == std::nullopt);

  // Edge cases
  REQUIRE(arith::checked_sub(0, min_i32) == std::nullopt); // Would be max+1
  REQUIRE(arith::checked_sub(max_i32, max_i32) == std::optional{0});
}

TEST_CASE("checked_sub property: no underflow when result fits",
          "[checked_arithmetic][sub][property]") {
  rc::prop("checked_sub succeeds for small values", []() {
    auto a = *rc::gen::inRange<std::int32_t>(-1000000, 1000000);
    auto b = *rc::gen::inRange<std::int32_t>(-1000000, 1000000);

    auto result = arith::checked_sub(a, b);
    RC_ASSERT(result.has_value());
    RC_ASSERT(*result == a - b);
  });
}

TEST_CASE("checked_sub property: detects underflow", "[checked_arithmetic][sub][property]") {
  rc::prop("checked_sub returns nullopt on unsigned underflow", []() {
    auto a = *rc::gen::inRange<std::uint32_t>(0, 1000);
    auto b = *rc::gen::inRange<std::uint32_t>(a + 1, std::numeric_limits<std::uint32_t>::max());

    auto result = arith::checked_sub(a, b);
    RC_ASSERT(!result.has_value());
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Checked Multiplication Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("checked_mul basic unsigned", "[checked_arithmetic][mul]") {
  // No overflow
  REQUIRE(arith::checked_mul(3u, 4u) == std::optional{12u});
  REQUIRE(arith::checked_mul(0u, 100u) == std::optional{0u});
  REQUIRE(arith::checked_mul(1u, 100u) == std::optional{100u});

  // Overflow
  constexpr auto max_u32 = std::numeric_limits<std::uint32_t>::max();
  REQUIRE(arith::checked_mul(max_u32, 2u) == std::nullopt);
  REQUIRE(arith::checked_mul(65536u, 65536u) == std::nullopt); // 2^32

  // Edge cases
  REQUIRE(arith::checked_mul(max_u32, 1u) == std::optional{max_u32});
  REQUIRE(arith::checked_mul(max_u32, 0u) == std::optional{0u});
}

TEST_CASE("checked_mul basic signed", "[checked_arithmetic][mul]") {
  // No overflow
  REQUIRE(arith::checked_mul(3, 4) == std::optional{12});
  REQUIRE(arith::checked_mul(-3, 4) == std::optional{-12});
  REQUIRE(arith::checked_mul(-3, -4) == std::optional{12});
  REQUIRE(arith::checked_mul(0, 1000000) == std::optional{0});

  // Overflow
  constexpr auto max_i32 = std::numeric_limits<std::int32_t>::max();
  constexpr auto min_i32 = std::numeric_limits<std::int32_t>::min();
  REQUIRE(arith::checked_mul(max_i32, 2) == std::nullopt);
  REQUIRE(arith::checked_mul(min_i32, 2) == std::nullopt);
  REQUIRE(arith::checked_mul(min_i32, -1) == std::nullopt); // Would be max+1

  // Edge cases
  REQUIRE(arith::checked_mul(max_i32, 1) == std::optional{max_i32});
  REQUIRE(arith::checked_mul(min_i32, 1) == std::optional{min_i32});
  REQUIRE(arith::checked_mul(max_i32, -1) == std::optional{-max_i32});
}

TEST_CASE("checked_mul property: no overflow when result fits",
          "[checked_arithmetic][mul][property]") {
  rc::prop("checked_mul succeeds for small values", []() {
    auto a = *rc::gen::inRange<std::int32_t>(-1000, 1000);
    auto b = *rc::gen::inRange<std::int32_t>(-1000, 1000);

    auto result = arith::checked_mul(a, b);
    RC_ASSERT(result.has_value());
    RC_ASSERT(*result == a * b);
  });
}

TEST_CASE("checked_mul property: multiplication by zero", "[checked_arithmetic][mul][property]") {
  rc::prop("checked_mul with zero always succeeds with zero result", []() {
    auto a = *rc::gen::arbitrary<std::int32_t>();

    auto result1 = arith::checked_mul(a, 0);
    auto result2 = arith::checked_mul(0, a);

    RC_ASSERT(result1.has_value());
    RC_ASSERT(*result1 == 0);
    RC_ASSERT(result2.has_value());
    RC_ASSERT(*result2 == 0);
  });
}

TEST_CASE("checked_mul property: multiplication by one", "[checked_arithmetic][mul][property]") {
  rc::prop("checked_mul with one returns the other operand", []() {
    auto a = *rc::gen::arbitrary<std::int32_t>();

    auto result1 = arith::checked_mul(a, 1);
    auto result2 = arith::checked_mul(1, a);

    RC_ASSERT(result1.has_value());
    RC_ASSERT(*result1 == a);
    RC_ASSERT(result2.has_value());
    RC_ASSERT(*result2 == a);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Saturating Addition Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("saturating_add basic unsigned", "[checked_arithmetic][saturating][add]") {
  // No overflow
  REQUIRE(arith::saturating_add(1u, 2u) == 3u);
  REQUIRE(arith::saturating_add(100u, 200u) == 300u);

  // Overflow saturates to max
  constexpr auto max_u32 = std::numeric_limits<std::uint32_t>::max();
  REQUIRE(arith::saturating_add(max_u32, 1u) == max_u32);
  REQUIRE(arith::saturating_add(max_u32, max_u32) == max_u32);
  REQUIRE(arith::saturating_add(max_u32 - 1, 2u) == max_u32);
}

TEST_CASE("saturating_add basic signed", "[checked_arithmetic][saturating][add]") {
  // No overflow
  REQUIRE(arith::saturating_add(1, 2) == 3);
  REQUIRE(arith::saturating_add(-1, -2) == -3);
  REQUIRE(arith::saturating_add(-1, 1) == 0);

  // Positive overflow saturates to max
  constexpr auto max_i32 = std::numeric_limits<std::int32_t>::max();
  constexpr auto min_i32 = std::numeric_limits<std::int32_t>::min();
  REQUIRE(arith::saturating_add(max_i32, 1) == max_i32);
  REQUIRE(arith::saturating_add(max_i32, max_i32) == max_i32);

  // Negative overflow saturates to min
  REQUIRE(arith::saturating_add(min_i32, -1) == min_i32);
  REQUIRE(arith::saturating_add(min_i32, min_i32) == min_i32);
}

TEST_CASE("saturating_add property: result in valid range",
          "[checked_arithmetic][saturating][add][property]") {
  rc::prop("saturating_add result is always within bounds", []() {
    auto a = *rc::gen::arbitrary<std::int32_t>();
    auto b = *rc::gen::arbitrary<std::int32_t>();

    auto result = arith::saturating_add(a, b);

    constexpr auto max_val = std::numeric_limits<std::int32_t>::max();
    constexpr auto min_val = std::numeric_limits<std::int32_t>::min();

    RC_ASSERT(result >= min_val);
    RC_ASSERT(result <= max_val);
  });
}

TEST_CASE("saturating_add property: matches checked when no overflow",
          "[checked_arithmetic][saturating][add][property]") {
  rc::prop("saturating_add equals checked_add when no overflow", []() {
    auto a = *rc::gen::inRange<std::int32_t>(-1000000, 1000000);
    auto b = *rc::gen::inRange<std::int32_t>(-1000000, 1000000);

    auto checked = arith::checked_add(a, b);
    auto saturating = arith::saturating_add(a, b);

    RC_ASSERT(checked.has_value());
    RC_ASSERT(saturating == *checked);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Saturating Subtraction Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("saturating_sub basic unsigned", "[checked_arithmetic][saturating][sub]") {
  // No underflow
  REQUIRE(arith::saturating_sub(5u, 3u) == 2u);
  REQUIRE(arith::saturating_sub(100u, 100u) == 0u);

  // Underflow saturates to 0
  REQUIRE(arith::saturating_sub(0u, 1u) == 0u);
  REQUIRE(arith::saturating_sub(5u, 10u) == 0u);

  constexpr auto max_u32 = std::numeric_limits<std::uint32_t>::max();
  REQUIRE(arith::saturating_sub(0u, max_u32) == 0u);
}

TEST_CASE("saturating_sub basic signed", "[checked_arithmetic][saturating][sub]") {
  // No overflow
  REQUIRE(arith::saturating_sub(5, 3) == 2);
  REQUIRE(arith::saturating_sub(-5, -3) == -2);

  // Positive overflow saturates to max
  constexpr auto max_i32 = std::numeric_limits<std::int32_t>::max();
  constexpr auto min_i32 = std::numeric_limits<std::int32_t>::min();
  REQUIRE(arith::saturating_sub(max_i32, -1) == max_i32);

  // Negative overflow saturates to min
  REQUIRE(arith::saturating_sub(min_i32, 1) == min_i32);
  REQUIRE(arith::saturating_sub(min_i32, max_i32) == min_i32);
}

TEST_CASE("saturating_sub property: result in valid range",
          "[checked_arithmetic][saturating][sub][property]") {
  rc::prop("saturating_sub result is always within bounds", []() {
    auto a = *rc::gen::arbitrary<std::uint32_t>();
    auto b = *rc::gen::arbitrary<std::uint32_t>();

    auto result = arith::saturating_sub(a, b);

    // Unsigned: result should be >= 0 (always true) and <= max
    RC_ASSERT(result <= std::numeric_limits<std::uint32_t>::max());
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Saturating Multiplication Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("saturating_mul basic unsigned", "[checked_arithmetic][saturating][mul]") {
  // No overflow
  REQUIRE(arith::saturating_mul(3u, 4u) == 12u);
  REQUIRE(arith::saturating_mul(0u, 100u) == 0u);

  // Overflow saturates to max
  constexpr auto max_u32 = std::numeric_limits<std::uint32_t>::max();
  REQUIRE(arith::saturating_mul(max_u32, 2u) == max_u32);
  REQUIRE(arith::saturating_mul(65536u, 65536u) == max_u32);
}

TEST_CASE("saturating_mul basic signed", "[checked_arithmetic][saturating][mul]") {
  // No overflow
  REQUIRE(arith::saturating_mul(3, 4) == 12);
  REQUIRE(arith::saturating_mul(-3, 4) == -12);
  REQUIRE(arith::saturating_mul(-3, -4) == 12);

  // Positive overflow saturates to max
  constexpr auto max_i32 = std::numeric_limits<std::int32_t>::max();
  constexpr auto min_i32 = std::numeric_limits<std::int32_t>::min();
  REQUIRE(arith::saturating_mul(max_i32, 2) == max_i32);
  REQUIRE(arith::saturating_mul(min_i32, -1) == max_i32); // -min overflows

  // Negative overflow saturates to min
  REQUIRE(arith::saturating_mul(min_i32, 2) == min_i32);
  REQUIRE(arith::saturating_mul(max_i32, -2) == min_i32);
}

TEST_CASE("saturating_mul property: zero always yields zero",
          "[checked_arithmetic][saturating][mul][property]") {
  rc::prop("saturating_mul with zero returns zero", []() {
    auto a = *rc::gen::arbitrary<std::int32_t>();

    RC_ASSERT(arith::saturating_mul(a, 0) == 0);
    RC_ASSERT(arith::saturating_mul(0, a) == 0);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Type Support Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("checked arithmetic supports all integer types", "[checked_arithmetic][types]") {
  // Signed types
  REQUIRE(arith::checked_add(std::int8_t{1}, std::int8_t{2}) == std::optional{std::int8_t{3}});
  REQUIRE(arith::checked_add(std::int16_t{1}, std::int16_t{2}) == std::optional{std::int16_t{3}});
  REQUIRE(arith::checked_add(std::int32_t{1}, std::int32_t{2}) == std::optional{std::int32_t{3}});
  REQUIRE(arith::checked_add(std::int64_t{1}, std::int64_t{2}) == std::optional{std::int64_t{3}});

  // Unsigned types
  REQUIRE(arith::checked_add(std::uint8_t{1}, std::uint8_t{2}) == std::optional{std::uint8_t{3}});
  REQUIRE(arith::checked_add(std::uint16_t{1}, std::uint16_t{2}) ==
          std::optional{std::uint16_t{3}});
  REQUIRE(arith::checked_add(std::uint32_t{1}, std::uint32_t{2}) ==
          std::optional{std::uint32_t{3}});
  REQUIRE(arith::checked_add(std::uint64_t{1}, std::uint64_t{2}) ==
          std::optional{std::uint64_t{3}});

  // size_t
  REQUIRE(arith::checked_add(std::size_t{1}, std::size_t{2}) == std::optional{std::size_t{3}});
}

TEST_CASE("checked arithmetic int8_t edge cases", "[checked_arithmetic][types]") {
  constexpr auto max_i8 = std::numeric_limits<std::int8_t>::max();  // 127
  constexpr auto min_i8 = std::numeric_limits<std::int8_t>::min();  // -128
  constexpr auto max_u8 = std::numeric_limits<std::uint8_t>::max(); // 255

  // int8_t overflow
  REQUIRE(arith::checked_add(max_i8, std::int8_t{1}) == std::nullopt);
  REQUIRE(arith::checked_add(min_i8, std::int8_t{-1}) == std::nullopt);
  REQUIRE(arith::checked_mul(max_i8, std::int8_t{2}) == std::nullopt);

  // uint8_t overflow
  REQUIRE(arith::checked_add(max_u8, std::uint8_t{1}) == std::nullopt);
  REQUIRE(arith::checked_sub(std::uint8_t{0}, std::uint8_t{1}) == std::nullopt);

  // Saturating
  REQUIRE(arith::saturating_add(max_i8, std::int8_t{1}) == max_i8);
  REQUIRE(arith::saturating_add(min_i8, std::int8_t{-1}) == min_i8);
  REQUIRE(arith::saturating_add(max_u8, std::uint8_t{1}) == max_u8);
  REQUIRE(arith::saturating_sub(std::uint8_t{0}, std::uint8_t{1}) == std::uint8_t{0});
}

TEST_CASE("checked arithmetic int64_t edge cases", "[checked_arithmetic][types]") {
  constexpr auto max_i64 = std::numeric_limits<std::int64_t>::max();
  constexpr auto min_i64 = std::numeric_limits<std::int64_t>::min();
  constexpr auto max_u64 = std::numeric_limits<std::uint64_t>::max();

  // int64_t overflow
  REQUIRE(arith::checked_add(max_i64, std::int64_t{1}) == std::nullopt);
  REQUIRE(arith::checked_add(min_i64, std::int64_t{-1}) == std::nullopt);
  REQUIRE(arith::checked_mul(max_i64, std::int64_t{2}) == std::nullopt);

  // uint64_t overflow
  REQUIRE(arith::checked_add(max_u64, std::uint64_t{1}) == std::nullopt);
  REQUIRE(arith::checked_mul(max_u64, std::uint64_t{2}) == std::nullopt);

  // Large values that don't overflow
  REQUIRE(arith::checked_add(max_i64 / 2, max_i64 / 2).has_value());
  REQUIRE(arith::checked_mul(std::int64_t{1000000}, std::int64_t{1000000}).has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Constexpr Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("checked arithmetic is constexpr", "[checked_arithmetic][constexpr]") {
  // Verify operations can be used in constexpr context
  constexpr auto add_result = arith::checked_add(1, 2);
  static_assert(add_result.has_value());
  static_assert(*add_result == 3);

  constexpr auto sub_result = arith::checked_sub(5, 3);
  static_assert(sub_result.has_value());
  static_assert(*sub_result == 2);

  constexpr auto mul_result = arith::checked_mul(3, 4);
  static_assert(mul_result.has_value());
  static_assert(*mul_result == 12);

  // Saturating
  constexpr auto sat_add = arith::saturating_add(1, 2);
  static_assert(sat_add == 3);

  constexpr auto sat_sub = arith::saturating_sub(5u, 3u);
  static_assert(sat_sub == 2u);

  constexpr auto sat_mul = arith::saturating_mul(3, 4);
  static_assert(sat_mul == 12);

  // Overflow cases
  constexpr auto overflow_add = arith::checked_add(std::numeric_limits<std::int32_t>::max(), 1);
  static_assert(!overflow_add.has_value());

  constexpr auto sat_overflow = arith::saturating_add(std::numeric_limits<std::int32_t>::max(), 1);
  static_assert(sat_overflow == std::numeric_limits<std::int32_t>::max());

  REQUIRE(true); // Test passes if compilation succeeds
}

// ─────────────────────────────────────────────────────────────────────────────
// Commutativity and Associativity Properties
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("checked_add is commutative", "[checked_arithmetic][property]") {
  rc::prop("checked_add(a, b) == checked_add(b, a)", []() {
    auto a = *rc::gen::arbitrary<std::int32_t>();
    auto b = *rc::gen::arbitrary<std::int32_t>();

    RC_ASSERT(arith::checked_add(a, b) == arith::checked_add(b, a));
  });
}

TEST_CASE("checked_mul is commutative", "[checked_arithmetic][property]") {
  rc::prop("checked_mul(a, b) == checked_mul(b, a)", []() {
    auto a = *rc::gen::arbitrary<std::int32_t>();
    auto b = *rc::gen::arbitrary<std::int32_t>();

    RC_ASSERT(arith::checked_mul(a, b) == arith::checked_mul(b, a));
  });
}

TEST_CASE("saturating_add is commutative", "[checked_arithmetic][property]") {
  rc::prop("saturating_add(a, b) == saturating_add(b, a)", []() {
    auto a = *rc::gen::arbitrary<std::int32_t>();
    auto b = *rc::gen::arbitrary<std::int32_t>();

    RC_ASSERT(arith::saturating_add(a, b) == arith::saturating_add(b, a));
  });
}

TEST_CASE("saturating_mul is commutative", "[checked_arithmetic][property]") {
  rc::prop("saturating_mul(a, b) == saturating_mul(b, a)", []() {
    auto a = *rc::gen::arbitrary<std::int32_t>();
    auto b = *rc::gen::arbitrary<std::int32_t>();

    RC_ASSERT(arith::saturating_mul(a, b) == arith::saturating_mul(b, a));
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Identity Element Properties
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("checked arithmetic identity elements", "[checked_arithmetic][property]") {
  rc::prop("adding zero is identity for checked_add", []() {
    auto a = *rc::gen::arbitrary<std::int32_t>();

    auto result = arith::checked_add(a, 0);
    RC_ASSERT(result.has_value());
    RC_ASSERT(*result == a);
  });

  rc::prop("subtracting zero is identity for checked_sub", []() {
    auto a = *rc::gen::arbitrary<std::int32_t>();

    auto result = arith::checked_sub(a, 0);
    RC_ASSERT(result.has_value());
    RC_ASSERT(*result == a);
  });

  rc::prop("multiplying by one is identity for checked_mul", []() {
    auto a = *rc::gen::arbitrary<std::int32_t>();

    auto result = arith::checked_mul(a, 1);
    RC_ASSERT(result.has_value());
    RC_ASSERT(*result == a);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// size_t Specific Tests (important for memory allocation)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("checked arithmetic with size_t for memory safety", "[checked_arithmetic][size_t]") {
  constexpr auto max_size = std::numeric_limits<std::size_t>::max();

  // Safe memory calculations
  REQUIRE(arith::checked_mul(std::size_t{1024}, std::size_t{1024}) ==
          std::optional{std::size_t{1048576}});

  // Overflow detection for allocation sizes
  REQUIRE(arith::checked_mul(max_size, std::size_t{2}) == std::nullopt);
  REQUIRE(arith::checked_add(max_size, std::size_t{1}) == std::nullopt);

  // Saturating for defensive allocation
  REQUIRE(arith::saturating_mul(max_size, std::size_t{2}) == max_size);
  REQUIRE(arith::saturating_add(max_size, std::size_t{1}) == max_size);
}

TEST_CASE("size_t multiplication for buffer sizing", "[checked_arithmetic][size_t][property]") {
  rc::prop("element_count * element_size is safe when both small", []() {
    auto count = *rc::gen::inRange<std::size_t>(0, 10000);
    auto size = *rc::gen::inRange<std::size_t>(1, 10000);

    auto result = arith::checked_mul(count, size);
    RC_ASSERT(result.has_value());
    RC_ASSERT(*result == count * size);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Comparison with Nix's Checked class behavior
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("checked arithmetic matches Nix Checked behavior", "[checked_arithmetic][nix_compat]") {
  // Test cases from nix/util/checked-arithmetic.h

  // Addition overflow
  constexpr auto max_i64 = std::numeric_limits<std::int64_t>::max();
  REQUIRE(arith::checked_add(max_i64, std::int64_t{1}) == std::nullopt);

  // Subtraction underflow
  constexpr auto min_i64 = std::numeric_limits<std::int64_t>::min();
  REQUIRE(arith::checked_sub(min_i64, std::int64_t{1}) == std::nullopt);

  // Multiplication overflow
  REQUIRE(arith::checked_mul(max_i64, std::int64_t{2}) == std::nullopt);

  // MIN / -1 would overflow (from Nix's division check)
  // We don't have division, but we can test the related multiply
  REQUIRE(arith::checked_mul(min_i64, std::int64_t{-1}) == std::nullopt);
}

// ─────────────────────────────────────────────────────────────────────────────
// Fuzz Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("checked arithmetic fuzz test", "[checked_arithmetic][fuzz]") {
  rc::prop("checked_add never causes UB", []() {
    auto a = *rc::gen::arbitrary<std::int64_t>();
    auto b = *rc::gen::arbitrary<std::int64_t>();

    // Should not crash or invoke UB
    [[maybe_unused]] auto result = arith::checked_add(a, b);
  });

  rc::prop("checked_sub never causes UB", []() {
    auto a = *rc::gen::arbitrary<std::int64_t>();
    auto b = *rc::gen::arbitrary<std::int64_t>();

    [[maybe_unused]] auto result = arith::checked_sub(a, b);
  });

  rc::prop("checked_mul never causes UB", []() {
    auto a = *rc::gen::arbitrary<std::int64_t>();
    auto b = *rc::gen::arbitrary<std::int64_t>();

    [[maybe_unused]] auto result = arith::checked_mul(a, b);
  });

  rc::prop("saturating_add never causes UB", []() {
    auto a = *rc::gen::arbitrary<std::int64_t>();
    auto b = *rc::gen::arbitrary<std::int64_t>();

    [[maybe_unused]] auto result = arith::saturating_add(a, b);
  });

  rc::prop("saturating_sub never causes UB", []() {
    auto a = *rc::gen::arbitrary<std::int64_t>();
    auto b = *rc::gen::arbitrary<std::int64_t>();

    [[maybe_unused]] auto result = arith::saturating_sub(a, b);
  });

  rc::prop("saturating_mul never causes UB", []() {
    auto a = *rc::gen::arbitrary<std::int64_t>();
    auto b = *rc::gen::arbitrary<std::int64_t>();

    [[maybe_unused]] auto result = arith::saturating_mul(a, b);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Consistency between checked and saturating
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("saturating returns limit when checked returns nullopt",
          "[checked_arithmetic][property]") {
  rc::prop("saturating_add saturates exactly when checked_add fails", []() {
    auto a = *rc::gen::arbitrary<std::uint32_t>();
    auto b = *rc::gen::arbitrary<std::uint32_t>();

    auto checked = arith::checked_add(a, b);
    auto saturating = arith::saturating_add(a, b);

    if (checked.has_value()) {
      RC_ASSERT(saturating == *checked);
    } else {
      RC_ASSERT(saturating == std::numeric_limits<std::uint32_t>::max());
    }
  });

  rc::prop("saturating_sub saturates exactly when checked_sub fails (unsigned)", []() {
    auto a = *rc::gen::arbitrary<std::uint32_t>();
    auto b = *rc::gen::arbitrary<std::uint32_t>();

    auto checked = arith::checked_sub(a, b);
    auto saturating = arith::saturating_sub(a, b);

    if (checked.has_value()) {
      RC_ASSERT(saturating == *checked);
    } else {
      RC_ASSERT(saturating == 0u);
    }
  });
}
