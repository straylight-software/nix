// straylight // nix-language // tests
//
// Execution tests: compile Nix source to WASM and execute it
//
// These tests verify the complete pipeline with actual execution:
//   Nix source -> parse -> AST -> compile -> WASM -> execute -> verify result

#include <cstdint>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "nix-language/compile/compiler.hh"
#include "nix-language/compile/wasm_types.hh"
#include "nix-language/parse/parser.hh"
#include "nix-language/runtime/wasm_executor.hh"

using namespace nix::language;
using namespace nix::language::runtime;
using namespace nix::language::compile;

// =============================================================================
// Helper: compile and execute Nix source
// =============================================================================

struct eval_result {
  bool success;
  nix_value value;
  std::string error;
  std::string formatted; // formatted value for display
};

auto eval_nix(std::string_view source) -> eval_result {
  try {
    // Parse
    ast::symbol_table symbols;
    auto expr = parse::parse(source, symbols);

    // Compile
    compile::compiler comp(symbols);
    auto module = comp.compile(expr);

    if (!module.validate()) {
      return {false, 0, "WASM validation failed", ""};
    }

    // Execute
    wasm_executor executor;
    auto result = executor.execute(module.emit_binary());

    if (!result.success) {
      return {false, 0, result.error, ""};
    }

    return {true, result.value, "", executor.format_value(result.value)};

  } catch (const std::exception& e) {
    return {false, 0, e.what(), ""};
  }
}

/// helper to check integer result
auto expect_int(std::string_view source, std::int32_t expected) -> void {
  auto result = eval_nix(source);
  INFO("Source: " << source);
  INFO("Error: " << result.error);
  REQUIRE(result.success);
  REQUIRE(is_int(result.value));
  REQUIRE(get_int_value(result.value) == expected);
}

/// helper to check boolean result
auto expect_bool(std::string_view source, bool expected) -> void {
  auto result = eval_nix(source);
  INFO("Source: " << source);
  INFO("Error: " << result.error);
  REQUIRE(result.success);
  REQUIRE(is_bool(result.value));
  REQUIRE(get_bool_value(result.value) == expected);
}

/// helper to check null result
auto expect_null(std::string_view source) -> void {
  auto result = eval_nix(source);
  INFO("Source: " << source);
  INFO("Error: " << result.error);
  REQUIRE(result.success);
  REQUIRE(is_null(result.value));
}

/// helper to check string result
auto expect_string(std::string_view source, std::string_view expected) -> void {
  auto result = eval_nix(source);
  INFO("Source: " << source);
  INFO("Error: " << result.error);
  INFO("Formatted: " << result.formatted);
  REQUIRE(result.success);
  REQUIRE(is_string(result.value));
  // The formatted output includes quotes, so check the formatted value
  REQUIRE(result.formatted == "\"" + std::string(expected) + "\"");
}

// =============================================================================
// Integer Literals
// =============================================================================

TEST_CASE("exec: integer literals", "[execution]") {
  expect_int("0", 0);
  expect_int("1", 1);
  expect_int("42", 42);
  expect_int("123456", 123456);
}

TEST_CASE("exec: negative integers", "[execution]") {
  // Nix doesn't have negative literals, uses unary minus
  expect_int("-1", -1);
  expect_int("-42", -42);
}

// =============================================================================
// Boolean Literals
// =============================================================================

TEST_CASE("exec: boolean literals", "[execution]") {
  expect_bool("true", true);
  expect_bool("false", false);
}

// =============================================================================
// Null
// =============================================================================

TEST_CASE("exec: null literal", "[execution]") {
  expect_null("null");
}

// =============================================================================
// Arithmetic
// =============================================================================

TEST_CASE("exec: addition", "[execution]") {
  expect_int("1 + 2", 3);
  expect_int("10 + 20", 30);
  expect_int("0 + 0", 0);
}

TEST_CASE("exec: subtraction", "[execution]") {
  expect_int("5 - 3", 2);
  expect_int("10 - 20", -10);
  expect_int("0 - 0", 0);
}

TEST_CASE("exec: multiplication", "[execution]") {
  expect_int("3 * 4", 12);
  expect_int("7 * 6", 42);
  expect_int("0 * 100", 0);
}

TEST_CASE("exec: division", "[execution]") {
  expect_int("10 / 2", 5);
  expect_int("42 / 6", 7);
  expect_int("7 / 2", 3); // integer division
}

TEST_CASE("exec: nested arithmetic", "[execution]") {
  expect_int("(1 + 2) * 3", 9);
  expect_int("10 - 2 * 3", 4); // precedence: 10 - (2 * 3)
  expect_int("(10 - 2) * 3", 24);
}

TEST_CASE("exec: complex arithmetic", "[execution]") {
  expect_int("1 + 2 + 3 + 4 + 5", 15);
  expect_int("10 * 10 - 1", 99);
  expect_int("100 / 10 / 2", 5);
}

// =============================================================================
// Comparison
// =============================================================================

