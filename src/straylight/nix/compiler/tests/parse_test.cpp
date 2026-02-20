// straylight // nix-language // tests
//
// Unit tests for PEGTL parser actions building AST
// Tests parse() function with full AST construction

// Catch2 must be included before rapidcheck/catch.h for v3 compatibility
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "straylight/nix/compiler/ast/expression.h"
#include "straylight/nix/compiler/ast/symbol_table.h"
#include "straylight/nix/compiler/parse/parser.h"

namespace ast = straylight::nix::compiler::ast;
namespace parse = straylight::nix::compiler::parse;

// =============================================================================
// helper: extract expression variant from unique_ptr
// =============================================================================

template <typename T>
auto get_expr(const ast::expression& expr) -> const T* {
  if (!expr) {
    return nullptr;
  }
  return std::get_if<T>(&expr->data_);
}

template <typename T>
auto require_expr(const ast::expression& expr) -> const T& {
  REQUIRE(expr);
  const T* ptr = std::get_if<T>(&expr->data_);
  REQUIRE(ptr != nullptr);
  return *ptr;
}

// =============================================================================
// integer literal tests
// =============================================================================

TEST_CASE("parse integer literals", "[parse][integer]") {
  ast::symbol_table symbols;

  SECTION("simple integers") {
    auto expr = parse::parse("42", symbols);
    const auto& integer = require_expr<ast::expression_integer>(expr);
    REQUIRE(integer.value_ == 42);
  }

  SECTION("zero") {
    auto expr = parse::parse("0", symbols);
    const auto& integer = require_expr<ast::expression_integer>(expr);
    REQUIRE(integer.value_ == 0);
  }

  SECTION("large integer") {
    auto expr = parse::parse("9223372036854775807", symbols);
    const auto& integer = require_expr<ast::expression_integer>(expr);
    REQUIRE(integer.value_ == INT64_MAX);
  }

  SECTION("integer in parentheses") {
    auto expr = parse::parse("(42)", symbols);
    const auto& integer = require_expr<ast::expression_integer>(expr);
    REQUIRE(integer.value_ == 42);
  }
}

// =============================================================================
// float literal tests
// =============================================================================

TEST_CASE("parse float literals", "[parse][float]") {
  ast::symbol_table symbols;

  SECTION("simple float") {
    auto expr = parse::parse("3.14", symbols);
    const auto& flt = require_expr<ast::expression_float>(expr);
    REQUIRE(flt.value_ == Catch::Approx(3.14));
  }

  SECTION("zero point five") {
    auto expr = parse::parse("0.5", symbols);
    const auto& flt = require_expr<ast::expression_float>(expr);
    REQUIRE(flt.value_ == Catch::Approx(0.5));
  }

  SECTION("float with exponent") {
    auto expr = parse::parse("1.5e10", symbols);
    const auto& flt = require_expr<ast::expression_float>(expr);
    REQUIRE(flt.value_ == Catch::Approx(1.5e10));
  }

  SECTION("leading dot float") {
    auto expr = parse::parse(".5", symbols);
    const auto& flt = require_expr<ast::expression_float>(expr);
    REQUIRE(flt.value_ == Catch::Approx(0.5));
  }
}

// =============================================================================
// identifier tests
// =============================================================================

TEST_CASE("parse identifiers", "[parse][identifier]") {
  ast::symbol_table symbols;

  SECTION("simple identifier") {
    auto expr = parse::parse("foo", symbols);
    const auto& id = require_expr<ast::expression_identifier>(expr);
    REQUIRE(id.name_ == symbols.intern("foo"));
  }

  SECTION("identifier with underscore") {
    auto expr = parse::parse("foo_bar", symbols);
    const auto& id = require_expr<ast::expression_identifier>(expr);
    REQUIRE(id.name_ == symbols.intern("foo_bar"));
  }

  SECTION("identifier with hyphen") {
    auto expr = parse::parse("foo-bar", symbols);
    const auto& id = require_expr<ast::expression_identifier>(expr);
    REQUIRE(id.name_ == symbols.intern("foo-bar"));
  }

  SECTION("identifier with prime") {
    auto expr = parse::parse("x'", symbols);
    const auto& id = require_expr<ast::expression_identifier>(expr);
    REQUIRE(id.name_ == symbols.intern("x'"));
  }

  SECTION("true is identifier") {
    auto expr = parse::parse("true", symbols);
    const auto& id = require_expr<ast::expression_identifier>(expr);
    REQUIRE(id.name_ == symbols.intern("true"));
  }

  SECTION("false is identifier") {
    auto expr = parse::parse("false", symbols);
    const auto& id = require_expr<ast::expression_identifier>(expr);
    REQUIRE(id.name_ == symbols.intern("false"));
  }

  SECTION("null is identifier") {
    auto expr = parse::parse("null", symbols);
    const auto& id = require_expr<ast::expression_identifier>(expr);
    REQUIRE(id.name_ == symbols.intern("null"));
  }
}

// =============================================================================
// string literal tests
// =============================================================================

