#pragma once
/// @file nix-language/parse/lower.h
/// Lower our parse tree to AST.
///
/// This pass:
/// - Interns symbol names
/// - Converts source_span to ast::source_position
/// - Transforms tree nodes to ast expression types

#include <stdexcept>
#include <variant>

#include "straylight/language/ast/expression.h"
#include "straylight/language/ast/symbol_table.h"
#include "straylight/language/parse/tree.h"

namespace straylight::language::parse {

// ============================================================================
// error type
// ============================================================================

class lower_error : public std::runtime_error {
public:
  explicit lower_error(const std::string& message) : std::runtime_error(message) {}
};

// ============================================================================
// lowering context
// ============================================================================

struct lower_context {
  ast::symbol_table& symbols;

  explicit lower_context(ast::symbol_table& sym) : symbols(sym) {}

  [[nodiscard]] auto make_position(const source_span& span) const -> ast::source_position {
    return ast::source_position{static_cast<std::uint32_t>(span.begin_byte),
                                static_cast<std::uint32_t>(span.begin_line),
                                static_cast<std::uint32_t>(span.begin_column)};
  }
};

// ============================================================================
// forward declaration
// ============================================================================

[[nodiscard]] auto lower_tree(const tree_node& node, lower_context& context) -> ast::expression;

// ============================================================================
// helper to get span from any node
// ============================================================================

inline auto get_span(const tree_variant& data) -> source_span {
  return std::visit(
      [](const auto& node) -> source_span {
        if constexpr (requires { node.span; }) {
          return node.span;
        } else {
          return source_span{};
        }
      },
      data);
}

// ============================================================================
// literal lowering
// ============================================================================

inline auto lower_integer(const node_integer& node, lower_context& context) -> ast::expression {
  auto position = context.make_position(node.span);
  return std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_integer{position, node.value}});
}

inline auto lower_float(const node_float& node, lower_context& context) -> ast::expression {
  auto position = context.make_position(node.span);
  return std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_float{position, node.value}});
}

inline auto lower_string(const node_string& node, lower_context& context) -> ast::expression {
  auto position = context.make_position(node.span);
  return std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_string{position, node.value}});
}

inline auto lower_string_interpolated(const node_string_interpolated& node, lower_context& context)
    -> ast::expression {
  auto position = context.make_position(node.span);

  std::vector<std::variant<std::string, ast::expression>> parts;
  parts.reserve(node.parts.size());

  for (const auto& part : node.parts) {
    if (std::holds_alternative<std::string>(part)) {
      parts.emplace_back(std::get<std::string>(part));
    } else {
      parts.emplace_back(lower_tree(*std::get<tree>(part), context));
    }
  }

  return std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_string_interpolated{position, std::move(parts)}});
}

inline auto lower_path(const node_path& node, lower_context& context) -> ast::expression {
  auto position = context.make_position(node.span);
  return std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_path{position, node.value}});
}

inline auto lower_path_interpolated(const node_path_interpolated& node, lower_context& context)
    -> ast::expression {
  auto position = context.make_position(node.span);

  std::vector<std::variant<std::string, ast::expression>> parts;
  parts.reserve(node.parts.size());

  for (const auto& part : node.parts) {
    if (std::holds_alternative<std::string>(part)) {
      parts.emplace_back(std::get<std::string>(part));
    } else {
      parts.emplace_back(lower_tree(*std::get<tree>(part), context));
    }
  }

  return std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_path_interpolated{position, std::move(parts)}});
}

inline auto lower_identifier(const node_identifier& node, lower_context& context)
    -> ast::expression {
  auto position = context.make_position(node.span);
  auto symbol = context.symbols.intern(node.name);
  return std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_identifier{position, symbol}});
}

inline auto lower_uri(const node_uri& node, lower_context& context) -> ast::expression {
  // URIs are lowered to strings (they're deprecated)
  auto position = context.make_position(node.span);
  return std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_string{position, node.value}});
}

// ============================================================================
// operator lowering
// ============================================================================

