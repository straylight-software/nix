// straylight::nix::util::finally tests
//
// Tests for scope guard primitives: Finally, ScopeSuccess, ScopeFail

#include <functional>
#include <stdexcept>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "straylight/nix/util/finally.h"

namespace sg = straylight::nix::util;

// ─────────────────────────────────────────────────────────────────────────────
// Finally - basic functionality
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Finally runs action on scope exit", "[finally]") {
  bool called = false;
  {
    auto guard = sg::finally([&] { called = true; });
    REQUIRE_FALSE(called);
  }
  REQUIRE(called);
}

TEST_CASE("Finally can be constructed directly", "[finally]") {
  bool called = false;
  {
    sg::Finally guard([&] { called = true; });
    REQUIRE_FALSE(called);
  }
  REQUIRE(called);
}

TEST_CASE("Finally dismiss() prevents action", "[finally]") {
  bool called = false;
  {
    auto guard = sg::finally([&] { called = true; });
    guard.dismiss();
  }
  REQUIRE_FALSE(called);
}

TEST_CASE("Finally reset() changes action with std::function", "[finally]") {
  int value = 0;
  {
    sg::Finally<std::function<void()>> guard([&] { value = 1; });
    guard.reset([&] { value = 2; });
  }
  REQUIRE(value == 2);
}

TEST_CASE("Finally reset() after dismiss() sets new action", "[finally]") {
  int value = 0;
  {
    sg::Finally<std::function<void()>> guard([&] { value = 1; });
    guard.dismiss();
    guard.reset([&] { value = 3; });
  }
  REQUIRE(value == 3);
}

TEST_CASE("Finally active() reflects state", "[finally]") {
  auto guard = sg::finally([] {});
  REQUIRE(guard.active());
  guard.dismiss();
  REQUIRE_FALSE(guard.active());
}

TEST_CASE("Finally move construction transfers ownership", "[finally]") {
  bool called = false;
  {
    auto guard1 = sg::finally([&] { called = true; });
    auto guard2 = std::move(guard1);
    REQUIRE_FALSE(guard1.active());
    REQUIRE(guard2.active());
  }
  REQUIRE(called);
}

TEST_CASE("Finally move prevents double execution", "[finally]") {
  int count = 0;
  {
    auto guard1 = sg::finally([&] { ++count; });
    auto guard2 = std::move(guard1);
  }
  REQUIRE(count == 1);
}

TEST_CASE("Finally move assignment runs old action", "[finally]") {
  int value = 0;
  {
    // Use std::function so both guards have the same type
    sg::Finally<std::function<void()>> guard1([&] { value += 1; });
    sg::Finally<std::function<void()>> guard2([&] { value += 10; });
    guard1 = std::move(guard2);
    REQUIRE(value == 1); // Old action from guard1 ran
  }
  REQUIRE(value == 11); // guard1 now has guard2's action
}

TEST_CASE("Finally runs action on exception", "[finally]") {
  bool called = false;
  try {
    auto guard = sg::finally([&] { called = true; });
    throw std::runtime_error("test");
  } catch (...) {
    // Ignore
  }
  REQUIRE(called);
}

TEST_CASE("Finally with stateful callable", "[finally]") {
  std::string result;
  {
    auto guard = sg::finally([&, s = std::string("hello")] { result = s; });
  }
  REQUIRE(result == "hello");
}

TEST_CASE("Finally with std::function", "[finally]") {
  bool called = false;
  {
    std::function<void()> fn = [&] { called = true; };
    auto guard = sg::finally(std::move(fn));
  }
  REQUIRE(called);
}

// ─────────────────────────────────────────────────────────────────────────────
// ScopeSuccess - runs only on normal exit
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ScopeSuccess runs on normal exit", "[scope_success]") {
  bool called = false;
  {
    auto guard = sg::scope_success([&] { called = true; });
    REQUIRE_FALSE(called);
  }
  REQUIRE(called);
}

TEST_CASE("ScopeSuccess can be constructed directly", "[scope_success]") {
  bool called = false;
  {
    sg::ScopeSuccess guard([&] { called = true; });
    REQUIRE_FALSE(called);
  }
  REQUIRE(called);
}

TEST_CASE("ScopeSuccess does NOT run on exception", "[scope_success]") {
  bool called = false;
  try {
    auto guard = sg::scope_success([&] { called = true; });
    throw std::runtime_error("test");
  } catch (...) {
    // Ignore
  }
  REQUIRE_FALSE(called);
}