TEST_CASE("exec: less than", "[execution]") {
  expect_bool("1 < 2", true);
  expect_bool("2 < 1", false);
  expect_bool("1 < 1", false);
}

TEST_CASE("exec: less than or equal", "[execution]") {
  expect_bool("1 <= 2", true);
  expect_bool("2 <= 1", false);
  expect_bool("1 <= 1", true);
}

TEST_CASE("exec: greater than", "[execution]") {
  expect_bool("2 > 1", true);
  expect_bool("1 > 2", false);
  expect_bool("1 > 1", false);
}

TEST_CASE("exec: greater than or equal", "[execution]") {
  expect_bool("2 >= 1", true);
  expect_bool("1 >= 2", false);
  expect_bool("1 >= 1", true);
}

TEST_CASE("exec: equality", "[execution]") {
  expect_bool("1 == 1", true);
  expect_bool("1 == 2", false);
  expect_bool("true == true", true);
  expect_bool("true == false", false);
  expect_bool("null == null", true);
}

TEST_CASE("exec: inequality", "[execution]") {
  expect_bool("1 != 2", true);
  expect_bool("1 != 1", false);
  expect_bool("true != false", true);
}

// =============================================================================
// Boolean Operations
// =============================================================================

TEST_CASE("exec: boolean not", "[execution]") {
  expect_bool("!true", false);
  expect_bool("!false", true);
  expect_bool("!!true", true);
}

TEST_CASE("exec: boolean and", "[execution]") {
  expect_bool("true && true", true);
  expect_bool("true && false", false);
  expect_bool("false && true", false);
  expect_bool("false && false", false);
}

TEST_CASE("exec: boolean or", "[execution]") {
  expect_bool("true || true", true);
  expect_bool("true || false", true);
  expect_bool("false || true", true);
  expect_bool("false || false", false);
}

TEST_CASE("exec: boolean implication", "[execution]") {
  expect_bool("true -> true", true);
  expect_bool("true -> false", false);
  expect_bool("false -> true", true);
  expect_bool("false -> false", true);
}

// =============================================================================
// Conditionals
// =============================================================================

TEST_CASE("exec: if expression", "[execution]") {
  expect_int("if true then 1 else 2", 1);
  expect_int("if false then 1 else 2", 2);
  expect_int("if 1 < 2 then 10 else 20", 10);
  expect_int("if 2 < 1 then 10 else 20", 20);
}

TEST_CASE("exec: nested if", "[execution]") {
  expect_int("if true then (if false then 1 else 2) else 3", 2);
  expect_int("if false then 1 else (if true then 2 else 3)", 2);
}

// =============================================================================
// Let Expressions
// =============================================================================

TEST_CASE("exec: simple let", "[execution]") {
  expect_int("let x = 1; in x", 1);
  expect_int("let x = 42; in x", 42);
}

TEST_CASE("exec: let with arithmetic", "[execution]") {
  expect_int("let x = 1; y = 2; in x + y", 3);
  expect_int("let a = 10; b = 3; in a * b", 30);
}

TEST_CASE("exec: nested let", "[execution]") {
  expect_int("let x = 1; in let y = 2; in x + y", 3);
  expect_int("let x = 10; in let x = 20; in x", 20); // shadowing
}

TEST_CASE("exec: let referencing earlier bindings", "[execution]") {
  expect_int("let x = 1; y = x + 1; in y", 2);
  expect_int("let a = 5; b = a * 2; c = b + 1; in c", 11);
}

// =============================================================================
// Functions (Lambdas)
// =============================================================================

TEST_CASE("exec: identity function", "[execution]") {
  expect_int("(x: x) 42", 42);
  expect_bool("(x: x) true", true);
}

TEST_CASE("exec: arithmetic in lambda", "[execution]") {
  expect_int("(x: x + 1) 5", 6);
  expect_int("(x: x * 2) 21", 42);
}

TEST_CASE("exec: lambda with multiple args (curried)", "[execution]") {
  expect_int("(x: y: x + y) 1 2", 3);
  expect_int("(a: b: c: a * b + c) 2 3 4", 10);
}

TEST_CASE("exec: lambda in let", "[execution]") {
  expect_int("let f = x: x + 1; in f 5", 6);
  expect_int("let double = x: x * 2; in double 21", 42);
}

TEST_CASE("exec: closure captures variable", "[execution]") {
  expect_int("let x = 10; in (y: x + y) 5", 15);
  expect_int("let a = 2; b = 3; in (x: a * x + b) 4", 11);
}

// =============================================================================
// Lists
// =============================================================================

TEST_CASE("exec: empty list", "[execution]") {
  auto result = eval_nix("[]");
  INFO("Error: " << result.error);
  REQUIRE(result.success);
  REQUIRE(is_list(result.value));
  REQUIRE(get_payload(result.value) == 0); // count = 0
}

TEST_CASE("exec: simple list", "[execution]") {
  auto result = eval_nix("[1 2 3]");
  INFO("Error: " << result.error);
  REQUIRE(result.success);
  REQUIRE(is_list(result.value));
  // For now just check it's a list with some elements
  // (full element access requires more runtime support)
}

