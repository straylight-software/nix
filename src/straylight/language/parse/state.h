#pragma once
///@file straylight/language/parse/state.h
/// Parser state management for PEGTL actions.

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "straylight/language/ast/expression.h"
#include "straylight/language/ast/symbol_table.h"

namespace straylight::language::parse {

/// parse error with position information
class parse_error : public std::runtime_error {
public:
  parse_error(std::string message, ast::source_position position)
      : std::runtime_error(std::move(message)), position_(position) {}

  [[nodiscard]] auto position() const noexcept -> ast::source_position { return position_; }

private:
  ast::source_position position_;
};

/// parser state passed through PEGTL actions
/// manages symbol interning, position tracking, and expression stack
struct parser_state {
  ast::symbol_table& symbols;
  std::filesystem::path base_path;
  const char* input_begin;

  /// compute source position from input iterator
  [[nodiscard]] auto position_at(const auto& input) const noexcept -> ast::source_position {
    auto byte_offset = static_cast<std::uint32_t>(input.begin() - input_begin);
    // line/column computed lazily if needed for error messages
    return ast::source_position{byte_offset, 0, 0};
  }

  [[nodiscard]] auto position_at_end(const auto& input) const noexcept -> ast::source_position {
    auto byte_offset = static_cast<std::uint32_t>(input.end() - input_begin);
    return ast::source_position{byte_offset, 0, 0};
  }
};

/// indented string line for ''..'' string processing
struct indented_string_line {
  std::string_view indentation;
  ast::source_position position;
  bool has_content = false;
  std::vector<std::pair<ast::source_position, std::variant<std::string_view, ast::expression>>>
      parts;
};

/// strip indentation from multiline strings per nix semantics
[[nodiscard]] inline auto strip_indentation(ast::source_position position,
                                            std::vector<indented_string_line>&& lines)
    -> ast::expression {
  // empty string case
  if (lines.size() == 1 && lines.front().parts.empty()) {
    return std::make_unique<ast::expression_node>(
        ast::expression_variant{ast::expression_string{position, ""}});
  }

  // trim trailing whitespace-only line
  if (lines.back().parts.empty()) {
    lines.back().indentation = {};
  }

  // find minimum indentation (ignoring whitespace-only lines)
  std::size_t minimum_indentation = std::string::npos;
  for (const auto& line : lines) {
    if (line.has_content) {
      minimum_indentation = std::min(minimum_indentation, line.indentation.size());
    }
  }
  if (minimum_indentation == std::string::npos) {
    minimum_indentation = 0;
  }

  // strip common indentation and concatenate
  std::vector<std::variant<std::string, ast::expression>> result_parts;
  std::string current_literal;

  auto flush_literal = [&]() {
    if (!current_literal.empty()) {
      result_parts.emplace_back(std::move(current_literal));
      current_literal.clear();
    }
  };

  for (auto& line : lines) {
    // add stripped indentation
    if (line.indentation.size() > minimum_indentation) {
      current_literal += line.indentation.substr(minimum_indentation);
    }

    for (auto& [part_position, part] : line.parts) {
      if (auto* str = std::get_if<std::string_view>(&part)) {
        current_literal += *str;
      } else {
        flush_literal();
        result_parts.emplace_back(std::move(std::get<ast::expression>(part)));
      }
    }
  }

  flush_literal();

  // single string case
  if (result_parts.size() == 1 && std::holds_alternative<std::string>(result_parts[0])) {
    return std::make_unique<ast::expression_node>(ast::expression_variant{
        ast::expression_string{position, std::get<std::string>(result_parts[0])}});
  }

  // interpolated string
  return std::make_unique<ast::expression_node>(ast::expression_variant{
      ast::expression_string_interpolated{position, std::move(result_parts)}});
}

} // namespace straylight::language::parse
