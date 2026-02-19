// straylight // nix-language // tests
//
// Unit tests for WASM compiler

// Catch2 must be included before rapidcheck/catch.h for v3 compatibility
#include <cstdint>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "nix-language/ast/expression.hh"
#include "nix-language/ast/symbol_table.hh"
#include "nix-language/compile/compiler.hh"
#include "nix-language/compile/wasm_types.hh"

using namespace nix::language;

// =============================================================================
// helper: create expression nodes
// =============================================================================

template <typename T>
ast::expression make_expr(T&& value) {
  return std::make_unique<ast::expression_node>(ast::expression_variant{std::forward<T>(value)});
}

ast::source_position pos(std::uint32_t offset = 0) {
  return {offset, 1, 1};
}

// =============================================================================
// basic literal compilation
// =============================================================================

TEST_CASE("compile integer literal", "[compiler][literal]") {
  ast::symbol_table symbols;
  auto expr = make_expr(ast::expression_integer{pos(), 42});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  REQUIRE(wat.find("i64.const") != std::string::npos);
}

TEST_CASE("compile negative integer", "[compiler][literal]") {
  ast::symbol_table symbols;
  auto expr = make_expr(ast::expression_integer{pos(), -123});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());
}

TEST_CASE("compile string literal", "[compiler][literal]") {
  ast::symbol_table symbols;
  auto expr = make_expr(ast::expression_string{pos(), "hello world"});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should have data segment with the string
  REQUIRE(wat.find("data") != std::string::npos);
}

TEST_CASE("compile float literal", "[compiler][literal]") {
  ast::symbol_table symbols;
  auto expr = make_expr(ast::expression_float{pos(), 3.14159});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());
}

TEST_CASE("compile path literal", "[compiler][literal]") {
  ast::symbol_table symbols;
  auto expr = make_expr(ast::expression_path{pos(), "/nix/store/test"});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());
}

// =============================================================================
// string interpolation
// =============================================================================