// =============================================================================
// Attribute Sets
// =============================================================================

TEST_CASE("exec: empty attrset", "[execution]") {
  auto result = eval_nix("{}");
  INFO("Error: " << result.error);
  REQUIRE(result.success);
  REQUIRE(is_attrset(result.value));
  REQUIRE(get_payload(result.value) == 0); // count = 0
}

TEST_CASE("exec: simple attrset", "[execution]") {
  auto result = eval_nix("{ x = 1; y = 2; }");
  INFO("Error: " << result.error);
  REQUIRE(result.success);
  REQUIRE(is_attrset(result.value));
}

TEST_CASE("exec: attrset selection", "[execution]") {
  expect_int("{ x = 42; }.x", 42);
  expect_int("{ a = 1; b = 2; }.b", 2);
}

TEST_CASE("exec: nested attrset selection", "[execution]") {
  expect_int("{ a = { b = 42; }; }.a.b", 42);
}

TEST_CASE("exec: multi-segment path merging", "[execution]") {
  expect_int("{ a.b = 1; a.c = 2; }.a.b", 1);
  expect_int("{ a.b = 1; a.c = 2; }.a.c", 2);
}

// =============================================================================
// Recursive Bindings
// =============================================================================

TEST_CASE("exec: recursive attrset", "[execution]") {
  expect_int("rec { x = 1; y = x + 1; }.y", 2);
  expect_int("rec { a = 5; b = a * 2; }.b", 10);
}

// =============================================================================
// Inherit
// =============================================================================

TEST_CASE("exec: inherit in attrset", "[execution]") {
  expect_int("let x = 42; in { inherit x; }.x", 42);
}

TEST_CASE("exec: inherit from expression", "[execution]") {
  expect_int("{ inherit ({ x = 42; }) x; }.x", 42);
}

// =============================================================================
// With Expression
// =============================================================================

TEST_CASE("exec: with expression", "[execution]") {
  expect_int("with { x = 42; }; x", 42);
  expect_int("with { a = 1; b = 2; }; a + b", 3);
}

TEST_CASE("exec: with shadowing", "[execution]") {
  // let bindings should shadow with
  expect_int("let x = 10; in with { x = 20; }; x", 10);
}

// =============================================================================
// Assert
// =============================================================================

TEST_CASE("exec: assert succeeds", "[execution]") {
  expect_int("assert true; 42", 42);
  expect_int("assert 1 < 2; 100", 100);
}

TEST_CASE("exec: assert fails", "[execution]") {
  auto result = eval_nix("assert false; 42");
  REQUIRE_FALSE(result.success);
  REQUIRE(result.error.find("assertion") != std::string::npos);
}

// =============================================================================
// Complex Expressions
// =============================================================================

TEST_CASE("exec: fibonacci-like computation", "[execution]") {
  // Can't do true recursion without builtins.fix, but can test the structure
  expect_int("let a = 1; b = 1; c = a + b; d = b + c; e = c + d; in e", 5);
}

TEST_CASE("exec: function composition", "[execution]") {
  expect_int("let f = x: x + 1; g = x: x * 2; in f (g 5)", 11);
  expect_int("let f = x: x + 1; g = x: x * 2; in g (f 5)", 12);
}

TEST_CASE("exec: higher-order function", "[execution]") {
  expect_int("let apply = f: x: f x; in apply (x: x + 1) 5", 6);
  expect_int("let twice = f: x: f (f x); in twice (x: x + 1) 5", 7);
}

// Debug test to investigate let binding issues
TEST_CASE("debug: let ref earlier binding", "[debug]") {
  auto result = eval_nix("let x = 1; y = x + 1; in y");
  INFO("Success: " << result.success);
  INFO("Error: " << result.error);
  INFO("Value hex: 0x" << std::hex << result.value);
  INFO("Value tag: " << (result.value & 0xFFFFFFFF));
  INFO("Value payload: " << (result.value >> 32));
  INFO("Formatted: " << result.formatted);

  if (result.success) {
    INFO("is_int: " << is_int(result.value));
    INFO("is_thunk: " << is_thunk(result.value));
    INFO("is_null: " << is_null(result.value));
  }

  REQUIRE(result.success);
  REQUIRE(is_int(result.value));
}

// =============================================================================
// Builtins - Type Predicates
// =============================================================================

TEST_CASE("exec: builtins.isNull", "[execution][builtins]") {
  expect_bool("builtins.isNull null", true);
  expect_bool("builtins.isNull 1", false);
  expect_bool("builtins.isNull true", false);
  expect_bool("builtins.isNull []", false);
  expect_bool("builtins.isNull {}", false);
}

TEST_CASE("exec: builtins.isBool", "[execution][builtins]") {
  expect_bool("builtins.isBool true", true);
  expect_bool("builtins.isBool false", true);
  expect_bool("builtins.isBool null", false);
  expect_bool("builtins.isBool 1", false);
}

