#pragma once
/// @file nix-language/parse/convert.h
/// Convert PEGTL parse_tree nodes to our clean tree types.
///
/// This walks the PEGTL tree exactly once, building our node types.
/// Handles:
/// - Extracting literal values from source spans
/// - Operator precedence climbing for binary expressions
/// - Collecting list elements, bindings, formals, etc.

#include <charconv>
#include <stdexcept>
#include <string>
#include <string_view>

#include <tao/pegtl.hpp>
#include <tao/pegtl/contrib/parse_tree.hpp>

#include "straylight/nix/compiler/parse/grammar.h"
#include "straylight/nix/compiler/parse/tree.h"

namespace straylight::nix::compiler::parse {

namespace p = tao::pegtl;
namespace g = grammar;

// ============================================================================
// error type
// ============================================================================

class convert_error : public std::runtime_error {
public:
  explicit convert_error(const std::string& message) : std::runtime_error(message) {}
};

// ============================================================================
// helper: extract source_span from PEGTL node
// ============================================================================

inline auto make_span(const p::parse_tree::node& node) -> source_span {
  source_span span;
  if (node.has_content()) {
    // PEGTL byte is 0-indexed, line and column are 1-indexed
    span.begin_byte = node.m_begin.byte;
    span.end_byte = node.m_end.byte;
    span.begin_line = node.m_begin.line;
    span.begin_column = node.m_begin.column;
  }
  return span;
}

// ============================================================================
// forward declaration
// ============================================================================

[[nodiscard]] auto convert_node(const p::parse_tree::node& node) -> tree;

// ============================================================================
// operator precedence table
// ============================================================================

struct operator_info {
  binary_op_kind kind;
  std::uint8_t precedence;
  bool right_associative;
};

inline auto is_has_attribute_operator(std::string_view op_type) -> bool {
  return op_type.find("op::has_attribute") != std::string_view::npos;
}

inline auto get_operator_info(std::string_view op_type) -> std::optional<operator_info> {
  // The op_type is the demangled rule name, e.g.
  // "straylight::nix::compiler::parse::grammar::op::add" We check for the operator name at the end

  if (op_type.find("op::add") != std::string_view::npos) {
    return operator_info{binary_op_kind::add, 6, false};
  }
  if (op_type.find("op::subtract") != std::string_view::npos) {
    return operator_info{binary_op_kind::subtract, 6, false};
  }
  if (op_type.find("op::multiply") != std::string_view::npos) {
    return operator_info{binary_op_kind::multiply, 7, false};
  }
  if (op_type.find("op::divide") != std::string_view::npos) {
    return operator_info{binary_op_kind::divide, 7, false};
  }
  if (op_type.find("op::concatenate") != std::string_view::npos) {
    return operator_info{binary_op_kind::concatenate, 5, true};
  }
  if (op_type.find("op::update") != std::string_view::npos) {
    return operator_info{binary_op_kind::update, 4, true};
  }
  if (op_type.find("op::less_equal") != std::string_view::npos) {
    return operator_info{binary_op_kind::less_equal, 3, false};
  }
  if (op_type.find("op::greater_equal") != std::string_view::npos) {
    return operator_info{binary_op_kind::greater_equal, 3, false};
  }
  if (op_type.find("op::less") != std::string_view::npos) {
    return operator_info{binary_op_kind::less, 3, false};
  }
  if (op_type.find("op::greater") != std::string_view::npos) {
    return operator_info{binary_op_kind::greater, 3, false};
  }
  if (op_type.find("op::not_equals") != std::string_view::npos) {
    return operator_info{binary_op_kind::not_equals, 3, false};
  }
  if (op_type.find("op::equals") != std::string_view::npos) {
    return operator_info{binary_op_kind::equals, 3, false};
  }
  if (op_type.find("op::logical_and") != std::string_view::npos) {
    return operator_info{binary_op_kind::logical_and, 2, false};
  }
  if (op_type.find("op::logical_or") != std::string_view::npos) {
    return operator_info{binary_op_kind::logical_or, 1, false};
  }
  if (op_type.find("op::implies") != std::string_view::npos) {
    return operator_info{binary_op_kind::logical_implies, 0, true};
  }
  if (op_type.find("op::pipe_right") != std::string_view::npos) {
    return operator_info{binary_op_kind::pipe_right, 8, false};
  }
  if (op_type.find("op::pipe_left") != std::string_view::npos) {
    return operator_info{binary_op_kind::pipe_left, 8, true};
  }

  return std::nullopt;
}

struct unary_operator_info {
  unary_op_kind kind;
  std::uint8_t precedence; // Higher = tighter binding (same convention as binary ops in this file)
};

inline auto get_unary_operator_info(std::string_view op_type)
    -> std::optional<unary_operator_info> {
  // Precedence values use "higher = tighter binding" convention (matching binary ops in this file).
  // In Nix, unary minus binds tighter than logical not:
  //   -x ? y  parses as  (-x) ? y   (unary minus is tighter)
  //   !x ? y  parses as  !(x ? y)   (? is tighter than !)
  //
  // has_attribute (?) has precedence 4, so:
  //   unary_minus needs precedence > 4 to bind tighter than ?
  //   logical_not needs precedence < 4 to bind looser than ?
  if (op_type.find("op::unary_minus") != std::string_view::npos) {
    return unary_operator_info{unary_op_kind::negate, 9}; // Tighter than ? (4)
  }
  if (op_type.find("op::logical_not") != std::string_view::npos) {
    return unary_operator_info{unary_op_kind::logical_not, 3}; // Looser than ? (4)
  }
  return std::nullopt;
}

// ============================================================================
// attribute path converter
// ============================================================================

// Forward declaration needed for convert_attribute_path
[[nodiscard]] auto convert_node(const p::parse_tree::node& node) -> tree;

inline auto convert_attribute_path(const p::parse_tree::node& path_node)
    -> std::vector<std::variant<std::string, tree>> {
  std::vector<std::variant<std::string, tree>> path;

  for (const auto& child : path_node.children) {
    const auto& type = child->type;
    if (type.find("attribute_simple") != std::string_view::npos) {
      // Simple identifier - store as string
      path.emplace_back(std::string(child->string_view()));
    } else if (type.find("attribute_dynamic") != std::string_view::npos) {
      // Dynamic attribute ${expr} - convert the expression child
      if (!child->children.empty()) {
        path.emplace_back(convert_node(*child->children[0]));
      }
    } else if (type.find("attribute_path") != std::string_view::npos) {
      // Nested attribute_path - recurse
      auto nested = convert_attribute_path(*child);
      for (auto& segment : nested) {
        path.emplace_back(std::move(segment));
      }
    } else {
      // Unknown attribute type - try to use string_view if it has content
      if (child->has_content()) {
        path.emplace_back(std::string(child->string_view()));
      }
    }
  }

  return path;
}

// ============================================================================
// precedence climbing for binary expressions
// ============================================================================

class precedence_climber {
public:
  explicit precedence_climber(const std::vector<std::unique_ptr<p::parse_tree::node>>& children)
      : children_(children), pos_(0) {}