TEST_CASE("compile empty string interpolation", "[compiler][interpolation]") {
  ast::symbol_table symbols;
  std::vector<std::variant<std::string, ast::expression>> parts;
  auto expr = make_expr(ast::expression_string_interpolated{pos(), std::move(parts)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());
}

TEST_CASE("compile single literal string interpolation", "[compiler][interpolation]") {
  ast::symbol_table symbols;
  std::vector<std::variant<std::string, ast::expression>> parts;
  parts.push_back(std::string("hello world"));
  auto expr = make_expr(ast::expression_string_interpolated{pos(), std::move(parts)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  REQUIRE(wat.find("data") != std::string::npos);
}

TEST_CASE("compile string interpolation with expression", "[compiler][interpolation]") {
  ast::symbol_table symbols;

  // "hello ${42}!"
  std::vector<std::variant<std::string, ast::expression>> parts;
  parts.push_back(std::string("hello "));
  parts.push_back(make_expr(ast::expression_integer{pos(), 42}));
  parts.push_back(std::string("!"));
  auto expr = make_expr(ast::expression_string_interpolated{pos(), std::move(parts)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should call __toString for the expression part
  REQUIRE(wat.find("__toString") != std::string::npos);
  // should call __concatStrings to join the parts
  REQUIRE(wat.find("__concatStrings") != std::string::npos);
}

TEST_CASE("compile string interpolation with multiple expressions", "[compiler][interpolation]") {
  ast::symbol_table symbols;

  // "${1} + ${2} = ${3}"
  std::vector<std::variant<std::string, ast::expression>> parts;
  parts.push_back(make_expr(ast::expression_integer{pos(), 1}));
  parts.push_back(std::string(" + "));
  parts.push_back(make_expr(ast::expression_integer{pos(), 2}));
  parts.push_back(std::string(" = "));
  parts.push_back(make_expr(ast::expression_integer{pos(), 3}));
  auto expr = make_expr(ast::expression_string_interpolated{pos(), std::move(parts)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  REQUIRE(wat.find("__toString") != std::string::npos);
  REQUIRE(wat.find("__concatStrings") != std::string::npos);
}

TEST_CASE("compile string interpolation with nested expression", "[compiler][interpolation]") {
  ast::symbol_table symbols;

  // "result: ${1 + 2}"
  auto left = make_expr(ast::expression_integer{pos(), 1});
  auto right = make_expr(ast::expression_integer{pos(), 2});
  auto add_expr = make_expr(ast::expression_binary_operation{pos(), ast::binary_operator::add,
                                                             std::move(left), std::move(right)});

  std::vector<std::variant<std::string, ast::expression>> parts;
  parts.push_back(std::string("result: "));
  parts.push_back(std::move(add_expr));
  auto expr = make_expr(ast::expression_string_interpolated{pos(), std::move(parts)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  REQUIRE(wat.find("__add") != std::string::npos);
  REQUIRE(wat.find("__toString") != std::string::npos);
  REQUIRE(wat.find("__concatStrings") != std::string::npos);
}

// =============================================================================
// path interpolation
// =============================================================================

TEST_CASE("compile empty path interpolation", "[compiler][interpolation]") {
  ast::symbol_table symbols;
  std::vector<std::variant<std::string, ast::expression>> parts;
  auto expr = make_expr(ast::expression_path_interpolated{pos(), std::move(parts)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());
}

TEST_CASE("compile single literal path interpolation", "[compiler][interpolation]") {
  ast::symbol_table symbols;
  std::vector<std::variant<std::string, ast::expression>> parts;
  parts.push_back(std::string("./my-file.nix"));
  auto expr = make_expr(ast::expression_path_interpolated{pos(), std::move(parts)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  REQUIRE(wat.find("data") != std::string::npos);
}

TEST_CASE("compile path interpolation with expression", "[compiler][interpolation]") {
  ast::symbol_table symbols;

  // ./${name}/file.nix
  auto name = symbols.intern("name");
  std::vector<std::variant<std::string, ast::expression>> parts;
  parts.push_back(std::string("./"));
  parts.push_back(make_expr(ast::expression_identifier{pos(), name}));
  parts.push_back(std::string("/file.nix"));
  auto expr = make_expr(ast::expression_path_interpolated{pos(), std::move(parts)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should call __toString for the expression part
  REQUIRE(wat.find("__toString") != std::string::npos);
  // should call __concatStrings to join the parts
  REQUIRE(wat.find("__concatStrings") != std::string::npos);
  // path tag conversion uses bit operations
  REQUIRE(wat.find("i64.shr_u") != std::string::npos);
  REQUIRE(wat.find("i64.or") != std::string::npos);
}

TEST_CASE("compile path interpolation with multiple expressions", "[compiler][interpolation]") {
  ast::symbol_table symbols;

  // ./${dir}/${file}.nix
  auto dir = symbols.intern("dir");
  auto file = symbols.intern("file");
  std::vector<std::variant<std::string, ast::expression>> parts;
  parts.push_back(std::string("./"));
  parts.push_back(make_expr(ast::expression_identifier{pos(), dir}));
  parts.push_back(std::string("/"));
  parts.push_back(make_expr(ast::expression_identifier{pos(), file}));
  parts.push_back(std::string(".nix"));
  auto expr = make_expr(ast::expression_path_interpolated{pos(), std::move(parts)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  REQUIRE(wat.find("__toString") != std::string::npos);
  REQUIRE(wat.find("__concatStrings") != std::string::npos);
}

// =============================================================================
// binary operations
// =============================================================================

TEST_CASE("compile addition", "[compiler][binop]") {
  ast::symbol_table symbols;
  auto left = make_expr(ast::expression_integer{pos(), 1});
  auto right = make_expr(ast::expression_integer{pos(), 2});
  auto expr = make_expr(ast::expression_binary_operation{pos(), ast::binary_operator::add,
                                                         std::move(left), std::move(right)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  REQUIRE(wat.find("__add") != std::string::npos);
}

TEST_CASE("compile subtraction", "[compiler][binop]") {
  ast::symbol_table symbols;
  auto left = make_expr(ast::expression_integer{pos(), 10});
  auto right = make_expr(ast::expression_integer{pos(), 3});
  auto expr = make_expr(ast::expression_binary_operation{pos(), ast::binary_operator::subtract,
                                                         std::move(left), std::move(right)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  REQUIRE(wat.find("__sub") != std::string::npos);
}

TEST_CASE("compile comparison operators", "[compiler][binop]") {
  ast::symbol_table symbols;

  SECTION("less than") {
    auto left = make_expr(ast::expression_integer{pos(), 1});
    auto right = make_expr(ast::expression_integer{pos(), 2});
    auto expr = make_expr(ast::expression_binary_operation{pos(), ast::binary_operator::less_than,
                                                           std::move(left), std::move(right)});

    compile::compiler comp(symbols);
    auto module = comp.compile(expr);
    REQUIRE(module.validate());
  }

  SECTION("equals") {
    auto left = make_expr(ast::expression_integer{pos(), 1});
    auto right = make_expr(ast::expression_integer{pos(), 1});
    auto expr = make_expr(ast::expression_binary_operation{pos(), ast::binary_operator::equals,
                                                           std::move(left), std::move(right)});

    compile::compiler comp(symbols);
    auto module = comp.compile(expr);
    REQUIRE(module.validate());
  }
}

// =============================================================================
// unary operations
// =============================================================================

TEST_CASE("compile unary not", "[compiler][unary]") {
  ast::symbol_table symbols;
  auto operand = make_expr(ast::expression_integer{pos(), 0}); // will be coerced to bool
  auto expr = make_expr(
      ast::expression_unary_operation{pos(), ast::unary_operator::logical_not, std::move(operand)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  REQUIRE(wat.find("__not") != std::string::npos);
}

TEST_CASE("compile unary negate", "[compiler][unary]") {
  ast::symbol_table symbols;
  auto operand = make_expr(ast::expression_integer{pos(), 42});
  auto expr = make_expr(
      ast::expression_unary_operation{pos(), ast::unary_operator::negate, std::move(operand)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  REQUIRE(wat.find("__negate") != std::string::npos);
}

// =============================================================================
// if expressions
// =============================================================================

TEST_CASE("compile if expression", "[compiler][control]") {
  ast::symbol_table symbols;
  auto cond = make_expr(ast::expression_integer{pos(), 1}); // truthy
  auto then_branch = make_expr(ast::expression_integer{pos(), 10});
  auto else_branch = make_expr(ast::expression_integer{pos(), 20});
  auto expr = make_expr(
      ast::expression_if{pos(), std::move(cond), std::move(then_branch), std::move(else_branch)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  REQUIRE(wat.find("if") != std::string::npos);
}

// =============================================================================
// list expressions
// =============================================================================

TEST_CASE("compile empty list", "[compiler][list]") {
  ast::symbol_table symbols;
  std::vector<ast::expression> elements;
  auto expr = make_expr(ast::expression_list{pos(), std::move(elements)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());
}

TEST_CASE("compile list with elements", "[compiler][list]") {
  ast::symbol_table symbols;
  std::vector<ast::expression> elements;
  elements.push_back(make_expr(ast::expression_integer{pos(), 1}));
  elements.push_back(make_expr(ast::expression_integer{pos(), 2}));
  elements.push_back(make_expr(ast::expression_integer{pos(), 3}));
  auto expr = make_expr(ast::expression_list{pos(), std::move(elements)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  REQUIRE(wat.find("__makeList") != std::string::npos);
}

// =============================================================================
// attribute sets
// =============================================================================

TEST_CASE("compile empty attrset", "[compiler][attrset]") {
  ast::symbol_table symbols;
  std::vector<ast::binding_variant> bindings;
  auto expr = make_expr(ast::expression_attribute_set{pos(), false, std::move(bindings)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());
}

TEST_CASE("compile simple attrset", "[compiler][attrset]") {
  ast::symbol_table symbols;

  ast::attribute_path path;
  path.segments.push_back(ast::attribute_name{pos(), symbols.intern("x")});

  auto value = make_expr(ast::expression_integer{pos(), 42});
  ast::binding_attribute binding{pos(), std::move(path), std::move(value)};

  std::vector<ast::binding_variant> bindings;
  bindings.push_back(std::move(binding));

  auto expr = make_expr(ast::expression_attribute_set{pos(), false, std::move(bindings)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  REQUIRE(wat.find("__makeAttrs") != std::string::npos);
}

TEST_CASE("compile attrset with dynamic key", "[compiler][attrset][dynamic]") {
  ast::symbol_table symbols;

  // { ${key} = 42; } where key is an identifier
  auto key_name = symbols.intern("key");
  auto key_expr = make_expr(ast::expression_identifier{pos(), key_name});

  ast::attribute_path path;
  path.segments.push_back(ast::attribute_name{pos(), std::move(key_expr)});

  auto value = make_expr(ast::expression_integer{pos(), 42});
  ast::binding_attribute binding{pos(), std::move(path), std::move(value)};

  std::vector<ast::binding_variant> bindings;
  bindings.push_back(std::move(binding));

  auto expr = make_expr(ast::expression_attribute_set{pos(), false, std::move(bindings)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should use __makeAttrsDynamic for dynamic keys
  REQUIRE(wat.find("__makeAttrsDynamic") != std::string::npos);
}

TEST_CASE("compile attrset with mixed static and dynamic keys", "[compiler][attrset][dynamic]") {
  ast::symbol_table symbols;

  // { x = 1; ${key} = 2; y = 3; }
  auto key_name = symbols.intern("key");

  std::vector<ast::binding_variant> bindings;

  // x = 1
  ast::attribute_path path1;
  path1.segments.push_back(ast::attribute_name{pos(), symbols.intern("x")});
  bindings.push_back(ast::binding_attribute{pos(), std::move(path1),
                                            make_expr(ast::expression_integer{pos(), 1})});

  // ${key} = 2
  auto key_expr = make_expr(ast::expression_identifier{pos(), key_name});
  ast::attribute_path path2;
  path2.segments.push_back(ast::attribute_name{pos(), std::move(key_expr)});
  bindings.push_back(ast::binding_attribute{pos(), std::move(path2),
                                            make_expr(ast::expression_integer{pos(), 2})});

  // y = 3
  ast::attribute_path path3;
  path3.segments.push_back(ast::attribute_name{pos(), symbols.intern("y")});
  bindings.push_back(ast::binding_attribute{pos(), std::move(path3),
                                            make_expr(ast::expression_integer{pos(), 3})});

  auto expr = make_expr(ast::expression_attribute_set{pos(), false, std::move(bindings)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  REQUIRE(wat.find("__makeAttrsDynamic") != std::string::npos);
}

TEST_CASE("compile select with dynamic key", "[compiler][select][dynamic]") {
  ast::symbol_table symbols;

  // set.${key} where set and key are identifiers
  auto set_name = symbols.intern("set");
  auto key_name = symbols.intern("key");

  auto subject = make_expr(ast::expression_identifier{pos(), set_name});
  auto key_expr = make_expr(ast::expression_identifier{pos(), key_name});

  ast::attribute_path path;
  path.segments.push_back(ast::attribute_name{pos(), std::move(key_expr)});

  auto expr =
      make_expr(ast::expression_select{pos(), std::move(subject), std::move(path), std::nullopt});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should use __selectDynamic for dynamic keys
  REQUIRE(wat.find("__selectDynamic") != std::string::npos);
}

TEST_CASE("compile select with mixed static and dynamic path", "[compiler][select][dynamic]") {
  ast::symbol_table symbols;

  // set.a.${key}.c where set and key are identifiers
  auto set_name = symbols.intern("set");
  auto key_name = symbols.intern("key");

  auto subject = make_expr(ast::expression_identifier{pos(), set_name});

  ast::attribute_path path;
  path.segments.push_back(ast::attribute_name{pos(), symbols.intern("a")});
  auto key_expr = make_expr(ast::expression_identifier{pos(), key_name});
  path.segments.push_back(ast::attribute_name{pos(), std::move(key_expr)});
  path.segments.push_back(ast::attribute_name{pos(), symbols.intern("c")});

  auto expr =
      make_expr(ast::expression_select{pos(), std::move(subject), std::move(path), std::nullopt});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should have both static __select and __selectDynamic calls
  REQUIRE(wat.find("__select") != std::string::npos);
  REQUIRE(wat.find("__selectDynamic") != std::string::npos);
}

TEST_CASE("compile has_attribute with dynamic key", "[compiler][hasattr][dynamic]") {
  ast::symbol_table symbols;

  // set ? ${key} where set and key are identifiers
  auto set_name = symbols.intern("set");
  auto key_name = symbols.intern("key");

  auto subject = make_expr(ast::expression_identifier{pos(), set_name});
  auto key_expr = make_expr(ast::expression_identifier{pos(), key_name});

  ast::attribute_path path;
  path.segments.push_back(ast::attribute_name{pos(), std::move(key_expr)});

  auto expr = make_expr(ast::expression_has_attribute{pos(), std::move(subject), std::move(path)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should use __hasAttrDynamic for dynamic keys
  REQUIRE(wat.find("__hasAttrDynamic") != std::string::npos);
}

// =============================================================================
// assert expressions
// =============================================================================

TEST_CASE("compile assert expression", "[compiler][control]") {
  ast::symbol_table symbols;
  auto cond = make_expr(ast::expression_integer{pos(), 1});
  auto body = make_expr(ast::expression_integer{pos(), 42});
  auto expr = make_expr(ast::expression_assert{pos(), std::move(cond), std::move(body)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  REQUIRE(wat.find("__throw") != std::string::npos);
}

// =============================================================================
// lambda expressions
// =============================================================================

TEST_CASE("compile simple lambda", "[compiler][lambda]") {
  ast::symbol_table symbols;

  // x: x (identity function)
  auto arg_name = symbols.intern("x");
  auto pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{arg_name});
  auto body = make_expr(ast::expression_identifier{pos(), arg_name});

  auto expr = make_expr(ast::expression_lambda{pos(), std::move(pattern), std::move(body)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should have a lambda function
  REQUIRE(wat.find("__lambda_0") != std::string::npos);
  // should have function table
  REQUIRE(wat.find("table") != std::string::npos);
}

TEST_CASE("compile lambda with constant body", "[compiler][lambda]") {
  ast::symbol_table symbols;

  // x: 42
  auto arg_name = symbols.intern("x");
  auto pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{arg_name});
  auto body = make_expr(ast::expression_integer{pos(), 42});

  auto expr = make_expr(ast::expression_lambda{pos(), std::move(pattern), std::move(body)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());
}

TEST_CASE("compile nested lambdas", "[compiler][lambda]") {
  ast::symbol_table symbols;

  // x: y: x (returns first argument)
  auto x_name = symbols.intern("x");
  auto y_name = symbols.intern("y");

  // inner lambda: y: x
  auto inner_pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{y_name});
  auto inner_body = make_expr(ast::expression_identifier{pos(), x_name});
  auto inner_lambda =
      make_expr(ast::expression_lambda{pos(), std::move(inner_pattern), std::move(inner_body)});

  // outer lambda: x: (y: x)
  auto outer_pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{x_name});
  auto expr =
      make_expr(ast::expression_lambda{pos(), std::move(outer_pattern), std::move(inner_lambda)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should have two lambda functions
  REQUIRE(wat.find("__lambda_0") != std::string::npos);
  REQUIRE(wat.find("__lambda_1") != std::string::npos);
}

TEST_CASE("compile lambda with attrset pattern", "[compiler][lambda]") {
  ast::symbol_table symbols;

  // { a, b }: a
  auto a_name = symbols.intern("a");
  auto b_name = symbols.intern("b");

  std::vector<ast::formal_parameter> formals;
  formals.push_back(ast::formal_parameter{pos(), a_name, std::nullopt});
  formals.push_back(ast::formal_parameter{pos(), b_name, std::nullopt});

  auto pattern = std::make_unique<ast::pattern_variant>(
      ast::pattern_attrset{std::nullopt, std::move(formals), false});
  auto body = make_expr(ast::expression_identifier{pos(), a_name});

  auto expr = make_expr(ast::expression_lambda{pos(), std::move(pattern), std::move(body)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should extract attributes using __select
  REQUIRE(wat.find("__select") != std::string::npos);
}

TEST_CASE("compile lambda with default parameter", "[compiler][lambda]") {
  ast::symbol_table symbols;

  // { a ? 42 }: a
  auto a_name = symbols.intern("a");
  auto default_val = make_expr(ast::expression_integer{pos(), 42});

  std::vector<ast::formal_parameter> formals;
  formals.push_back(ast::formal_parameter{pos(), a_name, std::move(default_val)});

  auto pattern = std::make_unique<ast::pattern_variant>(
      ast::pattern_attrset{std::nullopt, std::move(formals), false});
  auto body = make_expr(ast::expression_identifier{pos(), a_name});

  auto expr = make_expr(ast::expression_lambda{pos(), std::move(pattern), std::move(body)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should have conditional logic for default value
  REQUIRE(wat.find("if") != std::string::npos);
}

TEST_CASE("compile lambda with @pattern", "[compiler][lambda]") {
  ast::symbol_table symbols;

  // { a }@args: args
  auto a_name = symbols.intern("a");
  auto args_name = symbols.intern("args");

  std::vector<ast::formal_parameter> formals;
  formals.push_back(ast::formal_parameter{pos(), a_name, std::nullopt});

  auto pattern = std::make_unique<ast::pattern_variant>(
      ast::pattern_attrset{args_name, std::move(formals), false});
  auto body = make_expr(ast::expression_identifier{pos(), args_name});

  auto expr = make_expr(ast::expression_lambda{pos(), std::move(pattern), std::move(body)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());
}

// =============================================================================
// function application
// =============================================================================

TEST_CASE("compile function application", "[compiler][application]") {
  ast::symbol_table symbols;

  // f 42 where f is a free variable
  auto f_name = symbols.intern("f");
  auto func = make_expr(ast::expression_identifier{pos(), f_name});

  std::vector<ast::expression> arguments;
  arguments.push_back(make_expr(ast::expression_integer{pos(), 42}));

  auto expr = make_expr(ast::expression_application{pos(), std::move(func), std::move(arguments)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  REQUIRE(wat.find("__apply") != std::string::npos);
}

// =============================================================================
// module output
// =============================================================================

TEST_CASE("emit WASM binary", "[compiler][output]") {
  ast::symbol_table symbols;
  auto expr = make_expr(ast::expression_integer{pos(), 42});

  auto binary = compile::compile_to_wasm(expr, symbols, false);

  // WASM magic number
  REQUIRE(binary.size() >= 8);
  REQUIRE(binary[0] == 0x00);
  REQUIRE(binary[1] == 0x61);
  REQUIRE(binary[2] == 0x73);
  REQUIRE(binary[3] == 0x6d);
}

TEST_CASE("emit WAT text", "[compiler][output]") {
  ast::symbol_table symbols;
  auto expr = make_expr(ast::expression_integer{pos(), 42});

  auto wat = compile::compile_to_wat(expr, symbols, false);

  REQUIRE(wat.find("(module") != std::string::npos);
  REQUIRE(wat.find("(func") != std::string::npos);
  REQUIRE(wat.find("(export \"main\"") != std::string::npos);
}

// =============================================================================
// closures (lambdas capturing outer variables)
// =============================================================================

TEST_CASE("compile closure capturing single variable", "[compiler][closure]") {
  ast::symbol_table symbols;

  // let x = 42; in y: x
  // The lambda `y: x` captures `x` from the enclosing let
  auto x_name = symbols.intern("x");
  auto y_name = symbols.intern("y");

  // inner lambda: y: x
  auto inner_pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{y_name});
  auto inner_body = make_expr(ast::expression_identifier{pos(), x_name});
  auto inner_lambda =
      make_expr(ast::expression_lambda{pos(), std::move(inner_pattern), std::move(inner_body)});

  // let binding: x = 42
  ast::attribute_path x_path;
  x_path.segments.push_back(ast::attribute_name{pos(), x_name});
  ast::binding_attribute x_binding{pos(), std::move(x_path),
                                   make_expr(ast::expression_integer{pos(), 42})};
  std::vector<ast::binding_variant> let_bindings;
  let_bindings.push_back(std::move(x_binding));

  // wrap in outer lambda so let is in function context
  auto outer_arg = symbols.intern("_");
  auto outer_pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{outer_arg});
  auto let_expr =
      make_expr(ast::expression_let{pos(), std::move(let_bindings), std::move(inner_lambda)});
  auto outer_lambda =
      make_expr(ast::expression_lambda{pos(), std::move(outer_pattern), std::move(let_expr)});

  compile::compiler comp(symbols);
  auto module = comp.compile(outer_lambda);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should have two lambda functions
  REQUIRE(wat.find("__lambda_0") != std::string::npos);
  REQUIRE(wat.find("__lambda_1") != std::string::npos);
}

TEST_CASE("compile nested closure (const function)", "[compiler][closure]") {
  ast::symbol_table symbols;

  // x: y: x - the classic const function
  // inner lambda captures x from outer lambda
  auto x_name = symbols.intern("x");
  auto y_name = symbols.intern("y");

  // inner lambda: y: x (captures x)
  auto inner_pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{y_name});
  auto inner_body = make_expr(ast::expression_identifier{pos(), x_name});
  auto inner_lambda =
      make_expr(ast::expression_lambda{pos(), std::move(inner_pattern), std::move(inner_body)});

  // outer lambda: x: (y: x)
  auto outer_pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{x_name});
  auto expr =
      make_expr(ast::expression_lambda{pos(), std::move(outer_pattern), std::move(inner_lambda)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should have two lambda functions
  REQUIRE(wat.find("__lambda_0") != std::string::npos);
  REQUIRE(wat.find("__lambda_1") != std::string::npos);
  // inner lambda should load captured value from closure environment
  REQUIRE(wat.find("i64.load") != std::string::npos);
}

TEST_CASE("compile closure with multiple captures", "[compiler][closure]") {
  ast::symbol_table symbols;

  // x: y: z: x + y (captures both x and y)
  auto x_name = symbols.intern("x");
  auto y_name = symbols.intern("y");
  auto z_name = symbols.intern("z");

  // innermost: z: x + y
  auto z_pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{z_name});
  auto x_ref = make_expr(ast::expression_identifier{pos(), x_name});
  auto y_ref = make_expr(ast::expression_identifier{pos(), y_name});
  auto add_expr = make_expr(ast::expression_binary_operation{pos(), ast::binary_operator::add,
                                                             std::move(x_ref), std::move(y_ref)});
  auto z_lambda =
      make_expr(ast::expression_lambda{pos(), std::move(z_pattern), std::move(add_expr)});

  // middle: y: (z: x + y)
  auto y_pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{y_name});
  auto y_lambda =
      make_expr(ast::expression_lambda{pos(), std::move(y_pattern), std::move(z_lambda)});

  // outer: x: (y: (z: x + y))
  auto x_pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{x_name});
  auto expr = make_expr(ast::expression_lambda{pos(), std::move(x_pattern), std::move(y_lambda)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should have three lambda functions
  REQUIRE(wat.find("__lambda_0") != std::string::npos);
  REQUIRE(wat.find("__lambda_1") != std::string::npos);
  REQUIRE(wat.find("__lambda_2") != std::string::npos);
}

TEST_CASE("compile lambda with no captures (simple function)", "[compiler][closure]") {
  ast::symbol_table symbols;

  // x: x + 1 - no captures, just uses its argument
  auto x_name = symbols.intern("x");

  auto x_pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{x_name});
  auto x_ref = make_expr(ast::expression_identifier{pos(), x_name});
  auto one = make_expr(ast::expression_integer{pos(), 1});
  auto add_expr = make_expr(ast::expression_binary_operation{pos(), ast::binary_operator::add,
                                                             std::move(x_ref), std::move(one)});
  auto expr = make_expr(ast::expression_lambda{pos(), std::move(x_pattern), std::move(add_expr)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should NOT have i64.store for closure (no captures)
  // the lambda value should be a simple constant
  REQUIRE(wat.find("__lambda_0") != std::string::npos);
}

// =============================================================================
// free variable analyzer tests
// =============================================================================

TEST_CASE("free variable analyzer finds free variables", "[compiler][fva]") {
  ast::symbol_table symbols;
  auto x_name = symbols.intern("x");
  auto y_name = symbols.intern("y");

  // expression: x + y with only x bound
  auto x_ref = make_expr(ast::expression_identifier{pos(), x_name});
  auto y_ref = make_expr(ast::expression_identifier{pos(), y_name});
  auto add_expr = make_expr(ast::expression_binary_operation{pos(), ast::binary_operator::add,
                                                             std::move(x_ref), std::move(y_ref)});

  std::vector<ast::symbol> bound{x_name};
  auto free_vars = compile::free_variable_analyzer::analyze(add_expr, bound);

  REQUIRE(free_vars.size() == 1);
  REQUIRE(free_vars[0] == y_name);
}

TEST_CASE("free variable analyzer handles lambda binding", "[compiler][fva]") {
  ast::symbol_table symbols;
  auto x_name = symbols.intern("x");

  // expression: x: x - x is bound by the lambda, so no free variables
  auto pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{x_name});
  auto body = make_expr(ast::expression_identifier{pos(), x_name});
  auto lambda = make_expr(ast::expression_lambda{pos(), std::move(pattern), std::move(body)});

  std::vector<ast::symbol> bound{};
  auto free_vars = compile::free_variable_analyzer::analyze(lambda, bound);

  REQUIRE(free_vars.empty());
}

TEST_CASE("free variable analyzer finds captures in nested lambda", "[compiler][fva]") {
  ast::symbol_table symbols;
  auto x_name = symbols.intern("x");
  auto y_name = symbols.intern("y");

  // expression: x: (y: x) - x is free in inner lambda
  auto inner_pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{y_name});
  auto x_ref = make_expr(ast::expression_identifier{pos(), x_name});
  auto inner_lambda =
      make_expr(ast::expression_lambda{pos(), std::move(inner_pattern), std::move(x_ref)});

  auto outer_pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{x_name});
  auto outer_lambda =
      make_expr(ast::expression_lambda{pos(), std::move(outer_pattern), std::move(inner_lambda)});

  // analyzing the inner lambda with only y bound
  // but we analyze from the outer context
  std::vector<ast::symbol> bound{};
  auto free_vars = compile::free_variable_analyzer::analyze(outer_lambda, bound);

  // outer lambda has no free vars (x is bound by it)
  REQUIRE(free_vars.empty());
}

// =============================================================================
// inherit bindings in attribute sets
// =============================================================================

TEST_CASE("compile attrset with inherit from outer scope", "[compiler][attrset][inherit]") {
  ast::symbol_table symbols;

  // _: let x = 42; in { inherit x; }
  // inherit x pulls x from outer scope
  auto x_name = symbols.intern("x");
  auto dummy_arg = symbols.intern("_");

  // let binding: x = 42
  ast::attribute_path x_path;
  x_path.segments.push_back(ast::attribute_name{pos(), x_name});
  ast::binding_attribute x_binding{pos(), std::move(x_path),
                                   make_expr(ast::expression_integer{pos(), 42})};
  std::vector<ast::binding_variant> let_bindings;
  let_bindings.push_back(std::move(x_binding));

  // attrset with inherit x
  std::vector<ast::attribute_name> inherit_attrs;
  inherit_attrs.push_back(ast::attribute_name{pos(), x_name});
  ast::binding_inherit inherit_binding{pos(), std::nullopt, std::move(inherit_attrs)};
  std::vector<ast::binding_variant> attrset_bindings;
  attrset_bindings.push_back(std::move(inherit_binding));
  auto attrset =
      make_expr(ast::expression_attribute_set{pos(), false, std::move(attrset_bindings)});

  // let x = 42; in { inherit x; }
  auto let_expr =
      make_expr(ast::expression_let{pos(), std::move(let_bindings), std::move(attrset)});

  // wrap in lambda for function context
  auto pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{dummy_arg});
  auto expr = make_expr(ast::expression_lambda{pos(), std::move(pattern), std::move(let_expr)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should have __makeAttrs call
  REQUIRE(wat.find("__makeAttrs") != std::string::npos);
}

TEST_CASE("compile attrset with inherit from expression", "[compiler][attrset][inherit]") {
  ast::symbol_table symbols;

  // _: let src = { x = 1; y = 2; }; in { inherit (src) x; }
  // inherit (src) x pulls x from the src attrset
  auto src_name = symbols.intern("src");
  auto x_name = symbols.intern("x");
  auto y_name = symbols.intern("y");
  auto dummy_arg = symbols.intern("_");

  // inner attrset: { x = 1; y = 2; }
  ast::attribute_path x_path;
  x_path.segments.push_back(ast::attribute_name{pos(), x_name});
  ast::attribute_path y_path;
  y_path.segments.push_back(ast::attribute_name{pos(), y_name});
  std::vector<ast::binding_variant> inner_bindings;
  inner_bindings.push_back(ast::binding_attribute{pos(), std::move(x_path),
                                                  make_expr(ast::expression_integer{pos(), 1})});
  inner_bindings.push_back(ast::binding_attribute{pos(), std::move(y_path),
                                                  make_expr(ast::expression_integer{pos(), 2})});
  auto inner_attrset =
      make_expr(ast::expression_attribute_set{pos(), false, std::move(inner_bindings)});

  // let binding: src = { x = 1; y = 2; }
  ast::attribute_path src_path;
  src_path.segments.push_back(ast::attribute_name{pos(), src_name});
  ast::binding_attribute src_binding{pos(), std::move(src_path), std::move(inner_attrset)};
  std::vector<ast::binding_variant> let_bindings;
  let_bindings.push_back(std::move(src_binding));

  // attrset with inherit (src) x
  std::vector<ast::attribute_name> inherit_attrs;
  inherit_attrs.push_back(ast::attribute_name{pos(), x_name});
  auto src_ref = make_expr(ast::expression_identifier{pos(), src_name});
  ast::binding_inherit inherit_binding{pos(), std::move(src_ref), std::move(inherit_attrs)};
  std::vector<ast::binding_variant> result_bindings;
  result_bindings.push_back(std::move(inherit_binding));
  auto result_attrset =
      make_expr(ast::expression_attribute_set{pos(), false, std::move(result_bindings)});

  // let src = ...; in { inherit (src) x; }
  auto let_expr =
      make_expr(ast::expression_let{pos(), std::move(let_bindings), std::move(result_attrset)});

  // wrap in lambda for function context
  auto pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{dummy_arg});
  auto expr = make_expr(ast::expression_lambda{pos(), std::move(pattern), std::move(let_expr)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should have __select call to extract attribute from src
  REQUIRE(wat.find("__select") != std::string::npos);
  REQUIRE(wat.find("__makeAttrs") != std::string::npos);
}

TEST_CASE("compile attrset with multiple inherit attrs from expression",
          "[compiler][attrset][inherit]") {
  ast::symbol_table symbols;

  // _: let src = { a = 1; b = 2; }; in { inherit (src) a b; }
  auto src_name = symbols.intern("src");
  auto a_name = symbols.intern("a");
  auto b_name = symbols.intern("b");
  auto dummy_arg = symbols.intern("_");

  // inner attrset: { a = 1; b = 2; }
  ast::attribute_path a_path;
  a_path.segments.push_back(ast::attribute_name{pos(), a_name});
  ast::attribute_path b_path;
  b_path.segments.push_back(ast::attribute_name{pos(), b_name});
  std::vector<ast::binding_variant> inner_bindings;
  inner_bindings.push_back(ast::binding_attribute{pos(), std::move(a_path),
                                                  make_expr(ast::expression_integer{pos(), 1})});
  inner_bindings.push_back(ast::binding_attribute{pos(), std::move(b_path),
                                                  make_expr(ast::expression_integer{pos(), 2})});
  auto inner_attrset =
      make_expr(ast::expression_attribute_set{pos(), false, std::move(inner_bindings)});

  // let binding: src = { a = 1; b = 2; }
  ast::attribute_path src_path;
  src_path.segments.push_back(ast::attribute_name{pos(), src_name});
  ast::binding_attribute src_binding{pos(), std::move(src_path), std::move(inner_attrset)};
  std::vector<ast::binding_variant> let_bindings;
  let_bindings.push_back(std::move(src_binding));

  // attrset with inherit (src) a b (multiple attributes)
  std::vector<ast::attribute_name> inherit_attrs;
  inherit_attrs.push_back(ast::attribute_name{pos(), a_name});
  inherit_attrs.push_back(ast::attribute_name{pos(), b_name});
  auto src_ref = make_expr(ast::expression_identifier{pos(), src_name});
  ast::binding_inherit inherit_binding{pos(), std::move(src_ref), std::move(inherit_attrs)};
  std::vector<ast::binding_variant> result_bindings;
  result_bindings.push_back(std::move(inherit_binding));
  auto result_attrset =
      make_expr(ast::expression_attribute_set{pos(), false, std::move(result_bindings)});

  // let src = ...; in { inherit (src) a b; }
  auto let_expr =
      make_expr(ast::expression_let{pos(), std::move(let_bindings), std::move(result_attrset)});

  // wrap in lambda for function context
  auto pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{dummy_arg});
  auto expr = make_expr(ast::expression_lambda{pos(), std::move(pattern), std::move(let_expr)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should have __select calls for both attributes
  REQUIRE(wat.find("__select") != std::string::npos);
}

// =============================================================================
// inherit bindings in let expressions
// =============================================================================

TEST_CASE("compile let with inherit from outer scope", "[compiler][let][inherit]") {
  ast::symbol_table symbols;

  // _: let outer = 10; in let inherit outer; in outer
  auto outer_name = symbols.intern("outer");
  auto dummy_arg = symbols.intern("_");

  // outer let binding: outer = 10
  ast::attribute_path outer_path;
  outer_path.segments.push_back(ast::attribute_name{pos(), outer_name});
  ast::binding_attribute outer_binding{pos(), std::move(outer_path),
                                       make_expr(ast::expression_integer{pos(), 10})};
  std::vector<ast::binding_variant> outer_let_bindings;
  outer_let_bindings.push_back(std::move(outer_binding));

  // inner let binding: inherit outer
  std::vector<ast::attribute_name> inherit_attrs;
  inherit_attrs.push_back(ast::attribute_name{pos(), outer_name});
  ast::binding_inherit inherit_binding{pos(), std::nullopt, std::move(inherit_attrs)};
  std::vector<ast::binding_variant> inner_let_bindings;
  inner_let_bindings.push_back(std::move(inherit_binding));

  // inner body: outer (reference to inherited binding)
  auto outer_ref = make_expr(ast::expression_identifier{pos(), outer_name});

  // inner let: let inherit outer; in outer
  auto inner_let =
      make_expr(ast::expression_let{pos(), std::move(inner_let_bindings), std::move(outer_ref)});

  // outer let: let outer = 10; in (inner let)
  auto outer_let =
      make_expr(ast::expression_let{pos(), std::move(outer_let_bindings), std::move(inner_let)});

  // wrap in lambda for function context
  auto pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{dummy_arg});
  auto expr = make_expr(ast::expression_lambda{pos(), std::move(pattern), std::move(outer_let)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());
}

TEST_CASE("compile let with inherit from expression", "[compiler][let][inherit]") {
  ast::symbol_table symbols;

  // _: let src = { x = 5; }; in let inherit (src) x; in x
  auto src_name = symbols.intern("src");
  auto x_name = symbols.intern("x");
  auto dummy_arg = symbols.intern("_");

  // src attrset: { x = 5; }
  ast::attribute_path x_path;
  x_path.segments.push_back(ast::attribute_name{pos(), x_name});
  std::vector<ast::binding_variant> src_bindings;
  src_bindings.push_back(ast::binding_attribute{pos(), std::move(x_path),
                                                make_expr(ast::expression_integer{pos(), 5})});
  auto src_attrset =
      make_expr(ast::expression_attribute_set{pos(), false, std::move(src_bindings)});

  // outer let binding: src = { x = 5; }
  ast::attribute_path src_path;
  src_path.segments.push_back(ast::attribute_name{pos(), src_name});
  ast::binding_attribute src_binding{pos(), std::move(src_path), std::move(src_attrset)};
  std::vector<ast::binding_variant> outer_let_bindings;
  outer_let_bindings.push_back(std::move(src_binding));

  // inner let binding: inherit (src) x
  std::vector<ast::attribute_name> inherit_attrs;
  inherit_attrs.push_back(ast::attribute_name{pos(), x_name});
  auto src_ref = make_expr(ast::expression_identifier{pos(), src_name});
  ast::binding_inherit inherit_binding{pos(), std::move(src_ref), std::move(inherit_attrs)};
  std::vector<ast::binding_variant> inner_let_bindings;
  inner_let_bindings.push_back(std::move(inherit_binding));

  // inner body: x (reference to inherited binding)
  auto x_ref = make_expr(ast::expression_identifier{pos(), x_name});

  // inner let: let inherit (src) x; in x
  auto inner_let =
      make_expr(ast::expression_let{pos(), std::move(inner_let_bindings), std::move(x_ref)});

  // outer let: let src = ...; in (inner let)
  auto outer_let =
      make_expr(ast::expression_let{pos(), std::move(outer_let_bindings), std::move(inner_let)});

  // wrap in lambda for function context
  auto pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{dummy_arg});
  auto expr = make_expr(ast::expression_lambda{pos(), std::move(pattern), std::move(outer_let)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should have __select call to extract x from src
  REQUIRE(wat.find("__select") != std::string::npos);
}

// =============================================================================
// multi-segment attribute paths
// =============================================================================

TEST_CASE("compile attrset with two-segment path", "[compiler][attrset][multi-segment]") {
  ast::symbol_table symbols;

  // { a.b = 42; } should create { a = { b = 42; }; }
  ast::attribute_path path;
  path.segments.push_back(ast::attribute_name{pos(), symbols.intern("a")});
  path.segments.push_back(ast::attribute_name{pos(), symbols.intern("b")});

  auto value = make_expr(ast::expression_integer{pos(), 42});
  ast::binding_attribute binding{pos(), std::move(path), std::move(value)};

  std::vector<ast::binding_variant> bindings;
  bindings.push_back(std::move(binding));

  auto expr = make_expr(ast::expression_attribute_set{pos(), false, std::move(bindings)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should call __makeAttrs twice (once for outer { a = ...; }, once for inner { b = 42; })
  REQUIRE(wat.find("__makeAttrs") != std::string::npos);
}

TEST_CASE("compile attrset with three-segment path", "[compiler][attrset][multi-segment]") {
  ast::symbol_table symbols;

  // { a.b.c = 1; } should create { a = { b = { c = 1; }; }; }
  ast::attribute_path path;
  path.segments.push_back(ast::attribute_name{pos(), symbols.intern("a")});
  path.segments.push_back(ast::attribute_name{pos(), symbols.intern("b")});
  path.segments.push_back(ast::attribute_name{pos(), symbols.intern("c")});

  auto value = make_expr(ast::expression_integer{pos(), 1});
  ast::binding_attribute binding{pos(), std::move(path), std::move(value)};

  std::vector<ast::binding_variant> bindings;
  bindings.push_back(std::move(binding));

  auto expr = make_expr(ast::expression_attribute_set{pos(), false, std::move(bindings)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should have multiple __makeAttrs calls for nested structure
  REQUIRE(wat.find("__makeAttrs") != std::string::npos);
}

TEST_CASE("compile attrset with mixed single and multi-segment paths",
          "[compiler][attrset][multi-segment]") {
  ast::symbol_table symbols;

  // { x = 1; a.b = 2; } should create { x = 1; a = { b = 2; }; }
  ast::attribute_path path1;
  path1.segments.push_back(ast::attribute_name{pos(), symbols.intern("x")});
  auto value1 = make_expr(ast::expression_integer{pos(), 1});
  ast::binding_attribute binding1{pos(), std::move(path1), std::move(value1)};

  ast::attribute_path path2;
  path2.segments.push_back(ast::attribute_name{pos(), symbols.intern("a")});
  path2.segments.push_back(ast::attribute_name{pos(), symbols.intern("b")});
  auto value2 = make_expr(ast::expression_integer{pos(), 2});
  ast::binding_attribute binding2{pos(), std::move(path2), std::move(value2)};

  std::vector<ast::binding_variant> bindings;
  bindings.push_back(std::move(binding1));
  bindings.push_back(std::move(binding2));

  auto expr = make_expr(ast::expression_attribute_set{pos(), false, std::move(bindings)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());
}

TEST_CASE("compile attrset rejects merging multi-segment paths",
          "[compiler][attrset][multi-segment]") {
  ast::symbol_table symbols;

  // { a.b = 1; a.c = 2; } should throw because we don't support merging yet
  ast::attribute_path path1;
  path1.segments.push_back(ast::attribute_name{pos(), symbols.intern("a")});
  path1.segments.push_back(ast::attribute_name{pos(), symbols.intern("b")});
  auto value1 = make_expr(ast::expression_integer{pos(), 1});
  ast::binding_attribute binding1{pos(), std::move(path1), std::move(value1)};

  ast::attribute_path path2;
  path2.segments.push_back(ast::attribute_name{pos(), symbols.intern("a")});
  path2.segments.push_back(ast::attribute_name{pos(), symbols.intern("c")});
  auto value2 = make_expr(ast::expression_integer{pos(), 2});
  ast::binding_attribute binding2{pos(), std::move(path2), std::move(value2)};

  std::vector<ast::binding_variant> bindings;
  bindings.push_back(std::move(binding1));
  bindings.push_back(std::move(binding2));

  auto expr = make_expr(ast::expression_attribute_set{pos(), false, std::move(bindings)});

  compile::compiler comp(symbols);

  REQUIRE_THROWS_AS(comp.compile(expr), compile::compilation_error);
}

// =============================================================================
// multi-segment let bindings
// =============================================================================

TEST_CASE("compile let with two-segment path", "[compiler][let][multi-segment]") {
  ast::symbol_table symbols;

  // _: let a.b = 42; in a
  // should bind a to { b = 42; }
  auto a_name = symbols.intern("a");
  auto b_name = symbols.intern("b");
  auto dummy_arg = symbols.intern("_");

  ast::attribute_path path;
  path.segments.push_back(ast::attribute_name{pos(), a_name});
  path.segments.push_back(ast::attribute_name{pos(), b_name});
  auto value = make_expr(ast::expression_integer{pos(), 42});
  ast::binding_attribute binding{pos(), std::move(path), std::move(value)};

  std::vector<ast::binding_variant> let_bindings;
  let_bindings.push_back(std::move(binding));

  auto body = make_expr(ast::expression_identifier{pos(), a_name});
  auto let_expr = make_expr(ast::expression_let{pos(), std::move(let_bindings), std::move(body)});

  // wrap in lambda for function context
  auto pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{dummy_arg});
  auto expr = make_expr(ast::expression_lambda{pos(), std::move(pattern), std::move(let_expr)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should have __makeAttrs for the nested attrset
  REQUIRE(wat.find("__makeAttrs") != std::string::npos);
}

TEST_CASE("compile let with three-segment path", "[compiler][let][multi-segment]") {
  ast::symbol_table symbols;

  // _: let a.b.c = 1; in a
  auto a_name = symbols.intern("a");
  auto b_name = symbols.intern("b");
  auto c_name = symbols.intern("c");
  auto dummy_arg = symbols.intern("_");

  ast::attribute_path path;
  path.segments.push_back(ast::attribute_name{pos(), a_name});
  path.segments.push_back(ast::attribute_name{pos(), b_name});
  path.segments.push_back(ast::attribute_name{pos(), c_name});
  auto value = make_expr(ast::expression_integer{pos(), 1});
  ast::binding_attribute binding{pos(), std::move(path), std::move(value)};

  std::vector<ast::binding_variant> let_bindings;
  let_bindings.push_back(std::move(binding));

  auto body = make_expr(ast::expression_identifier{pos(), a_name});
  auto let_expr = make_expr(ast::expression_let{pos(), std::move(let_bindings), std::move(body)});

  // wrap in lambda for function context
  auto pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{dummy_arg});
  auto expr = make_expr(ast::expression_lambda{pos(), std::move(pattern), std::move(let_expr)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());
}

TEST_CASE("compile let rejects merging multi-segment paths", "[compiler][let][multi-segment]") {
  ast::symbol_table symbols;

  // _: let a.b = 1; a.c = 2; in a
  // should throw because we don't support merging yet
  auto a_name = symbols.intern("a");
  auto b_name = symbols.intern("b");
  auto c_name = symbols.intern("c");
  auto dummy_arg = symbols.intern("_");

  ast::attribute_path path1;
  path1.segments.push_back(ast::attribute_name{pos(), a_name});
  path1.segments.push_back(ast::attribute_name{pos(), b_name});
  auto value1 = make_expr(ast::expression_integer{pos(), 1});
  ast::binding_attribute binding1{pos(), std::move(path1), std::move(value1)};

  ast::attribute_path path2;
  path2.segments.push_back(ast::attribute_name{pos(), a_name});
  path2.segments.push_back(ast::attribute_name{pos(), c_name});
  auto value2 = make_expr(ast::expression_integer{pos(), 2});
  ast::binding_attribute binding2{pos(), std::move(path2), std::move(value2)};

  std::vector<ast::binding_variant> let_bindings;
  let_bindings.push_back(std::move(binding1));
  let_bindings.push_back(std::move(binding2));

  auto body = make_expr(ast::expression_identifier{pos(), a_name});
  auto let_expr = make_expr(ast::expression_let{pos(), std::move(let_bindings), std::move(body)});

  // wrap in lambda for function context
  auto pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{dummy_arg});
  auto expr = make_expr(ast::expression_lambda{pos(), std::move(pattern), std::move(let_expr)});

  compile::compiler comp(symbols);

  REQUIRE_THROWS_AS(comp.compile(expr), compile::compilation_error);
}

// =============================================================================
// with expressions
// =============================================================================

TEST_CASE("compile simple with expression", "[compiler][with]") {
  ast::symbol_table symbols;

  // _: with { x = 42; }; x
  auto x_name = symbols.intern("x");
  auto dummy_arg = symbols.intern("_");

  // namespace: { x = 42; }
  ast::attribute_path x_path;
  x_path.segments.push_back(ast::attribute_name{pos(), x_name});
  auto x_value = make_expr(ast::expression_integer{pos(), 42});
  ast::binding_attribute x_binding{pos(), std::move(x_path), std::move(x_value)};
  std::vector<ast::binding_variant> ns_bindings;
  ns_bindings.push_back(std::move(x_binding));
  auto namespace_expr =
      make_expr(ast::expression_attribute_set{pos(), false, std::move(ns_bindings)});

  // body: x
  auto body = make_expr(ast::expression_identifier{pos(), x_name});

  // with namespace; body
  auto with_expr =
      make_expr(ast::expression_with{pos(), std::move(namespace_expr), std::move(body)});

  // wrap in lambda for function context
  auto pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{dummy_arg});
  auto expr = make_expr(ast::expression_lambda{pos(), std::move(pattern), std::move(with_expr)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should have __hasAttr and __select calls for with scope lookup
  REQUIRE(wat.find("__hasAttr") != std::string::npos);
  REQUIRE(wat.find("__select") != std::string::npos);
}

TEST_CASE("compile nested with expressions", "[compiler][with]") {
  ast::symbol_table symbols;

  // _: with { x = 1; }; with { y = 2; }; x + y
  auto x_name = symbols.intern("x");
  auto y_name = symbols.intern("y");
  auto dummy_arg = symbols.intern("_");

  // outer namespace: { x = 1; }
  ast::attribute_path x_path;
  x_path.segments.push_back(ast::attribute_name{pos(), x_name});
  auto x_value = make_expr(ast::expression_integer{pos(), 1});
  ast::binding_attribute x_binding{pos(), std::move(x_path), std::move(x_value)};
  std::vector<ast::binding_variant> outer_bindings;
  outer_bindings.push_back(std::move(x_binding));
  auto outer_ns = make_expr(ast::expression_attribute_set{pos(), false, std::move(outer_bindings)});

  // inner namespace: { y = 2; }
  ast::attribute_path y_path;
  y_path.segments.push_back(ast::attribute_name{pos(), y_name});
  auto y_value = make_expr(ast::expression_integer{pos(), 2});
  ast::binding_attribute y_binding{pos(), std::move(y_path), std::move(y_value)};
  std::vector<ast::binding_variant> inner_bindings;
  inner_bindings.push_back(std::move(y_binding));
  auto inner_ns = make_expr(ast::expression_attribute_set{pos(), false, std::move(inner_bindings)});

  // body: x + y
  auto x_ref = make_expr(ast::expression_identifier{pos(), x_name});
  auto y_ref = make_expr(ast::expression_identifier{pos(), y_name});
  auto add_expr = make_expr(ast::expression_binary_operation{pos(), ast::binary_operator::add,
                                                             std::move(x_ref), std::move(y_ref)});

  // inner with: with { y = 2; }; x + y
  auto inner_with =
      make_expr(ast::expression_with{pos(), std::move(inner_ns), std::move(add_expr)});

  // outer with: with { x = 1; }; (inner_with)
  auto outer_with =
      make_expr(ast::expression_with{pos(), std::move(outer_ns), std::move(inner_with)});

  // wrap in lambda
  auto pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{dummy_arg});
  auto expr = make_expr(ast::expression_lambda{pos(), std::move(pattern), std::move(outer_with)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());
}

TEST_CASE("compile with expression shadowed by lexical binding", "[compiler][with]") {
  ast::symbol_table symbols;

  // x: with { x = 99; }; x
  // lexical x should shadow with's x
  auto x_name = symbols.intern("x");

  // namespace: { x = 99; }
  ast::attribute_path x_path;
  x_path.segments.push_back(ast::attribute_name{pos(), x_name});
  auto x_value = make_expr(ast::expression_integer{pos(), 99});
  ast::binding_attribute x_binding{pos(), std::move(x_path), std::move(x_value)};
  std::vector<ast::binding_variant> ns_bindings;
  ns_bindings.push_back(std::move(x_binding));
  auto namespace_expr =
      make_expr(ast::expression_attribute_set{pos(), false, std::move(ns_bindings)});

  // body: x (should refer to lambda argument, not with's x)
  auto body = make_expr(ast::expression_identifier{pos(), x_name});

  // with namespace; body
  auto with_expr =
      make_expr(ast::expression_with{pos(), std::move(namespace_expr), std::move(body)});

  // wrap in lambda: x: (with { x = 99; }; x)
  auto pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{x_name});
  auto expr = make_expr(ast::expression_lambda{pos(), std::move(pattern), std::move(with_expr)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // The x reference should be a local.get (argument), not a __hasAttr/__select
  // The with namespace should still be evaluated though
  REQUIRE(wat.find("local.get") != std::string::npos);
}

TEST_CASE("compile with expression with inner with shadowing", "[compiler][with]") {
  ast::symbol_table symbols;

  // _: with { x = 1; }; with { x = 2; }; x
  // inner with's x should shadow outer with's x
  auto x_name = symbols.intern("x");
  auto dummy_arg = symbols.intern("_");

  // outer namespace: { x = 1; }
  ast::attribute_path x_path1;
  x_path1.segments.push_back(ast::attribute_name{pos(), x_name});
  auto x_value1 = make_expr(ast::expression_integer{pos(), 1});
  ast::binding_attribute x_binding1{pos(), std::move(x_path1), std::move(x_value1)};
  std::vector<ast::binding_variant> outer_bindings;
  outer_bindings.push_back(std::move(x_binding1));
  auto outer_ns = make_expr(ast::expression_attribute_set{pos(), false, std::move(outer_bindings)});

  // inner namespace: { x = 2; }
  ast::attribute_path x_path2;
  x_path2.segments.push_back(ast::attribute_name{pos(), x_name});
  auto x_value2 = make_expr(ast::expression_integer{pos(), 2});
  ast::binding_attribute x_binding2{pos(), std::move(x_path2), std::move(x_value2)};
  std::vector<ast::binding_variant> inner_bindings;
  inner_bindings.push_back(std::move(x_binding2));
  auto inner_ns = make_expr(ast::expression_attribute_set{pos(), false, std::move(inner_bindings)});

  // body: x
  auto body = make_expr(ast::expression_identifier{pos(), x_name});

  // inner with
  auto inner_with = make_expr(ast::expression_with{pos(), std::move(inner_ns), std::move(body)});

  // outer with
  auto outer_with =
      make_expr(ast::expression_with{pos(), std::move(outer_ns), std::move(inner_with)});

  // wrap in lambda
  auto pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{dummy_arg});
  auto expr = make_expr(ast::expression_lambda{pos(), std::move(pattern), std::move(outer_with)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should have multiple __hasAttr calls (one for each with scope checked)
  // the inner with should be checked first
  REQUIRE(wat.find("__hasAttr") != std::string::npos);
}

// =============================================================================
// recursive attribute sets (rec { })
// =============================================================================

TEST_CASE("compile simple recursive attrset", "[compiler][rec]") {
  ast::symbol_table symbols;

  // _: rec { a = 1; b = 2; }
  auto a_name = symbols.intern("a");
  auto b_name = symbols.intern("b");
  auto dummy_arg = symbols.intern("_");

  ast::attribute_path a_path;
  a_path.segments.push_back(ast::attribute_name{pos(), a_name});
  auto a_value = make_expr(ast::expression_integer{pos(), 1});
  ast::binding_attribute a_binding{pos(), std::move(a_path), std::move(a_value)};

  ast::attribute_path b_path;
  b_path.segments.push_back(ast::attribute_name{pos(), b_name});
  auto b_value = make_expr(ast::expression_integer{pos(), 2});
  ast::binding_attribute b_binding{pos(), std::move(b_path), std::move(b_value)};

  std::vector<ast::binding_variant> bindings;
  bindings.push_back(std::move(a_binding));
  bindings.push_back(std::move(b_binding));

  auto rec_set = make_expr(ast::expression_attribute_set{pos(), true, std::move(bindings)});

  // wrap in lambda
  auto pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{dummy_arg});
  auto expr = make_expr(ast::expression_lambda{pos(), std::move(pattern), std::move(rec_set)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  REQUIRE(wat.find("__makeAttrs") != std::string::npos);
}

TEST_CASE("compile recursive attrset with self-reference", "[compiler][rec]") {
  ast::symbol_table symbols;

  // _: rec { a = 1; b = a + 1; }
  // b references a from the same attrset
  auto a_name = symbols.intern("a");
  auto b_name = symbols.intern("b");
  auto dummy_arg = symbols.intern("_");

  ast::attribute_path a_path;
  a_path.segments.push_back(ast::attribute_name{pos(), a_name});
  auto a_value = make_expr(ast::expression_integer{pos(), 1});
  ast::binding_attribute a_binding{pos(), std::move(a_path), std::move(a_value)};

  ast::attribute_path b_path;
  b_path.segments.push_back(ast::attribute_name{pos(), b_name});
  // b = a + 1
  auto a_ref = make_expr(ast::expression_identifier{pos(), a_name});
  auto one = make_expr(ast::expression_integer{pos(), 1});
  auto b_value = make_expr(ast::expression_binary_operation{pos(), ast::binary_operator::add,
                                                            std::move(a_ref), std::move(one)});
  ast::binding_attribute b_binding{pos(), std::move(b_path), std::move(b_value)};

  std::vector<ast::binding_variant> bindings;
  bindings.push_back(std::move(a_binding));
  bindings.push_back(std::move(b_binding));

  auto rec_set = make_expr(ast::expression_attribute_set{pos(), true, std::move(bindings)});

  // wrap in lambda
  auto pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{dummy_arg});
  auto expr = make_expr(ast::expression_lambda{pos(), std::move(pattern), std::move(rec_set)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());

  auto wat = module.emit_text();
  // should have local.get for reading 'a' when computing 'b'
  REQUIRE(wat.find("local.get") != std::string::npos);
}

TEST_CASE("compile recursive attrset with mutual reference", "[compiler][rec]") {
  ast::symbol_table symbols;

  // _: rec { a = b; b = 42; }
  // a references b, b is defined after a
  auto a_name = symbols.intern("a");
  auto b_name = symbols.intern("b");
  auto dummy_arg = symbols.intern("_");

  ast::attribute_path a_path;
  a_path.segments.push_back(ast::attribute_name{pos(), a_name});
  auto a_value = make_expr(ast::expression_identifier{pos(), b_name}); // a = b
  ast::binding_attribute a_binding{pos(), std::move(a_path), std::move(a_value)};

  ast::attribute_path b_path;
  b_path.segments.push_back(ast::attribute_name{pos(), b_name});
  auto b_value = make_expr(ast::expression_integer{pos(), 42}); // b = 42
  ast::binding_attribute b_binding{pos(), std::move(b_path), std::move(b_value)};

  std::vector<ast::binding_variant> bindings;
  bindings.push_back(std::move(a_binding));
  bindings.push_back(std::move(b_binding));

  auto rec_set = make_expr(ast::expression_attribute_set{pos(), true, std::move(bindings)});

  // wrap in lambda
  auto pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{dummy_arg});
  auto expr = make_expr(ast::expression_lambda{pos(), std::move(pattern), std::move(rec_set)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());
}

TEST_CASE("compile recursive attrset with inherit from outer", "[compiler][rec]") {
  ast::symbol_table symbols;

  // x: rec { inherit x; y = x + 1; }
  // inherits x from outer scope, y references the inherited x
  auto x_name = symbols.intern("x");
  auto y_name = symbols.intern("y");

  // inherit x;
  std::vector<ast::attribute_name> inherit_attrs;
  inherit_attrs.push_back(ast::attribute_name{pos(), x_name});
  ast::binding_inherit inherit_binding{pos(), std::nullopt, std::move(inherit_attrs)};

  // y = x + 1
  ast::attribute_path y_path;
  y_path.segments.push_back(ast::attribute_name{pos(), y_name});
  auto x_ref = make_expr(ast::expression_identifier{pos(), x_name});
  auto one = make_expr(ast::expression_integer{pos(), 1});
  auto y_value = make_expr(ast::expression_binary_operation{pos(), ast::binary_operator::add,
                                                            std::move(x_ref), std::move(one)});
  ast::binding_attribute y_binding{pos(), std::move(y_path), std::move(y_value)};

  std::vector<ast::binding_variant> bindings;
  bindings.push_back(std::move(inherit_binding));
  bindings.push_back(std::move(y_binding));

  auto rec_set = make_expr(ast::expression_attribute_set{pos(), true, std::move(bindings)});

  // wrap in lambda: x: rec { inherit x; y = x + 1; }
  auto pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{x_name});
  auto expr = make_expr(ast::expression_lambda{pos(), std::move(pattern), std::move(rec_set)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());
}

TEST_CASE("compile empty recursive attrset", "[compiler][rec]") {
  ast::symbol_table symbols;

  // _: rec { }
  auto dummy_arg = symbols.intern("_");

  std::vector<ast::binding_variant> bindings;
  auto rec_set = make_expr(ast::expression_attribute_set{pos(), true, std::move(bindings)});

  // wrap in lambda
  auto pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{dummy_arg});
  auto expr = make_expr(ast::expression_lambda{pos(), std::move(pattern), std::move(rec_set)});

  compile::compiler comp(symbols);
  auto module = comp.compile(expr);

  REQUIRE(module.validate());
}