TEST_CASE("parse string literals", "[parse][string]") {
  ast::symbol_table symbols;

  SECTION("empty string") {
    auto expr = parse::parse("\"\"", symbols);
    const auto& str = require_expr<ast::expression_string>(expr);
    REQUIRE(str.value_.empty());
  }

  SECTION("simple string") {
    auto expr = parse::parse("\"hello\"", symbols);
    const auto& str = require_expr<ast::expression_string>(expr);
    REQUIRE(str.value_ == "hello");
  }

  SECTION("string with spaces") {
    auto expr = parse::parse("\"hello world\"", symbols);
    const auto& str = require_expr<ast::expression_string>(expr);
    REQUIRE(str.value_ == "hello world");
  }

  SECTION("string with escape newline") {
    auto expr = parse::parse("\"hello\\nworld\"", symbols);
    const auto& str = require_expr<ast::expression_string>(expr);
    REQUIRE(str.value_ == "hello\nworld");
  }

  SECTION("string with escape tab") {
    auto expr = parse::parse("\"hello\\tworld\"", symbols);
    const auto& str = require_expr<ast::expression_string>(expr);
    REQUIRE(str.value_ == "hello\tworld");
  }

  SECTION("string with escaped backslash") {
    auto expr = parse::parse("\"hello\\\\world\"", symbols);
    const auto& str = require_expr<ast::expression_string>(expr);
    REQUIRE(str.value_ == "hello\\world");
  }

  SECTION("string with escaped quote") {
    auto expr = parse::parse("\"hello\\\"world\"", symbols);
    const auto& str = require_expr<ast::expression_string>(expr);
    REQUIRE(str.value_ == "hello\"world");
  }
}

TEST_CASE("parse interpolated strings", "[parse][string][interpolation]") {
  ast::symbol_table symbols;

  SECTION("string with single interpolation") {
    auto expr = parse::parse("\"hello ${name}\"", symbols);
    const auto& str = require_expr<ast::expression_string_interpolated>(expr);
    REQUIRE(str.parts_.size() == 2);

    // first part is literal
    const auto* literal = std::get_if<std::string>(&str.parts_[0]);
    REQUIRE(literal != nullptr);
    REQUIRE(*literal == "hello ");

    // second part is expression
    const auto* interp = std::get_if<ast::expression>(&str.parts_[1]);
    REQUIRE(interp != nullptr);
    const auto& id = require_expr<ast::expression_identifier>(*interp);
    REQUIRE(id.name_ == symbols.intern("name"));
  }

  SECTION("string with multiple interpolations") {
    auto expr = parse::parse("\"${a} and ${b}\"", symbols);
    const auto& str = require_expr<ast::expression_string_interpolated>(expr);
    REQUIRE(str.parts_.size() == 3);
  }

  SECTION("string with only interpolation") {
    auto expr = parse::parse("\"${x}\"", symbols);
    // when there's only interpolation, we still get interpolated string
    const auto& str = require_expr<ast::expression_string_interpolated>(expr);
    REQUIRE(str.parts_.size() == 1);
  }
}

// =============================================================================
// binary operator tests
// =============================================================================

TEST_CASE("parse binary arithmetic operations", "[parse][binary][arithmetic]") {
  ast::symbol_table symbols;

  SECTION("addition") {
    auto expr = parse::parse("1 + 2", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op_ == ast::binary_operator::add);

    const auto& left = require_expr<ast::expression_integer>(binop.left_);
    REQUIRE(left.value_ == 1);

    const auto& right = require_expr<ast::expression_integer>(binop.right_);
    REQUIRE(right.value_ == 2);
  }

  SECTION("subtraction") {
    auto expr = parse::parse("5 - 3", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op_ == ast::binary_operator::subtract);
  }

  SECTION("multiplication") {
    auto expr = parse::parse("2 * 3", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op_ == ast::binary_operator::multiply);
  }

  SECTION("division") {
    auto expr = parse::parse("6 / 2", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op_ == ast::binary_operator::divide);
  }
}

TEST_CASE("parse binary comparison operations", "[parse][binary][comparison]") {
  ast::symbol_table symbols;

  SECTION("less than") {
    auto expr = parse::parse("1 < 2", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op_ == ast::binary_operator::less_than);
  }

  SECTION("greater than") {
    auto expr = parse::parse("2 > 1", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op_ == ast::binary_operator::greater_than);
  }

  SECTION("less than or equal") {
    auto expr = parse::parse("1 <= 2", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op_ == ast::binary_operator::less_than_or_equal);
  }

  SECTION("greater than or equal") {
    auto expr = parse::parse("2 >= 1", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op_ == ast::binary_operator::greater_than_or_equal);
  }

  SECTION("equals") {
    auto expr = parse::parse("1 == 1", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op_ == ast::binary_operator::equals);
  }

  SECTION("not equals") {
    auto expr = parse::parse("1 != 2", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op_ == ast::binary_operator::not_equals);
  }
}

TEST_CASE("parse binary logical operations", "[parse][binary][logical]") {
  ast::symbol_table symbols;

  SECTION("logical and") {
    auto expr = parse::parse("true && false", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op_ == ast::binary_operator::logical_and);
  }

  SECTION("logical or") {
    auto expr = parse::parse("true || false", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op_ == ast::binary_operator::logical_or);
  }

  SECTION("logical implies") {
    auto expr = parse::parse("true -> false", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op_ == ast::binary_operator::logical_implies);
  }
}