  [[nodiscard]] auto parse() -> tree { return parse_expression(0); }

private:
  const std::vector<std::unique_ptr<p::parse_tree::node>>& children_;
  std::size_t pos_;

  [[nodiscard]] auto parse_expression(std::uint8_t min_precedence) -> tree {
    auto left = parse_primary();

    while (pos_ < children_.size()) {
      const auto& child = *children_[pos_];

      // Check for has_attribute operator (special case - attribute path is embedded)
      if (is_has_attribute_operator(child.type)) {
        // has_attribute has precedence 4 (from grammar)
        constexpr std::uint8_t has_attr_precedence = 4;
        if (has_attr_precedence < min_precedence) {
          break;
        }

        ++pos_; // consume the has_attribute operator

        // Extract the attribute path from the operator's children
        // The has_attribute operator contains the attribute_path
        std::vector<std::variant<std::string, tree>> path;
        for (const auto& attr_child : child.children) {
          if (attr_child->type.find("attribute_path") != std::string_view::npos) {
            path = convert_attribute_path(*attr_child);
          } else if (attr_child->type.find("attribute_simple") != std::string_view::npos) {
            // Direct attribute (single-segment path)
            path.emplace_back(std::string(attr_child->string_view()));
          } else if (attr_child->type.find("attribute_dynamic") != std::string_view::npos) {
            // Dynamic attribute
            if (!attr_child->children.empty()) {
              path.emplace_back(convert_node(*attr_child->children[0]));
            }
          }
        }

        auto span = make_span(child);
        left = make_tree<node_has_attribute>(std::move(left), std::move(path), span);
        continue;
      }

      // Check for binary operator
      auto op_info = get_operator_info(child.type);

      if (!op_info.has_value()) {
        break; // not an operator
      }

      if (op_info->precedence < min_precedence) {
        break;
      }

      ++pos_; // consume operator

      std::uint8_t next_min =
          op_info->right_associative ? op_info->precedence : op_info->precedence + 1;

      auto right = parse_expression(next_min);
      auto span = make_span(child);

      left = make_tree<node_binary_op>(op_info->kind, std::move(left), std::move(right), span);
    }

    return left;
  }

  [[nodiscard]] auto parse_primary() -> tree {
    if (pos_ >= children_.size()) {
      throw convert_error("unexpected end of expression");
    }

    const auto& child = *children_[pos_];

    // check for unary operator
    auto unary_info = get_unary_operator_info(child.type);
    if (unary_info.has_value()) {
      ++pos_;
      // Parse the operand with the unary operator's precedence.
      // This ensures that binary operators with tighter binding (lower precedence number)
      // are included in the operand. For example:
      //   !builtins ? nixVersion  ->  !(builtins ? nixVersion)
      // Because ? (precedence 4) binds tighter than ! (precedence 8).
      auto operand = parse_expression(unary_info->precedence);
      return make_tree<node_unary_op>(unary_info->kind, std::move(operand), make_span(child));
    }

    // otherwise it's a value
    ++pos_;
    return convert_node(child);
  }
};

// ============================================================================
// literal converters
// ============================================================================

inline auto convert_integer(const p::parse_tree::node& node) -> tree {
  auto text = node.string_view();
  std::int64_t value = 0;
  auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{}) {
    throw convert_error("invalid integer literal: " + std::string(text));
  }
  return make_tree<node_integer>(value, make_span(node));
}