inline auto to_ast_binary_op(binary_op_kind kind) -> ast::binary_operator {
  switch (kind) {
    case binary_op_kind::add:
      return ast::binary_operator::add;
    case binary_op_kind::subtract:
      return ast::binary_operator::subtract;
    case binary_op_kind::multiply:
      return ast::binary_operator::multiply;
    case binary_op_kind::divide:
      return ast::binary_operator::divide;
    case binary_op_kind::equals:
      return ast::binary_operator::equals;
    case binary_op_kind::not_equals:
      return ast::binary_operator::not_equals;
    case binary_op_kind::less:
      return ast::binary_operator::less_than;
    case binary_op_kind::greater:
      return ast::binary_operator::greater_than;
    case binary_op_kind::less_equal:
      return ast::binary_operator::less_than_or_equal;
    case binary_op_kind::greater_equal:
      return ast::binary_operator::greater_than_or_equal;
    case binary_op_kind::logical_and:
      return ast::binary_operator::logical_and;
    case binary_op_kind::logical_or:
      return ast::binary_operator::logical_or;
    case binary_op_kind::logical_implies:
      return ast::binary_operator::logical_implies;
    case binary_op_kind::update:
      return ast::binary_operator::update;
    case binary_op_kind::concatenate:
      return ast::binary_operator::concatenate;
    case binary_op_kind::pipe_right:
    case binary_op_kind::pipe_left:
      // Pipes are desugared to application in lower_binary_op
      throw lower_error("pipe operators should be handled specially");
  }
  throw lower_error("unknown binary operator");
}

inline auto lower_binary_op(const node_binary_op& node, lower_context& context) -> ast::expression {
  auto position = context.make_position(node.span);
  auto left = lower_tree(*node.left, context);
  auto right = lower_tree(*node.right, context);

  // Handle pipe operators as function application
  if (node.op == binary_op_kind::pipe_right) {
    // arg |> fn  -->  fn(arg)
    std::vector<ast::expression> args;
    args.push_back(std::move(left));
    return std::make_unique<ast::expression_node>(ast::expression_variant{
        ast::expression_application{position, std::move(right), std::move(args)}});
  }

  if (node.op == binary_op_kind::pipe_left) {
    // fn <| arg  -->  fn(arg)
    std::vector<ast::expression> args;
    args.push_back(std::move(right));
    return std::make_unique<ast::expression_node>(ast::expression_variant{
        ast::expression_application{position, std::move(left), std::move(args)}});
  }

  auto ast_op = to_ast_binary_op(node.op);
  return std::make_unique<ast::expression_node>(ast::expression_variant{
      ast::expression_binary_operation{position, ast_op, std::move(left), std::move(right)}});
}

inline auto lower_unary_op(const node_unary_op& node, lower_context& context) -> ast::expression {
  auto position = context.make_position(node.span);
  auto operand = lower_tree(*node.operand, context);

  if (node.op == unary_op_kind::negate) {
    // Desugared to: 0 - operand
    auto zero = std::make_unique<ast::expression_node>(
        ast::expression_variant{ast::expression_integer{position, 0}});
    return std::make_unique<ast::expression_node>(
        ast::expression_variant{ast::expression_binary_operation{
            position, ast::binary_operator::subtract, std::move(zero), std::move(operand)}});
  }

  return std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_unary_operation{
          position, ast::unary_operator::logical_not, std::move(operand)}});
}

inline auto lower_has_attribute(const node_has_attribute& node, lower_context& context)
    -> ast::expression {
  auto position = context.make_position(node.span);
  auto subject = lower_tree(*node.subject, context);

  // Convert the path segments to ast::attribute_path
  ast::attribute_path path;
  path.segments_.reserve(node.path.size());

  for (const auto& segment : node.path) {
    if (std::holds_alternative<std::string>(segment)) {
      // Static attribute name
      auto symbol = context.symbols.intern(std::get<std::string>(segment));
      path.segments_.push_back(ast::attribute_name{position, symbol});
    } else {
      // Dynamic attribute expression
      auto expr = lower_tree(*std::get<tree>(segment), context);
      path.segments_.push_back(ast::attribute_name{position, std::move(expr)});
    }
  }

  return std::make_unique<ast::expression_node>(ast::expression_variant{
      ast::expression_has_attribute{position, std::move(subject), std::move(path)}});
}

