#include <string>

#include <catch2/catch_test_macros.hpp>

#include "nix-language/ast/expression.hh"
#include "nix-language/ast/symbol_table.hh"
#include "nix-language/eval/eval.hh"
#include "nix-language/parse/parser.hh"

namespace ast = nix::language::ast;
namespace parse = nix::language::parse;
namespace eval = nix::language::eval;

// Helper to evaluate a string and return the printed result
auto evaluate(const std::string& source) -> std::string {
  ast::symbol_table symbols;
  auto expr = parse::parse(source, symbols);
  eval::evaluator evaluator(symbols);
  auto result = evaluator.eval(expr);
  return evaluator.print_value(result);
}

// Helper that returns the value_ptr for more detailed checks
auto eval_value(const std::string& source, ast::symbol_table& symbols) -> eval::value_ptr {
  auto expr = parse::parse(source, symbols);
  eval::evaluator evaluator(symbols);
  return evaluator.force(evaluator.eval(expr));
}

TEST_CASE("eval integer literals", "[eval][literal]") {
  REQUIRE(evaluate("42") == "42");
  REQUIRE(evaluate("0") == "0");
  REQUIRE(evaluate("-5") == "-5");
}

TEST_CASE("eval float literals", "[eval][literal]") {
  auto result = evaluate("3.14");
  REQUIRE(result.find("3.14") != std::string::npos);
}

TEST_CASE("eval string literals", "[eval][literal]") {
  REQUIRE(evaluate("\"hello\"") == "\"hello\"");
  REQUIRE(evaluate("\"\"") == "\"\"");
}

TEST_CASE("eval boolean literals", "[eval][literal]") {
  REQUIRE(evaluate("true") == "true");
  REQUIRE(evaluate("false") == "false");
}

TEST_CASE("eval null", "[eval][literal]") {
  REQUIRE(evaluate("null") == "null");
}

TEST_CASE("eval arithmetic", "[eval][operator]") {
  REQUIRE(evaluate("1 + 2") == "3");
  REQUIRE(evaluate("10 - 3") == "7");
  REQUIRE(evaluate("4 * 5") == "20");
  REQUIRE(evaluate("20 / 4") == "5");
  REQUIRE(evaluate("1 + 2 * 3") == "7");
  REQUIRE(evaluate("(1 + 2) * 3") == "9");
}

TEST_CASE("eval comparison", "[eval][operator]") {
  REQUIRE(evaluate("1 < 2") == "true");
  REQUIRE(evaluate("2 < 1") == "false");
  REQUIRE(evaluate("1 <= 1") == "true");
  REQUIRE(evaluate("1 == 1") == "true");
  REQUIRE(evaluate("1 == 2") == "false");
  REQUIRE(evaluate("1 != 2") == "true");
}

TEST_CASE("eval logical operators", "[eval][operator]") {
  REQUIRE(evaluate("true && true") == "true");
  REQUIRE(evaluate("true && false") == "false");
  REQUIRE(evaluate("false && true") == "false");
  REQUIRE(evaluate("true || false") == "true");
  REQUIRE(evaluate("false || false") == "false");
  REQUIRE(evaluate("!true") == "false");
  REQUIRE(evaluate("!false") == "true");
}

TEST_CASE("eval string concatenation", "[eval][operator]") {
  REQUIRE(evaluate("\"hello\" + \" \" + \"world\"") == "\"hello world\"");
}

TEST_CASE("eval list literal", "[eval][list]") {
  REQUIRE(evaluate("[]") == "[ ]");
  REQUIRE(evaluate("[ 1 2 3 ]") == "[ 1 2 3 ]");
}

TEST_CASE("eval list concatenation", "[eval][list]") {
  REQUIRE(evaluate("[ 1 2 ] ++ [ 3 4 ]") == "[ 1 2 3 4 ]");
}

TEST_CASE("eval attribute set", "[eval][attrset]") {
  REQUIRE(evaluate("{}") == "{ }");
  REQUIRE(evaluate("{ x = 1; }").find("x = 1") != std::string::npos);
}

TEST_CASE("eval select", "[eval][select]") {
  REQUIRE(evaluate("{ x = 42; }.x") == "42");
  REQUIRE(evaluate("{ a = { b = 1; }; }.a.b") == "1");
}

TEST_CASE("eval select with default", "[eval][select]") {
  REQUIRE(evaluate("{ }.x or 99") == "99");
  REQUIRE(evaluate("{ x = 1; }.x or 99") == "1");
}

TEST_CASE("eval has attribute", "[eval][hasattr]") {
  REQUIRE(evaluate("{ x = 1; } ? x") == "true");
  REQUIRE(evaluate("{ x = 1; } ? y") == "false");
}

TEST_CASE("eval if expression", "[eval][control]") {
  REQUIRE(evaluate("if true then 1 else 2") == "1");
  REQUIRE(evaluate("if false then 1 else 2") == "2");
  REQUIRE(evaluate("if 1 < 2 then \"yes\" else \"no\"") == "\"yes\"");
}