inline auto convert_float(const p::parse_tree::node& node) -> tree {
  auto text = node.string_view();
  try {
    double value = std::stod(std::string(text));
    return make_tree<node_float>(value, make_span(node));
  } catch (...) {
    throw convert_error("invalid float literal: " + std::string(text));
  }
}

inline auto convert_identifier(const p::parse_tree::node& node) -> tree {
  return make_tree<node_identifier>(std::string(node.string_view()), make_span(node));
}

inline auto convert_string(const p::parse_tree::node& node) -> tree {
  // For simple strings, content is between quotes
  auto text = node.string_view();

  // Check if this has interpolation children
  if (node.children.empty()) {
    // Simple string - strip quotes and handle escapes
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
      text = text.substr(1, text.size() - 2);
    }
    // TODO: process escape sequences
    return make_tree<node_string>(std::string(text), make_span(node));
  }

  // Interpolated string - collect parts from children
  std::vector<std::variant<std::string, tree>> parts;
  std::string current_literal;

  for (const auto& child : node.children) {
    if (child->type.find("interpolation") != std::string_view::npos) {
      // Flush current literal
      if (!current_literal.empty()) {
        parts.emplace_back(std::move(current_literal));
        current_literal.clear();
      }
      // Convert the interpolated expression (first child of interpolation)
      if (!child->children.empty()) {
        parts.emplace_back(convert_node(*child->children[0]));
      }
    } else if (child->type.find("literal") != std::string_view::npos) {
      current_literal += child->string_view();
    } else if (child->type.find("escape") != std::string_view::npos) {
      auto esc = child->string_view();
      if (esc == "n") {
        current_literal += '\n';
      } else if (esc == "r") {
        current_literal += '\r';
      } else if (esc == "t") {
        current_literal += '\t';
      } else {
        current_literal += esc;
      }
    }
  }

  // Flush remaining literal
  if (!current_literal.empty()) {
    parts.emplace_back(std::move(current_literal));
  }

  if (parts.size() == 1 && std::holds_alternative<std::string>(parts[0])) {
    return make_tree<node_string>(std::get<std::string>(parts[0]), make_span(node));
  }

  return make_tree<node_string_interpolated>(std::move(parts), make_span(node));
}

// Helper to calculate minimum indentation of non-empty lines
inline auto calculate_min_indent(const std::vector<std::string_view>& lines) -> std::size_t {
  std::size_t min_indent = std::string_view::npos;
  for (const auto& line : lines) {
    if (line.empty()) {
      continue;
    }
    std::size_t indent = 0;
    for (char character : line) {
      if (character == ' ') {
        ++indent;
      } else if (character == '\t') {
        // Tabs count as moving to the next multiple of 8
        indent = ((indent / 8) + 1) * 8;
      } else {
        break;
      }
    }
    // Only count lines that have non-whitespace content
    if (indent < line.size()) {
      min_indent = std::min(min_indent, indent);
    }
  }
  return min_indent == std::string_view::npos ? 0 : min_indent;
}

// Helper to strip indentation from a line
inline auto strip_indent(std::string_view line, std::size_t indent) -> std::string_view {
  std::size_t current = 0;
  std::size_t position = 0;
  while (position < line.size() && current < indent) {
    if (line[position] == ' ') {
      ++current;
      ++position;
    } else if (line[position] == '\t') {
      std::size_t next = ((current / 8) + 1) * 8;
      if (next > indent) {
        break;
      }
      current = next;
      ++position;
    } else {
      break;
    }
  }
  return line.substr(position);
}

