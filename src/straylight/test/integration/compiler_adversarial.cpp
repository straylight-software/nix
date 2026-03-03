// straylight // nix-language // tests
//
// Adversarial tests - edge cases, boundary conditions, and malicious inputs
//
// These tests are designed to break things. If any of these fail, we have
// a security or correctness bug.

#include <cstdint>
#include <limits>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "straylight/nix/compiler/ast/expression.h"
#include "straylight/nix/compiler/ast/symbol_table.h"
#include "straylight/nix/compiler/compile/compiler.h"
#include "straylight/nix/compiler/compile/wasm_types.h"
#include "straylight/nix/compiler/parse/parser.h"
#include "straylight/nix/compiler/runtime/memory_layout.h"
#include "straylight/nix/compiler/runtime/runtime.h"
#include "straylight/nix/compiler/runtime/wasm_executor.h"

namespace mem = straylight::nix::compiler::memory_layout;
namespace ast = straylight::nix::compiler::ast;
namespace parse = straylight::nix::compiler::parse;
namespace compile = straylight::nix::compiler::compile;
namespace runtime = straylight::nix::compiler::runtime;

// =============================================================================
// Helper
// =============================================================================

namespace {
struct eval_result {
  bool success;
  runtime::nix_value value;
  std::string error;
};
} // namespace

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

auto expect_success(std::string_view source) -> eval_result {
  auto result = eval_nix(source);
  INFO("Source: " << source);
  INFO("Error: " << result.error);
  REQUIRE(result.success);
  return result;
}

auto expect_failure(std::string_view source) -> void {
  auto result = eval_nix(source);
  INFO("Source: " << source);
  REQUIRE_FALSE(result.success);
}

auto expect_parse_failure(std::string_view source) -> void {
  INFO("Source: " << source);
  try {
    ast::symbol_table symbols;
    parse::parse(source, symbols);
    FAIL("Expected parse failure but parsing succeeded");
  } catch (const std::exception&) {
    // expected
  }
}

// =============================================================================
// Integer boundary conditions
// =============================================================================

TEST_CASE("adversarial: integer overflow", "[adversarial][integer]") {
  // These should not crash - behavior may be wrap-around or error
  SECTION("max int32") {
    auto result = eval_nix("2147483647"); // INT32_MAX
    REQUIRE(result.success);
    REQUIRE(runtime::is_int(result.value));
    REQUIRE(runtime::get_int_value(result.value) == 2147483647);
  }

  SECTION("min int32") {
    auto result = eval_nix("-2147483648"); // INT32_MIN
    // Nix evaluates -2147483648 as -(2147483648) which may overflow
    // The important thing is we don't crash
    // (result may be success or overflow error depending on implementation)
  }

  SECTION("overflow addition") {
    auto result = eval_nix("2147483647 + 1");
    // May overflow - must not crash
  }

  SECTION("underflow subtraction") {
    auto result = eval_nix("-2147483648 - 1");
    // May underflow - must not crash
  }

  SECTION("overflow multiplication") {
    auto result = eval_nix("2147483647 * 2");
    // May overflow - must not crash
  }
}

// =============================================================================
// Division edge cases
// =============================================================================

TEST_CASE("adversarial: division by zero", "[adversarial][division]") {
  SECTION("integer division by zero") {
    expect_failure("1 / 0");
  }

  SECTION("zero divided by zero") {
    expect_failure("0 / 0");
  }

  SECTION("negative divided by zero") {
    expect_failure("-1 / 0");
  }

  SECTION("division by zero in expression") {
    expect_failure("let x = 0; in 42 / x");
  }

  SECTION("conditional division by zero - true branch not taken") {
    // Should succeed because we don't evaluate the division
    auto result = eval_nix("if true then 1 else 1 / 0");
    REQUIRE(result.success);
  }

  SECTION("conditional division by zero - false branch not taken") {
    auto result = eval_nix("if false then 1 / 0 else 1");
    REQUIRE(result.success);
  }
}