TEST_CASE("parse other binary operations", "[parse][binary]") {
  ast::symbol_table symbols;

  SECTION("concatenate") {
    auto expr = parse::parse("a ++ b", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op_ == ast::binary_operator::concatenate);
  }

  SECTION("update") {
    auto expr = parse::parse("a // b", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op_ == ast::binary_operator::update);
  }
}

// =============================================================================
// operator precedence tests
// =============================================================================

TEST_CASE("parse operator precedence", "[parse][precedence]") {
  ast::symbol_table symbols;

  SECTION("multiplication binds tighter than addition") {
    // 1 + 2 * 3 should parse as 1 + (2 * 3)
    auto expr = parse::parse("1 + 2 * 3", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op_ == ast::binary_operator::add);

    const auto& left = require_expr<ast::expression_integer>(binop.left_);
    REQUIRE(left.value_ == 1);

    const auto& right = require_expr<ast::expression_binary_operation>(binop.right_);
    REQUIRE(right.op_ == ast::binary_operator::multiply);
  }

  SECTION("comparison binds looser than arithmetic") {
    // 1 + 2 < 4 should parse as (1 + 2) < 4
    auto expr = parse::parse("1 + 2 < 4", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op_ == ast::binary_operator::less_than);

    const auto& left = require_expr<ast::expression_binary_operation>(binop.left_);
    REQUIRE(left.op_ == ast::binary_operator::add);
  }

  SECTION("logical and binds tighter than logical or") {
    // a || b && c should parse as a || (b && c)
    auto expr = parse::parse("a || b && c", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op_ == ast::binary_operator::logical_or);

    const auto& right = require_expr<ast::expression_binary_operation>(binop.right_);
    REQUIRE(right.op_ == ast::binary_operator::logical_and);
  }

  SECTION("implies is right-associative") {
    // a -> b -> c should parse as a -> (b -> c)
    auto expr = parse::parse("a -> b -> c", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op_ == ast::binary_operator::logical_implies);

    const auto& id = require_expr<ast::expression_identifier>(binop.left_);
    REQUIRE(id.name_ == symbols.intern("a"));

    const auto& right = require_expr<ast::expression_binary_operation>(binop.right_);
    REQUIRE(right.op_ == ast::binary_operator::logical_implies);
  }

  SECTION("update is right-associative") {
    // a // b // c should parse as a // (b // c)
    auto expr = parse::parse("a // b // c", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op_ == ast::binary_operator::update);

    const auto& right = require_expr<ast::expression_binary_operation>(binop.right_);
    REQUIRE(right.op_ == ast::binary_operator::update);
  }

  SECTION("concatenate is right-associative") {
    // a ++ b ++ c should parse as a ++ (b ++ c)
    auto expr = parse::parse("a ++ b ++ c", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op_ == ast::binary_operator::concatenate);

    const auto& right = require_expr<ast::expression_binary_operation>(binop.right_);
    REQUIRE(right.op_ == ast::binary_operator::concatenate);
  }
}

// =============================================================================
// unary operator tests
// =============================================================================

TEST_CASE("parse unary operations", "[parse][unary]") {
  ast::symbol_table symbols;

  SECTION("logical not") {
    auto expr = parse::parse("!true", symbols);
    const auto& unop = require_expr<ast::expression_unary_operation>(expr);
    REQUIRE(unop.op_ == ast::unary_operator::logical_not);
  }

  SECTION("unary minus (desugared to 0 - x)") {
    auto expr = parse::parse("-42", symbols);
    // unary minus is desugared to 0 - x
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op_ == ast::binary_operator::subtract);

    const auto& zero = require_expr<ast::expression_integer>(binop.left_);
    REQUIRE(zero.value_ == 0);

    const auto& value = require_expr<ast::expression_integer>(binop.right_);
    REQUIRE(value.value_ == 42);
  }
}

// =============================================================================
// pipe operator tests
// =============================================================================

TEST_CASE("parse pipe operators", "[parse][pipe]") {
  ast::symbol_table symbols;

  SECTION("pipe right: x |> f becomes f(x)") {
    auto expr = parse::parse("x |> f", symbols);
    const auto& app = require_expr<ast::expression_application>(expr);

    const auto& func = require_expr<ast::expression_identifier>(app.function_);
    REQUIRE(func.name_ == symbols.intern("f"));

    REQUIRE(app.arguments_.size() == 1);
    const auto& arg = require_expr<ast::expression_identifier>(app.arguments_[0]);
    REQUIRE(arg.name_ == symbols.intern("x"));
  }

  SECTION("pipe left: f <| x becomes f(x)") {
    auto expr = parse::parse("f <| x", symbols);
    const auto& app = require_expr<ast::expression_application>(expr);

    const auto& func = require_expr<ast::expression_identifier>(app.function_);
    REQUIRE(func.name_ == symbols.intern("f"));

    REQUIRE(app.arguments_.size() == 1);
    const auto& arg = require_expr<ast::expression_identifier>(app.arguments_[0]);
    REQUIRE(arg.name_ == symbols.intern("x"));
  }
}

// =============================================================================
// has attribute tests
// =============================================================================