inline auto convert_indented_string(const p::parse_tree::node& node) -> tree {
  // Indented string content is between '' delimiters
  auto text = node.string_view();

  // Strip the '' delimiters
  if (text.size() >= 4 && text.substr(0, 2) == "''" && text.substr(text.size() - 2) == "''") {
    text = text.substr(2, text.size() - 4);
  }

  // If first character is newline, strip it (Nix semantics)
  if (!text.empty() && text.front() == '\n') {
    text = text.substr(1);
  }

  // Check if this has interpolation children
  if (node.children.empty()) {
    // Simple indented string - process indentation
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    for (std::size_t idx = 0; idx < text.size(); ++idx) {
      if (text[idx] == '\n') {
        lines.push_back(text.substr(start, idx - start));
        start = idx + 1;
      }
    }
    if (start <= text.size()) {
      lines.push_back(text.substr(start));
    }

    // Calculate minimum indentation
    std::size_t min_indent = calculate_min_indent(lines);

    // Build result with stripped indentation
    std::string result;
    for (std::size_t idx = 0; idx < lines.size(); ++idx) {
      if (idx > 0) {
        result += '\n';
      }
      auto stripped = strip_indent(lines[idx], min_indent);
      result += stripped;
    }

    // Handle escape sequences: ''' -> '', ''$ -> $, ''\n -> newline, etc.
    std::string processed;
    processed.reserve(result.size());
    for (std::size_t idx = 0; idx < result.size(); ++idx) {
      if (idx + 2 < result.size() && result[idx] == '\'' && result[idx + 1] == '\'') {
        char next = result[idx + 2];
        if (next == '\'') {
          // ''' -> ''
          processed += "''";
          idx += 2;
        } else if (next == '$') {
          // ''$ -> $
          processed += '$';
          idx += 2;
        } else if (next == '\\') {
          // ''\x -> escape sequence
          if (idx + 3 < result.size()) {
            char escape_char = result[idx + 3];
            if (escape_char == 'n') {
              processed += '\n';
            } else if (escape_char == 'r') {
              processed += '\r';
            } else if (escape_char == 't') {
              processed += '\t';
            } else {
              processed += escape_char;
            }
            idx += 3;
          } else {
            processed += result[idx];
          }
        } else {
          processed += result[idx];
        }
      } else {
        processed += result[idx];
      }
    }

    return make_tree<node_string>(std::move(processed), make_span(node));
  }

  // Interpolated indented string
  // Strategy: Use byte positions to find where each ${...} is in the raw string,
  // then extract literal parts between them.
  //
  // The raw content includes '' delimiters and everything between.
  // Each child is an interpolation expression. We need to find the ${ that
  // precedes each child and the } that follows it.

  auto full_text = node.string_view();
  std::size_t parent_start = node.m_begin.byte;

  // Collect interpolation positions (relative to parent start)
  struct interp_info {
    std::size_t dollar_brace_pos; // position of ${ in full_text
    std::size_t close_brace_pos;  // position of } in full_text
    tree expression;
  };
  std::vector<interp_info> interpolations;

  for (const auto& child : node.children) {
    // Child's byte position is absolute, convert to relative
    std::size_t child_start = child->m_begin.byte - parent_start;
    std::size_t child_end = child->m_end.byte - parent_start;

    // Find the ${ before this child - scan backwards from child_start
    std::size_t dollar_pos = child_start;
    while (dollar_pos > 0 && !(full_text[dollar_pos] == '$' && dollar_pos + 1 < full_text.size() &&
                               full_text[dollar_pos + 1] == '{')) {
      --dollar_pos;
    }

    // Find the } after this child
    std::size_t close_pos = child_end;
    while (close_pos < full_text.size() && full_text[close_pos] != '}') {
      ++close_pos;
    }

    interpolations.push_back(interp_info{dollar_pos, close_pos, convert_node(*child)});
  }

  // Start after the opening ''
  std::size_t content_start = 2;

  // Strip leading newline if present (Nix semantics)
  if (content_start < full_text.size() && full_text[content_start] == '\n') {
    ++content_start;
  }

  std::size_t end_pos = full_text.size() - 2;

  // First pass: collect all literal parts to calculate minimum indentation
  std::vector<std::string_view> all_lines;
  std::size_t literal_start = content_start;
  for (const auto& interp : interpolations) {
    if (interp.dollar_brace_pos > literal_start) {
      auto literal = full_text.substr(literal_start, interp.dollar_brace_pos - literal_start);
      // Split literal into lines
      std::size_t start = 0;
      for (std::size_t idx = 0; idx < literal.size(); ++idx) {
        if (literal[idx] == '\n') {
          all_lines.push_back(literal.substr(start, idx - start));
          start = idx + 1;
        }
      }
      if (start <= literal.size()) {
        all_lines.push_back(literal.substr(start));
      }
    }
    literal_start = interp.close_brace_pos + 1;
  }
  // Trailing literal
  if (literal_start < end_pos) {
    auto literal = full_text.substr(literal_start, end_pos - literal_start);
    std::size_t start = 0;
    for (std::size_t idx = 0; idx < literal.size(); ++idx) {
      if (literal[idx] == '\n') {
        all_lines.push_back(literal.substr(start, idx - start));
        start = idx + 1;
      }
    }
    if (start <= literal.size()) {
      all_lines.push_back(literal.substr(start));
    }
  }

  std::size_t min_indent = calculate_min_indent(all_lines);

  // Helper to strip indent from a literal string
  auto strip_literal_indent = [min_indent](const std::string& lit) -> std::string {
    std::string result;
    std::size_t start = 0;
    bool first_line = true;
    for (std::size_t idx = 0; idx <= lit.size(); ++idx) {
      if (idx == lit.size() || lit[idx] == '\n') {
        auto line = std::string_view(lit).substr(start, idx - start);
        if (!first_line) {
          result += '\n';
        }
        result += strip_indent(line, min_indent);
        first_line = false;
        start = idx + 1;
      }
    }
    return result;
  };

  // Second pass: build final parts with indent-stripped literals
  std::vector<std::variant<std::string, tree>> parts;
  literal_start = content_start;

  for (auto& interp : interpolations) {
    if (interp.dollar_brace_pos > literal_start) {
      auto literal = full_text.substr(literal_start, interp.dollar_brace_pos - literal_start);
      if (!literal.empty()) {
        parts.emplace_back(strip_literal_indent(std::string(literal)));
      }
    }
    parts.emplace_back(std::move(interp.expression));
    literal_start = interp.close_brace_pos + 1;
  }

  if (literal_start < end_pos) {
    auto literal = full_text.substr(literal_start, end_pos - literal_start);
    if (!literal.empty()) {
      parts.emplace_back(strip_literal_indent(std::string(literal)));
    }
  }

  if (parts.size() == 1 && std::holds_alternative<std::string>(parts[0])) {
    return make_tree<node_string>(std::get<std::string>(parts[0]), make_span(node));
  }

  return make_tree<node_string_interpolated>(std::move(parts), make_span(node));
}

