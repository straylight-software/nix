// straylight // nix-language // tests
//
// Unit tests for AST types and symbol table

// Catch2 must be included before rapidcheck/catch.h for v3 compatibility
// clang-format off
#include <catch2/catch_test_macros.hpp>
// clang-format on

#include <memory>
#include <string>
#include <variant>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include "straylight/nix/compiler/ast/expression.h"
#include "straylight/nix/compiler/ast/symbol_table.h"

namespace ast = straylight::nix::compiler::ast;

// =============================================================================
// symbol_table tests
// =============================================================================

TEST_CASE("symbol_table interns strings", "[ast][symbol]") {
  ast::symbol_table symbols;

  auto s1 = symbols.intern("hello");
  auto s2 = symbols.intern("world");
  auto s3 = symbols.intern("hello");

  REQUIRE(s1 == s3);
  REQUIRE(s1 != s2);
}

TEST_CASE("symbol_table lookup returns original string", "[ast][symbol]") {
  ast::symbol_table symbols;

  auto sym = symbols.intern("test_string");
  auto retrieved = symbols.lookup(sym);

  REQUIRE(retrieved == "test_string");
}

TEST_CASE("symbol_table size tracks interned count", "[ast][symbol]") {
  ast::symbol_table symbols;

  REQUIRE(symbols.size() == 0);

  symbols.intern("a");
  REQUIRE(symbols.size() == 1);

  symbols.intern("b");
  REQUIRE(symbols.size() == 2);

  // duplicate doesn't increase size
  symbols.intern("a");
  REQUIRE(symbols.size() == 2);
}

TEST_CASE("symbol_table well-known symbols", "[ast][symbol]") {
  ast::symbol_table symbols;

  auto sym_true = symbols.symbol_true();
  auto sym_false = symbols.symbol_false();
  auto sym_null = symbols.symbol_null();
  auto sym_or = symbols.symbol_or();

  REQUIRE(symbols.lookup(sym_true) == "true");
  REQUIRE(symbols.lookup(sym_false) == "false");
  REQUIRE(symbols.lookup(sym_null) == "null");
  REQUIRE(symbols.lookup(sym_or) == "or");

  // calling again returns same symbol
  REQUIRE(symbols.symbol_true() == sym_true);
}

TEST_CASE("symbol comparison operators", "[ast][symbol]") {
  ast::symbol_table symbols;

  auto a = symbols.intern("aaa");
  auto b = symbols.intern("bbb");
  auto a2 = symbols.intern("aaa");

  REQUIRE(a == a2);
  REQUIRE(a != b);
  REQUIRE(a < b); // lexicographic by index, not string content
}

// =============================================================================
// source_position tests
// =============================================================================

TEST_CASE("source_position basic construction", "[ast][position]") {
  ast::source_position pos{100, 5, 10};

  REQUIRE(pos.byte_offset_ == 100);
  REQUIRE(pos.line_ == 5);
  REQUIRE(pos.column_ == 10);
}

// =============================================================================
// expression type tests
// =============================================================================

TEST_CASE("expression_integer construction", "[ast][expr]") {
  ast::source_position pos{0, 1, 1};
  ast::expression_integer expr{pos, 42};

  REQUIRE(expr.position_.byte_offset_ == 0);
  REQUIRE(expr.value_ == 42);
}

TEST_CASE("expression_integer negative values", "[ast][expr]") {
  ast::source_position pos{0, 1, 1};
  ast::expression_integer expr{pos, -9223372036854775807LL};

  REQUIRE(expr.value_ == -9223372036854775807LL);
}

TEST_CASE("expression_float construction", "[ast][expr]") {
  ast::source_position pos{0, 1, 1};
  ast::expression_float expr{pos, 3.14159};

  REQUIRE(expr.value_ == 3.14159);
}

TEST_CASE("expression_string construction", "[ast][expr]") {
  ast::source_position pos{0, 1, 1};
  ast::expression_string expr{pos, "hello world"};

  REQUIRE(expr.value_ == "hello world");
}