// ============================================================================
// compound expression lowering
// ============================================================================

inline auto lower_list(const node_list& node, lower_context& context) -> ast::expression {
  auto position = context.make_position(node.span);

  std::vector<ast::expression> elements;
  elements.reserve(node.elements.size());

  for (const auto& elem : node.elements) {
    elements.push_back(lower_tree(*elem, context));
  }

  return std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_list{position, std::move(elements)}});
}

// Helper to convert a path segment vector to ast::attribute_path
inline auto lower_path_to_attribute_path(const std::vector<std::variant<std::string, tree>>& path,
                                         ast::source_position position, lower_context& context)
    -> ast::attribute_path {
  ast::attribute_path result;
  result.segments_.reserve(path.size());

  for (const auto& segment : path) {
    if (std::holds_alternative<std::string>(segment)) {
      auto symbol = context.symbols.intern(std::get<std::string>(segment));
      result.segments_.push_back(ast::attribute_name{position, symbol});
    } else {
      auto expr = lower_tree(*std::get<tree>(segment), context);
      result.segments_.push_back(ast::attribute_name{position, std::move(expr)});
    }
  }

  return result;
}

inline auto lower_attribute_set(const node_attribute_set& node, lower_context& context)
    -> ast::expression {
  auto position = context.make_position(node.span);

  std::vector<ast::binding_variant> bindings;
  bindings.reserve(node.bindings.size());

  for (const auto& binding : node.bindings) {
    if (std::holds_alternative<node_attribute_binding>(binding)) {
      const auto& attr_binding = std::get<node_attribute_binding>(binding);
      auto binding_pos = context.make_position(attr_binding.span);
      auto path = lower_path_to_attribute_path(attr_binding.path, binding_pos, context);
      auto value = lower_tree(*attr_binding.value, context);
      bindings.emplace_back(ast::binding_attribute{binding_pos, std::move(path), std::move(value)});
    } else {
      const auto& inherit_binding = std::get<node_inherit_binding>(binding);
      auto binding_pos = context.make_position(inherit_binding.span);

      std::optional<ast::expression> from_expr;
      if (inherit_binding.from) {
        from_expr = lower_tree(**inherit_binding.from, context);
      }

      std::vector<ast::attribute_name> attributes;
      attributes.reserve(inherit_binding.attributes.size());
      for (const auto& attr : inherit_binding.attributes) {
        if (std::holds_alternative<std::string>(attr)) {
          auto symbol = context.symbols.intern(std::get<std::string>(attr));
          attributes.push_back(ast::attribute_name{binding_pos, symbol});
        } else {
          auto expr = lower_tree(*std::get<tree>(attr), context);
          attributes.push_back(ast::attribute_name{binding_pos, std::move(expr)});
        }
      }

      bindings.emplace_back(
          ast::binding_inherit{binding_pos, std::move(from_expr), std::move(attributes)});
    }
  }

  return std::make_unique<ast::expression_node>(ast::expression_variant{
      ast::expression_attribute_set{position, node.is_recursive, std::move(bindings)}});
}

inline auto lower_select(const node_select& node, lower_context& context) -> ast::expression {
  auto position = context.make_position(node.span);
  auto subject = lower_tree(*node.subject, context);
  auto path = lower_path_to_attribute_path(node.path, position, context);

  std::optional<ast::expression> default_value;
  if (node.default_value) {
    default_value = lower_tree(**node.default_value, context);
  }

  return std::make_unique<ast::expression_node>(ast::expression_variant{ast::expression_select{
      position, std::move(subject), std::move(path), std::move(default_value)}});
}