inline auto convert_uri(const p::parse_tree::node& node) -> tree {
  return make_tree<node_uri>(std::string(node.string_view()), make_span(node));
}

inline auto convert_path(const p::parse_tree::node& node) -> tree {
  // TODO: handle interpolated paths
  return make_tree<node_path>(std::string(node.string_view()), make_span(node));
}

// ============================================================================
// compound expression converters
// ============================================================================

inline auto convert_list(const p::parse_tree::node& node) -> tree {
  std::vector<tree> elements;
  elements.reserve(node.children.size());

  for (const auto& child : node.children) {
    elements.push_back(convert_node(*child));
  }

  return make_tree<node_list>(std::move(elements), make_span(node));
}

inline auto convert_inherit_binding(const p::parse_tree::node& node) -> node_inherit_binding {
  // inherit_binding children:
  // - optionally: inherit_from (expression in parens)
  // - optionally: inherit_attributes (list of attribute names)

  std::optional<tree> from;
  std::vector<std::variant<std::string, tree>> attributes;

  for (const auto& child : node.children) {
    const auto& type = child->type;
    if (type.find("inherit_from") != std::string_view::npos) {
      // The inherit_from contains an expression
      if (!child->children.empty()) {
        from = convert_node(*child->children[0]);
      }
    } else if (type.find("attribute_simple") != std::string_view::npos) {
      attributes.emplace_back(std::string(child->string_view()));
    } else if (type.find("attribute_dynamic") != std::string_view::npos) {
      if (!child->children.empty()) {
        attributes.emplace_back(convert_node(*child->children[0]));
      }
    } else if (type.find("inherit_attributes") != std::string_view::npos ||
               type.find("attribute_path") != std::string_view::npos) {
      // Process children of the attributes list
      for (const auto& attr : child->children) {
        if (attr->type.find("attribute_simple") != std::string_view::npos) {
          attributes.emplace_back(std::string(attr->string_view()));
        } else if (attr->type.find("attribute_dynamic") != std::string_view::npos) {
          if (!attr->children.empty()) {
            attributes.emplace_back(convert_node(*attr->children[0]));
          }
        }
      }
    }
  }

  return node_inherit_binding{std::move(from), std::move(attributes), make_span(node)};
}

inline auto convert_attribute_binding(const p::parse_tree::node& node) -> node_attribute_binding {
  // attribute_binding children:
  // - binding_path (attribute_path)
  // - binding_value (expression)

  std::vector<std::variant<std::string, tree>> path;
  tree value;

  for (const auto& child : node.children) {
    const auto& type = child->type;
    if (type.find("binding_path") != std::string_view::npos ||
        type.find("attribute_path") != std::string_view::npos) {
      path = convert_attribute_path(*child);
    } else if (type.find("binding_value") != std::string_view::npos) {
      if (!child->children.empty()) {
        value = convert_node(*child->children[0]);
      }
    }
  }

  return node_attribute_binding{std::move(path), std::move(value), make_span(node)};
}

inline auto convert_attribute_set(const p::parse_tree::node& node, bool is_recursive) -> tree {
  std::vector<node_binding> bindings;

  for (const auto& child : node.children) {
    const auto& type = child->type;
    if (type.find("inherit_binding") != std::string_view::npos) {
      bindings.emplace_back(convert_inherit_binding(*child));
    } else if (type.find("attribute_binding") != std::string_view::npos) {
      bindings.emplace_back(convert_attribute_binding(*child));
    }
    // Skip other children (separators, braces, etc.)
  }

  return make_tree<node_attribute_set>(is_recursive, std::move(bindings), make_span(node));
}

inline auto convert_let(const p::parse_tree::node& node) -> tree {
  // let_expression children: bindings..., body expression
  // The body is the last child (after 'in' keyword)

  std::vector<node_binding> bindings;
  tree body;

  for (std::size_t idx = 0; idx < node.children.size(); ++idx) {
    const auto& child = *node.children[idx];
    const auto& type = child.type;

    if (type.find("inherit_binding") != std::string_view::npos) {
      bindings.emplace_back(convert_inherit_binding(child));
    } else if (type.find("attribute_binding") != std::string_view::npos) {
      bindings.emplace_back(convert_attribute_binding(child));
    } else {
      // Last non-binding child is the body
      body = convert_node(child);
    }
  }

  return make_tree<node_let>(std::move(bindings), std::move(body), make_span(node));
}