TEST_CASE("eval let expression", "[eval][let]") {
  REQUIRE(evaluate("let x = 1; in x") == "1");
  REQUIRE(evaluate("let x = 1; y = 2; in x + y") == "3");
  REQUIRE(evaluate("let x = 1; in let y = 2; in x + y") == "3");
}

TEST_CASE("eval recursive let", "[eval][let]") {
  // let bindings can reference each other
  REQUIRE(evaluate("let x = 1; y = x + 1; in y") == "2");
}

TEST_CASE("eval lambda", "[eval][lambda]") {
  REQUIRE(evaluate("(x: x) 42") == "42");
  REQUIRE(evaluate("(x: x + 1) 5") == "6");
  REQUIRE(evaluate("(x: y: x + y) 1 2") == "3");
}

TEST_CASE("eval lambda with attrset pattern", "[eval][lambda]") {
  REQUIRE(evaluate("({ x }: x) { x = 42; }") == "42");
  REQUIRE(evaluate("({ x, y }: x + y) { x = 1; y = 2; }") == "3");
}

TEST_CASE("eval lambda with default", "[eval][lambda]") {
  REQUIRE(evaluate("({ x ? 10 }: x) { }") == "10");
  REQUIRE(evaluate("({ x ? 10 }: x) { x = 5; }") == "5");
}

TEST_CASE("eval lambda with @pattern", "[eval][lambda]") {
  REQUIRE(evaluate("({ x }@args: args.x) { x = 42; }") == "42");
}

TEST_CASE("eval recursive attrset", "[eval][rec]") {
  REQUIRE(evaluate("rec { x = 1; y = x + 1; }.y") == "2");
}

TEST_CASE("eval with expression", "[eval][with]") {
  REQUIRE(evaluate("with { x = 1; }; x") == "1");
  REQUIRE(evaluate("with { x = 1; y = 2; }; x + y") == "3");
}

TEST_CASE("eval assert", "[eval][assert]") {
  REQUIRE(evaluate("assert true; 42") == "42");
  REQUIRE_THROWS_AS(evaluate("assert false; 42"), eval::eval_error);
}

TEST_CASE("eval update operator", "[eval][operator]") {
  auto result = evaluate("{ x = 1; } // { y = 2; }");
  REQUIRE(result.find("x = 1") != std::string::npos);
  REQUIRE(result.find("y = 2") != std::string::npos);
}

TEST_CASE("eval builtins.add", "[eval][builtins]") {
  REQUIRE(evaluate("builtins.add 1 2") == "3");
}

TEST_CASE("eval builtins.length", "[eval][builtins]") {
  REQUIRE(evaluate("builtins.length [ 1 2 3 ]") == "3");
  REQUIRE(evaluate("builtins.length []") == "0");
}

TEST_CASE("eval builtins.head and tail", "[eval][builtins]") {
  REQUIRE(evaluate("builtins.head [ 1 2 3 ]") == "1");
  REQUIRE(evaluate("builtins.tail [ 1 2 3 ]") == "[ 2 3 ]");
}

TEST_CASE("eval builtins.map", "[eval][builtins]") {
  REQUIRE(evaluate("builtins.map (x: x * 2) [ 1 2 3 ]") == "[ 2 4 6 ]");
}

TEST_CASE("eval builtins.filter", "[eval][builtins]") {
  REQUIRE(evaluate("builtins.filter (x: x > 1) [ 1 2 3 ]") == "[ 2 3 ]");
}

TEST_CASE("eval builtins.attrNames", "[eval][builtins]") {
  auto result = evaluate("builtins.attrNames { b = 1; a = 2; }");
  // Order may vary, just check both are present
  REQUIRE(result.find("\"a\"") != std::string::npos);
  REQUIRE(result.find("\"b\"") != std::string::npos);
}

TEST_CASE("eval builtins.hasAttr", "[eval][builtins]") {
  REQUIRE(evaluate("builtins.hasAttr \"x\" { x = 1; }") == "true");
  REQUIRE(evaluate("builtins.hasAttr \"y\" { x = 1; }") == "false");
}

TEST_CASE("eval builtins.typeOf", "[eval][builtins]") {
  REQUIRE(evaluate("builtins.typeOf 42") == "\"int\"");
  REQUIRE(evaluate("builtins.typeOf \"hello\"") == "\"string\"");
  REQUIRE(evaluate("builtins.typeOf true") == "\"bool\"");
  REQUIRE(evaluate("builtins.typeOf [ ]") == "\"list\"");
  REQUIRE(evaluate("builtins.typeOf { }") == "\"set\"");
  REQUIRE(evaluate("builtins.typeOf (x: x)") == "\"lambda\"");
}