inline auto lower_apply(const node_apply& node, lower_context& context) -> ast::expression {
  auto position = context.make_position(node.span);
  auto function = lower_tree(*node.function, context);

  std::vector<ast::expression> arguments;
  arguments.reserve(node.arguments.size());

  for (const auto& arg : node.arguments) {
    arguments.push_back(lower_tree(*arg, context));
  }

  return std::make_unique<ast::expression_node>(ast::expression_variant{
      ast::expression_application{position, std::move(function), std::move(arguments)}});
}

// ============================================================================
// control flow lowering
// ============================================================================

inline auto lower_if(const node_if& node, lower_context& context) -> ast::expression {
  auto position = context.make_position(node.span);
  auto condition = lower_tree(*node.condition, context);
  auto then_branch = lower_tree(*node.then_branch, context);
  auto else_branch = lower_tree(*node.else_branch, context);

  return std::make_unique<ast::expression_node>(ast::expression_variant{ast::expression_if{
      position, std::move(condition), std::move(then_branch), std::move(else_branch)}});
}

inline auto lower_with(const node_with& node, lower_context& context) -> ast::expression {
  auto position = context.make_position(node.span);
  auto namespace_expr = lower_tree(*node.namespace_expr, context);
  auto body = lower_tree(*node.body, context);

  return std::make_unique<ast::expression_node>(ast::expression_variant{
      ast::expression_with{position, std::move(namespace_expr), std::move(body)}});
}

inline auto lower_assert(const node_assert& node, lower_context& context) -> ast::expression {
  auto position = context.make_position(node.span);
  auto condition = lower_tree(*node.condition, context);
  auto body = lower_tree(*node.body, context);

  return std::make_unique<ast::expression_node>(ast::expression_variant{
      ast::expression_assert{position, std::move(condition), std::move(body)}});
}

inline auto lower_let(const node_let& node, lower_context& context) -> ast::expression {
  auto position = context.make_position(node.span);

  std::vector<ast::binding_variant> bindings;
  bindings.reserve(node.bindings.size());

  for (const auto& binding : node.bindings) {
    if (std::holds_alternative<node_attribute_binding>(binding)) {
      const auto& attr_binding = std::get<node_attribute_binding>(binding);
      auto binding_pos = context.make_position(attr_binding.span);
      auto path = lower_path_to_attribute_path(attr_binding.path, binding_pos, context);
      auto value = lower_tree(*attr_binding.value, context);
      bindings.emplace_back(ast::binding_attribute{binding_pos, std::move(path), std::move(value)});
    } else {
      const auto& inherit_binding = std::get<node_inherit_binding>(binding);
      auto binding_pos = context.make_position(inherit_binding.span);

      std::optional<ast::expression> from_expr;
      if (inherit_binding.from) {
        from_expr = lower_tree(**inherit_binding.from, context);
      }

      std::vector<ast::attribute_name> attributes;
      attributes.reserve(inherit_binding.attributes.size());
      for (const auto& attr : inherit_binding.attributes) {
        if (std::holds_alternative<std::string>(attr)) {
          auto symbol = context.symbols.intern(std::get<std::string>(attr));
          attributes.push_back(ast::attribute_name{binding_pos, symbol});
        } else {
          auto expr = lower_tree(*std::get<tree>(attr), context);
          attributes.push_back(ast::attribute_name{binding_pos, std::move(expr)});
        }
      }

      bindings.emplace_back(
          ast::binding_inherit{binding_pos, std::move(from_expr), std::move(attributes)});
    }
  }

  auto body = lower_tree(*node.body, context);

  return std::make_unique<ast::expression_node>(
      ast::expression_variant{ast::expression_let{position, std::move(bindings), std::move(body)}});
}

// ============================================================================
// lambda lowering
// ============================================================================