inline auto convert_if(const p::parse_tree::node& node) -> tree {
  // Children: condition, then_branch, else_branch
  if (node.children.size() != 3) {
    throw convert_error("if expression requires 3 children");
  }

  auto condition = convert_node(*node.children[0]);
  auto then_branch = convert_node(*node.children[1]);
  auto else_branch = convert_node(*node.children[2]);

  return make_tree<node_if>(std::move(condition), std::move(then_branch), std::move(else_branch),
                            make_span(node));
}

inline auto convert_with(const p::parse_tree::node& node) -> tree {
  if (node.children.size() != 2) {
    throw convert_error("with expression requires 2 children");
  }

  auto namespace_expr = convert_node(*node.children[0]);
  auto body = convert_node(*node.children[1]);

  return make_tree<node_with>(std::move(namespace_expr), std::move(body), make_span(node));
}

inline auto convert_assert(const p::parse_tree::node& node) -> tree {
  if (node.children.size() != 2) {
    throw convert_error("assert expression requires 2 children");
  }

  auto condition = convert_node(*node.children[0]);
  auto body = convert_node(*node.children[1]);

  return make_tree<node_assert>(std::move(condition), std::move(body), make_span(node));
}

inline auto convert_lambda_simple(const p::parse_tree::node& node) -> tree {
  // Children: argument name, body
  if (node.children.size() != 2) {
    throw convert_error("simple lambda requires 2 children");
  }

  auto arg_name = std::string(node.children[0]->string_view());
  auto body = convert_node(*node.children[1]);

  node_pattern pattern = node_pattern_simple{arg_name, make_span(*node.children[0])};

  return make_tree<node_lambda>(std::move(pattern), std::move(body), make_span(node));
}

inline auto convert_lambda_attrs(const p::parse_tree::node& node) -> tree {
  // Children can be in two orders:
  // { a }@args: body → [formals, lambda_argument, body]
  // args@{ a }: body → [lambda_argument, formals, body]
  //
  // Within formals:
  // - formal children (each with formal_name, optional default)
  // - optional formals_ellipsis

  std::vector<node_formal> formals;
  bool has_ellipsis = false;
  std::optional<std::string> at_name;
  tree body;
  source_span formals_span;

  for (const auto& child : node.children) {
    const auto& type = child->type;

    if (type.find("::formals") != std::string_view::npos) {
      formals_span = make_span(*child);
      // Process formals children
      for (const auto& formal_child : child->children) {
        const auto& formal_type = formal_child->type;

        if (formal_type.find("::formals_ellipsis") != std::string_view::npos) {
          has_ellipsis = true;
        } else if (formal_type.find("::formal") != std::string_view::npos) {
          // formal has formal_name as first child, optional default as second
          std::string name;
          std::optional<tree> default_value;
          source_span formal_span = make_span(*formal_child);

          for (const auto& part : formal_child->children) {
            const auto& part_type = part->type;
            if (part_type.find("::formal_name") != std::string_view::npos) {
              name = std::string(part->string_view());
            } else {
              // This is the default value (wrapped in application usually)
              default_value = convert_node(*part);
            }
          }

          formals.push_back(node_formal{std::move(name), std::move(default_value), formal_span});
        }
      }
    } else if (type.find("::lambda_argument") != std::string_view::npos) {
      at_name = std::string(child->string_view());
    } else {
      // Last child is the body
      body = convert_node(*child);
    }
  }

  node_pattern pattern =
      node_pattern_attrset{std::move(formals), has_ellipsis, std::move(at_name), formals_span};

  return make_tree<node_lambda>(std::move(pattern), std::move(body), make_span(node));
}

inline auto convert_select(const p::parse_tree::node& node) -> tree {
  // select children:
  // - First child is the subject (from select_head)
  // - Second child (if present) is select_attr (attribute path)
  // - Third child (if present) could be select_as_app_or then default value
  //
  // If there's only one child (no attribute path), just forward to that child

  if (node.children.empty()) {
    throw convert_error("empty select expression");
  }

  // Single child = no attribute access, forward through
  if (node.children.size() == 1) {
    return convert_node(*node.children[0]);
  }

  // First child is always the subject
  tree subject = convert_node(*node.children[0]);

  std::vector<std::variant<std::string, tree>> path;
  std::optional<tree> default_value;
  bool found_or = false;

  for (std::size_t idx = 1; idx < node.children.size(); ++idx) {
    const auto& child = *node.children[idx];
    const auto& type = child.type;

    // Skip 'or' marker but note we've seen it
    // There are two types of 'or' markers:
    // - select_as_app_or: for function application context (f or g)
    // - select_or_default: for select with default (x.a or default)
    if (type.find("select_as_app_or") != std::string_view::npos ||
        type.find("select_or_default") != std::string_view::npos) {
      found_or = true;
      continue;
    }

    // Handle attribute path nodes
    if (type.find("select_attr") != std::string_view::npos ||
        type.find("attribute_path") != std::string_view::npos) {
      path = convert_attribute_path(child);
      continue;
    }

    // Handle direct attribute_simple nodes (part of path, shouldn't happen but handle it)
    if (type.find("attribute_simple") != std::string_view::npos) {
      path.emplace_back(std::string(child.string_view()));
      continue;
    }

    // Handle direct attribute_dynamic nodes (part of path)
    if (type.find("attribute_dynamic") != std::string_view::npos) {
      if (!child.children.empty()) {
        path.emplace_back(convert_node(*child.children[0]));
      }
      continue;
    }

    // If after 'or', this is the default value (another select expression)
    if (found_or) {
      default_value = convert_node(child);
      continue;
    }
  }

  return make_tree<node_select>(std::move(subject), std::move(path), std::move(default_value),
                                make_span(node));
}

