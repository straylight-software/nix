#pragma once
///@file nix-language/parse/actions.hh
/// PEGTL actions that build AST nodes from parsed input.
///
/// This implements the parser → AST construction using PEGTL actions.
/// The main state type is expression_state which manages expression and operator stacks
/// for precedence parsing. Nested constructs use the change_head pattern to switch
/// to specialized state types.

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <boost/container/small_vector.hpp>
#include <tao/pegtl.hpp>

#include "nix-language/ast/expression.hh"
#include "nix-language/ast/symbol_table.hh"
#include "nix-language/parse/grammar.hh"
#include "nix-language/parse/state.hh"

namespace nix::language::parse {

namespace p = tao::pegtl;
namespace g = grammar;

// ============================================================================
// change_head - state switching pattern for nested constructs
// Based on Lix parser implementation (LGPL-2.1)
// ============================================================================

/// change_head: switches to a new state type NewState for the duration of Rule matching.
/// When the rule succeeds, the success() or success0() method is called on the Action.
/// Based on Lix parser implementation (LGPL-2.1)
template <typename NewState>
struct change_head : p::maybe_nothing {
  template <typename Rule, p::apply_mode A, p::rewind_mode M, template <typename...> class Action,
            template <typename...> class Control, typename ParseInput, typename State,
            typename... States>
  [[nodiscard]] static auto match(ParseInput& input, State&& state, States&&... states) -> bool {
    const auto begin = input.iterator();

    if constexpr (std::is_constructible_v<NewState, State, States...>) {
      NewState new_state(state, states...);
      if (p::match<Rule, A, M, Action, Control>(input, new_state, states...)) {
        if constexpr (A == p::apply_mode::action) {
          call_success<Action<Rule>>(nullptr, begin, input, new_state, state, states...);
        }
        return true;
      }
      return false;
    } else if constexpr (std::is_default_constructible_v<NewState>) {
      NewState new_state;
      if (p::match<Rule, A, M, Action, Control>(input, new_state, states...)) {
        if constexpr (A == p::apply_mode::action) {
          call_success<Action<Rule>>(nullptr, begin, input, new_state, state, states...);
        }
        return true;
      }
      return false;
    } else {
      static_assert(sizeof(NewState) == 0, "unable to instantiate new state");
    }
  }

private:
  // SFINAE: prefer success0 if available, otherwise use success
  template <typename Target, typename ParseInput, typename... S>
  static auto call_success(decltype(Target::success0(std::declval<S&>()...), nullptr),
                           auto& /*begin*/, ParseInput& /*input*/, S&... states) -> void {
    Target::success0(states...);
  }

  template <typename Target, typename ParseInput, typename... S>
  static auto call_success(void*, auto& begin, ParseInput& input, S&... states) -> void {
    const typename ParseInput::action_t action_input(begin, input);
    Target::success(action_input, states...);
  }
};

// ============================================================================
// expression state - manages expression stack during parsing
// ============================================================================

/// Wrapper for has_attr operator that includes the attribute path
struct has_attr_op {
  ast::attribute_path path;
};

/// operator entry for precedence parsing
struct operator_entry {
  ast::source_position position;
  std::uint8_t precedence;
  g::op::kind associativity;
  std::variant<g::op::logical_not, g::op::unary_minus, g::op::implies, g::op::logical_or,
               g::op::logical_and, g::op::equals, g::op::not_equals, g::op::less_equal,
               g::op::greater_equal, g::op::update, g::op::concatenate, g::op::less, g::op::greater,
               g::op::add, g::op::subtract, g::op::multiply, g::op::divide, g::op::pipe_right,
               g::op::pipe_left, has_attr_op>
      op;
};

/// expression parsing state with operator precedence handling
struct expression_state {
  // expression stack - small_vector optimized for common case
  boost::container::small_vector<std::pair<ast::source_position, ast::expression>, 4> expressions;

  // operator stack for precedence parsing
  boost::container::small_vector<operator_entry, 2> operators;

  /// push an expression onto the stack
  void push_expression(ast::source_position position, ast::expression expr) {
    expressions.emplace_back(position, std::move(expr));
  }

