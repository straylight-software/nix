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

#include "nix-language/ast/expression.hh"
#include "nix-language/ast/symbol_table.hh"
#include "nix-language/parse/actions.hh"

namespace ast = nix::language::ast;
namespace parse = nix::language::parse;

// =============================================================================
// helper: extract expression variant from unique_ptr
// =============================================================================

template <typename T>
auto get_expr(const ast::expression& expr) -> const T* {
  if (!expr) {
    return nullptr;
  }
  return std::get_if<T>(&expr->data);
}

template <typename T>
auto require_expr(const ast::expression& expr) -> const T& {
  REQUIRE(expr);
  const T* ptr = std::get_if<T>(&expr->data);
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
    REQUIRE(integer.value == 42);
  }

  SECTION("zero") {
    auto expr = parse::parse("0", symbols);
    const auto& integer = require_expr<ast::expression_integer>(expr);
    REQUIRE(integer.value == 0);
  }

  SECTION("large integer") {
    auto expr = parse::parse("9223372036854775807", symbols);
    const auto& integer = require_expr<ast::expression_integer>(expr);
    REQUIRE(integer.value == INT64_MAX);
  }

  SECTION("integer in parentheses") {
    auto expr = parse::parse("(42)", symbols);
    const auto& integer = require_expr<ast::expression_integer>(expr);
    REQUIRE(integer.value == 42);
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
    REQUIRE(flt.value == Catch::Approx(3.14));
  }

  SECTION("zero point five") {
    auto expr = parse::parse("0.5", symbols);
    const auto& flt = require_expr<ast::expression_float>(expr);
    REQUIRE(flt.value == Catch::Approx(0.5));
  }

  SECTION("float with exponent") {
    auto expr = parse::parse("1.5e10", symbols);
    const auto& flt = require_expr<ast::expression_float>(expr);
    REQUIRE(flt.value == Catch::Approx(1.5e10));
  }

  SECTION("leading dot float") {
    auto expr = parse::parse(".5", symbols);
    const auto& flt = require_expr<ast::expression_float>(expr);
    REQUIRE(flt.value == Catch::Approx(0.5));
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
    REQUIRE(id.name == symbols.intern("foo"));
  }

  SECTION("identifier with underscore") {
    auto expr = parse::parse("foo_bar", symbols);
    const auto& id = require_expr<ast::expression_identifier>(expr);
    REQUIRE(id.name == symbols.intern("foo_bar"));
  }

  SECTION("identifier with hyphen") {
    auto expr = parse::parse("foo-bar", symbols);
    const auto& id = require_expr<ast::expression_identifier>(expr);
    REQUIRE(id.name == symbols.intern("foo-bar"));
  }

  SECTION("identifier with prime") {
    auto expr = parse::parse("x'", symbols);
    const auto& id = require_expr<ast::expression_identifier>(expr);
    REQUIRE(id.name == symbols.intern("x'"));
  }

  SECTION("true is identifier") {
    auto expr = parse::parse("true", symbols);
    const auto& id = require_expr<ast::expression_identifier>(expr);
    REQUIRE(id.name == symbols.intern("true"));
  }

  SECTION("false is identifier") {
    auto expr = parse::parse("false", symbols);
    const auto& id = require_expr<ast::expression_identifier>(expr);
    REQUIRE(id.name == symbols.intern("false"));
  }

  SECTION("null is identifier") {
    auto expr = parse::parse("null", symbols);
    const auto& id = require_expr<ast::expression_identifier>(expr);
    REQUIRE(id.name == symbols.intern("null"));
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
    REQUIRE(str.value.empty());
  }

  SECTION("simple string") {
    auto expr = parse::parse("\"hello\"", symbols);
    const auto& str = require_expr<ast::expression_string>(expr);
    REQUIRE(str.value == "hello");
  }

  SECTION("string with spaces") {
    auto expr = parse::parse("\"hello world\"", symbols);
    const auto& str = require_expr<ast::expression_string>(expr);
    REQUIRE(str.value == "hello world");
  }

  SECTION("string with escape newline") {
    auto expr = parse::parse("\"hello\\nworld\"", symbols);
    const auto& str = require_expr<ast::expression_string>(expr);
    REQUIRE(str.value == "hello\nworld");
  }

  SECTION("string with escape tab") {
    auto expr = parse::parse("\"hello\\tworld\"", symbols);
    const auto& str = require_expr<ast::expression_string>(expr);
    REQUIRE(str.value == "hello\tworld");
  }

  SECTION("string with escaped backslash") {
    auto expr = parse::parse("\"hello\\\\world\"", symbols);
    const auto& str = require_expr<ast::expression_string>(expr);
    REQUIRE(str.value == "hello\\world");
  }

  SECTION("string with escaped quote") {
    auto expr = parse::parse("\"hello\\\"world\"", symbols);
    const auto& str = require_expr<ast::expression_string>(expr);
    REQUIRE(str.value == "hello\"world");
  }
}