inline auto convert_binary_expression(const p::parse_tree::node& node) -> tree {
  if (node.children.empty()) {
    throw convert_error("empty binary expression");
  }

  if (node.children.size() == 1) {
    // Single child - just forward it
    return convert_node(*node.children[0]);
  }

  // Use precedence climbing
  precedence_climber climber(node.children);
  return climber.parse();
}

// Helper to check if a node is an attribute path component
inline auto is_attr_component(const p::parse_tree::node& node) -> bool {
  return node.type.find("attribute_simple") != std::string_view::npos ||
         node.type.find("attribute_dynamic") != std::string_view::npos;
}

inline auto convert_application(const p::parse_tree::node& node) -> tree {
  if (node.children.empty()) {
    throw convert_error("empty application");
  }

  if (node.children.size() == 1) {
    return convert_node(*node.children[0]);
  }

  // Check if this is a select expression (possibly with default):
  // Pattern 1: subject.attr.attr... (all after first are attrs)
  // Pattern 2: subject.attr.attr... or default (attrs followed by one non-attr)
  //
  // Count leading attribute components after first child
  std::size_t attr_count = 0;
  for (std::size_t idx = 1; idx < node.children.size(); ++idx) {
    if (is_attr_component(*node.children[idx])) {
      ++attr_count;
    } else {
      break;
    }
  }

  // It's a select if there's at least one attr component AND
  // either all remaining are attrs, or there's an "or" keyword followed by default
  bool has_attrs = attr_count > 0;
  std::size_t remaining = node.children.size() - 1 - attr_count;
  bool is_pure_select = has_attrs && remaining == 0;

  // Check for "or" keyword to identify select-with-default (vs application)
  bool has_or_keyword = false;
  if (has_attrs && remaining >= 1) {
    const auto& after_attrs = *node.children[1 + attr_count];
    has_or_keyword = after_attrs.type.find("select_or_default") != std::string_view::npos ||
                     after_attrs.type.find("select_as_app_or") != std::string_view::npos;
  }
  bool is_select_with_default = has_attrs && has_or_keyword && remaining == 2;

  if (is_pure_select || is_select_with_default) {
    // This is a select expression: subject.path [or default]
    tree subject = convert_node(*node.children[0]);
    std::vector<std::variant<std::string, tree>> path;
    path.reserve(attr_count);

    for (std::size_t idx = 1; idx <= attr_count; ++idx) {
      const auto& child = *node.children[idx];
      if (child.type.find("attribute_simple") != std::string_view::npos) {
        path.emplace_back(std::string(child.string_view()));
      } else if (child.type.find("attribute_dynamic") != std::string_view::npos) {
        if (!child.children.empty()) {
          path.emplace_back(convert_node(*child.children[0]));
        }
      }
    }

    std::optional<tree> default_value = std::nullopt;
    if (is_select_with_default) {
      // Skip past the "or" keyword to get the default value
      default_value = convert_node(*node.children[2 + attr_count]);
    }

    return make_tree<node_select>(std::move(subject), std::move(path), std::move(default_value),
                                  make_span(node));
  }

  // Handle application: function followed by arguments
  // The function may be a select expression (subject.path)
  std::vector<tree> arguments;
  std::size_t idx = 0;

  // Check if first child is followed by attr components - if so, function is a select
  tree function;
  if (node.children.size() > 1 && is_attr_component(*node.children[1])) {
    // Function is a select: first child + following attr components
    tree subject = convert_node(*node.children[0]);
    std::vector<std::variant<std::string, tree>> path;
    idx = 1;

    while (idx < node.children.size() && is_attr_component(*node.children[idx])) {
      const auto& child = *node.children[idx];
      if (child.type.find("attribute_simple") != std::string_view::npos) {
        path.emplace_back(std::string(child.string_view()));
      } else if (child.type.find("attribute_dynamic") != std::string_view::npos) {
        if (!child.children.empty()) {
          path.emplace_back(convert_node(*child.children[0]));
        }
      }
      ++idx;
    }

    function =
        make_tree<node_select>(std::move(subject), std::move(path), std::nullopt, make_span(node));
  } else {
    // Function is just the first child
    function = convert_node(*node.children[0]);
    idx = 1;
  }

  while (idx < node.children.size()) {
    // Check if current position starts a select: non-attr followed by attr components
    if (idx + 1 < node.children.size() && !is_attr_component(*node.children[idx]) &&
        is_attr_component(*node.children[idx + 1])) {
      // This is a select - consume subject and all following attr components
      tree subject = convert_node(*node.children[idx]);
      std::vector<std::variant<std::string, tree>> path;
      ++idx;

      while (idx < node.children.size() && is_attr_component(*node.children[idx])) {
        const auto& child = *node.children[idx];
        if (child.type.find("attribute_simple") != std::string_view::npos) {
          path.emplace_back(std::string(child.string_view()));
        } else if (child.type.find("attribute_dynamic") != std::string_view::npos) {
          if (!child.children.empty()) {
            path.emplace_back(convert_node(*child.children[0]));
          }
        }
        ++idx;
      }

      arguments.push_back(make_tree<node_select>(std::move(subject), std::move(path), std::nullopt,
                                                 make_span(node)));
    } else {
      // Regular argument
      arguments.push_back(convert_node(*node.children[idx]));
      ++idx;
    }
  }

  if (arguments.empty()) {
    // No arguments after function - just return function
    return function;
  }

  return make_tree<node_apply>(std::move(function), std::move(arguments), make_span(node));
}

