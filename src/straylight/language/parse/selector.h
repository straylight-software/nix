#pragma once
/// @file nix-language/parse/selector.h
/// PEGTL parse_tree selector - controls which grammar rules become tree nodes.
///
/// Rules listed here will appear in the parse tree. All others are transparent
/// (their children are hoisted to the parent). This keeps the tree minimal.

#include <tao/pegtl.hpp>
#include <tao/pegtl/contrib/parse_tree.hpp>

#include "straylight/language/parse/grammar.h"

namespace straylight::language::parse {

namespace p = tao::pegtl;
namespace g = grammar;

/// Selector template - default is to not store the rule
template <typename Rule>
struct tree_selector : std::false_type {};

// ============================================================================
// literals - store these with their content
// ============================================================================

template <>
struct tree_selector<g::expr::integer> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::expr::floating> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::expr::identifier> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::expr::string> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::expr::indented_string> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::expr::uri> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::expr::path_expr> : p::parse_tree::store_content {};

// ============================================================================
// string parts - for interpolation handling
// ============================================================================

template <typename... Content>
struct tree_selector<g::string_rule::literal<Content...>> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::string_rule::escape> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::string_rule::interpolation> : std::true_type {};

// ============================================================================
// operators - store the operator symbol
// ============================================================================

template <>
struct tree_selector<g::expr::operator_marker<g::op::add>> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::expr::operator_marker<g::op::subtract>> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::expr::operator_marker<g::op::multiply>> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::expr::operator_marker<g::op::divide>> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::expr::operator_marker<g::op::equals>> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::expr::operator_marker<g::op::not_equals>> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::expr::operator_marker<g::op::less>> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::expr::operator_marker<g::op::greater>> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::expr::operator_marker<g::op::less_equal>> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::expr::operator_marker<g::op::greater_equal>>
    : p::parse_tree::store_content {};

template <>
struct tree_selector<g::expr::operator_marker<g::op::logical_and>> : p::parse_tree::store_content {
};

template <>
struct tree_selector<g::expr::operator_marker<g::op::logical_or>> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::expr::operator_marker<g::op::implies>> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::expr::operator_marker<g::op::update>> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::expr::operator_marker<g::op::concatenate>> : p::parse_tree::store_content {
};

template <>
struct tree_selector<g::expr::operator_marker<g::op::pipe_right>> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::expr::operator_marker<g::op::pipe_left>> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::expr::operator_marker<g::op::logical_not>> : p::parse_tree::store_content {
};

template <>
struct tree_selector<g::expr::operator_marker<g::op::unary_minus>> : p::parse_tree::store_content {
};

template <>
struct tree_selector<g::expr::operator_marker<g::op::has_attribute>>
    : p::parse_tree::store_content {};

// ============================================================================
// compound expressions - store structure, discard content
// ============================================================================

template <>
struct tree_selector<g::expr::list> : std::true_type {};

// list_entry is transparent - its children bubble up to list
// template <>
// struct tree_selector<g::expr::list_entry> : std::true_type {};

template <>
struct tree_selector<g::expr::attribute_set> : std::true_type {};

template <>
struct tree_selector<g::expr::recursive_set> : std::true_type {};

// select is transparent - for now select expressions aren't fully supported
// TODO: implement proper select expression handling
// template <>
// struct tree_selector<g::expr::select> : std::true_type {};

// select_head is transparent - values bubble up
// template <>
// struct tree_selector<g::expr::select_head> : std::true_type {};

// select_attr is transparent - attribute_path children bubble up
// template <>
// struct tree_selector<g::expr::select_attr> : std::true_type {};

// select_as_app_or is captured for "or" default handling in function application context
template <>
struct tree_selector<g::expr::select_as_app_or> : std::true_type {};

// select_or_default is captured for "or" in select path context (x.y or z)
template <>
struct tree_selector<g::expr::select_or_default> : std::true_type {};

template <>
struct tree_selector<g::expr::application> : std::true_type {};

// ============================================================================
// attribute paths and bindings
// ============================================================================

template <>
struct tree_selector<g::attribute_path> : std::true_type {};

template <>
struct tree_selector<g::attribute_simple> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::attribute_dynamic> : std::true_type {};

template <>
struct tree_selector<g::binding_path> : std::true_type {};

template <>
struct tree_selector<g::binding_value> : std::true_type {};

template <>
struct tree_selector<g::inherit_binding> : std::true_type {};

template <>
struct tree_selector<g::inherit_from> : std::true_type {};

template <>
struct tree_selector<g::inherit_attributes> : std::true_type {};

template <>
struct tree_selector<g::attribute_binding> : std::true_type {};

// ============================================================================
// lambda
// ============================================================================

template <>
struct tree_selector<g::expr::lambda> : std::true_type {};

template <>
struct tree_selector<g::expr::lambda_pattern_simple> : std::true_type {};

template <>
struct tree_selector<g::expr::lambda_pattern_attrs> : std::true_type {};

template <>
struct tree_selector<g::expr::lambda_argument> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::formals> : std::true_type {};

template <>
struct tree_selector<g::formal> : std::true_type {};

template <>
struct tree_selector<g::formal_name> : p::parse_tree::store_content {};

template <>
struct tree_selector<g::formals_ellipsis> : std::true_type {};

// ============================================================================
// control flow
// ============================================================================

template <>
struct tree_selector<g::expr::if_expression> : std::true_type {};

template <>
struct tree_selector<g::expr::let_expression> : std::true_type {};

template <>
struct tree_selector<g::expr::with_expression> : std::true_type {};

template <>
struct tree_selector<g::expr::assert_expression> : std::true_type {};

// ============================================================================
// binary expression - need this to group operands/operators
// ============================================================================

template <>
struct tree_selector<g::expr::binary_expression> : p::parse_tree::fold_one {};

} // namespace straylight::language::parse
