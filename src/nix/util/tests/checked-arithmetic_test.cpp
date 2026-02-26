// straylight // nix // util // tests
//
// Unit tests for checked arithmetic operations

// Catch2 must be included before rapidcheck/catch.h
#include <catch2/catch_test_macros.hpp>

// RapidCheck for property-based testing
#include <cstdint>
#include <limits>
#include <sstream>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include "nix/util/checked-arithmetic.h"


// ─────────────────────────────────────────────────────────────────────────────
// Construction and basic value access
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("checked construction and value access", "[checked][construction]") {
  nix::checked::checked_t<int32_t> value{42};
  REQUIRE(static_cast<int32_t>(value) == 42);
  REQUIRE(value.value == 42);
}

TEST_CASE("checked copy construction", "[checked][construction]") {
  nix::checked::checked_t<int64_t> original{123456789};
  nix::checked::checked_t<int64_t> copy{original};
  REQUIRE(copy.value == 123456789);
}

TEST_CASE("checked comparison operators", "[checked][comparison]") {
  nix::checked::checked_t<int32_t> a{10};
  nix::checked::checked_t<int32_t> b{20};
  nix::checked::checked_t<int32_t> c{10};

  REQUIRE(a < b);
  REQUIRE(b > a);
  REQUIRE(a == c);
  REQUIRE(a <= c);
  REQUIRE(a >= c);
  REQUIRE(a != b);

  // Comparison with raw value via spaceship operator
  REQUIRE((a <=> 10) == std::strong_ordering::equal);
  REQUIRE((a <=> 15) == std::strong_ordering::less);
  REQUIRE((a <=> 5) == std::strong_ordering::greater);
}

// ─────────────────────────────────────────────────────────────────────────────
// Addition tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("checked addition no overflow", "[checked][addition]") {
  nix::checked::checked_t<int32_t> a{100};
  nix::checked::checked_t<int32_t> b{200};

  auto result = a + b;
  REQUIRE_FALSE(result.overflowed());
  REQUIRE_FALSE(result.divideByZero());
  REQUIRE(result.valueChecked().has_value());
  REQUIRE(result.valueChecked().value() == 300);
  REQUIRE(result.valueWrapping() == 300);
}

TEST_CASE("checked addition with raw value", "[checked][addition]") {
  nix::checked::checked_t<int32_t> a{50};

  auto result = a + 25;
  REQUIRE_FALSE(result.overflowed());
  REQUIRE(result.valueChecked().value() == 75);
}

TEST_CASE("checked addition overflow at int_max", "[checked][addition]") {
  constexpr int32_t max_value = std::numeric_limits<int32_t>::max();
  nix::checked::checked_t<int32_t> a{max_value};
  nix::checked::checked_t<int32_t> b{1};

  auto result = a + b;
  REQUIRE(result.overflowed());
  REQUIRE_FALSE(result.valueChecked().has_value());
  // valueWrapping should return the wrapped value without throwing
  // (overflow is not DivByZero)
  REQUIRE(result.valueWrapping() == std::numeric_limits<int32_t>::min());
}

TEST_CASE("checked addition overflow with large values", "[checked][addition]") {
  constexpr int32_t max_value = std::numeric_limits<int32_t>::max();
  nix::checked::checked_t<int32_t> a{max_value};
  nix::checked::checked_t<int32_t> b{max_value};

  auto result = a + b;
  REQUIRE(result.overflowed());
  REQUIRE_FALSE(result.valueChecked().has_value());
}

TEST_CASE("checked addition negative overflow", "[checked][addition]") {
  constexpr int32_t min_value = std::numeric_limits<int32_t>::min();
  nix::checked::checked_t<int32_t> a{min_value};
  nix::checked::checked_t<int32_t> b{-1};

  auto result = a + b;
  REQUIRE(result.overflowed());
  REQUIRE_FALSE(result.valueChecked().has_value());
}