  /// pop an expression from the stack
  [[nodiscard]] auto pop_expression() -> std::pair<ast::source_position, ast::expression> {
    auto result = std::move(expressions.back());
    expressions.pop_back();
    return result;
  }

  /// pop just the expression (discarding position)
  [[nodiscard]] auto pop_expression_only() -> ast::expression {
    return std::move(pop_expression().second);
  }

  /// create and push an expression of type T
  template <typename T, typename... Args>
  auto emplace_expression(ast::source_position position, Args&&... args) -> T& {
    auto node = std::make_unique<ast::expression_node>(
        ast::expression_variant{T{std::forward<Args>(args)...}});
    auto& result = std::get<T>(node->data);
    push_expression(position, std::move(node));
    return result;
  }

  /// reduce operators down to given precedence
  void reduce(std::uint8_t to_precedence) {
    while (!operators.empty()) {
      auto& [position, precedence, kind, op] = operators.back();

      // stop if operator has lower precedence (higher number)
      // or same precedence but not left-associative
      if (precedence > to_precedence ||
          (precedence == to_precedence && kind != g::op::kind::left_associative)) {
        break;
      }

      apply_operator(position, op);
      operators.pop_back();
    }
  }

  /// push an operator, reducing higher-precedence ops first
  template <typename Op>
  void push_operator(ast::source_position position, Op op) {
    if constexpr (Op::associativity != g::op::kind::unary) {
      reduce(Op::precedence);
    }

    // check for non-associative operators
    if (!operators.empty() && Op::associativity == g::op::kind::non_associative) {
      auto& back = operators.back();
      if (back.associativity == g::op::kind::non_associative && back.precedence == Op::precedence) {
        throw parse_error("non-associative operators cannot be chained", position);
      }
    }

    operators.emplace_back(
        operator_entry{position, Op::precedence, Op::associativity, std::move(op)});
  }

  /// push has_attr operator with its path
  void push_has_attr(ast::source_position position, ast::attribute_path path) {
    reduce(g::op::has_attribute::precedence);
    operators.emplace_back(operator_entry{position, g::op::has_attribute::precedence,
                                          g::op::has_attribute::associativity,
                                          has_attr_op{std::move(path)}});
  }