TEST_CASE("exec: builtins.isInt", "[execution][builtins]") {
  expect_bool("builtins.isInt 42", true);
  expect_bool("builtins.isInt 0", true);
  expect_bool("builtins.isInt (-5)", true);
  expect_bool("builtins.isInt null", false);
  expect_bool("builtins.isInt true", false);
}

TEST_CASE("exec: builtins.isString", "[execution][builtins]") {
  expect_bool("builtins.isString \"hello\"", true);
  expect_bool("builtins.isString \"\"", true);
  expect_bool("builtins.isString 42", false);
  expect_bool("builtins.isString null", false);
}

TEST_CASE("exec: builtins.isList", "[execution][builtins]") {
  expect_bool("builtins.isList []", true);
  expect_bool("builtins.isList [1 2 3]", true);
  expect_bool("builtins.isList 42", false);
  expect_bool("builtins.isList {}", false);
}

TEST_CASE("exec: builtins.isAttrs", "[execution][builtins]") {
  expect_bool("builtins.isAttrs {}", true);
  expect_bool("builtins.isAttrs { x = 1; }", true);
  expect_bool("builtins.isAttrs []", false);
  expect_bool("builtins.isAttrs 42", false);
}

TEST_CASE("exec: builtins.isFunction", "[execution][builtins]") {
  expect_bool("builtins.isFunction (x: x)", true);
  expect_bool("builtins.isFunction builtins.isFunction", true);
  expect_bool("builtins.isFunction 42", false);
  expect_bool("builtins.isFunction null", false);
}

// =============================================================================
// Builtins - List Operations
// =============================================================================

TEST_CASE("exec: builtins.length", "[execution][builtins]") {
  expect_int("builtins.length []", 0);
  expect_int("builtins.length [1]", 1);
  expect_int("builtins.length [1 2 3]", 3);
  expect_int("builtins.length [1 2 3 4 5]", 5);
}

TEST_CASE("exec: builtins.head", "[execution][builtins]") {
  expect_int("builtins.head [42]", 42);
  expect_int("builtins.head [1 2 3]", 1);
  expect_int("builtins.head [99 88 77]", 99);
}

TEST_CASE("exec: builtins.tail", "[execution][builtins]") {
  expect_int("builtins.length (builtins.tail [1 2 3])", 2);
  expect_int("builtins.head (builtins.tail [1 2 3])", 2);
  expect_int("builtins.length (builtins.tail [1])", 0);
}

TEST_CASE("exec: builtins.elemAt", "[execution][builtins]") {
  expect_int("builtins.elemAt [10 20 30] 0", 10);
  expect_int("builtins.elemAt [10 20 30] 1", 20);
  expect_int("builtins.elemAt [10 20 30] 2", 30);
}

TEST_CASE("exec: builtins.elem", "[execution][builtins]") {
  expect_bool("builtins.elem 2 [1 2 3]", true);
  expect_bool("builtins.elem 5 [1 2 3]", false);
  expect_bool("builtins.elem 1 []", false);
}

// =============================================================================
// Builtins - Attrset Operations
// =============================================================================

TEST_CASE("exec: builtins.attrNames", "[execution][builtins]") {
  expect_int("builtins.length (builtins.attrNames {})", 0);
  expect_int("builtins.length (builtins.attrNames { a = 1; })", 1);
  expect_int("builtins.length (builtins.attrNames { a = 1; b = 2; c = 3; })", 3);
}

TEST_CASE("exec: builtins.attrValues", "[execution][builtins]") {
  expect_int("builtins.length (builtins.attrValues {})", 0);
  expect_int("builtins.length (builtins.attrValues { a = 1; })", 1);
  // Sum of values in sorted order: a=1, b=2, c=3
  expect_int("builtins.head (builtins.attrValues { z = 99; a = 1; })", 1); // sorted by key: a first
}

// =============================================================================
// Builtins - String Operations
// =============================================================================

TEST_CASE("exec: builtins.stringLength", "[execution][builtins]") {
  expect_int("builtins.stringLength \"\"", 0);
  expect_int("builtins.stringLength \"hello\"", 5);
  expect_int("builtins.stringLength \"hello world\"", 11);
}

TEST_CASE("exec: builtins.typeOf", "[execution][builtins]") {
  // Test that typeOf returns strings - check by using string operations
  expect_int("builtins.stringLength (builtins.typeOf null)", 4); // "null"
  expect_int("builtins.stringLength (builtins.typeOf true)", 4); // "bool"
  expect_int("builtins.stringLength (builtins.typeOf 42)", 3);   // "int"
  expect_int("builtins.stringLength (builtins.typeOf [])", 4);   // "list"
  expect_int("builtins.stringLength (builtins.typeOf {})", 3);   // "set"
}

// =============================================================================
// Builtins - Combined Operations
// =============================================================================

TEST_CASE("exec: builtins composition", "[execution][builtins]") {
  // head of tail
  expect_int("builtins.head (builtins.tail [1 2 3])", 2);
  // length of attrNames
  expect_int("builtins.length (builtins.attrNames { a = 1; b = 2; })", 2);
  // nested type check
  expect_bool("builtins.isList (builtins.tail [1 2 3])", true);
  expect_bool("builtins.isList (builtins.attrNames { a = 1; })", true);
}