// TODO: has_attribute requires special handling in the converter
// (attribute path on right side, not a simple binary operand)
TEST_CASE("parse has attribute", "[parse][hasattr][!mayfail]") {
  ast::symbol_table symbols;

  SECTION("simple has attribute") {
    auto expr = parse::parse("x ? y", symbols);
    const auto& hasattr = require_expr<ast::expression_has_attribute>(expr);

    const auto& subject = require_expr<ast::expression_identifier>(hasattr.subject_);
    REQUIRE(subject.name_ == symbols.intern("x"));

    REQUIRE(hasattr.path_.segments_.size() == 1);
  }

  SECTION("has attribute with path") {
    auto expr = parse::parse("x ? a.b.c", symbols);
    const auto& hasattr = require_expr<ast::expression_has_attribute>(expr);
    REQUIRE(hasattr.path_.segments_.size() == 3);
  }
}

// =============================================================================
// source position tests
// =============================================================================

// TODO: byte_offset tracking needs work in the new pipeline
TEST_CASE("parse tracks source positions", "[parse][position][!mayfail]") {
  ast::symbol_table symbols;

  SECTION("integer position at start") {
    auto expr = parse::parse("42", symbols);
    const auto& integer = require_expr<ast::expression_integer>(expr);
    REQUIRE(integer.position_.byte_offset_ == 0);
  }

  SECTION("expression with whitespace") {
    auto expr = parse::parse("  42", symbols);
    const auto& integer = require_expr<ast::expression_integer>(expr);
    // byte offset accounts for leading whitespace
    REQUIRE(integer.position_.byte_offset_ == 2);
  }

  SECTION("multiline expression") {
    auto expr = parse::parse("\n\n42", symbols);
    const auto& integer = require_expr<ast::expression_integer>(expr);
    // two newlines = 2 bytes
    REQUIRE(integer.position_.byte_offset_ == 2);
  }
}

// =============================================================================
// list expression tests
// =============================================================================

TEST_CASE("parse list expressions", "[parse][list]") {
  ast::symbol_table symbols;

  SECTION("empty list") {
    auto expr = parse::parse("[]", symbols);
    const auto& list = require_expr<ast::expression_list>(expr);
    REQUIRE(list.elements_.empty());
  }

  SECTION("empty list with whitespace") {
    auto expr = parse::parse("[  ]", symbols);
    const auto& list = require_expr<ast::expression_list>(expr);
    REQUIRE(list.elements_.empty());
  }

  SECTION("single element list") {
    auto expr = parse::parse("[ 42 ]", symbols);
    const auto& list = require_expr<ast::expression_list>(expr);
    REQUIRE(list.elements_.size() == 1);
    const auto& elem = require_expr<ast::expression_integer>(list.elements_[0]);
    REQUIRE(elem.value_ == 42);
  }

  SECTION("multiple integer elements") {
    auto expr = parse::parse("[ 1 2 3 ]", symbols);
    const auto& list = require_expr<ast::expression_list>(expr);
    REQUIRE(list.elements_.size() == 3);

    const auto& e1 = require_expr<ast::expression_integer>(list.elements_[0]);
    REQUIRE(e1.value_ == 1);
    const auto& e2 = require_expr<ast::expression_integer>(list.elements_[1]);
    REQUIRE(e2.value_ == 2);
    const auto& e3 = require_expr<ast::expression_integer>(list.elements_[2]);
    REQUIRE(e3.value_ == 3);
  }

  SECTION("mixed element types") {
    auto expr = parse::parse("[ 1 \"hello\" 3.14 ]", symbols);
    const auto& list = require_expr<ast::expression_list>(expr);
    REQUIRE(list.elements_.size() == 3);

    const auto* int_elem = get_expr<ast::expression_integer>(list.elements_[0]);
    REQUIRE(int_elem != nullptr);
    REQUIRE(int_elem->value_ == 1);

    const auto* str_elem = get_expr<ast::expression_string>(list.elements_[1]);
    REQUIRE(str_elem != nullptr);
    REQUIRE(str_elem->value_ == "hello");

    const auto* flt_elem = get_expr<ast::expression_float>(list.elements_[2]);
    REQUIRE(flt_elem != nullptr);
    REQUIRE(flt_elem->value_ == Catch::Approx(3.14));
  }

  SECTION("nested lists") {
    auto expr = parse::parse("[ [ 1 2 ] [ 3 4 ] ]", symbols);
    const auto& outer = require_expr<ast::expression_list>(expr);
    REQUIRE(outer.elements_.size() == 2);

    const auto& inner1 = require_expr<ast::expression_list>(outer.elements_[0]);
    REQUIRE(inner1.elements_.size() == 2);
    REQUIRE(require_expr<ast::expression_integer>(inner1.elements_[0]).value_ == 1);
    REQUIRE(require_expr<ast::expression_integer>(inner1.elements_[1]).value_ == 2);

    const auto& inner2 = require_expr<ast::expression_list>(outer.elements_[1]);
    REQUIRE(inner2.elements_.size() == 2);
    REQUIRE(require_expr<ast::expression_integer>(inner2.elements_[0]).value_ == 3);
    REQUIRE(require_expr<ast::expression_integer>(inner2.elements_[1]).value_ == 4);
  }

  SECTION("list with identifiers") {
    auto expr = parse::parse("[ x y z ]", symbols);
    const auto& list = require_expr<ast::expression_list>(expr);
    REQUIRE(list.elements_.size() == 3);

    const auto& id1 = require_expr<ast::expression_identifier>(list.elements_[0]);
    REQUIRE(symbols.lookup(id1.name_) == "x");
    const auto& id2 = require_expr<ast::expression_identifier>(list.elements_[1]);
    REQUIRE(symbols.lookup(id2.name_) == "y");
    const auto& id3 = require_expr<ast::expression_identifier>(list.elements_[2]);
    REQUIRE(symbols.lookup(id3.name_) == "z");
  }

  SECTION("list with strings") {
    auto expr = parse::parse(R"([ "a" "b" "c" ])", symbols);
    const auto& list = require_expr<ast::expression_list>(expr);
    REQUIRE(list.elements_.size() == 3);

    REQUIRE(require_expr<ast::expression_string>(list.elements_[0]).value_ == "a");
    REQUIRE(require_expr<ast::expression_string>(list.elements_[1]).value_ == "b");
    REQUIRE(require_expr<ast::expression_string>(list.elements_[2]).value_ == "c");
  }

  SECTION("list with parenthesized expressions") {
    auto expr = parse::parse("[ (1 + 2) (3 * 4) ]", symbols);
    const auto& list = require_expr<ast::expression_list>(expr);
    REQUIRE(list.elements_.size() == 2);

    // First element is 1 + 2
    const auto& add = require_expr<ast::expression_binary_operation>(list.elements_[0]);
    REQUIRE(add.op_ == ast::binary_operator::add);

    // Second element is 3 * 4
    const auto& mul = require_expr<ast::expression_binary_operation>(list.elements_[1]);
    REQUIRE(mul.op_ == ast::binary_operator::multiply);
  }
}