inline auto lower_lambda(const node_lambda& node, lower_context& context) -> ast::expression {
  auto position = context.make_position(node.span);
  auto body = lower_tree(*node.body, context);

  std::unique_ptr<ast::pattern_variant> pattern;

  if (std::holds_alternative<node_pattern_simple>(node.pattern)) {
    const auto& simple = std::get<node_pattern_simple>(node.pattern);
    auto symbol = context.symbols.intern(simple.name);
    pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{symbol});
  } else {
    const auto& attrset = std::get<node_pattern_attrset>(node.pattern);

    std::vector<ast::formal_parameter> formals;
    formals.reserve(attrset.formals.size());

    for (const auto& f : attrset.formals) {
      auto formal_pos = context.make_position(f.span);
      auto formal_symbol = context.symbols.intern(f.name);
      std::optional<ast::expression> default_value;
      if (f.default_value) {
        default_value = lower_tree(**f.default_value, context);
      }
      formals.push_back(ast::formal_parameter{formal_pos, formal_symbol, std::move(default_value)});
    }

    std::optional<ast::symbol> at_name;
    if (attrset.at_name) {
      at_name = context.symbols.intern(*attrset.at_name);
    }

    // pattern_attrset: argument_name, formals, has_ellipsis
    pattern = std::make_unique<ast::pattern_variant>(
        ast::pattern_attrset{at_name, std::move(formals), attrset.has_ellipsis});
  }

  return std::make_unique<ast::expression_node>(ast::expression_variant{
      ast::expression_lambda{position, std::move(pattern), std::move(body)}});
}

// ============================================================================
// main dispatcher
// ============================================================================

[[nodiscard]] inline auto lower_tree(const tree_node& node, lower_context& context)
    -> ast::expression {
  return std::visit(
      [&context](const auto& data) -> ast::expression {
        using T = std::decay_t<decltype(data)>;

        if constexpr (std::is_same_v<T, node_integer>) {
          return lower_integer(data, context);
        } else if constexpr (std::is_same_v<T, node_float>) {
          return lower_float(data, context);
        } else if constexpr (std::is_same_v<T, node_string>) {
          return lower_string(data, context);
        } else if constexpr (std::is_same_v<T, node_string_interpolated>) {
          return lower_string_interpolated(data, context);
        } else if constexpr (std::is_same_v<T, node_path>) {
          return lower_path(data, context);
        } else if constexpr (std::is_same_v<T, node_path_interpolated>) {
          return lower_path_interpolated(data, context);
        } else if constexpr (std::is_same_v<T, node_identifier>) {
          return lower_identifier(data, context);
        } else if constexpr (std::is_same_v<T, node_uri>) {
          return lower_uri(data, context);
        } else if constexpr (std::is_same_v<T, node_binary_op>) {
          return lower_binary_op(data, context);
        } else if constexpr (std::is_same_v<T, node_unary_op>) {
          return lower_unary_op(data, context);
        } else if constexpr (std::is_same_v<T, node_has_attribute>) {
          return lower_has_attribute(data, context);
        } else if constexpr (std::is_same_v<T, node_list>) {
          return lower_list(data, context);
        } else if constexpr (std::is_same_v<T, node_attribute_set>) {
          return lower_attribute_set(data, context);
        } else if constexpr (std::is_same_v<T, node_select>) {
          return lower_select(data, context);
        } else if constexpr (std::is_same_v<T, node_apply>) {
          return lower_apply(data, context);
        } else if constexpr (std::is_same_v<T, node_if>) {
          return lower_if(data, context);
        } else if constexpr (std::is_same_v<T, node_with>) {
          return lower_with(data, context);
        } else if constexpr (std::is_same_v<T, node_assert>) {
          return lower_assert(data, context);
        } else if constexpr (std::is_same_v<T, node_let>) {
          return lower_let(data, context);
        } else if constexpr (std::is_same_v<T, node_lambda>) {
          return lower_lambda(data, context);
        } else {
          throw lower_error("unhandled tree node type");
        }
      },
      node.data);
}

// ============================================================================
// entry point
// ============================================================================

/// Lower a parse tree to AST
[[nodiscard]] inline auto lower(const tree& root, ast::symbol_table& symbols) -> ast::expression {
  lower_context context{symbols};
  return lower_tree(*root, context);
}

} // namespace straylight::language::parse