TEST_CASE("checked addition unsigned overflow", "[checked][addition]") {
  constexpr uint32_t max_value = std::numeric_limits<uint32_t>::max();
  nix::checked::checked_t<uint32_t> a{max_value};
  nix::checked::checked_t<uint32_t> b{1};

  auto result = a + b;
  REQUIRE(result.overflowed());
  REQUIRE_FALSE(result.valueChecked().has_value());
  REQUIRE(result.valueWrapping() == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Subtraction tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("checked subtraction no overflow", "[checked][subtraction]") {
  nix::checked::checked_t<int32_t> a{300};
  nix::checked::checked_t<int32_t> b{100};

  auto result = a - b;
  REQUIRE_FALSE(result.overflowed());
  REQUIRE(result.valueChecked().value() == 200);
}

TEST_CASE("checked subtraction with raw value", "[checked][subtraction]") {
  nix::checked::checked_t<int32_t> a{100};

  auto result = a - 30;
  REQUIRE_FALSE(result.overflowed());
  REQUIRE(result.valueChecked().value() == 70);
}

TEST_CASE("checked subtraction overflow at int_min", "[checked][subtraction]") {
  constexpr int32_t min_value = std::numeric_limits<int32_t>::min();
  nix::checked::checked_t<int32_t> a{min_value};
  nix::checked::checked_t<int32_t> b{1};

  auto result = a - b;
  REQUIRE(result.overflowed());
  REQUIRE_FALSE(result.valueChecked().has_value());
}

TEST_CASE("checked subtraction unsigned underflow", "[checked][subtraction]") {
  nix::checked::checked_t<uint32_t> a{0};
  nix::checked::checked_t<uint32_t> b{1};

  auto result = a - b;
  REQUIRE(result.overflowed());
  REQUIRE_FALSE(result.valueChecked().has_value());
  REQUIRE(result.valueWrapping() == std::numeric_limits<uint32_t>::max());
}

// ─────────────────────────────────────────────────────────────────────────────
// Multiplication tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("checked multiplication no overflow", "[checked][multiplication]") {
  nix::checked::checked_t<int32_t> a{100};
  nix::checked::checked_t<int32_t> b{200};

  auto result = a * b;
  REQUIRE_FALSE(result.overflowed());
  REQUIRE(result.valueChecked().value() == 20000);
}

TEST_CASE("checked multiplication with raw value", "[checked][multiplication]") {
  nix::checked::checked_t<int32_t> a{50};

  auto result = a * 10;
  REQUIRE_FALSE(result.overflowed());
  REQUIRE(result.valueChecked().value() == 500);
}

TEST_CASE("checked multiplication overflow", "[checked][multiplication]") {
  constexpr int32_t max_value = std::numeric_limits<int32_t>::max();
  nix::checked::checked_t<int32_t> a{max_value};
  nix::checked::checked_t<int32_t> b{2};

  auto result = a * b;
  REQUIRE(result.overflowed());
  REQUIRE_FALSE(result.valueChecked().has_value());
}

TEST_CASE("checked multiplication by zero", "[checked][multiplication]") {
  constexpr int32_t max_value = std::numeric_limits<int32_t>::max();
  nix::checked::checked_t<int32_t> a{max_value};
  nix::checked::checked_t<int32_t> b{0};

  auto result = a * b;
  REQUIRE_FALSE(result.overflowed());
  REQUIRE(result.valueChecked().value() == 0);
}

TEST_CASE("checked multiplication negative overflow", "[checked][multiplication]") {
  constexpr int32_t min_value = std::numeric_limits<int32_t>::min();
  nix::checked::checked_t<int32_t> a{min_value};
  nix::checked::checked_t<int32_t> b{2};

  auto result = a * b;
  REQUIRE(result.overflowed());
  REQUIRE_FALSE(result.valueChecked().has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Division tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("checked division no overflow", "[checked][division]") {
  nix::checked::checked_t<int32_t> a{100};
  nix::checked::checked_t<int32_t> b{5};

  auto result = a / b;
  REQUIRE_FALSE(result.overflowed());
  REQUIRE_FALSE(result.divideByZero());
  REQUIRE(result.valueChecked().value() == 20);
}

TEST_CASE("checked division with raw value", "[checked][division]") {
  nix::checked::checked_t<int32_t> a{100};

  auto result = a / 4;
  REQUIRE_FALSE(result.overflowed());
  REQUIRE(result.valueChecked().value() == 25);
}

TEST_CASE("checked division by zero", "[checked][division]") {
  nix::checked::checked_t<int32_t> a{100};
  nix::checked::checked_t<int32_t> b{0};

  auto result = a / b;
  REQUIRE(result.divideByZero());
  REQUIRE_FALSE(result.overflowed());
  REQUIRE_FALSE(result.valueChecked().has_value());
  REQUIRE_THROWS_AS(result.valueWrapping(), nix::checked::divide_by_zero_t);
}

TEST_CASE("checked division by zero with raw value", "[checked][division]") {
  nix::checked::checked_t<int32_t> a{100};

  auto result = a / 0;
  REQUIRE(result.divideByZero());
  REQUIRE_THROWS_AS(result.valueWrapping(), nix::checked::divide_by_zero_t);
}

TEST_CASE("checked division overflow int_min by negative one", "[checked][division]") {
  // This is the only way signed division can overflow:
  // INT_MIN / -1 would produce -INT_MIN which exceeds INT_MAX
  constexpr int32_t min_value = std::numeric_limits<int32_t>::min();
  nix::checked::checked_t<int32_t> a{min_value};
  nix::checked::checked_t<int32_t> b{-1};

  auto result = a / b;
  REQUIRE(result.overflowed());
  REQUIRE_FALSE(result.divideByZero());
  REQUIRE_FALSE(result.valueChecked().has_value());
  // The wrapped value should be min_value (wraps back)
  REQUIRE(result.valueWrapping() == min_value);
}

TEST_CASE("checked division unsigned cannot overflow", "[checked][division]") {
  constexpr uint32_t max_value = std::numeric_limits<uint32_t>::max();
  nix::checked::checked_t<uint32_t> a{max_value};
  nix::checked::checked_t<uint32_t> b{1};

  auto result = a / b;
  REQUIRE_FALSE(result.overflowed());
  REQUIRE(result.valueChecked().value() == max_value);
}

// ─────────────────────────────────────────────────────────────────────────────
// Result type tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("result equality", "[checked][result]") {
  nix::checked::checked_t<int32_t> a{10};
  nix::checked::checked_t<int32_t> b{20};

  auto result_one = a + b;
  auto result_two = a + b;

  REQUIRE(result_one == result_two);
}

TEST_CASE("result value_checked returns nullopt on overflow", "[checked][result]") {
  constexpr int32_t max_value = std::numeric_limits<int32_t>::max();
  nix::checked::checked_t<int32_t> a{max_value};

  auto result = a + 1;
  REQUIRE_FALSE(result.valueChecked().has_value());
}

TEST_CASE("result value_checked returns nullopt on divide by zero", "[checked][result]") {
  nix::checked::checked_t<int32_t> a{10};

  auto result = a / 0;
  REQUIRE_FALSE(result.valueChecked().has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Multi-type tests (different integral types)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("checked int8_t overflow detection", "[checked][int8]") {
  constexpr int8_t max_value = std::numeric_limits<int8_t>::max();
  nix::checked::checked_t<int8_t> a{max_value};

  auto result = a + static_cast<int8_t>(1);
  REQUIRE(result.overflowed());
}

TEST_CASE("checked int64_t large values", "[checked][int64]") {
  constexpr int64_t large_value = 1'000'000'000'000LL;
  nix::checked::checked_t<int64_t> a{large_value};
  nix::checked::checked_t<int64_t> b{large_value};

  auto result = a * b;
  // This should overflow since 10^12 * 10^12 = 10^24 > INT64_MAX (~9.2 * 10^18)
  REQUIRE(result.overflowed());
}

TEST_CASE("checked uint8_t wraparound detection", "[checked][uint8]") {
  constexpr uint8_t max_value = std::numeric_limits<uint8_t>::max();
  nix::checked::checked_t<uint8_t> a{max_value};

  auto result = a + static_cast<uint8_t>(1);
  REQUIRE(result.overflowed());
  REQUIRE(result.valueWrapping() == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Output stream test
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("checked ostream output", "[checked][ostream]") {
  nix::checked::checked_t<int32_t> value{42};
  std::ostringstream stream;
  stream << value;
  REQUIRE(stream.str() == "42");
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-based tests with RapidCheck
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("checked arithmetic properties int32", "[checked][property]") {
  rc::prop("addition identity: x + 0 == x", []() {
    auto x = *rc::gen::arbitrary<int32_t>();
    nix::checked::checked_t<int32_t> checked_x{x};

    auto result = checked_x + 0;
    RC_ASSERT(!result.overflowed());
    RC_ASSERT(result.valueChecked().value() == x);
  });

  rc::prop("subtraction identity: x - 0 == x", []() {
    auto x = *rc::gen::arbitrary<int32_t>();
    nix::checked::checked_t<int32_t> checked_x{x};

    auto result = checked_x - 0;
    RC_ASSERT(!result.overflowed());
    RC_ASSERT(result.valueChecked().value() == x);
  });

  rc::prop("multiplication identity: x * 1 == x", []() {
    auto x = *rc::gen::arbitrary<int32_t>();
    nix::checked::checked_t<int32_t> checked_x{x};

    auto result = checked_x * 1;
    RC_ASSERT(!result.overflowed());
    RC_ASSERT(result.valueChecked().value() == x);
  });

  rc::prop("division identity: x / 1 == x", []() {
    auto x = *rc::gen::arbitrary<int32_t>();
    nix::checked::checked_t<int32_t> checked_x{x};

    auto result = checked_x / 1;
    RC_ASSERT(!result.overflowed());
    RC_ASSERT(!result.divideByZero());
    RC_ASSERT(result.valueChecked().value() == x);
  });

  rc::prop("self subtraction: x - x == 0", []() {
    auto x = *rc::gen::arbitrary<int32_t>();
    nix::checked::checked_t<int32_t> checked_x{x};

    auto result = checked_x - x;
    RC_ASSERT(!result.overflowed());
    RC_ASSERT(result.valueChecked().value() == 0);
  });

  rc::prop("multiplication by zero: x * 0 == 0", []() {
    auto x = *rc::gen::arbitrary<int32_t>();
    nix::checked::checked_t<int32_t> checked_x{x};

    auto result = checked_x * 0;
    RC_ASSERT(!result.overflowed());
    RC_ASSERT(result.valueChecked().value() == 0);
  });

  rc::prop("addition commutativity: a + b == b + a (when no overflow)", []() {
    // Use smaller range to reduce overflow likelihood
    auto a = *rc::gen::inRange<int32_t>(-1000000, 1000000);
    auto b = *rc::gen::inRange<int32_t>(-1000000, 1000000);
    nix::checked::checked_t<int32_t> checked_a{a};
    nix::checked::checked_t<int32_t> checked_b{b};

    auto result_ab = checked_a + b;
    auto result_ba = checked_b + a;

    RC_ASSERT(result_ab.overflowed() == result_ba.overflowed());
    if (!result_ab.overflowed()) {
      RC_ASSERT(result_ab.valueChecked().value() == result_ba.valueChecked().value());
    }
  });

  rc::prop("multiplication commutativity: a * b == b * a", []() {
    // Use smaller range to reduce overflow likelihood
    auto a = *rc::gen::inRange<int32_t>(-10000, 10000);
    auto b = *rc::gen::inRange<int32_t>(-10000, 10000);
    nix::checked::checked_t<int32_t> checked_a{a};
    nix::checked::checked_t<int32_t> checked_b{b};

    auto result_ab = checked_a * b;
    auto result_ba = checked_b * a;

    RC_ASSERT(result_ab.overflowed() == result_ba.overflowed());
    if (!result_ab.overflowed()) {
      RC_ASSERT(result_ab.valueChecked().value() == result_ba.valueChecked().value());
    }
  });
}

TEST_CASE("checked overflow detection properties", "[checked][property][overflow]") {
  rc::prop("int_max + positive always overflows", []() {
    constexpr int32_t max_value = std::numeric_limits<int32_t>::max();
    auto positive = *rc::gen::inRange<int32_t>(1, 1000000);
    nix::checked::checked_t<int32_t> checked_max{max_value};

    auto result = checked_max + positive;
    RC_ASSERT(result.overflowed());
  });

  rc::prop("int_min - positive always overflows", []() {
    constexpr int32_t min_value = std::numeric_limits<int32_t>::min();
    auto positive = *rc::gen::inRange<int32_t>(1, 1000000);
    nix::checked::checked_t<int32_t> checked_min{min_value};

    auto result = checked_min - positive;
    RC_ASSERT(result.overflowed());
  });

  rc::prop("division by zero is always detected", []() {
    auto x = *rc::gen::arbitrary<int32_t>();
    nix::checked::checked_t<int32_t> checked_x{x};

    auto result = checked_x / 0;
    RC_ASSERT(result.divideByZero());
    RC_ASSERT(!result.valueChecked().has_value());
  });

  rc::prop("self division equals one (except for zero)", []() {
    auto x = *rc::gen::arbitrary<int32_t>();
    RC_PRE(x != 0); // Precondition: x is not zero
    nix::checked::checked_t<int32_t> checked_x{x};

    auto result = checked_x / x;
    RC_ASSERT(!result.overflowed());
    RC_ASSERT(!result.divideByZero());
    RC_ASSERT(result.valueChecked().value() == 1);
  });

  rc::prop("wrapping value is accessible on overflow (not div by zero)", []() {
    constexpr int32_t max_value = std::numeric_limits<int32_t>::max();
    auto positive = *rc::gen::inRange<int32_t>(1, 1000000);
    nix::checked::checked_t<int32_t> checked_max{max_value};

    auto result = checked_max + positive;
    // Should not throw - overflow allows wrapped access
    auto wrapped = result.valueWrapping();
    // The wrapped value should be negative (wrapped around)
    RC_ASSERT(wrapped < 0);
  });
}

TEST_CASE("checked unsigned arithmetic properties", "[checked][property][unsigned]") {
  rc::prop("uint32 addition identity: x + 0 == x", []() {
    auto x = *rc::gen::arbitrary<uint32_t>();
    nix::checked::checked_t<uint32_t> checked_x{x};

    auto result = checked_x + static_cast<uint32_t>(0);
    RC_ASSERT(!result.overflowed());
    RC_ASSERT(result.valueChecked().value() == x);
  });

  rc::prop("uint32_max + positive always overflows", []() {
    constexpr uint32_t max_value = std::numeric_limits<uint32_t>::max();
    auto positive = *rc::gen::inRange<uint32_t>(1, 1000000);
    nix::checked::checked_t<uint32_t> checked_max{max_value};

    auto result = checked_max + positive;
    RC_ASSERT(result.overflowed());
  });

  rc::prop("uint32 zero minus positive always overflows", []() {
    auto positive = *rc::gen::inRange<uint32_t>(1, 1000000);
    nix::checked::checked_t<uint32_t> zero{0};

    auto result = zero - positive;
    RC_ASSERT(result.overflowed());
  });
}