// =============================================================================
// error handling tests
// =============================================================================

TEST_CASE("parse error handling", "[parse][error]") {
  ast::symbol_table symbols;

  SECTION("unclosed string throws") {
    REQUIRE_THROWS_AS(parse::parse("\"unclosed", symbols), parse::parse_error);
  }

  SECTION("unclosed list throws") {
    REQUIRE_THROWS_AS(parse::parse("[", symbols), parse::parse_error);
  }

  SECTION("invalid syntax throws") {
    REQUIRE_THROWS_AS(parse::parse("@invalid", symbols), parse::parse_error);
  }
}

// =============================================================================
// attribute set tests
// =============================================================================

TEST_CASE("parse attribute sets", "[parse][attrset]") {
  ast::symbol_table symbols;

  SECTION("empty attribute set") {
    auto expr = parse::parse("{}", symbols);
    const auto& attrset = require_expr<ast::expression_attribute_set>(expr);
    REQUIRE_FALSE(attrset.is_recursive_);
    REQUIRE(attrset.bindings_.empty());
  }

  SECTION("empty attribute set with whitespace") {
    auto expr = parse::parse("{  }", symbols);
    const auto& attrset = require_expr<ast::expression_attribute_set>(expr);
    REQUIRE_FALSE(attrset.is_recursive_);
    REQUIRE(attrset.bindings_.empty());
  }

  SECTION("single binding") {
    auto expr = parse::parse("{ x = 1; }", symbols);
    const auto& attrset = require_expr<ast::expression_attribute_set>(expr);
    REQUIRE_FALSE(attrset.is_recursive_);
    REQUIRE(attrset.bindings_.size() == 1);

    const auto* binding = std::get_if<ast::binding_attribute>(&attrset.bindings_[0]);
    REQUIRE(binding != nullptr);
    REQUIRE(binding->path_.segments_.size() == 1);

    const auto* name = std::get_if<ast::symbol>(&binding->path_.segments_[0].value_);
    REQUIRE(name != nullptr);
    REQUIRE(symbols.lookup(*name) == "x");

    const auto& value = require_expr<ast::expression_integer>(binding->value_);
    REQUIRE(value.value_ == 1);
  }

  SECTION("multiple bindings") {
    auto expr = parse::parse("{ x = 1; y = 2; z = 3; }", symbols);
    const auto& attrset = require_expr<ast::expression_attribute_set>(expr);
    REQUIRE(attrset.bindings_.size() == 3);
  }

  SECTION("nested path binding") {
    auto expr = parse::parse("{ a.b.c = 42; }", symbols);
    const auto& attrset = require_expr<ast::expression_attribute_set>(expr);
    REQUIRE(attrset.bindings_.size() == 1);

    const auto* binding = std::get_if<ast::binding_attribute>(&attrset.bindings_[0]);
    REQUIRE(binding != nullptr);
    REQUIRE(binding->path_.segments_.size() == 3);
  }

  SECTION("recursive attribute set") {
    auto expr = parse::parse("rec { x = 1; y = x; }", symbols);
    const auto& attrset = require_expr<ast::expression_attribute_set>(expr);
    REQUIRE(attrset.is_recursive_);
    REQUIRE(attrset.bindings_.size() == 2);
  }

  SECTION("inherit binding") {
    auto expr = parse::parse("{ inherit x; }", symbols);
    const auto& attrset = require_expr<ast::expression_attribute_set>(expr);
    REQUIRE(attrset.bindings_.size() == 1);

    const auto* inherit = std::get_if<ast::binding_inherit>(&attrset.bindings_[0]);
    REQUIRE(inherit != nullptr);
    REQUIRE_FALSE(inherit->from_expression_.has_value());
    REQUIRE(inherit->attributes_.size() == 1);
  }

  SECTION("inherit from expression") {
    auto expr = parse::parse("{ inherit (x) a b; }", symbols);
    const auto& attrset = require_expr<ast::expression_attribute_set>(expr);
    REQUIRE(attrset.bindings_.size() == 1);

    const auto* inherit = std::get_if<ast::binding_inherit>(&attrset.bindings_[0]);
    REQUIRE(inherit != nullptr);
    REQUIRE(inherit->from_expression_.has_value());
    REQUIRE(inherit->attributes_.size() == 2);
  }

  SECTION("mixed bindings") {
    auto expr = parse::parse("{ x = 1; inherit y; z = 3; }", symbols);
    const auto& attrset = require_expr<ast::expression_attribute_set>(expr);
    REQUIRE(attrset.bindings_.size() == 3);

    REQUIRE(std::holds_alternative<ast::binding_attribute>(attrset.bindings_[0]));
    REQUIRE(std::holds_alternative<ast::binding_inherit>(attrset.bindings_[1]));
    REQUIRE(std::holds_alternative<ast::binding_attribute>(attrset.bindings_[2]));
  }

  SECTION("nested attribute sets") {
    auto expr = parse::parse("{ outer = { inner = 1; }; }", symbols);
    const auto& attrset = require_expr<ast::expression_attribute_set>(expr);
    REQUIRE(attrset.bindings_.size() == 1);

    const auto* binding = std::get_if<ast::binding_attribute>(&attrset.bindings_[0]);
    REQUIRE(binding != nullptr);

    const auto& inner = require_expr<ast::expression_attribute_set>(binding->value_);
    REQUIRE(inner.bindings_.size() == 1);
  }
}

