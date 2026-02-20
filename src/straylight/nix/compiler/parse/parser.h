#pragma once
/// @file nix-language/parse/parser.h
/// Unified parser entry point.
///
/// Pipeline: source -> PEGTL parse_tree -> our tree -> AST
///
/// This file provides the main parse() function that takes source text
/// and returns an AST expression.

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#include <tao/pegtl.hpp>
#include <tao/pegtl/contrib/parse_tree.hpp>

#include "straylight/nix/compiler/ast/expression.h"
#include "straylight/nix/compiler/ast/symbol_table.h"
#include "straylight/nix/compiler/parse/convert.h"
#include "straylight/nix/compiler/parse/grammar.h"
#include "straylight/nix/compiler/parse/lower.h"
#include "straylight/nix/compiler/parse/selector.h"
#include "straylight/nix/compiler/parse/tree.h"

namespace straylight::nix::compiler::parse {

namespace p = tao::pegtl;
namespace g = grammar;

// ============================================================================
// parse error type
// ============================================================================

class parse_error : public std::runtime_error {
public:
  ast::source_position position;

  parse_error(const std::string& message, ast::source_position pos)
      : std::runtime_error(message), position(pos) {}

  explicit parse_error(const std::string& message)
      : std::runtime_error(message), position{0, 1, 1} {}
};

// ============================================================================
// parser entry point
// ============================================================================

/// Parse Nix source code to AST
///
/// @param source The Nix source code to parse
/// @param symbols Symbol table for interning identifiers
/// @param base_path Base path for resolving relative paths (default: ".")
/// @return The parsed AST expression
/// @throws parse_error on syntax errors
[[nodiscard]] inline auto parse(std::string_view source, ast::symbol_table& symbols,
                                std::filesystem::path base_path = ".") -> ast::expression {
  // Create PEGTL input
  p::memory_input input(source, "input");

  try {
    // Step 1: Parse source to PEGTL parse_tree
    auto pegtl_tree = p::parse_tree::parse<g::root, tree_selector>(input);

    if (!pegtl_tree) {
      throw parse_error("parse failed");
    }

    // Step 2: Convert PEGTL tree to our tree types
    tree our_tree = convert(*pegtl_tree);

    // Step 3: Lower our tree to AST
    return lower(our_tree, symbols);

  } catch (const p::parse_error& error) {
    // Extract position from PEGTL error
    const auto& positions = error.positions();
    if (!positions.empty()) {
      const auto& pos = positions[0];
      throw parse_error(std::string(error.message()),
                        ast::source_position{static_cast<std::uint32_t>(pos.byte),
                                             static_cast<std::uint32_t>(pos.line),
                                             static_cast<std::uint32_t>(pos.column)});
    }
    throw parse_error(std::string(error.message()));
  } catch (const convert_error& error) {
    throw parse_error(error.what());
  } catch (const lower_error& error) {
    throw parse_error(error.what());
  }
}

/// Parse to intermediate tree (for debugging/testing)
[[nodiscard]] inline auto parse_to_tree(std::string_view source) -> tree {
  p::memory_input input(source, "input");

  auto pegtl_tree = p::parse_tree::parse<g::root, tree_selector>(input);

  if (!pegtl_tree) {
    throw parse_error("parse failed");
  }

  return convert(*pegtl_tree);
}

/// Debug: dump PEGTL parse tree structure
inline void dump_pegtl_tree(const p::parse_tree::node& node, std::string& output, int depth = 0) {
  std::string indent(depth * 2, ' ');
  output += indent + std::string(node.type);
  if (node.has_content()) {
    output += " [" + std::string(node.string_view()) + "]";
  }
  output += "\n";
  for (const auto& child : node.children) {
    dump_pegtl_tree(*child, output, depth + 1);
  }
}

/// Debug: get PEGTL tree dump for source
[[nodiscard]] inline auto debug_parse_tree(std::string_view source) -> std::string {
  p::memory_input input(source, "input");

  auto pegtl_tree = p::parse_tree::parse<g::root, tree_selector>(input);

  if (!pegtl_tree) {
    return "parse failed";
  }

  std::string output;
  dump_pegtl_tree(*pegtl_tree, output);
  return output;
}

} // namespace straylight::nix::compiler::parse