  /// finish parsing, reducing all remaining operators
  [[nodiscard]] auto finish() -> std::pair<ast::source_position, ast::expression> {
    reduce(255);
    return pop_expression();
  }

private:
  /// dispatch operator application
  void apply_operator(ast::source_position position, auto& op) {
    std::visit(
        [&](auto& concrete_op) {
          using Op = std::decay_t<decltype(concrete_op)>;

          if constexpr (std::is_same_v<Op, g::op::logical_not>) {
            auto operand = pop_expression_only();
            emplace_expression<ast::expression_unary_operation>(
                position, position, ast::unary_operator::logical_not, std::move(operand));
          } else if constexpr (std::is_same_v<Op, g::op::unary_minus>) {
            // negate is desugared to: 0 - x
            auto operand = pop_expression_only();
            emplace_expression<ast::expression_binary_operation>(
                position, position, ast::binary_operator::subtract,
                std::make_unique<ast::expression_node>(
                    ast::expression_variant{ast::expression_integer{position, 0}}),
                std::move(operand));
          } else if constexpr (std::is_same_v<Op, g::op::implies>) {
            apply_binary(position, ast::binary_operator::logical_implies);
          } else if constexpr (std::is_same_v<Op, g::op::logical_or>) {
            apply_binary(position, ast::binary_operator::logical_or);
          } else if constexpr (std::is_same_v<Op, g::op::logical_and>) {
            apply_binary(position, ast::binary_operator::logical_and);
          } else if constexpr (std::is_same_v<Op, g::op::equals>) {
            apply_binary(position, ast::binary_operator::equals);
          } else if constexpr (std::is_same_v<Op, g::op::not_equals>) {
            apply_binary(position, ast::binary_operator::not_equals);
          } else if constexpr (std::is_same_v<Op, g::op::less>) {
            apply_binary(position, ast::binary_operator::less_than);
          } else if constexpr (std::is_same_v<Op, g::op::greater>) {
            apply_binary(position, ast::binary_operator::greater_than);
          } else if constexpr (std::is_same_v<Op, g::op::less_equal>) {
            apply_binary(position, ast::binary_operator::less_than_or_equal);
          } else if constexpr (std::is_same_v<Op, g::op::greater_equal>) {
            apply_binary(position, ast::binary_operator::greater_than_or_equal);
          } else if constexpr (std::is_same_v<Op, g::op::update>) {
            apply_binary(position, ast::binary_operator::update);
          } else if constexpr (std::is_same_v<Op, g::op::concatenate>) {
            apply_binary(position, ast::binary_operator::concatenate);
          } else if constexpr (std::is_same_v<Op, g::op::add>) {
            apply_binary(position, ast::binary_operator::add);
          } else if constexpr (std::is_same_v<Op, g::op::subtract>) {
            apply_binary(position, ast::binary_operator::subtract);
          } else if constexpr (std::is_same_v<Op, g::op::multiply>) {
            apply_binary(position, ast::binary_operator::multiply);
          } else if constexpr (std::is_same_v<Op, g::op::divide>) {
            apply_binary(position, ast::binary_operator::divide);
          } else if constexpr (std::is_same_v<Op, g::op::pipe_right>) {
            // |> : arg |> fn  becomes fn(arg)
            auto function = pop_expression_only();
            auto argument = pop_expression_only();
            std::vector<ast::expression> arguments;
            arguments.push_back(std::move(argument));
            emplace_expression<ast::expression_application>(position, position, std::move(function),
                                                            std::move(arguments));
          } else if constexpr (std::is_same_v<Op, g::op::pipe_left>) {
            // <| : fn <| arg  becomes fn(arg)
            auto argument = pop_expression_only();
            auto function = pop_expression_only();
            std::vector<ast::expression> arguments;
            arguments.push_back(std::move(argument));
            emplace_expression<ast::expression_application>(position, position, std::move(function),
                                                            std::move(arguments));
          } else if constexpr (std::is_same_v<Op, has_attr_op>) {
            auto subject = pop_expression_only();
            emplace_expression<ast::expression_has_attribute>(
                position, position, std::move(subject), std::move(concrete_op.path));
          }
        },
        op);
  }

  void apply_binary(ast::source_position position, ast::binary_operator op) {
    auto right = pop_expression_only();
    auto left = pop_expression_only();
    emplace_expression<ast::expression_binary_operation>(position, position, op, std::move(left),
                                                         std::move(right));
  }
};

// ============================================================================
// subexpression_state - wraps parent expression state with implicit conversion
// ============================================================================

struct subexpression_state {
  expression_state* parent;

  explicit subexpression_state(expression_state& up, parser_state& /*ps*/) : parent(&up) {}

  // Allow construction from another subexpression_state (for nested contexts)
  explicit subexpression_state(subexpression_state& up, parser_state& /*ps*/) : parent(up.parent) {}

  // Implicit conversion to expression_state& allows actions to work seamlessly
  operator expression_state&() { return *parent; }

  auto operator*() -> expression_state& { return *parent; }
  auto operator->() -> expression_state* { return parent; }
};

// ============================================================================
// PEGTL action base - default does nothing
// ============================================================================

template <typename Rule>
struct build_ast : p::nothing<Rule> {};

// ============================================================================
// literal expressions
// ============================================================================

template <>
struct build_ast<g::expr::identifier> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    auto position = parser.position_at(input);
    auto symbol = parser.symbols.intern(input.string_view());
    state.emplace_expression<ast::expression_identifier>(position, position, symbol);
  }
};

template <>
struct build_ast<g::expr::integer> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    auto position = parser.position_at(input);
    std::int64_t value;
    auto result = std::from_chars(input.begin(), input.end(), value);
    if (result.ec != std::errc{}) {
      throw parse_error("invalid integer literal", position);
    }
    state.emplace_expression<ast::expression_integer>(position, position, value);
  }
};

template <>
struct build_ast<g::expr::floating> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    auto position = parser.position_at(input);
    try {
      double value = std::stod(std::string(input.string_view()));
      state.emplace_expression<ast::expression_float>(position, position, value);
    } catch (...) {
      throw parse_error("invalid float literal", position);
    }
  }
};

// ============================================================================
// string expressions
// ============================================================================