// ============================================================================
// main dispatcher
// ============================================================================

[[nodiscard]] inline auto convert_node(const p::parse_tree::node& node) -> tree {
  const auto& type = node.type;

  // Root node - process first child
  if (node.is_root()) {
    if (node.children.empty()) {
      throw convert_error("empty parse tree");
    }
    return convert_node(*node.children[0]);
  }

  // Literals
  if (type.find("::integer") != std::string_view::npos) {
    return convert_integer(node);
  }
  if (type.find("::floating") != std::string_view::npos) {
    return convert_float(node);
  }
  if (type.find("::identifier") != std::string_view::npos) {
    return convert_identifier(node);
  }
  // attribute_simple can appear in select expressions - treat as identifier
  if (type.find("::attribute_simple") != std::string_view::npos) {
    return convert_identifier(node);
  }
  if (type.find("::string") != std::string_view::npos &&
      type.find("indented") == std::string_view::npos) {
    return convert_string(node);
  }
  if (type.find("::indented_string") != std::string_view::npos) {
    return convert_indented_string(node);
  }
  if (type.find("::uri") != std::string_view::npos) {
    return convert_uri(node);
  }
  if (type.find("::path") != std::string_view::npos) {
    return convert_path(node);
  }

  // Compound
  if (type.find("::list>") != std::string_view::npos ||
      type.find("expr::list") != std::string_view::npos) {
    return convert_list(node);
  }
  if (type.find("::recursive_set") != std::string_view::npos) {
    return convert_attribute_set(node, true);
  }
  if (type.find("::attribute_set") != std::string_view::npos) {
    return convert_attribute_set(node, false);
  }
  if (type.find("::let_expression") != std::string_view::npos) {
    return convert_let(node);
  }
  if (type.find("::if_expression") != std::string_view::npos) {
    return convert_if(node);
  }
  if (type.find("::with_expression") != std::string_view::npos) {
    return convert_with(node);
  }
  if (type.find("::assert_expression") != std::string_view::npos) {
    return convert_assert(node);
  }
  if (type.find("::binary_expression") != std::string_view::npos) {
    return convert_binary_expression(node);
  }
  if (type.find("::application") != std::string_view::npos) {
    return convert_application(node);
  }
  if (type.find("::lambda_pattern_simple") != std::string_view::npos) {
    return convert_lambda_simple(node);
  }
  if (type.find("::lambda_pattern_attrs") != std::string_view::npos) {
    return convert_lambda_attrs(node);
  }

  // Select expression (when it has an attribute path - due to fold_one)
  // Be careful not to match select_or_default or select_as_app_or or select_attr or select_head
  if ((type.find("::select>") != std::string_view::npos ||
       type.find("expr::select") != std::string_view::npos) &&
      type.find("select_or") == std::string_view::npos &&
      type.find("select_as") == std::string_view::npos &&
      type.find("select_attr") == std::string_view::npos &&
      type.find("select_head") == std::string_view::npos) {
    return convert_select(node);
  }

  // Transparent nodes - forward to child
  if (type.find("::select_head") != std::string_view::npos ||
      type.find("::list_entry") != std::string_view::npos) {
    if (node.children.size() == 1) {
      return convert_node(*node.children[0]);
    }
    if (!node.children.empty()) {
      return convert_node(*node.children[0]);
    }
  }

  // Lambda wrapper
  if (type.find("::lambda") != std::string_view::npos) {
    if (!node.children.empty()) {
      return convert_node(*node.children[0]);
    }
  }

  throw convert_error("unknown node type: " + std::string(type));
}

// ============================================================================
// entry point
// ============================================================================

/// Convert a PEGTL parse_tree to our tree type
[[nodiscard]] inline auto convert(const p::parse_tree::node& root) -> tree {
  return convert_node(root);
}

} // namespace straylight::nix::compiler::parse