TEST_CASE("expression_string with special characters", "[ast][expr]") {
  ast::source_position pos{0, 1, 1};
  ast::expression_string expr{pos, "line1\nline2\ttab"};

  REQUIRE(expr.value_ == "line1\nline2\ttab");
}

TEST_CASE("expression_identifier construction", "[ast][expr]") {
  ast::symbol_table symbols;
  auto name = symbols.intern("myVar");

  ast::source_position pos{0, 1, 1};
  ast::expression_identifier expr{pos, name};

  REQUIRE(symbols.lookup(expr.name_) == "myVar");
}

TEST_CASE("expression_list construction", "[ast][expr]") {
  ast::source_position pos{0, 1, 1};

  std::vector<ast::expression> elements;
  elements.push_back(std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_integer{pos, 1}}));
  elements.push_back(std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_integer{pos, 2}}));
  elements.push_back(std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_integer{pos, 3}}));

  ast::expression_list list{pos, std::move(elements)};

  REQUIRE(list.elements_.size() == 3);
}

TEST_CASE("expression_binary_operation construction", "[ast][expr]") {
  ast::source_position pos{0, 1, 1};

  auto left = std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_integer{pos, 1}});
  auto right = std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_integer{pos, 2}});

  ast::expression_binary_operation binop{pos, ast::binary_operator::add, std::move(left),
                                         std::move(right)};

  REQUIRE(binop.op_ == ast::binary_operator::add);
}

TEST_CASE("expression_unary_operation construction", "[ast][expr]") {
  ast::source_position pos{0, 1, 1};

  auto operand = std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_integer{pos, 42}});

  ast::expression_unary_operation unop{pos, ast::unary_operator::negate, std::move(operand)};

  REQUIRE(unop.op_ == ast::unary_operator::negate);
}

// =============================================================================
// expression_node and variant tests
// =============================================================================

TEST_CASE("expression_node wraps variants correctly", "[ast][expr]") {
  ast::source_position pos{0, 1, 1};

  auto node = std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_integer{pos, 42}});

  REQUIRE(std::holds_alternative<expression_integer>(node->data_));

  auto& int_expr = std::get<expression_integer>(node->data_);
  REQUIRE(int_expr.value_ == 42);
}

TEST_CASE("get_position extracts position from any expression", "[ast][expr]") {
  ast::source_position pos1{10, 2, 5};
  ast::source_position pos2{20, 3, 10};

  auto int_expr = std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_integer{pos1, 42}});
  auto str_expr =
      std::make_unique<expression_node>(expression_variant{expression_string{pos2, "test"}});

  REQUIRE(ast::get_position(int_expr).byte_offset_ == 10);
  REQUIRE(ast::get_position(str_expr).byte_offset_ == 20);
}

// =============================================================================
// attribute_path tests
// =============================================================================

TEST_CASE("attribute_path with static segments", "[ast][attr]") {
  ast::symbol_table symbols;

  ast::attribute_path path;
  path.segments_.push_back(ast::attribute_name{{0, 0, 0}, symbols.intern("foo")});
  path.segments_.push_back(ast::attribute_name{{0, 0, 0}, symbols.intern("bar")});
  path.segments_.push_back(ast::attribute_name{{0, 0, 0}, symbols.intern("baz")});

  REQUIRE(path.segments_.size() == 3);
  REQUIRE_FALSE(path.segments_[0].is_dynamic());
  REQUIRE_FALSE(path.segments_[1].is_dynamic());
  REQUIRE_FALSE(path.segments_[2].is_dynamic());
}

TEST_CASE("attribute_path with dynamic segment", "[ast][attr]") {
  ast::source_position pos{0, 1, 1};

  auto dynamic_expr =
      std::make_unique<expression_node>(expression_variant{expression_string{pos, "dynamic_key"}});

  ast::attribute_path path;
  path.segments_.push_back(ast::attribute_name{pos, std::move(dynamic_expr)});

  REQUIRE(path.segments_.size() == 1);
  REQUIRE(path.segments_[0].is_dynamic());
}