/// String parsing state - accumulates literals and interpolations
struct string_state : subexpression_state {
  using subexpression_state::subexpression_state;

  std::string current_literal;
  ast::source_position start_position{};
  std::vector<std::variant<std::string, ast::expression>> parts;

  void append(ast::source_position position, std::string_view text) {
    if (current_literal.empty() && parts.empty()) {
      start_position = position;
    }
    current_literal += text;
  }

  void end_literal() {
    if (!current_literal.empty()) {
      parts.emplace_back(std::move(current_literal));
      current_literal.clear();
    }
  }

  auto finish(ast::source_position position) -> ast::expression {
    if (parts.empty()) {
      // simple string
      return std::make_unique<ast::expression_node>(
          ast::expression_variant{ast::expression_string{position, std::move(current_literal)}});
    }

    end_literal();
    return std::make_unique<ast::expression_node>(
        ast::expression_variant{ast::expression_string_interpolated{position, std::move(parts)}});
  }
};

// String rule actions use p::nothing as base to avoid PEGTL validation issues
// when the rules are matched in contexts where change_head hasn't switched states yet.
// The actual parsing happens when string_state is active via change_head.

template <typename... Content>
struct build_ast<g::string_rule::literal<Content...>> : p::maybe_nothing {
  // Only apply when state is string_state
  template <typename Input>
  static void apply(const Input& input, string_state& state, parser_state& parser) {
    state.append(parser.position_at(input), input.string_view());
  }
};

template <>
struct build_ast<g::string_rule::interpolation> : p::maybe_nothing {
  template <typename Input>
  static void apply(const Input& input, string_state& state, parser_state& /*parser*/) {
    state.end_literal();
    state.parts.emplace_back((*state).pop_expression_only());
  }
};

template <>
struct build_ast<g::string_rule::escape> : p::maybe_nothing {
  template <typename Input>
  static void apply(const Input& input, string_state& state, parser_state& parser) {
    char c = *input.begin();
    switch (c) {
      case 'n':
        state.append(parser.position_at(input), "\n");
        break;
      case 'r':
        state.append(parser.position_at(input), "\r");
        break;
      case 't':
        state.append(parser.position_at(input), "\t");
        break;
      case '\\':
      case '"':
      case '$':
        state.append(parser.position_at(input), input.string_view());
        break;
      default:
        // unknown escape - keep as-is for compatibility
        state.append(parser.position_at(input), input.string_view());
        break;
    }
  }
};

template <>
struct build_ast<g::string_double_quoted> : change_head<string_state> {
  static void success(const auto& input, string_state& str, expression_state& expr,
                      parser_state& parser) {
    auto position = parser.position_at(input);
    expr.push_expression(position, str.finish(position));
  }
};

// expr::string inherits from string_double_quoted but PEGTL looks up Action<expr::string>
template <>
struct build_ast<g::expr::string> : change_head<string_state> {
  static void success(const auto& input, string_state& str, expression_state& expr,
                      parser_state& parser) {
    auto position = parser.position_at(input);
    expr.push_expression(position, str.finish(position));
  }
};

// ============================================================================
// attribute path state (for has_attr and select)
// ============================================================================

struct attr_path_state : subexpression_state {
  using subexpression_state::subexpression_state;

  ast::attribute_path path;

  void push_static(ast::source_position position, ast::symbol sym) {
    path.segments.emplace_back(ast::attribute_name{position, sym});
  }

  void push_dynamic(ast::source_position position, ast::expression expr) {
    path.segments.emplace_back(ast::attribute_name{position, std::move(expr)});
  }
};

// These rules use p::maybe_nothing because they can be instantiated with
// different state types due to PEGTL's template expansion. The actual
// action only fires when the correct state type is present.

template <>
struct build_ast<g::attribute_simple> : p::maybe_nothing {
  template <typename Input>
  static void apply(const Input& input, attr_path_state& state, parser_state& parser) {
    auto position = parser.position_at(input);
    auto symbol = parser.symbols.intern(input.string_view());
    state.push_static(position, symbol);
  }
};

template <>
struct build_ast<g::attribute_dynamic> : p::maybe_nothing {
  template <typename Input>
  static void apply(const Input& input, attr_path_state& state, parser_state& parser) {
    auto position = parser.position_at(input);
    state.push_dynamic(position, (*state).pop_expression_only());
  }
};