TEST_CASE("exec: builtins with let", "[execution][builtins]") {
  expect_int("let xs = [1 2 3]; in builtins.length xs", 3);
  expect_int("let xs = [10 20 30]; in builtins.elemAt xs 1", 20);
  expect_bool("let f = x: x + 1; in builtins.isFunction f", true);
}

// =============================================================================
// Builtins - Higher-Order Functions
// =============================================================================

TEST_CASE("exec: builtins.map", "[execution][builtins]") {
  // map increment over list
  expect_int("builtins.head (builtins.map (x: x + 1) [10 20 30])", 11);
  // second element
  expect_int("builtins.elemAt (builtins.map (x: x * 2) [5 6 7]) 1", 12);
  // length preserved
  expect_int("builtins.length (builtins.map (x: x) [1 2 3 4 5])", 5);
  // empty list
  expect_int("builtins.length (builtins.map (x: x + 1) [])", 0);
}

TEST_CASE("exec: builtins.filter", "[execution][builtins]") {
  // filter evens (x % 2 == 0 means even, but we don't have mod, so use comparison)
  // filter positive
  expect_int("builtins.length (builtins.filter (x: x > 0) [1 2 3])", 3);
  expect_int("builtins.length (builtins.filter (x: x > 2) [1 2 3 4 5])", 3);
  // filter all
  expect_int("builtins.length (builtins.filter (x: true) [1 2 3])", 3);
  // filter none
  expect_int("builtins.length (builtins.filter (x: false) [1 2 3])", 0);
  // get first matching element
  expect_int("builtins.head (builtins.filter (x: x > 2) [1 2 3 4 5])", 3);
}

TEST_CASE("exec: builtins.foldl'", "[execution][builtins]") {
  // sum [1 2 3 4 5] = 15
  expect_int("builtins.foldl' (a: b: a + b) 0 [1 2 3 4 5]", 15);
  // product [1 2 3 4] = 24
  expect_int("builtins.foldl' (a: b: a * b) 1 [1 2 3 4]", 24);
  // empty list returns init
  expect_int("builtins.foldl' (a: b: a + b) 42 []", 42);
  // single element
  expect_int("builtins.foldl' (a: b: a + b) 10 [5]", 15);
}

TEST_CASE("exec: builtins.genList", "[execution][builtins]") {
  // genList (x: x) 5 = [0 1 2 3 4]
  expect_int("builtins.length (builtins.genList (x: x) 5)", 5);
  expect_int("builtins.head (builtins.genList (x: x) 5)", 0);
  expect_int("builtins.elemAt (builtins.genList (x: x) 5) 4", 4);
  // genList (x: x * 2) 3 = [0 2 4]
  expect_int("builtins.elemAt (builtins.genList (x: x * 2) 3) 2", 4);
  // empty
  expect_int("builtins.length (builtins.genList (x: x) 0)", 0);
}

TEST_CASE("exec: builtins.concatLists", "[execution][builtins]") {
  // concatLists [[1 2] [3 4]] = [1 2 3 4]
  expect_int("builtins.length (builtins.concatLists [[1 2] [3 4]])", 4);
  expect_int("builtins.head (builtins.concatLists [[10] [20]])", 10);
  expect_int("builtins.elemAt (builtins.concatLists [[10] [20]]) 1", 20);
  // empty lists
  expect_int("builtins.length (builtins.concatLists [])", 0);
  expect_int("builtins.length (builtins.concatLists [[] []])", 0);
  expect_int("builtins.length (builtins.concatLists [[1] [] [2]])", 2);
}

TEST_CASE("exec: higher-order composition", "[execution][builtins]") {
  // map then filter
  expect_int("builtins.head (builtins.filter (x: x > 10) (builtins.map (x: x * 2) [3 5 7]))", 14);
  // filter then map
  expect_int("builtins.head (builtins.map (x: x + 100) (builtins.filter (x: x > 2) [1 2 3]))", 103);
  // foldl' after map
  expect_int("builtins.foldl' (a: b: a + b) 0 (builtins.map (x: x * 2) [1 2 3])", 12);
  // concatLists with genList
  expect_int("builtins.length (builtins.concatLists (builtins.genList (i: [i]) 3))", 3);
}

// =============================================================================
// Error Handling Builtins
// =============================================================================

TEST_CASE("exec: builtins.throw", "[execution][builtins]") {
  auto result = eval_nix(R"(builtins.throw "test error message")");
  REQUIRE_FALSE(result.success);
  REQUIRE(result.error.find("test error message") != std::string::npos);
}

TEST_CASE("exec: builtins.abort", "[execution][builtins]") {
  auto result = eval_nix(R"(builtins.abort "fatal error")");
  REQUIRE_FALSE(result.success);
  REQUIRE(result.error.find("fatal error") != std::string::npos);
  REQUIRE(result.error.find("abort") != std::string::npos);
}