// =============================================================================
// pattern tests
// =============================================================================

TEST_CASE("pattern_simple construction", "[ast][pattern]") {
  ast::symbol_table symbols;
  auto arg = symbols.intern("x");

  ast::pattern_simple simple{arg};

  REQUIRE(symbols.lookup(simple.argument_name_) == "x");
}

TEST_CASE("pattern_attrset construction", "[ast][pattern]") {
  ast::symbol_table symbols;
  ast::source_position pos{0, 1, 1};

  std::vector<ast::formal_parameter> formals;
  formals.push_back(ast::formal_parameter{pos, symbols.intern("a"), std::nullopt});
  formals.push_back(ast::formal_parameter{pos, symbols.intern("b"), std::nullopt});

  ast::pattern_attrset pattern{symbols.intern("args"), std::move(formals), true};

  REQUIRE(pattern.argument_name_.has_value());
  REQUIRE(symbols.lookup(*pattern.argument_name_) == "args");
  REQUIRE(pattern.formals_.size() == 2);
  REQUIRE(pattern.has_ellipsis_);
}

TEST_CASE("pattern_attrset with default values", "[ast][pattern]") {
  ast::symbol_table symbols;
  ast::source_position pos{0, 1, 1};

  auto default_value = std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_integer{pos, 42}});

  std::vector<ast::formal_parameter> formals;
  formals.push_back(ast::formal_parameter{pos, symbols.intern("x"), std::move(default_value)});

  ast::pattern_attrset pattern{std::nullopt, std::move(formals), false};

  REQUIRE_FALSE(pattern.argument_name_.has_value());
  REQUIRE(pattern.formals_.size() == 1);
  REQUIRE(pattern.formals_[0].default_value_.has_value());
  REQUIRE_FALSE(pattern.has_ellipsis_);
}

// =============================================================================
// binding tests
// =============================================================================

TEST_CASE("binding_attribute construction", "[ast][binding]") {
  ast::symbol_table symbols;
  ast::source_position pos{0, 1, 1};

  ast::attribute_path path;
  path.segments_.push_back(ast::attribute_name{pos, symbols.intern("x")});

  auto value = std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_integer{pos, 42}});

  ast::binding_attribute binding{pos, std::move(path), std::move(value)};

  REQUIRE(binding.path_.segments_.size() == 1);
}

TEST_CASE("binding_inherit construction", "[ast][binding]") {
  ast::symbol_table symbols;
  ast::source_position pos{0, 1, 1};

  std::vector<ast::attribute_name> attrs;
  attrs.push_back(ast::attribute_name{pos, symbols.intern("a")});
  attrs.push_back(ast::attribute_name{pos, symbols.intern("b")});

  ast::binding_inherit inherit{pos, std::nullopt, std::move(attrs)};

  REQUIRE_FALSE(inherit.from_expression_.has_value());
  REQUIRE(inherit.attributes_.size() == 2);
}

TEST_CASE("binding_inherit with from expression", "[ast][binding]") {
  ast::symbol_table symbols;
  ast::source_position pos{0, 1, 1};

  auto from = std::make_unique<expression_node>(
      expression_variant{expression_identifier{pos, symbols.intern("pkg")}});

  std::vector<ast::attribute_name> attrs;
  attrs.push_back(ast::attribute_name{pos, symbols.intern("lib")});

  ast::binding_inherit inherit{pos, std::move(from), std::move(attrs)};

  REQUIRE(inherit.from_expression_.has_value());
  REQUIRE(inherit.attributes_.size() == 1);
}

// =============================================================================
// complex expression tests
// =============================================================================