TEST_CASE("parse interpolated strings", "[parse][string][interpolation]") {
  ast::symbol_table symbols;

  SECTION("string with single interpolation") {
    auto expr = parse::parse("\"hello ${name}\"", symbols);
    const auto& str = require_expr<ast::expression_string_interpolated>(expr);
    REQUIRE(str.parts.size() == 2);

    // first part is literal
    const auto* literal = std::get_if<std::string>(&str.parts[0]);
    REQUIRE(literal != nullptr);
    REQUIRE(*literal == "hello ");

    // second part is expression
    const auto* interp = std::get_if<ast::expression>(&str.parts[1]);
    REQUIRE(interp != nullptr);
    const auto& id = require_expr<ast::expression_identifier>(*interp);
    REQUIRE(id.name == symbols.intern("name"));
  }

  SECTION("string with multiple interpolations") {
    auto expr = parse::parse("\"${a} and ${b}\"", symbols);
    const auto& str = require_expr<ast::expression_string_interpolated>(expr);
    REQUIRE(str.parts.size() == 3);
  }

  SECTION("string with only interpolation") {
    auto expr = parse::parse("\"${x}\"", symbols);
    // when there's only interpolation, we still get interpolated string
    const auto& str = require_expr<ast::expression_string_interpolated>(expr);
    REQUIRE(str.parts.size() == 1);
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
    REQUIRE(binop.op == ast::binary_operator::add);

    const auto& left = require_expr<ast::expression_integer>(binop.left);
    REQUIRE(left.value == 1);

    const auto& right = require_expr<ast::expression_integer>(binop.right);
    REQUIRE(right.value == 2);
  }

  SECTION("subtraction") {
    auto expr = parse::parse("5 - 3", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op == ast::binary_operator::subtract);
  }

  SECTION("multiplication") {
    auto expr = parse::parse("2 * 3", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op == ast::binary_operator::multiply);
  }

  SECTION("division") {
    auto expr = parse::parse("6 / 2", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op == ast::binary_operator::divide);
  }
}

TEST_CASE("parse binary comparison operations", "[parse][binary][comparison]") {
  ast::symbol_table symbols;

  SECTION("less than") {
    auto expr = parse::parse("1 < 2", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op == ast::binary_operator::less_than);
  }

  SECTION("greater than") {
    auto expr = parse::parse("2 > 1", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op == ast::binary_operator::greater_than);
  }

  SECTION("less than or equal") {
    auto expr = parse::parse("1 <= 2", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op == ast::binary_operator::less_than_or_equal);
  }

  SECTION("greater than or equal") {
    auto expr = parse::parse("2 >= 1", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op == ast::binary_operator::greater_than_or_equal);
  }

  SECTION("equals") {
    auto expr = parse::parse("1 == 1", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op == ast::binary_operator::equals);
  }

  SECTION("not equals") {
    auto expr = parse::parse("1 != 2", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op == ast::binary_operator::not_equals);
  }
}

TEST_CASE("parse binary logical operations", "[parse][binary][logical]") {
  ast::symbol_table symbols;

  SECTION("logical and") {
    auto expr = parse::parse("true && false", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op == ast::binary_operator::logical_and);
  }

  SECTION("logical or") {
    auto expr = parse::parse("true || false", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op == ast::binary_operator::logical_or);
  }

  SECTION("logical implies") {
    auto expr = parse::parse("true -> false", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op == ast::binary_operator::logical_implies);
  }
}