// attribute_string is a string inside an attribute path - needs change_head<string_state>
// to properly parse the string content, then convert to attribute
template <>
struct build_ast<g::attribute_string> : p::maybe_nothing {
  // When used in attr_path_state context, parse the string and add to path
  template <typename Input>
  static void apply(const Input& input, attr_path_state& state, parser_state& parser) {
    // Parse the string content manually (simplified - just use literal)
    auto position = parser.position_at(input);
    // The input contains the full string including quotes
    auto str_view = input.string_view();
    if (str_view.size() >= 2 && str_view.front() == '"' && str_view.back() == '"') {
      // Simple unquoted string (no escapes/interpolations for now)
      auto content = std::string(str_view.substr(1, str_view.size() - 2));
      // TODO: handle escapes properly
      auto symbol = parser.symbols.intern(content);
      state.push_static(position, symbol);
    }
  }
};

// ============================================================================
// has_attribute operator - needs special handling to capture attr path
// ============================================================================

template <>
struct build_ast<g::expr::operator_marker<g::op::has_attribute>> : change_head<attr_path_state> {
  static void success(const auto& input, attr_path_state& attr, expression_state& expr,
                      parser_state& parser) {
    auto position = parser.position_at(input);
    expr.push_has_attr(position, std::move(attr.path));
  }
};

// ============================================================================
// binary/unary operators (except has_attribute)
// ============================================================================

template <>
struct build_ast<g::expr::operator_marker<g::op::logical_not>> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    state.push_operator(parser.position_at(input), g::op::logical_not{});
  }
};

template <>
struct build_ast<g::expr::operator_marker<g::op::unary_minus>> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    state.push_operator(parser.position_at(input), g::op::unary_minus{});
  }
};

template <>
struct build_ast<g::expr::operator_marker<g::op::implies>> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    state.push_operator(parser.position_at(input), g::op::implies{});
  }
};

template <>
struct build_ast<g::expr::operator_marker<g::op::logical_or>> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    state.push_operator(parser.position_at(input), g::op::logical_or{});
  }
};

template <>
struct build_ast<g::expr::operator_marker<g::op::logical_and>> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    state.push_operator(parser.position_at(input), g::op::logical_and{});
  }
};

template <>
struct build_ast<g::expr::operator_marker<g::op::equals>> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    state.push_operator(parser.position_at(input), g::op::equals{});
  }
};

template <>
struct build_ast<g::expr::operator_marker<g::op::not_equals>> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    state.push_operator(parser.position_at(input), g::op::not_equals{});
  }
};

template <>
struct build_ast<g::expr::operator_marker<g::op::less>> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    state.push_operator(parser.position_at(input), g::op::less{});
  }
};

template <>
struct build_ast<g::expr::operator_marker<g::op::greater>> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    state.push_operator(parser.position_at(input), g::op::greater{});
  }
};

template <>
struct build_ast<g::expr::operator_marker<g::op::less_equal>> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    state.push_operator(parser.position_at(input), g::op::less_equal{});
  }
};

template <>
struct build_ast<g::expr::operator_marker<g::op::greater_equal>> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    state.push_operator(parser.position_at(input), g::op::greater_equal{});
  }
};

template <>
struct build_ast<g::expr::operator_marker<g::op::update>> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    state.push_operator(parser.position_at(input), g::op::update{});
  }
};

template <>
struct build_ast<g::expr::operator_marker<g::op::concatenate>> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    state.push_operator(parser.position_at(input), g::op::concatenate{});
  }
};

template <>
struct build_ast<g::expr::operator_marker<g::op::add>> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    state.push_operator(parser.position_at(input), g::op::add{});
  }
};

template <>
struct build_ast<g::expr::operator_marker<g::op::subtract>> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    state.push_operator(parser.position_at(input), g::op::subtract{});
  }
};

template <>
struct build_ast<g::expr::operator_marker<g::op::multiply>> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    state.push_operator(parser.position_at(input), g::op::multiply{});
  }
};

template <>
struct build_ast<g::expr::operator_marker<g::op::divide>> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    state.push_operator(parser.position_at(input), g::op::divide{});
  }
};