TEST_CASE("ScopeSuccess dismiss() prevents action", "[scope_success]") {
  bool called = false;
  {
    auto guard = sg::scope_success([&] { called = true; });
    guard.dismiss();
  }
  REQUIRE_FALSE(called);
}

TEST_CASE("ScopeSuccess active() reflects state", "[scope_success]") {
  auto guard = sg::scope_success([] {});
  REQUIRE(guard.active());
  guard.dismiss();
  REQUIRE_FALSE(guard.active());
}

TEST_CASE("ScopeSuccess reset() changes action with std::function", "[scope_success]") {
  int value = 0;
  {
    sg::ScopeSuccess<std::function<void()>> guard([&] { value = 1; });
    guard.reset([&] { value = 2; });
  }
  REQUIRE(value == 2);
}

TEST_CASE("ScopeSuccess move construction", "[scope_success]") {
  bool called = false;
  {
    auto guard1 = sg::scope_success([&] { called = true; });
    auto guard2 = std::move(guard1);
    REQUIRE_FALSE(guard1.active());
    REQUIRE(guard2.active());
  }
  REQUIRE(called);
}

// ─────────────────────────────────────────────────────────────────────────────
// ScopeFail - runs only on exception
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ScopeFail does NOT run on normal exit", "[scope_fail]") {
  bool called = false;
  {
    auto guard = sg::scope_fail([&] { called = true; });
    REQUIRE_FALSE(called);
  }
  REQUIRE_FALSE(called);
}

TEST_CASE("ScopeFail can be constructed directly", "[scope_fail]") {
  bool called = false;
  {
    sg::ScopeFail guard([&] { called = true; });
    REQUIRE_FALSE(called);
  }
  REQUIRE_FALSE(called);
}

TEST_CASE("ScopeFail runs on exception", "[scope_fail]") {
  bool called = false;
  try {
    auto guard = sg::scope_fail([&] { called = true; });
    throw std::runtime_error("test");
  } catch (...) {
    // Ignore
  }
  REQUIRE(called);
}

TEST_CASE("ScopeFail dismiss() prevents action", "[scope_fail]") {
  bool called = false;
  try {
    auto guard = sg::scope_fail([&] { called = true; });
    guard.dismiss();
    throw std::runtime_error("test");
  } catch (...) {
    // Ignore
  }
  REQUIRE_FALSE(called);
}

TEST_CASE("ScopeFail active() reflects state", "[scope_fail]") {
  auto guard = sg::scope_fail([] {});
  REQUIRE(guard.active());
  guard.dismiss();
  REQUIRE_FALSE(guard.active());
}

TEST_CASE("ScopeFail reset() changes action with std::function", "[scope_fail]") {
  int value = 0;
  try {
    sg::ScopeFail<std::function<void()>> guard([&] { value = 1; });
    guard.reset([&] { value = 2; });
    throw std::runtime_error("test");
  } catch (...) {
    // Ignore
  }
  REQUIRE(value == 2);
}

TEST_CASE("ScopeFail move construction", "[scope_fail]") {
  bool called = false;
  try {
    auto guard1 = sg::scope_fail([&] { called = true; });
    auto guard2 = std::move(guard1);
    REQUIRE_FALSE(guard1.active());
    REQUIRE(guard2.active());
    throw std::runtime_error("test");
  } catch (...) {
    // Ignore
  }
  REQUIRE(called);
}

TEST_CASE("ScopeFail swallows exceptions in action", "[scope_fail]") {
  // This test verifies ScopeFail doesn't cause std::terminate
  // if the action throws during stack unwinding
  bool outer_caught = false;
  try {
    auto guard = sg::scope_fail([] { throw std::logic_error("inner"); });
    throw std::runtime_error("outer");
  } catch (const std::runtime_error&) {
    outer_caught = true;
  }
  REQUIRE(outer_caught);
}

// ─────────────────────────────────────────────────────────────────────────────
// Combined usage patterns
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Finally and ScopeSuccess together - normal exit", "[finally][scope_success]") {
  int finally_count = 0;
  int success_count = 0;
  {
    auto f = sg::finally([&] { ++finally_count; });
    auto s = sg::scope_success([&] { ++success_count; });
  }
  REQUIRE(finally_count == 1);
  REQUIRE(success_count == 1);
}