TEST_CASE("parse other binary operations", "[parse][binary]") {
  ast::symbol_table symbols;

  SECTION("concatenate") {
    auto expr = parse::parse("a ++ b", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op == ast::binary_operator::concatenate);
  }

  SECTION("update") {
    auto expr = parse::parse("a // b", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op == ast::binary_operator::update);
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
    REQUIRE(binop.op == ast::binary_operator::add);

    const auto& left = require_expr<ast::expression_integer>(binop.left);
    REQUIRE(left.value == 1);

    const auto& right = require_expr<ast::expression_binary_operation>(binop.right);
    REQUIRE(right.op == ast::binary_operator::multiply);
  }

  SECTION("comparison binds looser than arithmetic") {
    // 1 + 2 < 4 should parse as (1 + 2) < 4
    auto expr = parse::parse("1 + 2 < 4", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op == ast::binary_operator::less_than);

    const auto& left = require_expr<ast::expression_binary_operation>(binop.left);
    REQUIRE(left.op == ast::binary_operator::add);
  }

  SECTION("logical and binds tighter than logical or") {
    // a || b && c should parse as a || (b && c)
    auto expr = parse::parse("a || b && c", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op == ast::binary_operator::logical_or);

    const auto& right = require_expr<ast::expression_binary_operation>(binop.right);
    REQUIRE(right.op == ast::binary_operator::logical_and);
  }

  SECTION("implies is right-associative") {
    // a -> b -> c should parse as a -> (b -> c)
    auto expr = parse::parse("a -> b -> c", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op == ast::binary_operator::logical_implies);

    const auto& id = require_expr<ast::expression_identifier>(binop.left);
    REQUIRE(id.name == symbols.intern("a"));

    const auto& right = require_expr<ast::expression_binary_operation>(binop.right);
    REQUIRE(right.op == ast::binary_operator::logical_implies);
  }

  SECTION("update is right-associative") {
    // a // b // c should parse as a // (b // c)
    auto expr = parse::parse("a // b // c", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op == ast::binary_operator::update);

    const auto& right = require_expr<ast::expression_binary_operation>(binop.right);
    REQUIRE(right.op == ast::binary_operator::update);
  }

  SECTION("concatenate is right-associative") {
    // a ++ b ++ c should parse as a ++ (b ++ c)
    auto expr = parse::parse("a ++ b ++ c", symbols);
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op == ast::binary_operator::concatenate);

    const auto& right = require_expr<ast::expression_binary_operation>(binop.right);
    REQUIRE(right.op == ast::binary_operator::concatenate);
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
    REQUIRE(unop.op == ast::unary_operator::logical_not);
  }

  SECTION("unary minus (desugared to 0 - x)") {
    auto expr = parse::parse("-42", symbols);
    // unary minus is desugared to 0 - x
    const auto& binop = require_expr<ast::expression_binary_operation>(expr);
    REQUIRE(binop.op == ast::binary_operator::subtract);

    const auto& zero = require_expr<ast::expression_integer>(binop.left);
    REQUIRE(zero.value == 0);

    const auto& value = require_expr<ast::expression_integer>(binop.right);
    REQUIRE(value.value == 42);
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

    const auto& func = require_expr<ast::expression_identifier>(app.function);
    REQUIRE(func.name == symbols.intern("f"));

    REQUIRE(app.arguments.size() == 1);
    const auto& arg = require_expr<ast::expression_identifier>(app.arguments[0]);
    REQUIRE(arg.name == symbols.intern("x"));
  }

  SECTION("pipe left: f <| x becomes f(x)") {
    auto expr = parse::parse("f <| x", symbols);
    const auto& app = require_expr<ast::expression_application>(expr);

    const auto& func = require_expr<ast::expression_identifier>(app.function);
    REQUIRE(func.name == symbols.intern("f"));

    REQUIRE(app.arguments.size() == 1);
    const auto& arg = require_expr<ast::expression_identifier>(app.arguments[0]);
    REQUIRE(arg.name == symbols.intern("x"));
  }
}

// =============================================================================
// has attribute tests
// =============================================================================

TEST_CASE("parse has attribute", "[parse][hasattr]") {
  ast::symbol_table symbols;

  SECTION("simple has attribute") {
    auto expr = parse::parse("x ? y", symbols);
    const auto& hasattr = require_expr<ast::expression_has_attribute>(expr);

    const auto& subject = require_expr<ast::expression_identifier>(hasattr.subject);
    REQUIRE(subject.name == symbols.intern("x"));

    REQUIRE(hasattr.path.segments.size() == 1);
  }

  SECTION("has attribute with path") {
    auto expr = parse::parse("x ? a.b.c", symbols);
    const auto& hasattr = require_expr<ast::expression_has_attribute>(expr);
    REQUIRE(hasattr.path.segments.size() == 3);
  }
}

// =============================================================================
// source position tests
// =============================================================================

TEST_CASE("parse tracks source positions", "[parse][position]") {
  ast::symbol_table symbols;

  SECTION("integer position at start") {
    auto expr = parse::parse("42", symbols);
    const auto& integer = require_expr<ast::expression_integer>(expr);
    REQUIRE(integer.position.byte_offset == 0);
  }

  SECTION("expression with whitespace") {
    auto expr = parse::parse("  42", symbols);
    const auto& integer = require_expr<ast::expression_integer>(expr);
    // byte offset accounts for leading whitespace
    REQUIRE(integer.position.byte_offset == 2);
  }

  SECTION("multiline expression") {
    auto expr = parse::parse("\n\n42", symbols);
    const auto& integer = require_expr<ast::expression_integer>(expr);
    // two newlines = 2 bytes
    REQUIRE(integer.position.byte_offset == 2);
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
