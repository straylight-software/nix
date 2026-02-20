#pragma once
/// @file nix-language/parse/tree.h
/// Concrete syntax tree node types.
///
/// These are simple data structures with no SFINAE, no templates, no inheritance.
/// PEGTL produces its parse_tree, we walk it once to build these nodes,
/// then a separate pass lowers them to AST (interning symbols, precedence climbing, etc).

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace straylight::nix::compiler::parse {

// ============================================================================
// source location
// ============================================================================

struct source_span {
  std::size_t begin_byte = 0;
  std::size_t end_byte = 0;
  std::size_t begin_line = 1;
  std::size_t begin_column = 1;
};

// ============================================================================
// forward declarations
// ============================================================================

struct tree_node;
using tree = std::unique_ptr<tree_node>;

// ============================================================================
// literal nodes
// ============================================================================

struct node_integer {
  std::int64_t value;
  source_span span;
};

struct node_float {
  double value;
  source_span span;
};

struct node_string {
  std::string value;
  source_span span;
};

struct node_string_interpolated {
  std::vector<std::variant<std::string, tree>> parts;
  source_span span;
};

struct node_path {
  std::string value;
  source_span span;
};

struct node_path_interpolated {
  std::vector<std::variant<std::string, tree>> parts;
  source_span span;
};

struct node_identifier {
  std::string name;
  source_span span;
};

struct node_uri {
  std::string value;
  source_span span;
};

// ============================================================================
// operator nodes (precedence resolved later)
// ============================================================================

enum class binary_op_kind {
  add,
  subtract,
  multiply,
  divide,
  equals,
  not_equals,
  less,
  greater,
  less_equal,
  greater_equal,
  logical_and,
  logical_or,
  logical_implies,
  update,
  concatenate,
  pipe_right,
  pipe_left,
};

enum class unary_op_kind {
  negate,
  logical_not,
};

struct node_binary_op {
  binary_op_kind op;
  tree left;
  tree right;
  source_span span;
};

struct node_unary_op {
  unary_op_kind op;
  tree operand;
  source_span span;
};

struct node_has_attribute {
  tree subject;
  std::vector<std::variant<std::string, tree>> path;
  source_span span;
};

// ============================================================================
// compound expressions
// ============================================================================

struct node_list {
  std::vector<tree> elements;
  source_span span;
};

struct node_attribute_binding {
  std::vector<std::variant<std::string, tree>> path;
  tree value;
  source_span span;
};

struct node_inherit_binding {
  std::optional<tree> from;
  std::vector<std::variant<std::string, tree>> attributes;
  source_span span;
};

using node_binding = std::variant<node_attribute_binding, node_inherit_binding>;

struct node_attribute_set {
  bool is_recursive;
  std::vector<node_binding> bindings;
  source_span span;
};

struct node_select {
  tree subject;
  std::vector<std::variant<std::string, tree>> path;
  std::optional<tree> default_value;
  source_span span;
};

struct node_apply {
  tree function;
  std::vector<tree> arguments;
  source_span span;
};

// ============================================================================
// lambda nodes
// ============================================================================

struct node_formal {
  std::string name;
  std::optional<tree> default_value;
  source_span span;
};

struct node_pattern_simple {
  std::string name;
  source_span span;
};

struct node_pattern_attrset {
  std::vector<node_formal> formals;
  bool has_ellipsis;
  std::optional<std::string> at_name;
  source_span span;
};

using node_pattern = std::variant<node_pattern_simple, node_pattern_attrset>;

struct node_lambda {
  node_pattern pattern;
  tree body;
  source_span span;
};

// ============================================================================
// control flow
// ============================================================================

struct node_if {
  tree condition;
  tree then_branch;
  tree else_branch;
  source_span span;
};

struct node_let {
  std::vector<node_binding> bindings;
  tree body;
  source_span span;
};

struct node_with {
  tree namespace_expr;
  tree body;
  source_span span;
};

struct node_assert {
  tree condition;
  tree body;
  source_span span;
};

// ============================================================================
// the variant
// ============================================================================

using tree_variant = std::variant<
    // literals
    node_integer, node_float, node_string, node_string_interpolated, node_path,
    node_path_interpolated, node_identifier, node_uri,
    // operators
    node_binary_op, node_unary_op, node_has_attribute,
    // compound
    node_list, node_attribute_set, node_select, node_apply,
    // lambda
    node_lambda,
    // control flow
    node_if, node_let, node_with, node_assert>;

struct tree_node {
  tree_variant data;

  template <typename T>
  explicit tree_node(T&& value) : data(std::forward<T>(value)) {}
};

// ============================================================================
// convenience constructors
// ============================================================================

template <typename T, typename... Args>
[[nodiscard]] inline auto make_tree(Args&&... args) -> tree {
  return std::make_unique<tree_node>(T{std::forward<Args>(args)...});
}

} // namespace straylight::nix::compiler::parse