TEST_CASE("exec: builtins.tryEval success", "[execution][builtins]") {
  // tryEval on a successful evaluation
  expect_bool("(builtins.tryEval (1 + 1)).success", true);
  expect_int("(builtins.tryEval 42).value", 42);
  expect_int("(builtins.tryEval (2 * 3)).value", 6);
}

TEST_CASE("exec: builtins.tryEval failure", "[execution][builtins]") {
  // tryEval catching a throw
  expect_bool(R"((builtins.tryEval (builtins.throw "error")).success)", false);
  // value should be false on failure
  expect_bool(R"((builtins.tryEval (builtins.throw "error")).value)", false);
}

TEST_CASE("exec: builtins.tryEval with abort", "[execution][builtins]") {
  // abort should NOT be caught by tryEval
  auto result = eval_nix(R"(builtins.tryEval (builtins.abort "fatal"))");
  REQUIRE_FALSE(result.success);
  REQUIRE(result.error.find("abort") != std::string::npos);
}

TEST_CASE("exec: builtins.seq", "[execution][builtins]") {
  // seq forces first arg, returns second
  expect_int("builtins.seq 1 42", 42);
  expect_int("builtins.seq (1 + 1) 100", 100);
  expect_bool("builtins.seq true false", false);
  // seq should force and propagate errors from first arg
  auto result = eval_nix(R"(builtins.seq (builtins.throw "err") 42)");
  REQUIRE_FALSE(result.success);
}

TEST_CASE("exec: builtins.deepSeq", "[execution][builtins]") {
  // deepSeq deeply forces first arg, returns second
  expect_int("builtins.deepSeq 1 42", 42);
  expect_int("builtins.deepSeq [1 2 3] 100", 100);
  expect_int("builtins.deepSeq { a = 1; b = 2; } 200", 200);
  // nested structures
  expect_int("builtins.deepSeq [[1 2] [3 4]] 300", 300);
  expect_int("builtins.deepSeq { a = { b = 1; }; } 400", 400);
}

TEST_CASE("exec: builtins.trace", "[execution][builtins]") {
  // trace prints to stderr and returns second arg
  expect_int(R"(builtins.trace "debug message" 42)", 42);
  expect_bool(R"(builtins.trace "testing" true)", true);
  // trace with computed first arg
  expect_int("builtins.trace (1 + 1) 100", 100);
}

// =============================================================================
// Attrset Builtins (hasAttr, getAttr, removeAttrs)
// =============================================================================

TEST_CASE("exec: builtins.hasAttr", "[execution][builtins]") {
  expect_bool(R"(builtins.hasAttr "x" { x = 1; })", true);
  expect_bool(R"(builtins.hasAttr "y" { x = 1; })", false);
  expect_bool(R"(builtins.hasAttr "a" {})", false);
  expect_bool(R"(builtins.hasAttr "foo" { foo = null; bar = 2; })", true);
}

TEST_CASE("exec: builtins.getAttr", "[execution][builtins]") {
  expect_int(R"(builtins.getAttr "x" { x = 42; })", 42);
  expect_int(R"(builtins.getAttr "b" { a = 1; b = 2; })", 2);
  expect_bool(R"(builtins.getAttr "flag" { flag = true; })", true);
  // getAttr on missing attribute should fail
  auto result = eval_nix(R"(builtins.getAttr "missing" { x = 1; })");
  REQUIRE_FALSE(result.success);
  REQUIRE(result.error.find("not found") != std::string::npos);
}

TEST_CASE("exec: builtins.removeAttrs", "[execution][builtins]") {
  // Remove single attribute
  expect_int(
      R"(builtins.length (builtins.attrNames (builtins.removeAttrs { a = 1; b = 2; } ["a"])))", 1);
  expect_int(R"((builtins.removeAttrs { a = 1; b = 2; } ["a"]).b)", 2);
  // Remove multiple attributes
  expect_int(
      R"(builtins.length (builtins.attrNames (builtins.removeAttrs { a = 1; b = 2; c = 3; } ["a" "c"])))",
      1);
  // Remove non-existent attribute (no-op)
  expect_int(R"(builtins.length (builtins.attrNames (builtins.removeAttrs { a = 1; } ["b"])))", 1);
  // Remove all attributes
  expect_int(R"(builtins.length (builtins.attrNames (builtins.removeAttrs { a = 1; } ["a"])))", 0);
  // Remove from empty set
  expect_int(R"(builtins.length (builtins.attrNames (builtins.removeAttrs {} ["a"])))", 0);
}

// =============================================================================
// String Builtins (substring)
// =============================================================================

TEST_CASE("exec: builtins.substring", "[execution][builtins]") {
  // Basic substring
  expect_string(R"(builtins.substring 0 3 "hello")", "hel");
  expect_string(R"(builtins.substring 2 3 "hello")", "llo");
  expect_string(R"(builtins.substring 0 5 "hello")", "hello");
  // Start beyond end
  expect_string(R"(builtins.substring 10 3 "hello")", "");
  // Length beyond end (clamps to available)
  expect_string(R"(builtins.substring 3 100 "hello")", "lo");
  // Negative length means rest of string
  expect_string(R"(builtins.substring 2 (-1) "hello")", "llo");
  // Zero length
  expect_string(R"(builtins.substring 0 0 "hello")", "");
  // Empty string
  expect_string(R"(builtins.substring 0 3 "")", "");
}