TEST_CASE("adversarial: INT_MIN / -1 overflow", "[adversarial][division]") {
  // This is the classic integer overflow case: -2147483648 / -1 = 2147483648
  // which doesn't fit in int32
  auto result = eval_nix("-2147483648 / -1");
  // Must not crash - can return wrapped value or error
}

// =============================================================================
// Deeply nested expressions
// =============================================================================

TEST_CASE("adversarial: deep nesting", "[adversarial][nesting]") {
  SECTION("deeply nested parentheses") {
    std::string expr = "1";
    for (int idx = 0; idx < 100; ++idx) {
      expr = "(" + expr + ")";
    }
    auto result = eval_nix(expr);
    REQUIRE(result.success);
    REQUIRE(runtime::is_int(result.value));
    REQUIRE(runtime::get_int_value(result.value) == 1);
  }

  SECTION("deeply nested let expressions") {
    std::string expr = "1";
    for (int idx = 0; idx < 50; ++idx) {
      expr = "let x" + std::to_string(idx) + " = " + std::to_string(idx) + "; in " + expr;
    }
    auto result = eval_nix(expr);
    REQUIRE(result.success);
  }

  SECTION("deeply nested if expressions") {
    std::string expr = "42";
    for (int idx = 0; idx < 50; ++idx) {
      expr = "if true then " + expr + " else 0";
    }
    auto result = eval_nix(expr);
    REQUIRE(result.success);
    REQUIRE(runtime::get_int_value(result.value) == 42);
  }

  SECTION("deeply nested function application") {
    std::string expr = "1";
    for (int idx = 0; idx < 30; ++idx) {
      expr = "(x: x + 1) (" + expr + ")";
    }
    auto result = eval_nix(expr);
    REQUIRE(result.success);
    REQUIRE(runtime::get_int_value(result.value) == 31);
  }

  SECTION("deeply nested attrset selection") {
    // { a = { a = { a = ... } } }.a.a.a...
    std::string inner = "1";
    for (int idx = 0; idx < 20; ++idx) {
      inner = "{ a = " + inner + "; }";
    }
    std::string select = "";
    for (int idx = 0; idx < 20; ++idx) {
      select += ".a";
    }
    auto result = eval_nix(inner + select);
    REQUIRE(result.success);
    REQUIRE(runtime::get_int_value(result.value) == 1);
  }
}

// =============================================================================
// String edge cases
// =============================================================================

TEST_CASE("adversarial: string edge cases", "[adversarial][string]") {
  SECTION("empty string") {
    auto result = expect_success("\"\"");
    REQUIRE(runtime::is_string(result.value));
  }

  SECTION("string with null byte") {
    // Nix strings can contain null bytes
    auto result = eval_nix("\"hello\\x00world\"");
    // Should parse and evaluate (may or may not support \x escape)
  }

  SECTION("string with all escape sequences") {
    auto result = expect_success("\"\\n\\r\\t\\\\\\\"\"");
    REQUIRE(runtime::is_string(result.value));
  }

  SECTION("very long string") {
    std::string long_str = "\"" + std::string(10000, 'x') + "\"";
    auto result = eval_nix(long_str);
    REQUIRE(result.success);
    REQUIRE(runtime::is_string(result.value));
  }

  SECTION("unicode in string") {
    auto result = expect_success("\"hello\"");
    REQUIRE(runtime::is_string(result.value));
  }

  SECTION("multiline string") {
    auto result = expect_success("''\n  hello\n  world\n''");
    REQUIRE(runtime::is_string(result.value));
  }

  SECTION("empty multiline string") {
    auto result = expect_success("''''");
    REQUIRE(runtime::is_string(result.value));
  }
}

// =============================================================================
// List edge cases
// =============================================================================