template <>
struct build_ast<g::expr::operator_marker<g::op::pipe_right>> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    state.push_operator(parser.position_at(input), g::op::pipe_right{});
  }
};

template <>
struct build_ast<g::expr::operator_marker<g::op::pipe_left>> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    state.push_operator(parser.position_at(input), g::op::pipe_left{});
  }
};

// ============================================================================
// if expression
// ============================================================================

template <>
struct build_ast<g::expr::if_expression> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    auto position = parser.position_at(input);
    auto else_branch = state.pop_expression_only();
    auto then_branch = state.pop_expression_only();
    auto condition = state.pop_expression_only();
    state.emplace_expression<ast::expression_if>(position, position, std::move(condition),
                                                 std::move(then_branch), std::move(else_branch));
  }
};

// ============================================================================
// with expression
// ============================================================================

template <>
struct build_ast<g::expr::with_expression> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    auto position = parser.position_at(input);
    auto body = state.pop_expression_only();
    auto namespace_expr = state.pop_expression_only();
    state.emplace_expression<ast::expression_with>(position, position, std::move(namespace_expr),
                                                   std::move(body));
  }
};

// ============================================================================
// assert expression
// ============================================================================

template <>
struct build_ast<g::expr::assert_expression> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    auto position = parser.position_at(input);
    auto body = state.pop_expression_only();
    auto condition = state.pop_expression_only();
    state.emplace_expression<ast::expression_assert>(position, position, std::move(condition),
                                                     std::move(body));
  }
};

// ============================================================================
// lambda expressions (simple pattern only for now)
// ============================================================================

struct lambda_simple_state : subexpression_state {
  using subexpression_state::subexpression_state;

  ast::symbol argument_name{};
};

template <>
struct build_ast<g::expr::lambda_argument> : p::maybe_nothing {
  template <typename Input>
  static void apply(const Input& input, lambda_simple_state& state, parser_state& parser) {
    state.argument_name = parser.symbols.intern(input.string_view());
  }
};

template <>
struct build_ast<g::expr::lambda_pattern_simple> : change_head<lambda_simple_state> {
  static void success(const auto& input, lambda_simple_state& ls, expression_state& expr,
                      parser_state& parser) {
    auto position = parser.position_at(input);
    auto body = ls->pop_expression_only();
    auto pattern = std::make_unique<ast::pattern_variant>(ast::pattern_simple{ls.argument_name});
    expr.emplace_expression<ast::expression_lambda>(position, position, std::move(pattern),
                                                    std::move(body));
  }
};

// TODO: lambda_pattern_attrs requires formals parsing which has state nesting issues
// For now, we skip it and add when properly architected

// ============================================================================
// list expressions
// ============================================================================

/// list parsing state - tracks expression stack depth at entry
struct list_state : subexpression_state {
  /// expression stack size when list parsing started
  std::size_t start_stack_size = 0;

  explicit list_state(expression_state& up, parser_state& ps)
      : subexpression_state(up, ps), start_stack_size(up.expressions.size()) {}
};

template <>
struct build_ast<g::expr::list> : change_head<list_state> {
  static void success(const auto& input, list_state& ls, expression_state& expr,
                      parser_state& parser) {
    auto position = parser.position_at(input);

    // collect all expressions added during list parsing
    std::vector<ast::expression> elements;
    std::size_t count = ls->expressions.size() - ls.start_stack_size;
    elements.reserve(count);

    // pop in reverse order, then reverse to get correct order
    for (std::size_t i = 0; i < count; ++i) {
      elements.push_back(ls->pop_expression_only());
    }
    std::reverse(elements.begin(), elements.end());

    expr.emplace_expression<ast::expression_list>(position, position, std::move(elements));
  }
};

// ============================================================================
// path expressions - simplified
// ============================================================================

struct path_state : subexpression_state {
  using subexpression_state::subexpression_state;

  std::vector<std::variant<std::string, ast::expression>> parts;
  bool is_home_path = false;
  bool is_search_path = false;
};