TEST_CASE("exec: builtins.sort", "[execution][builtins]") {
  // Sort integers ascending
  expect_int("builtins.head (builtins.sort (a: b: a < b) [3 1 2])", 1);
  expect_int("builtins.elemAt (builtins.sort (a: b: a < b) [3 1 2]) 1", 2);
  expect_int("builtins.elemAt (builtins.sort (a: b: a < b) [3 1 2]) 2", 3);
  // Sort integers descending
  expect_int("builtins.head (builtins.sort (a: b: a > b) [3 1 2])", 3);
  // Empty list
  expect_int("builtins.length (builtins.sort (a: b: a < b) [])", 0);
  // Single element
  expect_int("builtins.head (builtins.sort (a: b: a < b) [42])", 42);
  // Already sorted
  expect_int("builtins.head (builtins.sort (a: b: a < b) [1 2 3])", 1);
}

// =============================================================================
// More Builtins
// =============================================================================

TEST_CASE("exec: builtins.replaceStrings", "[execution][builtins]") {
  expect_string(R"(builtins.replaceStrings ["a"] ["b"] "banana")", "bbnbnb");
  expect_string(R"(builtins.replaceStrings ["o"] ["0"] "foo")", "f00");
  expect_string(R"(builtins.replaceStrings ["ll"] ["LL"] "hello")", "heLLo");
  expect_string(R"(builtins.replaceStrings [] [] "hello")", "hello");
  expect_string(R"(builtins.replaceStrings ["x"] ["y"] "hello")", "hello");
  // Multiple replacements - first match wins
  expect_string(R"(builtins.replaceStrings ["a" "b"] ["1" "2"] "ab")", "12");
}

TEST_CASE("exec: builtins.toString", "[execution][builtins]") {
  expect_string(R"(builtins.toString 42)", "42");
  expect_string(R"(builtins.toString (-5))", "-5");
  expect_string(R"(builtins.toString null)", "");
  // string passthrough
  expect_string(R"(builtins.toString "hello")", "hello");
}

TEST_CASE("exec: builtins.concatStrings", "[execution][builtins]") {
  expect_string(R"(builtins.concatStrings ["a" "b" "c"])", "abc");
  expect_string(R"(builtins.concatStrings ["hello" " " "world"])", "hello world");
  expect_string(R"(builtins.concatStrings [])", "");
  expect_string(R"(builtins.concatStrings ["single"])", "single");
}

TEST_CASE("exec: builtins.concatStringsSep", "[execution][builtins]") {
  expect_string(R"(builtins.concatStringsSep ", " ["a" "b" "c"])", "a, b, c");
  expect_string(R"(builtins.concatStringsSep "-" ["hello" "world"])", "hello-world");
  expect_string(R"(builtins.concatStringsSep ":" [])", "");
  expect_string(R"(builtins.concatStringsSep "," ["single"])", "single");
  expect_string(R"(builtins.concatStringsSep "" ["a" "b" "c"])", "abc");
  // Longer separator
  expect_string(R"(builtins.concatStringsSep " -> " ["a" "b" "c"])", "a -> b -> c");
}

TEST_CASE("exec: builtins.all", "[execution][builtins]") {
  expect_bool("builtins.all (x: x > 0) [1 2 3]", true);
  expect_bool("builtins.all (x: x > 0) [1 0 3]", false);
  expect_bool("builtins.all (x: x > 0) []", true); // vacuously true
}

TEST_CASE("exec: builtins.any", "[execution][builtins]") {
  expect_bool("builtins.any (x: x > 0) [(-1) 0 1]", true);
  expect_bool("builtins.any (x: x > 0) [(-1) 0 (-2)]", false);
  expect_bool("builtins.any (x: x > 0) []", false); // vacuously false
}

TEST_CASE("exec: builtins.concatMap", "[execution][builtins]") {
  expect_int("builtins.length (builtins.concatMap (x: [x x]) [1 2])", 4);
  expect_int("builtins.head (builtins.concatMap (x: [x x]) [1 2])", 1);
  expect_int("builtins.elemAt (builtins.concatMap (x: [x x]) [1 2]) 2", 2);
}

TEST_CASE("exec: builtins.listToAttrs", "[execution][builtins]") {
  expect_int(R"((builtins.listToAttrs [{name = "x"; value = 1;}]).x)", 1);
  expect_int(R"((builtins.listToAttrs [{name = "a"; value = 1;} {name = "b"; value = 2;}]).b)", 2);
  // First occurrence wins
  expect_int(R"((builtins.listToAttrs [{name = "x"; value = 1;} {name = "x"; value = 2;}]).x)", 1);
}