TEST_CASE("expression_attribute_set construction", "[ast][expr]") {
  ast::symbol_table symbols;
  ast::source_position pos{0, 1, 1};

  ast::attribute_path path;
  path.segments_.push_back(ast::attribute_name{pos, symbols.intern("x")});

  auto value = std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_integer{pos, 42}});

  std::vector<ast::binding_variant> bindings;
  bindings.push_back(ast::binding_attribute{pos, std::move(path), std::move(value)});

  ast::expression_attribute_set attrset{pos, false, std::move(bindings)};

  REQUIRE_FALSE(attrset.is_recursive_);
  REQUIRE(attrset.bindings_.size() == 1);
}

TEST_CASE("expression_attribute_set recursive", "[ast][expr]") {
  ast::source_position pos{0, 1, 1};

  ast::expression_attribute_set attrset{pos, true, {}};

  REQUIRE(attrset.is_recursive_);
}

TEST_CASE("expression_let construction", "[ast][expr]") {
  ast::symbol_table symbols;
  ast::source_position pos{0, 1, 1};

  ast::attribute_path path;
  path.segments_.push_back(ast::attribute_name{pos, symbols.intern("x")});

  auto binding_value = std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_integer{pos, 1}});

  std::vector<ast::binding_variant> bindings;
  bindings.push_back(ast::binding_attribute{pos, std::move(path), std::move(binding_value)});

  auto body = std::make_unique<expression_node>(
      expression_variant{expression_identifier{pos, symbols.intern("x")}});

  ast::expression_let let_expr{pos, std::move(bindings), std::move(body)};

  REQUIRE(let_expr.bindings_.size() == 1);
}

TEST_CASE("expression_if construction", "[ast][expr]") {
  ast::source_position pos{0, 1, 1};

  auto cond = std::make_unique<expression_node>(
      ast::expression_variant{ast::expression_identifier{pos, ast::symbol{0}}});
  auto then_branch = std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_integer{pos, 1}});
  auto else_branch = std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_integer{pos, 2}});

  ast::expression_if if_expr{pos, std::move(cond), std::move(then_branch), std::move(else_branch)};

  REQUIRE(if_expr.condition_ != nullptr);
  REQUIRE(if_expr.then_branch_ != nullptr);
  REQUIRE(if_expr.else_branch_ != nullptr);
}

TEST_CASE("expression_lambda construction", "[ast][expr]") {
  ast::symbol_table symbols;
  ast::source_position pos{0, 1, 1};

  auto pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{symbols.intern("x")});
  auto body = std::make_unique<expression_node>(
      expression_variant{expression_identifier{pos, symbols.intern("x")}});

  ast::expression_lambda lambda{pos, std::move(pattern), std::move(body)};

  REQUIRE(lambda.argument_pattern_ != nullptr);
  REQUIRE(lambda.body_ != nullptr);
}

TEST_CASE("expression_application construction", "[ast][expr]") {
  ast::symbol_table symbols;
  ast::source_position pos{0, 1, 1};

  auto func = std::make_unique<expression_node>(
      expression_variant{expression_identifier{pos, symbols.intern("f")}});

  std::vector<ast::expression> args;
  args.push_back(std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_integer{pos, 1}}));
  args.push_back(std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_integer{pos, 2}}));

  ast::expression_application app{pos, std::move(func), std::move(args)};

  REQUIRE(app.arguments_.size() == 2);
}

TEST_CASE("expression_with construction", "[ast][expr]") {
  ast::symbol_table symbols;
  ast::source_position pos{0, 1, 1};

  auto ns = std::make_unique<expression_node>(
      expression_variant{expression_identifier{pos, symbols.intern("pkgs")}});
  auto body = std::make_unique<expression_node>(
      expression_variant{expression_identifier{pos, symbols.intern("hello")}});

  ast::expression_with with_expr{pos, std::move(ns), std::move(body)};

  REQUIRE(with_expr.namespace_expression_ != nullptr);
  REQUIRE(with_expr.body_ != nullptr);
}