// =============================================================================
// let expression tests
// =============================================================================

TEST_CASE("parse let expressions", "[parse][let]") {
  ast::symbol_table symbols;

  SECTION("simple let") {
    auto expr = parse::parse("let x = 1; in x", symbols);
    const auto& let_expr = require_expr<ast::expression_let>(expr);
    REQUIRE(let_expr.bindings_.size() == 1);

    const auto* binding = std::get_if<ast::binding_attribute>(&let_expr.bindings_[0]);
    REQUIRE(binding != nullptr);
    REQUIRE(binding->path_.segments_.size() == 1);

    const auto& body = require_expr<ast::expression_identifier>(let_expr.body_);
    REQUIRE(symbols.lookup(body.name_) == "x");
  }

  SECTION("multiple bindings") {
    auto expr = parse::parse("let x = 1; y = 2; z = 3; in x + y + z", symbols);
    const auto& let_expr = require_expr<ast::expression_let>(expr);
    REQUIRE(let_expr.bindings_.size() == 3);
  }

  SECTION("let with inherit") {
    auto expr = parse::parse("let inherit x; in x", symbols);
    const auto& let_expr = require_expr<ast::expression_let>(expr);
    REQUIRE(let_expr.bindings_.size() == 1);

    const auto* inherit = std::get_if<ast::binding_inherit>(&let_expr.bindings_[0]);
    REQUIRE(inherit != nullptr);
    REQUIRE(inherit->attributes_.size() == 1);
  }

  SECTION("nested let") {
    auto expr = parse::parse("let x = let y = 1; in y; in x", symbols);
    const auto& outer = require_expr<ast::expression_let>(expr);
    REQUIRE(outer.bindings_.size() == 1);

    const auto* binding = std::get_if<ast::binding_attribute>(&outer.bindings_[0]);
    REQUIRE(binding != nullptr);

    const auto& inner = require_expr<ast::expression_let>(binding->value_);
    REQUIRE(inner.bindings_.size() == 1);
  }

  SECTION("let with attribute set") {
    auto expr = parse::parse("let attrs = { x = 1; }; in attrs", symbols);
    const auto& let_expr = require_expr<ast::expression_let>(expr);
    REQUIRE(let_expr.bindings_.size() == 1);

    const auto* binding = std::get_if<ast::binding_attribute>(&let_expr.bindings_[0]);
    REQUIRE(binding != nullptr);

    const auto& attrset = require_expr<ast::expression_attribute_set>(binding->value_);
    REQUIRE(attrset.bindings_.size() == 1);
  }
}

// =============================================================================
// select expression tests
// =============================================================================

