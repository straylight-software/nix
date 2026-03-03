// straylight::nix::adapters::tests
//
// Tests for the eval_adapter - verifies straylight evaluator works through
// the adapter interface.

#include <straylight/nix/adapters/eval_adapter.h>

#include <catch2/catch_test_macros.hpp>

using straylight::nix::adapters::eval_adapter;
using straylight::nix::adapters::eval_adapter_error;
using straylight::nix::adapters::make_eval_adapter;

// ─────────────────────────────────────────────────────────────────────────────
// Basic evaluation tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("eval_adapter construction", "[adapter][eval]") {
  SECTION("standalone construction") {
    auto adapter = make_eval_adapter();
    REQUIRE(adapter != nullptr);
    REQUIRE_FALSE(adapter->has_store());
  }
}

TEST_CASE("eval_adapter simple integer evaluation", "[adapter][eval]") {
  auto adapter = make_eval_adapter();

  SECTION("literal integer") {
    auto result = adapter->eval_string("42");
    REQUIRE(result.has_value());
    REQUIRE(*result == "42");
  }

  SECTION("integer addition") {
    auto result = adapter->eval_string("1 + 2");
    REQUIRE(result.has_value());
    REQUIRE(*result == "3");
  }

  SECTION("integer multiplication") {
    auto result = adapter->eval_string("6 * 7");
    REQUIRE(result.has_value());
    REQUIRE(*result == "42");
  }

  SECTION("complex arithmetic") {
    auto result = adapter->eval_string("(10 + 5) * 2 - 3");
    REQUIRE(result.has_value());
    REQUIRE(*result == "27");
  }

  SECTION("negative numbers") {
    auto result = adapter->eval_string("-5 + 10");
    REQUIRE(result.has_value());
    REQUIRE(*result == "5");
  }
}

TEST_CASE("eval_adapter raw integer evaluation", "[adapter][eval]") {
  auto adapter = make_eval_adapter();

  // Straylight encoding: (payload << 32) | tag
  // Tag 2 = integer, payload = integer value
  constexpr auto make_int = [](std::int64_t n) { return (n << 32) | 2; };

  SECTION("raw integer value") {
    auto result = adapter->eval_raw("42");
    REQUIRE(result.has_value());
    REQUIRE(*result == make_int(42));
  }

  SECTION("raw computed value") {
    auto result = adapter->eval_raw("1 + 2");
    REQUIRE(result.has_value());
    REQUIRE(*result == make_int(3));
  }
}

TEST_CASE("eval_adapter boolean evaluation", "[adapter][eval]") {
  auto adapter = make_eval_adapter();

  SECTION("true literal") {
    auto result = adapter->eval_string("true");
    REQUIRE(result.has_value());
    REQUIRE(*result == "true");
  }

  SECTION("false literal") {
    auto result = adapter->eval_string("false");
    REQUIRE(result.has_value());
    REQUIRE(*result == "false");
  }

  SECTION("boolean and") {
    auto result = adapter->eval_string("true && false");
    REQUIRE(result.has_value());
    REQUIRE(*result == "false");
  }

  SECTION("boolean or") {
    auto result = adapter->eval_string("true || false");
    REQUIRE(result.has_value());
    REQUIRE(*result == "true");
  }

  SECTION("comparison") {
    auto result = adapter->eval_string("1 < 2");
    REQUIRE(result.has_value());
    REQUIRE(*result == "true");
  }

  SECTION("equality") {
    auto result = adapter->eval_string("42 == 42");
    REQUIRE(result.has_value());
    REQUIRE(*result == "true");
  }
}

TEST_CASE("eval_adapter null evaluation", "[adapter][eval]") {
  auto adapter = make_eval_adapter();

  auto result = adapter->eval_string("null");
  REQUIRE(result.has_value());
  REQUIRE(*result == "null");
}

TEST_CASE("eval_adapter let expression", "[adapter][eval]") {
  auto adapter = make_eval_adapter();

  SECTION("simple let") {
    auto result = adapter->eval_string("let x = 5; in x");
    REQUIRE(result.has_value());
    REQUIRE(*result == "5");
  }

  SECTION("let with computation") {
    auto result = adapter->eval_string("let x = 5; y = 10; in x + y");
    REQUIRE(result.has_value());
    REQUIRE(*result == "15");
  }

  SECTION("nested let") {
    auto result = adapter->eval_string("let x = 5; in let y = x * 2; in y");
    REQUIRE(result.has_value());
    REQUIRE(*result == "10");
  }
}

TEST_CASE("eval_adapter if-then-else", "[adapter][eval]") {
  auto adapter = make_eval_adapter();

  SECTION("if true") {
    auto result = adapter->eval_string("if true then 1 else 2");
    REQUIRE(result.has_value());
    REQUIRE(*result == "1");
  }

  SECTION("if false") {
    auto result = adapter->eval_string("if false then 1 else 2");
    REQUIRE(result.has_value());
    REQUIRE(*result == "2");
  }

  SECTION("if with comparison") {
    auto result = adapter->eval_string("if 5 > 3 then 100 else 0");
    REQUIRE(result.has_value());
    REQUIRE(*result == "100");
  }
}

TEST_CASE("eval_adapter lambda evaluation", "[adapter][eval]") {
  auto adapter = make_eval_adapter();

  SECTION("simple lambda application") {
    auto result = adapter->eval_string("(x: x + 1) 5");
    REQUIRE(result.has_value());
    REQUIRE(*result == "6");
  }

  SECTION("lambda with multiple args") {
    auto result = adapter->eval_string("(x: y: x + y) 3 4");
    REQUIRE(result.has_value());
    REQUIRE(*result == "7");
  }
}

TEST_CASE("eval_adapter error handling", "[adapter][eval]") {
  auto adapter = make_eval_adapter();

  SECTION("parse error - unclosed paren") {
    auto result = adapter->eval_string("(1 + 2");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == eval_adapter_error::parse_error);
  }

  SECTION("parse error - invalid syntax") {
    auto result = adapter->eval_string("let in");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == eval_adapter_error::parse_error);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// String evaluation tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("eval_adapter string evaluation", "[adapter][eval]") {
  auto adapter = make_eval_adapter();

  SECTION("simple string") {
    auto result = adapter->eval_string("\"hello\"");
    REQUIRE(result.has_value());
    REQUIRE(*result == "\"hello\"");
  }

  SECTION("string concatenation") {
    auto result = adapter->eval_string("\"hello\" + \" world\"");
    REQUIRE(result.has_value());
    REQUIRE(*result == "\"hello world\"");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Attribute set tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("eval_adapter attrset evaluation", "[adapter][eval]") {
  auto adapter = make_eval_adapter();

  SECTION("simple attrset access") {
    auto result = adapter->eval_string("{ x = 5; }.x");
    REQUIRE(result.has_value());
    REQUIRE(*result == "5");
  }

  SECTION("nested attrset access") {
    auto result = adapter->eval_string("{ a = { b = 10; }; }.a.b");
    REQUIRE(result.has_value());
    REQUIRE(*result == "10");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// List tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("eval_adapter list evaluation", "[adapter][eval]") {
  auto adapter = make_eval_adapter();

  SECTION("list element access via builtins.elemAt") {
    auto result = adapter->eval_string("builtins.elemAt [1 2 3] 0");
    REQUIRE(result.has_value());
    REQUIRE(*result == "1");
  }

  SECTION("list length") {
    auto result = adapter->eval_string("builtins.length [1 2 3 4 5]");
    REQUIRE(result.has_value());
    REQUIRE(*result == "5");
  }
}