TEST_CASE("expression_assert construction", "[ast][expr]") {
  ast::source_position pos{0, 1, 1};

  auto cond = std::make_unique<expression_node>(
      ast::expression_variant{ast::expression_identifier{pos, ast::symbol{0}}});
  auto body = std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_integer{pos, 42}});

  ast::expression_assert assert_expr{pos, std::move(cond), std::move(body)};

  REQUIRE(assert_expr.condition_ != nullptr);
  REQUIRE(assert_expr.body_ != nullptr);
}

TEST_CASE("expression_select construction", "[ast][expr]") {
  ast::symbol_table symbols;
  ast::source_position pos{0, 1, 1};

  auto subject = std::make_unique<expression_node>(
      expression_variant{expression_identifier{pos, symbols.intern("x")}});

  ast::attribute_path path;
  path.segments_.push_back(ast::attribute_name{pos, symbols.intern("foo")});

  ast::expression_select select{pos, std::move(subject), std::move(path), std::nullopt};

  REQUIRE_FALSE(select.default_value_.has_value());
}

TEST_CASE("expression_select with default", "[ast][expr]") {
  ast::symbol_table symbols;
  ast::source_position pos{0, 1, 1};

  auto subject = std::make_unique<expression_node>(
      expression_variant{expression_identifier{pos, symbols.intern("x")}});

  ast::attribute_path path;
  path.segments_.push_back(ast::attribute_name{pos, symbols.intern("foo")});

  auto default_val = std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_integer{pos, 0}});

  ast::expression_select select{pos, std::move(subject), std::move(path), std::move(default_val)};

  REQUIRE(select.default_value_.has_value());
}

// =============================================================================
// binary_operator enum tests
// =============================================================================

TEST_CASE("binary_operator enum values", "[ast][enum]") {
  REQUIRE(static_cast<std::uint8_t>(ast::binary_operator::add) !=
          static_cast<std::uint8_t>(ast::binary_operator::subtract));
  REQUIRE(static_cast<std::uint8_t>(ast::binary_operator::logical_and) !=
          static_cast<std::uint8_t>(ast::binary_operator::logical_or));
}

// =============================================================================
// property-based tests
// =============================================================================

TEST_CASE("symbol interning is idempotent", "[ast][symbol][property]") {
  rc::prop("interning the same string always returns the same symbol", []() {
    ast::symbol_table symbols;

    auto str = *rc::gen::container<std::string>(rc::gen::inRange('a', 'z'));

    auto s1 = symbols.intern(str);
    auto s2 = symbols.intern(str);

    RC_ASSERT(s1 == s2);
  });
}

TEST_CASE("different strings get different symbols", "[ast][symbol][property]") {
  rc::prop("different strings produce different symbols", []() {
    ast::symbol_table symbols;

    auto str1 = *rc::gen::nonEmpty(rc::gen::container<std::string>(rc::gen::inRange('a', 'm')));
    auto str2 = *rc::gen::nonEmpty(rc::gen::container<std::string>(rc::gen::inRange('n', 'z')));

    // only assert if strings are actually different
    RC_PRE(str1 != str2);

    auto s1 = symbols.intern(str1);
    auto s2 = symbols.intern(str2);

    RC_ASSERT(s1 != s2);
  });
}

TEST_CASE("symbol lookup roundtrips", "[ast][symbol][property]") {
  rc::prop("lookup(intern(s)) == s for all strings", []() {
    ast::symbol_table symbols;

    auto str = *rc::gen::string<std::string>();
    auto sym = symbols.intern(str);
    auto retrieved = symbols.lookup(sym);

    RC_ASSERT(retrieved == str);
  });
}

TEST_CASE("symbol table size grows monotonically", "[ast][symbol][property]") {
  rc::prop("size never decreases when adding symbols", []() {
    ast::symbol_table symbols;

    auto strings = *rc::gen::container<std::vector<std::string>>(rc::gen::string<std::string>());

    std::size_t prev_size = 0;
    for (const auto& s : strings) {
      symbols.intern(s);
      auto new_size = symbols.size();
      RC_ASSERT(new_size >= prev_size);
      prev_size = new_size;
    }
  });
}