TEST_CASE("parse select expressions", "[parse][select]") {
  ast::symbol_table symbols;

  SECTION("simple select") {
    auto expr = parse::parse("x.y", symbols);
    const auto& select = require_expr<ast::expression_select>(expr);

    const auto& subject = require_expr<ast::expression_identifier>(select.subject_);
    REQUIRE(symbols.lookup(subject.name_) == "x");

    REQUIRE(select.path_.segments_.size() == 1);
    const auto* name = std::get_if<ast::symbol>(&select.path_.segments_[0].value_);
    REQUIRE(name != nullptr);
    REQUIRE(symbols.lookup(*name) == "y");

    REQUIRE_FALSE(select.default_value_.has_value());
  }

  SECTION("nested select") {
    auto expr = parse::parse("a.b.c.d", symbols);
    const auto& select = require_expr<ast::expression_select>(expr);
    REQUIRE(select.path_.segments_.size() == 3);
  }

  SECTION("select with default") {
    auto expr = parse::parse("x.y or 42", symbols);
    const auto& select = require_expr<ast::expression_select>(expr);

    REQUIRE(select.path_.segments_.size() == 1);
    REQUIRE(select.default_value_.has_value());

    const auto& def = require_expr<ast::expression_integer>(*select.default_value_);
    REQUIRE(def.value_ == 42);
  }

  SECTION("select from attribute set") {
    auto expr = parse::parse("{ x = 1; }.x", symbols);
    const auto& select = require_expr<ast::expression_select>(expr);

    const auto& subject = require_expr<ast::expression_attribute_set>(select.subject_);
    REQUIRE(subject.bindings_.size() == 1);

    REQUIRE(select.path_.segments_.size() == 1);
  }

  SECTION("select in function application") {
    auto expr = parse::parse("f x.y", symbols);
    const auto& app = require_expr<ast::expression_application>(expr);
    REQUIRE(app.arguments_.size() == 1);

    const auto& arg = require_expr<ast::expression_select>(app.arguments_[0]);
    REQUIRE(arg.path_.segments_.size() == 1);
  }
}

// =============================================================================
// lambda expression tests
// =============================================================================

TEST_CASE("parse indented strings", "[parse][string]") {
  ast::symbol_table symbols;

  SECTION("simple indented string") {
    auto expr = parse::parse("''hello''", symbols);
    const auto& str = require_expr<ast::expression_string>(expr);
    REQUIRE(str.value_ == "hello");
  }

  SECTION("indented string with leading newline stripped") {
    auto expr = parse::parse("''\nhello''", symbols);
    const auto& str = require_expr<ast::expression_string>(expr);
    REQUIRE(str.value_ == "hello");
  }

  SECTION("multiline with common indentation stripped") {
    // The minimum indentation is 2 spaces, so both lines should have it stripped
    auto expr = parse::parse("''\n  line1\n  line2\n''", symbols);
    const auto& str = require_expr<ast::expression_string>(expr);
    REQUIRE(str.value_ == "line1\nline2\n");
  }

  SECTION("escape sequences") {
    // ''' produces ''
    auto expr = parse::parse("''a'''b''", symbols);
    const auto& str = require_expr<ast::expression_string>(expr);
    REQUIRE(str.value_ == "a''b");
  }

  SECTION("dollar escape") {
    // ''$ produces $
    auto expr = parse::parse("''a''$b''", symbols);
    const auto& str = require_expr<ast::expression_string>(expr);
    REQUIRE(str.value_ == "a$b");
  }

  SECTION("indented string with interpolation") {
    auto expr = parse::parse("''hello ${x}''", symbols);
    const auto& str = require_expr<ast::expression_string_interpolated>(expr);
    // Should have: ["hello ", expr(x)]
    REQUIRE(str.parts_.size() == 2);

    // First part is literal "hello "
    const auto* literal = std::get_if<std::string>(&str.parts_[0]);
    REQUIRE(literal != nullptr);
    REQUIRE(*literal == "hello ");

    // Second part is expression x
    const auto* interp = std::get_if<ast::expression>(&str.parts_[1]);
    REQUIRE(interp != nullptr);
  }

  SECTION("indented string with multiple interpolations") {
    auto expr = parse::parse("''a ${x} b ${y} c''", symbols);
    const auto& str = require_expr<ast::expression_string_interpolated>(expr);
    // Should have: ["a ", expr(x), " b ", expr(y), " c"]
    REQUIRE(str.parts_.size() == 5);

    const auto* a = std::get_if<std::string>(&str.parts_[0]);
    REQUIRE(a != nullptr);
    REQUIRE(*a == "a ");

    const auto* x = std::get_if<ast::expression>(&str.parts_[1]);
    REQUIRE(x != nullptr);

    const auto* b = std::get_if<std::string>(&str.parts_[2]);
    REQUIRE(b != nullptr);
    REQUIRE(*b == " b ");

    const auto* y = std::get_if<ast::expression>(&str.parts_[3]);
    REQUIRE(y != nullptr);

    const auto* c = std::get_if<std::string>(&str.parts_[4]);
    REQUIRE(c != nullptr);
    REQUIRE(*c == " c");
  }

  SECTION("multiline indented string with interpolation") {
    // ''
    //   a
    //   ${x}
    //   b
    // ''
    // Should strip 2-space indent from all lines
    auto expr = parse::parse("''\n  a\n  ${x}\n  b\n''", symbols);
    const auto& str = require_expr<ast::expression_string_interpolated>(expr);
    // Should have: ["a\n", expr(x), "\nb\n"]
    REQUIRE(str.parts_.size() == 3);

    const auto* a = std::get_if<std::string>(&str.parts_[0]);
    REQUIRE(a != nullptr);
    REQUIRE(*a == "a\n");

    const auto* x = std::get_if<ast::expression>(&str.parts_[1]);
    REQUIRE(x != nullptr);

    const auto* b = std::get_if<std::string>(&str.parts_[2]);
    REQUIRE(b != nullptr);
    REQUIRE(*b == "\nb\n");
  }
}