TEST_CASE("exec: builtins.mapAttrs", "[execution][builtins]") {
  // mapAttrs takes a function (name: value: result)
  expect_int(R"((builtins.mapAttrs (n: v: v + 1) { a = 1; b = 2; }).a)", 2);
  expect_int(R"((builtins.mapAttrs (n: v: v + 1) { a = 1; b = 2; }).b)", 3);
  // Using name in the function
  expect_string(R"((builtins.mapAttrs (n: v: n) { a = 1; b = 2; }).a)", "a");
  expect_string(R"((builtins.mapAttrs (n: v: n) { a = 1; b = 2; }).b)", "b");
  // Empty set
  expect_int(R"(builtins.length (builtins.attrNames (builtins.mapAttrs (n: v: v) {})))", 0);
}

TEST_CASE("exec: builtins.catAttrs", "[execution][builtins]") {
  expect_int(R"(builtins.head (builtins.catAttrs "x" [{ x = 1; } { x = 2; }]))", 1);
  expect_int(R"(builtins.length (builtins.catAttrs "x" [{ x = 1; } { x = 2; }]))", 2);
  // Missing attrs are skipped
  expect_int(R"(builtins.length (builtins.catAttrs "x" [{ x = 1; } { y = 2; }]))", 1);
  expect_int(R"(builtins.length (builtins.catAttrs "z" [{ x = 1; } { y = 2; }]))", 0);
  // Empty list
  expect_int(R"(builtins.length (builtins.catAttrs "x" []))", 0);
}

TEST_CASE("exec: builtins.partition", "[execution][builtins]") {
  // right contains elements where predicate is true
  expect_int(R"(builtins.length (builtins.partition (x: x > 0) [1 (-1) 2 (-2)]).right)", 2);
  expect_int(R"(builtins.head (builtins.partition (x: x > 0) [1 (-1) 2 (-2)]).right)", 1);
  // wrong contains elements where predicate is false
  expect_int(R"(builtins.length (builtins.partition (x: x > 0) [1 (-1) 2 (-2)]).wrong)", 2);
  expect_int(R"(builtins.head (builtins.partition (x: x > 0) [1 (-1) 2 (-2)]).wrong)", -1);
  // Empty list
  expect_int(R"(builtins.length (builtins.partition (x: x > 0) []).right)", 0);
  expect_int(R"(builtins.length (builtins.partition (x: x > 0) []).wrong)", 0);
}

TEST_CASE("exec: builtins.groupBy", "[execution][builtins]") {
  // Group by type attribute
  expect_int(
      R"(builtins.length (builtins.groupBy (x: x.type) [{ type = "a"; } { type = "b"; } { type = "a"; }]).a)",
      2);
  expect_int(
      R"(builtins.length (builtins.groupBy (x: x.type) [{ type = "a"; } { type = "b"; } { type = "a"; }]).b)",
      1);
  // Empty list
  expect_int(R"(builtins.length (builtins.attrNames (builtins.groupBy (x: x) [])))", 0);
}

TEST_CASE("exec: builtins arithmetic functions", "[execution][builtins]") {
  expect_int("builtins.add 2 3", 5);
  expect_int("builtins.sub 10 3", 7);
  expect_int("builtins.mul 4 5", 20);
  expect_int("builtins.div 10 3", 3);
  expect_bool("builtins.lessThan 1 2", true);
  expect_bool("builtins.lessThan 2 1", false);
  expect_bool("builtins.lessThan 1 1", false);
}

TEST_CASE("exec: builtins.floor", "[execution][builtins]") {
  expect_int("builtins.floor 3", 3);
  expect_int("builtins.floor 3.2", 3);
  expect_int("builtins.floor 3.9", 3);
  expect_int("builtins.floor (-3.2)", -4);
  expect_int("builtins.floor (-3.9)", -4);
  expect_int("builtins.floor 0.0", 0);
}

TEST_CASE("exec: builtins.ceil", "[execution][builtins]") {
  expect_int("builtins.ceil 3", 3);
  expect_int("builtins.ceil 3.2", 4);
  expect_int("builtins.ceil 3.9", 4);
  expect_int("builtins.ceil (-3.2)", -3);
  expect_int("builtins.ceil (-3.9)", -3);
  expect_int("builtins.ceil 0.0", 0);
}

TEST_CASE("exec: builtins bitwise ops", "[execution][builtins]") {
  // bitAnd
  expect_int("builtins.bitAnd 12 10", 8);   // 1100 & 1010 = 1000
  expect_int("builtins.bitAnd 255 15", 15); // 11111111 & 00001111 = 00001111
  expect_int("builtins.bitAnd 0 255", 0);

  // bitOr
  expect_int("builtins.bitOr 12 10", 14); // 1100 | 1010 = 1110
  expect_int("builtins.bitOr 8 4", 12);   // 1000 | 0100 = 1100
  expect_int("builtins.bitOr 0 255", 255);

  // bitXor
  expect_int("builtins.bitXor 12 10", 6); // 1100 ^ 1010 = 0110
  expect_int("builtins.bitXor 255 255", 0);
  expect_int("builtins.bitXor 0 255", 255);
}