TEST_CASE("eval string interpolation", "[eval][string]") {
  REQUIRE(evaluate("let x = \"world\"; in \"hello ${x}\"") == "\"hello world\"");
  REQUIRE(evaluate("let n = 42; in \"number: ${builtins.toString n}\"") == "\"number: 42\"");
}

TEST_CASE("eval lazy evaluation", "[eval][lazy]") {
  // This should not throw because the error branch is not evaluated
  REQUIRE(evaluate("let x = builtins.throw \"error\"; in 42") == "42");
}

TEST_CASE("eval foldl'", "[eval][builtins]") {
  REQUIRE(evaluate("builtins.foldl' (a: b: a + b) 0 [ 1 2 3 4 ]") == "10");
}

TEST_CASE("eval nested attribute paths", "[eval][attrset]") {
  REQUIRE(evaluate("{ a.b.c = 1; }.a.b.c") == "1");
  REQUIRE(evaluate("{ a.b = 1; a.c = 2; }.a.b") == "1");
  REQUIRE(evaluate("{ a.b = 1; a.c = 2; }.a.c") == "2");
  REQUIRE(evaluate("rec { a.b = x; x = 5; }.a.b") == "5");
}

TEST_CASE("eval builtins.elem", "[eval][builtins]") {
  REQUIRE(evaluate("builtins.elem 3 [ 1 2 3 4 ]") == "true");
  REQUIRE(evaluate("builtins.elem 5 [ 1 2 3 4 ]") == "false");
}

TEST_CASE("eval builtins.all and any", "[eval][builtins]") {
  REQUIRE(evaluate("builtins.all (x: x > 0) [ 1 2 3 ]") == "true");
  REQUIRE(evaluate("builtins.all (x: x > 1) [ 1 2 3 ]") == "false");
  REQUIRE(evaluate("builtins.any (x: x > 2) [ 1 2 3 ]") == "true");
  REQUIRE(evaluate("builtins.any (x: x > 5) [ 1 2 3 ]") == "false");
}

TEST_CASE("eval builtins.concatLists", "[eval][builtins]") {
  REQUIRE(evaluate("builtins.concatLists [ [ 1 2 ] [ 3 4 ] ]") == "[ 1 2 3 4 ]");
  REQUIRE(evaluate("builtins.concatLists [ ]") == "[ ]");
}

TEST_CASE("eval builtins.genList", "[eval][builtins]") {
  REQUIRE(evaluate("builtins.genList (x: x * x) 4") == "[ 0 1 4 9 ]");
  REQUIRE(evaluate("builtins.genList (x: x) 0") == "[ ]");
}

TEST_CASE("eval builtins.sort", "[eval][builtins]") {
  REQUIRE(evaluate("builtins.sort (a: b: a < b) [ 3 1 2 ]") == "[ 1 2 3 ]");
}

TEST_CASE("eval builtins.elemAt", "[eval][builtins]") {
  REQUIRE(evaluate("builtins.elemAt [ 1 2 3 ] 0") == "1");
  REQUIRE(evaluate("builtins.elemAt [ 1 2 3 ] 2") == "3");
}

TEST_CASE("eval builtins.stringLength", "[eval][builtins]") {
  REQUIRE(evaluate("builtins.stringLength \"hello\"") == "5");
  REQUIRE(evaluate("builtins.stringLength \"\"") == "0");
}

TEST_CASE("eval builtins.substring", "[eval][builtins]") {
  REQUIRE(evaluate("builtins.substring 0 3 \"hello\"") == "\"hel\"");
  REQUIRE(evaluate("builtins.substring 2 10 \"hello\"") == "\"llo\"");
}

TEST_CASE("eval builtins.replaceStrings", "[eval][builtins]") {
  REQUIRE(evaluate("builtins.replaceStrings [\"o\"] [\"0\"] \"foo\"") == "\"f00\"");
}

TEST_CASE("eval type checking builtins", "[eval][builtins]") {
  REQUIRE(evaluate("builtins.isNull null") == "true");
  REQUIRE(evaluate("builtins.isNull 1") == "false");
  REQUIRE(evaluate("builtins.isBool true") == "true");
  REQUIRE(evaluate("builtins.isInt 42") == "true");
  REQUIRE(evaluate("builtins.isString \"hi\"") == "true");
  REQUIRE(evaluate("builtins.isList [ ]") == "true");
  REQUIRE(evaluate("builtins.isAttrs { }") == "true");
  REQUIRE(evaluate("builtins.isFunction (x: x)") == "true");
}

TEST_CASE("eval builtins.tryEval", "[eval][builtins]") {
  auto success = evaluate("builtins.tryEval (1 + 1)");
  REQUIRE(success.find("success = true") != std::string::npos);
  REQUIRE(success.find("value = 2") != std::string::npos);

  auto failure = evaluate("builtins.tryEval (builtins.throw \"oops\")");
  REQUIRE(failure.find("success = false") != std::string::npos);
}