TEST_CASE("parse lambda expressions", "[parse][lambda]") {
  ast::symbol_table symbols;

  SECTION("simple lambda") {
    auto expr = parse::parse("x: x", symbols);
    const auto& lambda = require_expr<ast::expression_lambda>(expr);

    const auto* pattern = std::get_if<ast::pattern_simple>(lambda.argument_pattern_.get());
    REQUIRE(pattern != nullptr);
    REQUIRE(symbols.lookup(pattern->argument_name_) == "x");

    const auto& body = require_expr<ast::expression_identifier>(lambda.body_);
    REQUIRE(symbols.lookup(body.name_) == "x");
  }

  SECTION("attrset pattern basic") {
    auto expr = parse::parse("{ a }: a", symbols);
    const auto& lambda = require_expr<ast::expression_lambda>(expr);

    const auto* pattern = std::get_if<ast::pattern_attrset>(lambda.argument_pattern_.get());
    REQUIRE(pattern != nullptr);
    REQUIRE(pattern->formals_.size() == 1);
    REQUIRE(symbols.lookup(pattern->formals_[0].name_) == "a");
    REQUIRE_FALSE(pattern->formals_[0].default_value_.has_value());
    REQUIRE_FALSE(pattern->has_ellipsis_);
    REQUIRE_FALSE(pattern->argument_name_.has_value());
  }

  SECTION("attrset pattern with default") {
    auto expr = parse::parse("{ a ? 1 }: a", symbols);
    const auto& lambda = require_expr<ast::expression_lambda>(expr);

    const auto* pattern = std::get_if<ast::pattern_attrset>(lambda.argument_pattern_.get());
    REQUIRE(pattern != nullptr);
    REQUIRE(pattern->formals_.size() == 1);
    REQUIRE(symbols.lookup(pattern->formals_[0].name_) == "a");
    REQUIRE(pattern->formals_[0].default_value_.has_value());

    const auto& def = require_expr<ast::expression_integer>(*pattern->formals_[0].default_value_);
    REQUIRE(def.value_ == 1);
  }

  SECTION("attrset pattern with ellipsis") {
    auto expr = parse::parse("{ a, ... }: a", symbols);
    const auto& lambda = require_expr<ast::expression_lambda>(expr);

    const auto* pattern = std::get_if<ast::pattern_attrset>(lambda.argument_pattern_.get());
    REQUIRE(pattern != nullptr);
    REQUIRE(pattern->formals_.size() == 1);
    REQUIRE(pattern->has_ellipsis_);
  }

  SECTION("attrset pattern with @name after") {
    auto expr = parse::parse("{ a }@args: a", symbols);
    const auto& lambda = require_expr<ast::expression_lambda>(expr);

    const auto* pattern = std::get_if<ast::pattern_attrset>(lambda.argument_pattern_.get());
    REQUIRE(pattern != nullptr);
    REQUIRE(pattern->formals_.size() == 1);
    REQUIRE(pattern->argument_name_.has_value());
    REQUIRE(symbols.lookup(*pattern->argument_name_) == "args");
  }

  SECTION("attrset pattern with @name before") {
    auto expr = parse::parse("args@{ a }: a", symbols);
    const auto& lambda = require_expr<ast::expression_lambda>(expr);

    const auto* pattern = std::get_if<ast::pattern_attrset>(lambda.argument_pattern_.get());
    REQUIRE(pattern != nullptr);
    REQUIRE(pattern->formals_.size() == 1);
    REQUIRE(pattern->argument_name_.has_value());
    REQUIRE(symbols.lookup(*pattern->argument_name_) == "args");
  }

  SECTION("multiple formals") {
    auto expr = parse::parse("{ a, b, c }: a", symbols);
    const auto& lambda = require_expr<ast::expression_lambda>(expr);

    const auto* pattern = std::get_if<ast::pattern_attrset>(lambda.argument_pattern_.get());
    REQUIRE(pattern != nullptr);
    REQUIRE(pattern->formals_.size() == 3);
    REQUIRE(symbols.lookup(pattern->formals_[0].name_) == "a");
    REQUIRE(symbols.lookup(pattern->formals_[1].name_) == "b");
    REQUIRE(symbols.lookup(pattern->formals_[2].name_) == "c");
  }

  SECTION("complex pattern") {
    auto expr = parse::parse("{ a, b ? 2, ... }@args: a + b", symbols);
    const auto& lambda = require_expr<ast::expression_lambda>(expr);

    const auto* pattern = std::get_if<ast::pattern_attrset>(lambda.argument_pattern_.get());
    REQUIRE(pattern != nullptr);
    REQUIRE(pattern->formals_.size() == 2);
    REQUIRE_FALSE(pattern->formals_[0].default_value_.has_value());
    REQUIRE(pattern->formals_[1].default_value_.has_value());
    REQUIRE(pattern->has_ellipsis_);
    REQUIRE(pattern->argument_name_.has_value());
    REQUIRE(symbols.lookup(*pattern->argument_name_) == "args");
  }
}