template <>
struct build_ast<g::path_rule::anchor> : p::maybe_nothing {
  template <typename Input>
  static void apply(const Input& input, path_state& state, parser_state& parser) {
    // Convert relative path to absolute
    auto path_str = std::string(input.string_view());
    auto abs_path = std::filesystem::absolute(parser.base_path / path_str).string();
    state.parts.emplace_back(abs_path);
  }
};

template <>
struct build_ast<g::path_rule::home_anchor> : p::maybe_nothing {
  template <typename Input>
  static void apply(const Input& input, path_state& state, parser_state& /*parser*/) {
    state.is_home_path = true;
    // Keep the ~/... path as-is for now, expansion happens at eval time
    state.parts.emplace_back(std::string(input.string_view()));
  }
};

template <>
struct build_ast<g::path_rule::searched_path> : p::maybe_nothing {
  template <typename Input>
  static void apply(const Input& input, path_state& state, parser_state& /*parser*/) {
    state.is_search_path = true;
    state.parts.emplace_back(std::string(input.string_view()));
  }
};

template <typename... Content>
struct build_ast<g::path_rule::literal<Content...>> : p::maybe_nothing {
  template <typename Input>
  static void apply(const Input& input, path_state& state, parser_state& /*parser*/) {
    if (!state.parts.empty() && std::holds_alternative<std::string>(state.parts.back())) {
      std::get<std::string>(state.parts.back()) += input.string_view();
    } else {
      state.parts.emplace_back(std::string(input.string_view()));
    }
  }
};

template <>
struct build_ast<g::path_rule::interpolation> : p::maybe_nothing {
  template <typename Input>
  static void apply(const Input& input, path_state& state, parser_state& /*parser*/) {
    state.parts.emplace_back(state->pop_expression_only());
  }
};

template <>
struct build_ast<g::path> : change_head<path_state> {
  static void success(const auto& input, path_state& ps, expression_state& expr,
                      parser_state& parser) {
    auto position = parser.position_at(input);

    // Check if any interpolation
    bool has_interpolation = false;
    for (const auto& part : ps.parts) {
      if (std::holds_alternative<ast::expression>(part)) {
        has_interpolation = true;
        break;
      }
    }

    if (has_interpolation) {
      expr.emplace_expression<ast::expression_path_interpolated>(position, position,
                                                                 std::move(ps.parts));
    } else if (ps.parts.size() == 1 && std::holds_alternative<std::string>(ps.parts[0])) {
      expr.emplace_expression<ast::expression_path>(position, position,
                                                    std::get<std::string>(ps.parts[0]));
    } else {
      // Concatenate all string parts
      std::string full_path;
      for (const auto& part : ps.parts) {
        if (std::holds_alternative<std::string>(part)) {
          full_path += std::get<std::string>(part);
        }
      }
      expr.emplace_expression<ast::expression_path>(position, position, std::move(full_path));
    }
  }
};

template <>
struct build_ast<g::expr::path_expr> : change_head<path_state> {
  static void success(const auto& input, path_state& ps, expression_state& expr,
                      parser_state& parser) {
    build_ast<g::path>::success(input, ps, expr, parser);
  }
};

// ============================================================================
// URI expressions (deprecated but still supported)
// ============================================================================

template <>
struct build_ast<g::expr::uri> {
  static void apply(const auto& input, expression_state& state, parser_state& parser) {
    auto position = parser.position_at(input);
    // URIs are deprecated but parse as strings
    state.emplace_expression<ast::expression_string>(position, position,
                                                     std::string(input.string_view()));
  }
};

// ============================================================================
// control: use normal control
// ============================================================================

template <typename Rule>
using control = p::normal<Rule>;

// ============================================================================
// parser entry point
// ============================================================================

/// parse a nix expression from a string
[[nodiscard]] inline auto parse(std::string_view source, ast::symbol_table& symbols,
                                std::filesystem::path base_path = ".") -> ast::expression {
  parser_state state{symbols, std::move(base_path), source.data()};
  expression_state expr_state;

  p::memory_input input(source, "input");

  try {
    p::parse<g::root, build_ast, control>(input, expr_state, state);
    return expr_state.finish().second;
  } catch (const p::parse_error& error) {
    auto position = state.position_at(input);
    throw parse_error(std::string(error.message()), position);
  }
}

} // namespace nix::language::parse