TEST_CASE("adversarial: list edge cases", "[adversarial][list]") {
  SECTION("empty list") {
    auto result = expect_success("[]");
    REQUIRE(runtime::is_list(result.value));
  }

  SECTION("single element list") {
    auto result = expect_success("[ 1 ]");
    REQUIRE(runtime::is_list(result.value));
  }

  SECTION("large list") {
    std::string list = "[";
    for (int idx = 0; idx < 1000; ++idx) {
      list += " " + std::to_string(idx);
    }
    list += " ]";
    auto result = eval_nix(list);
    REQUIRE(result.success);
    REQUIRE(runtime::is_list(result.value));
  }

  SECTION("nested lists") {
    auto result = expect_success("[ [ [ [ 1 ] ] ] ]");
    REQUIRE(runtime::is_list(result.value));
  }

  SECTION("list concatenation") {
    auto result = expect_success("[ 1 2 ] ++ [ 3 4 ]");
    REQUIRE(runtime::is_list(result.value));
  }

  SECTION("empty list concatenation") {
    auto result = expect_success("[] ++ []");
    REQUIRE(runtime::is_list(result.value));
  }
}

// =============================================================================
// Attrset edge cases
// =============================================================================

TEST_CASE("adversarial: attrset edge cases", "[adversarial][attrset]") {
  SECTION("empty attrset") {
    auto result = expect_success("{}");
    REQUIRE(runtime::is_attrset(result.value));
  }

  SECTION("single attribute") {
    auto result = expect_success("{ x = 1; }");
    REQUIRE(runtime::is_attrset(result.value));
  }

  SECTION("many attributes") {
    std::string attrs = "{";
    for (int idx = 0; idx < 100; ++idx) {
      attrs += " attr" + std::to_string(idx) + " = " + std::to_string(idx) + ";";
    }
    attrs += " }";
    auto result = eval_nix(attrs);
    REQUIRE(result.success);
    REQUIRE(runtime::is_attrset(result.value));
  }

  SECTION("deeply nested attrset") {
    auto result = expect_success("{ a = { b = { c = { d = 1; }; }; }; }");
    REQUIRE(runtime::is_attrset(result.value));
  }

  SECTION("select from empty attrset - error") {
    expect_failure("{}.x");
  }

  SECTION("select non-existent attribute - error") {
    expect_failure("{ a = 1; }.b");
  }

  SECTION("has attribute on empty set") {
    auto result = expect_success("{} ? x");
    REQUIRE(runtime::is_bool(result.value));
    REQUIRE(runtime::get_bool_value(result.value) == false);
  }

  SECTION("attrset update merge") {
    // Both attributes should be present
    auto result = expect_success("let r = { a = 1; } // { b = 2; }; in r.a + r.b");
    REQUIRE(runtime::is_int(result.value));
    REQUIRE(runtime::get_int_value(result.value) == 3);
  }

  SECTION("attrset update override") {
    auto result = expect_success("({ a = 1; } // { a = 2; }).a");
    REQUIRE(runtime::is_int(result.value));
    REQUIRE(runtime::get_int_value(result.value) == 2); // right side wins
  }

  SECTION("attrset update with empty left") {
    auto result = expect_success("({} // { a = 1; }).a");
    REQUIRE(runtime::get_int_value(result.value) == 1);
  }

  SECTION("attrset update with empty right") {
    auto result = expect_success("({ a = 1; } // {}).a");
    REQUIRE(runtime::get_int_value(result.value) == 1);
  }

  SECTION("deep equality - lists") {
    auto result = expect_success("[1 2 3] == [1 2 3]");
    REQUIRE(runtime::is_bool(result.value));
    REQUIRE(runtime::get_bool_value(result.value) == true);
  }

  SECTION("deep equality - lists different") {
    auto result = expect_success("[1 2 3] == [1 2 4]");
    REQUIRE(runtime::get_bool_value(result.value) == false);
  }

  SECTION("deep equality - lists different length") {
    auto result = expect_success("[1 2] == [1 2 3]");
    REQUIRE(runtime::get_bool_value(result.value) == false);
  }

  SECTION("deep equality - nested lists") {
    auto result = expect_success("[[1] [2]] == [[1] [2]]");
    REQUIRE(runtime::get_bool_value(result.value) == true);
  }

  SECTION("deep equality - attrsets") {
    auto result = expect_success("{ a = 1; b = 2; } == { a = 1; b = 2; }");
    REQUIRE(runtime::get_bool_value(result.value) == true);
  }

  SECTION("deep equality - attrsets different value") {
    auto result = expect_success("{ a = 1; } == { a = 2; }");
    REQUIRE(runtime::get_bool_value(result.value) == false);
  }

  SECTION("deep equality - attrsets different keys") {
    auto result = expect_success("{ a = 1; } == { b = 1; }");
    REQUIRE(runtime::get_bool_value(result.value) == false);
  }

  SECTION("deep equality - empty") {
    auto result = expect_success("{} == {}");
    REQUIRE(runtime::get_bool_value(result.value) == true);
  }

  SECTION("deep equality - empty lists") {
    auto result = expect_success("[] == []");
    REQUIRE(runtime::get_bool_value(result.value) == true);
  }

  SECTION("recursive attrset self-reference") {
    auto result = expect_success("rec { x = 1; y = x + 1; }.y");
    REQUIRE(runtime::is_int(result.value));
    REQUIRE(runtime::get_int_value(result.value) == 2);
  }
}

