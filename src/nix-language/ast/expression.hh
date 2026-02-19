#pragma once
///@file nix-language/ast/expression.hh
/// Core AST types for Nix expressions, designed for AOT compilation to WASM.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace nix::language::ast {

/// source position for error reporting
struct source_position {
  std::uint32_t byte_offset;
  std::uint32_t line;
  std::uint32_t column;
};

/// interned symbol handle for identifiers and attribute names
/// symbols are interned at parse time for efficient comparison
struct symbol {
  std::uint32_t index;

  auto operator==(const symbol&) const noexcept -> bool = default;
  auto operator<=>(const symbol&) const noexcept = default;
};

// forward declare expression_node (the variant type is defined at the end)
struct expression_node;
using expression = std::unique_ptr<expression_node>;

/// attribute name: either a static symbol or a dynamic expression
struct attribute_name {
  source_position position;
  std::variant<symbol, expression> value;

  [[nodiscard]] auto is_dynamic() const noexcept -> bool {
    return std::holds_alternative<expression>(value);
  }
};

/// attribute path: foo.bar."baz".${dynamic}
struct attribute_path {
  std::vector<attribute_name> segments;
};

/// formal parameter in a lambda pattern: { name ? default, ... }
struct formal_parameter {
  source_position position;
  symbol name;
  std::optional<expression> default_value;
};

/// lambda argument pattern: simple identifier
struct pattern_simple {
  symbol argument_name;
};

/// lambda argument pattern: attribute set pattern { a, b ? default, ... }@name
struct pattern_attrset {
  std::optional<symbol> argument_name; // the @name in { ... }@name
  std::vector<formal_parameter> formals;
  bool has_ellipsis;
};

using pattern_variant = std::variant<pattern_simple, pattern_attrset>;

/// binding in an attribute set or let expression
struct binding_attribute {
  source_position position;
  attribute_path path;
  expression value;
};

/// inherit binding: inherit x y; or inherit (expr) x y;
struct binding_inherit {
  source_position position;
  std::optional<expression> from_expression;
  std::vector<attribute_name> attributes;
};

using binding_variant = std::variant<binding_attribute, binding_inherit>;

// expression types

struct expression_identifier {
  source_position position;
  symbol name;
};

struct expression_integer {
  source_position position;
  std::int64_t value;
};

struct expression_float {
  source_position position;
  double value;
};

struct expression_string {
  source_position position;
  std::string value; // already unescaped
};

/// string with interpolations: "hello ${name}"
struct expression_string_interpolated {
  source_position position;
  std::vector<std::variant<std::string, expression>> parts;
};

struct expression_path {
  source_position position;
  std::string value; // absolute path after resolution
};

/// path with interpolations
struct expression_path_interpolated {
  source_position position;
  std::vector<std::variant<std::string, expression>> parts;
};

struct expression_list {
  source_position position;
  std::vector<expression> elements;
};

struct expression_attribute_set {
  source_position position;
  bool is_recursive;
  std::vector<binding_variant> bindings;
};

struct expression_select {
  source_position position;
  expression subject;
  attribute_path path;
  std::optional<expression> default_value;
};

struct expression_has_attribute {
  source_position position;
  expression subject;
  attribute_path path;
};

struct expression_lambda {
  source_position position;
  std::unique_ptr<pattern_variant> argument_pattern;
  expression body;
};

struct expression_application {
  source_position position;
  expression function;
  std::vector<expression> arguments;
};

struct expression_let {
  source_position position;
  std::vector<binding_variant> bindings;
  expression body;
};

struct expression_with {
  source_position position;
  expression namespace_expression;
  expression body;
};

struct expression_if {
  source_position position;
  expression condition;
  expression then_branch;
  expression else_branch;
};

struct expression_assert {
  source_position position;
  expression condition;
  expression body;
};

// binary operators
enum class binary_operator : std::uint8_t {
  logical_and,
  logical_or,
  logical_implies,
  equals,
  not_equals,
  less_than,
  less_than_or_equal,
  greater_than,
  greater_than_or_equal,
  update,      // //
  concatenate, // ++
  add,         // +
  subtract,    // -
  multiply,    // *
  divide,      // /
  pipe_right,  // |>
  pipe_left,   // <|
};

struct expression_binary_operation {
  source_position position;
  binary_operator op;
  expression left;
  expression right;
};

// unary operators
enum class unary_operator : std::uint8_t {
  logical_not,
  negate,
};

struct expression_unary_operation {
  source_position position;
  unary_operator op;
  expression operand;
};

/// the main expression variant type
using expression_variant =
    std::variant<expression_identifier, expression_integer, expression_float, expression_string,
                 expression_string_interpolated, expression_path, expression_path_interpolated,
                 expression_list, expression_attribute_set, expression_select,
                 expression_has_attribute, expression_lambda, expression_application,
                 expression_let, expression_with, expression_if, expression_assert,
                 expression_binary_operation, expression_unary_operation>;

/// wrapper struct to allow unique_ptr forward declaration
struct expression_node {
  expression_variant data;

  // explicit constructors for each expression type
  explicit expression_node(expression_variant value) : data(std::move(value)) {}

  // rule of five
  ~expression_node() = default;
  expression_node(expression_node&&) = default;
  expression_node(const expression_node&) = delete;
  auto operator=(expression_node&&) -> expression_node& = default;
  auto operator=(const expression_node&) -> expression_node& = delete;
};

/// get the source position from any expression
[[nodiscard]] inline auto get_position(const expression& expr) noexcept -> source_position {
  return std::visit([](const auto& e) { return e.position; }, expr->data);
}

/// helper to create an expression from any expression type
template <typename T, typename... Args>
[[nodiscard]] inline auto make_expression(Args&&... args) -> expression {
  return std::make_unique<expression_node>(T{std::forward<Args>(args)...});
}

} // namespace nix::language::ast