TEST_CASE("Finally and ScopeFail together - normal exit", "[finally][scope_fail]") {
  int finally_count = 0;
  int fail_count = 0;
  {
    auto f = sg::finally([&] { ++finally_count; });
    auto s = sg::scope_fail([&] { ++fail_count; });
  }
  REQUIRE(finally_count == 1);
  REQUIRE(fail_count == 0);
}

TEST_CASE("Finally and ScopeFail together - exception", "[finally][scope_fail]") {
  int finally_count = 0;
  int fail_count = 0;
  try {
    auto f = sg::finally([&] { ++finally_count; });
    auto s = sg::scope_fail([&] { ++fail_count; });
    throw std::runtime_error("test");
  } catch (...) {
    // Ignore
  }
  REQUIRE(finally_count == 1);
  REQUIRE(fail_count == 1);
}

TEST_CASE("All three guards together - normal exit", "[finally][scope_success][scope_fail]") {
  int finally_count = 0;
  int success_count = 0;
  int fail_count = 0;
  {
    auto f = sg::finally([&] { ++finally_count; });
    auto s = sg::scope_success([&] { ++success_count; });
    auto e = sg::scope_fail([&] { ++fail_count; });
  }
  REQUIRE(finally_count == 1);
  REQUIRE(success_count == 1);
  REQUIRE(fail_count == 0);
}

TEST_CASE("All three guards together - exception", "[finally][scope_success][scope_fail]") {
  int finally_count = 0;
  int success_count = 0;
  int fail_count = 0;
  try {
    auto f = sg::finally([&] { ++finally_count; });
    auto s = sg::scope_success([&] { ++success_count; });
    auto e = sg::scope_fail([&] { ++fail_count; });
    throw std::runtime_error("test");
  } catch (...) {
    // Ignore
  }
  REQUIRE(finally_count == 1);
  REQUIRE(success_count == 0);
  REQUIRE(fail_count == 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Nested exception scenarios
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Nested scopes with exceptions", "[finally][scope_success][scope_fail]") {
  int outer_finally = 0;
  int outer_success = 0;
  int inner_finally = 0;
  int inner_success = 0;

  {
    auto of = sg::finally([&] { ++outer_finally; });
    auto os = sg::scope_success([&] { ++outer_success; });

    try {
      auto inf = sg::finally([&] { ++inner_finally; });
      auto ins = sg::scope_success([&] { ++inner_success; });
      throw std::runtime_error("test");
    } catch (...) {
      // Caught, outer scope continues normally
    }
  }

  REQUIRE(outer_finally == 1);
  REQUIRE(outer_success == 1);
  REQUIRE(inner_finally == 1);
  REQUIRE(inner_success == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Finally with empty lambda", "[finally]") {
  {
    auto guard = sg::finally([] {});
  }
  // Just verifies no crash
  REQUIRE(true);
}

TEST_CASE("Multiple dismiss calls are safe", "[finally]") {
  bool called = false;
  {
    auto guard = sg::finally([&] { called = true; });
    guard.dismiss();
    guard.dismiss();
    guard.dismiss();
  }
  REQUIRE_FALSE(called);
}

TEST_CASE("Reset after multiple dismisses with std::function", "[finally]") {
  int value = 0;
  {
    sg::Finally<std::function<void()>> guard([&] { value = 1; });
    guard.dismiss();
    guard.dismiss();
    guard.reset([&] { value = 42; });
  }
  REQUIRE(value == 42);
}

TEST_CASE("Finally with capturing lambda owning resources", "[finally]") {
  auto ptr = std::make_unique<int>(42);
  int captured_value = 0;
  {
    auto guard = sg::finally([p = std::move(ptr), &captured_value] { captured_value = *p; });
    REQUIRE(ptr == nullptr); // Moved into lambda
  }
  REQUIRE(captured_value == 42);
}

// ─────────────────────────────────────────────────────────────────────────────
// noexcept correctness
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Finally noexcept properties", "[finally][noexcept]") {
  auto nothrow_fn = []() noexcept {};

  // Factory function with nothrow callable should be noexcept
  static_assert(noexcept(sg::finally(nothrow_fn)));

  // dismiss is always noexcept
  auto guard = sg::finally(nothrow_fn);
  static_assert(noexcept(guard.dismiss()));

  // active() is always noexcept
  static_assert(noexcept(guard.active()));
}

TEST_CASE("ScopeFail destructor is noexcept", "[scope_fail][noexcept]") {
  // ScopeFail destructor must be noexcept to avoid std::terminate during unwind
  static_assert(std::is_nothrow_destructible_v<sg::ScopeFail<void (*)() noexcept>>);
}