// =============================================================================
// Lambda edge cases
// =============================================================================

TEST_CASE("adversarial: lambda edge cases", "[adversarial][lambda]") {
  SECTION("identity function") {
    auto result = expect_success("(x: x) 42");
    REQUIRE(runtime::get_int_value(result.value) == 42);
  }

  SECTION("constant function") {
    auto result = expect_success("(x: 42) 0");
    REQUIRE(runtime::get_int_value(result.value) == 42);
  }

  SECTION("deeply curried function") {
    auto result = expect_success("(a: b: c: d: e: a + b + c + d + e) 1 2 3 4 5");
    REQUIRE(runtime::get_int_value(result.value) == 15);
  }

  SECTION("closure captures many variables") {
    auto result = expect_success("let a = 1; b = 2; c = 3; d = 4; e = 5; "
                                 "in (x: a + b + c + d + e + x) 10");
    REQUIRE(runtime::get_int_value(result.value) == 25);
  }

  SECTION("returning a lambda") {
    auto result = expect_success("((x: y: x + y) 1) 2");
    REQUIRE(runtime::get_int_value(result.value) == 3);
  }

  SECTION("lambda shadowing") {
    auto result = expect_success("let x = 1; in (x: x) 2");
    REQUIRE(runtime::get_int_value(result.value) == 2);
  }

  SECTION("attrset pattern") {
    auto result = expect_success("({ x, y }: x + y) { x = 1; y = 2; }");
    REQUIRE(runtime::get_int_value(result.value) == 3);
  }

  SECTION("attrset pattern with default") {
    auto result = expect_success("({ x, y ? 10 }: x + y) { x = 1; }");
    REQUIRE(runtime::get_int_value(result.value) == 11);
  }

  SECTION("attrset pattern with @") {
    auto result = expect_success("(args@{ x }: x + args.x) { x = 1; }");
    REQUIRE(runtime::get_int_value(result.value) == 2);
  }
}

// =============================================================================
// With expression edge cases
// =============================================================================

TEST_CASE("adversarial: with edge cases", "[adversarial][with]") {
  SECTION("with shadows nothing") {
    auto result = expect_success("with { x = 1; }; x");
    REQUIRE(runtime::get_int_value(result.value) == 1);
  }

  SECTION("let shadows with") {
    auto result = expect_success("let x = 1; in with { x = 2; }; x");
    // let should shadow with
    REQUIRE(runtime::get_int_value(result.value) == 1);
  }

  SECTION("nested with") {
    auto result = expect_success("with { x = 1; }; with { y = 2; }; x + y");
    REQUIRE(runtime::get_int_value(result.value) == 3);
  }

  SECTION("with on non-attrset - error") {
    expect_failure("with 1; x");
  }

  SECTION("with missing attribute - error") {
    expect_failure("with { a = 1; }; b");
  }
}

// =============================================================================
// Assertion edge cases
// =============================================================================

TEST_CASE("adversarial: assert edge cases", "[adversarial][assert]") {
  SECTION("assert true") {
    auto result = expect_success("assert true; 42");
    REQUIRE(runtime::get_int_value(result.value) == 42);
  }

  SECTION("assert false") {
    expect_failure("assert false; 42");
  }

  SECTION("assert with expression") {
    auto result = expect_success("assert 1 < 2; 42");
    REQUIRE(runtime::get_int_value(result.value) == 42);
  }

  SECTION("chained asserts") {
    auto result = expect_success("assert true; assert true; assert true; 42");
    REQUIRE(runtime::get_int_value(result.value) == 42);
  }

  SECTION("assert non-boolean - error") {
    expect_failure("assert 1; 42");
  }

  SECTION("assert null - error") {
    expect_failure("assert null; 42");
  }
}

// =============================================================================
// Lazy evaluation edge cases
// =============================================================================

TEST_CASE("adversarial: lazy evaluation", "[adversarial][lazy]") {
  SECTION("unused error doesn't crash") {
    // The error is in an unused branch
    auto result = expect_success("let x = 1 / 0; in 42");
    REQUIRE(runtime::get_int_value(result.value) == 42);
  }

  SECTION("unused assertion doesn't fail") {
    auto result = expect_success("let x = assert false; 1; in 42");
    REQUIRE(runtime::get_int_value(result.value) == 42);
  }

  SECTION("short-circuit and") {
    // false && (1/0) should not evaluate the division
    auto result = expect_success("false && (1 / 0 == 1)");
    REQUIRE(runtime::is_bool(result.value));
    REQUIRE(runtime::get_bool_value(result.value) == false);
  }

  SECTION("short-circuit or") {
    // true || (1/0) should not evaluate the division
    auto result = expect_success("true || (1 / 0 == 1)");
    REQUIRE(runtime::is_bool(result.value));
    REQUIRE(runtime::get_bool_value(result.value) == true);
  }

  SECTION("short-circuit implication") {
    // false -> (1/0) should not evaluate the division (false implies anything)
    auto result = expect_success("false -> (1 / 0 == 1)");
    REQUIRE(runtime::is_bool(result.value));
    REQUIRE(runtime::get_bool_value(result.value) == true);
  }

  SECTION("list elements are lazy") {
    // Error in list element shouldn't crash if not accessed
    auto result = expect_success("let xs = [ 1 (1/0) 3 ]; in 42");
    REQUIRE(runtime::get_int_value(result.value) == 42);
  }

  SECTION("attrset values are lazy") {
    auto result = expect_success("let s = { x = 1; y = 1/0; }; in s.x");
    REQUIRE(runtime::get_int_value(result.value) == 1);
  }
}

// =============================================================================
// Parse error cases
// =============================================================================

TEST_CASE("adversarial: parse errors", "[adversarial][parse]") {
  SECTION("empty input") {
    expect_parse_failure("");
  }

  SECTION("unclosed string") {
    expect_parse_failure("\"hello");
  }

  SECTION("unclosed list") {
    expect_parse_failure("[ 1 2 3");
  }

  SECTION("unclosed attrset") {
    expect_parse_failure("{ x = 1;");
  }

  SECTION("unclosed parenthesis") {
    expect_parse_failure("(1 + 2");
  }

  SECTION("missing semicolon in let") {
    expect_parse_failure("let x = 1 in x");
  }

  SECTION("missing in in let") {
    expect_parse_failure("let x = 1; x");
  }

  // Note: "123abc" is actually valid Nix - it's parsed as "123 abc" (integer
  // applied to identifier). The real Nix parser accepts it and only fails
  // at evaluation when 'abc' is undefined.
  SECTION("invalid identifier starting with digit") {
    // True invalid identifiers must be tested differently - Nix allows
    // almost anything in identifiers. Let's test something that's actually
    // a syntax error: a bare backtick
    expect_parse_failure("`invalid");
  }

  SECTION("binary operator with no operand") {
    expect_parse_failure("1 +");
  }

  SECTION("double operator") {
    expect_parse_failure("1 + + 2");
  }

  SECTION("if without then") {
    expect_parse_failure("if true 1 else 2");
  }

  SECTION("if without else") {
    expect_parse_failure("if true then 1");
  }

  SECTION("lambda without body") {
    expect_parse_failure("x:");
  }
}

// =============================================================================
// Type error cases
// =============================================================================

TEST_CASE("adversarial: type errors", "[adversarial][type]") {
  SECTION("add int and string") {
    expect_failure("1 + \"hello\"");
  }

  SECTION("add int and list") {
    expect_failure("1 + []");
  }

  SECTION("add int and attrset") {
    expect_failure("1 + {}");
  }

  SECTION("add int and lambda") {
    expect_failure("1 + (x: x)");
  }

  SECTION("subtract strings") {
    expect_failure("\"a\" - \"b\"");
  }

  SECTION("multiply strings") {
    expect_failure("\"a\" * \"b\"");
  }

  SECTION("divide strings") {
    expect_failure("\"a\" / \"b\"");
  }

  SECTION("negate string") {
    expect_failure("-\"hello\"");
  }

  SECTION("not integer") {
    expect_failure("!1");
  }

  SECTION("not string") {
    expect_failure("!\"hello\"");
  }

  SECTION("call non-function") {
    expect_failure("1 2");
  }

  SECTION("call string") {
    expect_failure("\"hello\" 1");
  }

  SECTION("select from non-attrset") {
    expect_failure("1.x");
  }

  SECTION("select from list") {
    expect_failure("[1 2 3].x");
  }

  SECTION("concatenate list and non-list") {
    expect_failure("[ 1 ] ++ 2");
  }

  SECTION("update non-attrset") {
    expect_failure("1 // { x = 1; }");
  }
}

// =============================================================================
// Identifier edge cases
// =============================================================================

TEST_CASE("adversarial: identifier edge cases", "[adversarial][identifier]") {
  SECTION("single letter identifier") {
    auto result = expect_success("let x = 1; in x");
    REQUIRE(runtime::get_int_value(result.value) == 1);
  }

  SECTION("underscore identifier") {
    auto result = expect_success("let _ = 1; in _");
    REQUIRE(runtime::get_int_value(result.value) == 1);
  }

  SECTION("identifier with numbers") {
    auto result = expect_success("let x123 = 42; in x123");
    REQUIRE(runtime::get_int_value(result.value) == 42);
  }

  SECTION("identifier with hyphens") {
    auto result = expect_success("let my-var = 42; in my-var");
    REQUIRE(runtime::get_int_value(result.value) == 42);
  }

  SECTION("identifier with apostrophe") {
    auto result = expect_success("let x' = 42; in x'");
    REQUIRE(runtime::get_int_value(result.value) == 42);
  }

  SECTION("or as identifier (special case)") {
    // 'or' is a keyword but can also be an identifier in some contexts
    // This depends on Nix's grammar
  }

  SECTION("undefined variable - error") {
    expect_failure("undefined_variable");
  }
}

// =============================================================================
// Memory stress tests
// =============================================================================

TEST_CASE("adversarial: memory stress", "[adversarial][memory]") {
  SECTION("many strings") {
    std::string expr = "let ";
    for (int idx = 0; idx < 100; ++idx) {
      expr += "s" + std::to_string(idx) + " = \"string" + std::to_string(idx) + "\"; ";
    }
    expr += "in s99";
    auto result = eval_nix(expr);
    REQUIRE(result.success);
    REQUIRE(runtime::is_string(result.value));
  }

  SECTION("many attrsets") {
    std::string expr = "let ";
    for (int idx = 0; idx < 50; ++idx) {
      expr += "a" + std::to_string(idx) + " = { x = " + std::to_string(idx) + "; }; ";
    }
    expr += "in a49.x";
    auto result = eval_nix(expr);
    REQUIRE(result.success);
    REQUIRE(runtime::get_int_value(result.value) == 49);
  }

  SECTION("many closures") {
    std::string expr = "let ";
    for (int idx = 0; idx < 50; ++idx) {
      expr += "f" + std::to_string(idx) + " = x: x + " + std::to_string(idx) + "; ";
    }
    expr += "in f49 1";
    auto result = eval_nix(expr);
    REQUIRE(result.success);
    REQUIRE(runtime::get_int_value(result.value) == 50);
  }
}
