#pragma once
///@file straylight/language/parse/grammar.h
/// PEGTL grammar for Nix expressions.
/// Adapted from Lix project (LGPL-2.1), with modifications for WASM compilation.

#include <type_traits>
#include <variant>

#include <boost/container/small_vector.hpp>
#include <tao/pegtl.hpp>

// note: nix line endings are \n, \r\n (deprecated), \r (deprecated).
// the grammar does not use eol or eolf rules in favor of reproducing the
// old flex lexer as faithfully as possible, and deferring calculation of
// positions to downstream users.

namespace straylight::language::parse::grammar {

namespace p = tao::pegtl;

// ============================================================================
// character classes
// ============================================================================

namespace character {

struct path : p::sor<p::ranges<'a', 'z', 'A', 'Z', '0', '9'>, p::one<'.', '_', '-', '+'>> {};

struct path_separator : p::one<'/'> {};

struct identifier_first : p::ranges<'a', 'z', 'A', 'Z', '_'> {};

struct identifier_rest : p::sor<p::ranges<'a', 'z', 'A', 'Z', '0', '9'>, p::one<'_', '\'', '-'>> {};

struct uri_scheme_first : p::ranges<'a', 'z', 'A', 'Z'> {};

struct uri_scheme_rest : p::sor<p::ranges<'a', 'z', 'A', 'Z', '0', '9'>, p::one<'+', '-', '.'>> {};

struct uri_separator : p::one<':'> {};

struct uri_rest : p::sor<p::ranges<'a', 'z', 'A', 'Z', '0', '9'>,
                         p::one<'%', '/', '?', ':', '@', '&', '=', '+', '$', ',', '-', '_', '.',
                                '!', '~', '*', '\''>> {};

} // namespace character

// ============================================================================
// tokens
// ============================================================================

namespace token {

// helper: extend as path check
struct extend_as_path_ : p::seq<p::star<character::path>, p::not_at<TAO_PEGTL_STRING("/*")>,
                                p::not_at<TAO_PEGTL_STRING("//")>, character::path_separator,
                                p::sor<character::path, TAO_PEGTL_STRING("${")>> {};

// helper: extend as uri check
struct extend_as_uri_
    : p::seq<p::star<character::uri_scheme_rest>, character::uri_separator, character::uri_rest> {};

// keyword template: matches keyword only if not followed by identifier/path/uri
// continuation
template <typename S>
struct keyword_ : p::sor<p::seq<S, p::not_at<character::identifier_rest>,
                                p::not_at<extend_as_path_>, p::not_at<extend_as_uri_>>,
                         p::failure> {};

// keywords
struct keyword_if : keyword_<TAO_PEGTL_STRING("if")> {};
struct keyword_then : keyword_<TAO_PEGTL_STRING("then")> {};
struct keyword_else : keyword_<TAO_PEGTL_STRING("else")> {};
struct keyword_assert : keyword_<TAO_PEGTL_STRING("assert")> {};
struct keyword_with : keyword_<TAO_PEGTL_STRING("with")> {};
struct keyword_let : keyword_<TAO_PEGTL_STRING("let")> {};
struct keyword_in : keyword_<TAO_PEGTL_STRING("in")> {};
struct keyword_rec : keyword_<TAO_PEGTL_STRING("rec")> {};
struct keyword_inherit : keyword_<TAO_PEGTL_STRING("inherit")> {};
struct keyword_or : keyword_<TAO_PEGTL_STRING("or")> {};

// operators that need special handling
struct operator_minus : p::seq<p::one<'-'>, p::not_at<p::one<'>'>>, p::not_at<extend_as_path_>> {};

struct operator_divide : p::seq<p::one<'/'>, p::not_at<character::path>> {};

// match a rule, making sure we are not matching it where a keyword would match
template <typename... Rules>
struct not_at_any_keyword_
    : p::minus<p::seq<Rules...>,
               p::sor<TAO_PEGTL_STRING("inherit"), TAO_PEGTL_STRING("assert"),
                      TAO_PEGTL_STRING("else"), TAO_PEGTL_STRING("then"), TAO_PEGTL_STRING("with"),
                      TAO_PEGTL_STRING("let"), TAO_PEGTL_STRING("rec"), TAO_PEGTL_STRING("if"),
                      TAO_PEGTL_STRING("in"), TAO_PEGTL_STRING("or")>> {};

// identifier: complex due to path/uri ambiguity
struct identifier : not_at_any_keyword_<
                        p::sor<p::seq<character::uri_scheme_first,
                                      p::star<p::ranges<'a', 'z', 'A', 'Z', '0', '9', '-'>>,
                                      p::not_at<extend_as_uri_>>,
                               p::one<'_'>>,
                        p::star<p::sor<p::ranges<'a', 'z', 'A', 'Z', '0', '9'>, p::one<'_', '-'>>>,
                        p::not_at<extend_as_path_>, p::star<character::identifier_rest>> {};

// integer literal
// Nix allows leading zeros (007 is valid and equals 7)
// Not followed by `.digit` (that would be a float like 0.5)
struct integer
    : p::seq<p::sor<p::seq<p::range<'1', '9'>, p::star<p::digit>, p::not_at<p::one<'.'>>>,
                    p::seq<p::one<'0'>, p::not_at<p::one<'.'>, p::digit>, p::star<p::digit>>>,
             p::not_at<extend_as_path_>> {};

// float literal
// Valid: 1.0, 0.5, .5, 1.0e10 (must have digit after dot)
struct floating
    : p::seq<p::sor<p::seq<p::range<'1', '9'>, p::star<p::digit>, p::one<'.'>, p::plus<p::digit>>,
                    p::seq<p::one<'0'>, p::one<'.'>, p::plus<p::digit>>,
                    p::seq<p::one<'.'>, p::plus<p::digit>>>,
             p::opt<p::one<'E', 'e'>, p::opt<p::one<'+', '-'>>, p::plus<p::digit>>,
             p::not_at<extend_as_path_>> {};

// uri literal
struct uri : p::seq<character::uri_scheme_first, p::star<character::uri_scheme_rest>,
                    character::uri_separator, p::plus<character::uri_rest>> {};

// line ending (with deprecated CR handling)
struct end_of_line : p::sor<p::one<'\n'>, p::seq<p::one<'\r'>, p::opt<p::one<'\n'>>>> {};

// whitespace and comments
// Note: Block comment close uses TAO_PEGTL_STRING to avoid template parsing issues with '*','/'
struct block_comment_close : TAO_PEGTL_STRING("*/") {};

struct separator : p::sor<p::plus<p::one<' ', '\t'>>, end_of_line,
                          p::seq<p::one<'#'>, p::star<p::not_one<'\r', '\n'>>>,
                          p::seq<p::string<'/', '*'>, p::until<block_comment_close>>> {};

} // namespace token

// ============================================================================
// separators (whitespace/comments)
// ============================================================================

using separators = p::star<token::separator>;

// ============================================================================
// forward declarations
// ============================================================================

struct expression;

// ============================================================================
// strings
// ============================================================================

namespace string_rule {

template <typename... Inner>
struct literal : p::seq<Inner...> {};

struct interpolation : p::seq<p::string<'$', '{'>, separators, p::must<expression>, separators,
                              p::must<p::one<'}'>>> {};

struct escape : p::must<p::any> {};

} // namespace string_rule

struct string_double_quoted
    : p::seq<p::one<'"'>,
             p::star<p::sor<string_rule::literal<p::plus<p::not_one<'$', '"', '\\', '\r'>>>,
                            p::seq<p::one<'\r'>, p::opt<p::one<'\n'>>>, // cr/crlf
                            string_rule::interpolation,
                            string_rule::literal<p::one<'$'>, p::opt<p::one<'$'>>>,
                            p::seq<p::one<'\\'>, string_rule::escape>>>,
             p::must<p::one<'"'>>> {};

// ============================================================================
// indented strings
// ============================================================================

namespace indented_string_rule {

struct strip_first_line : p::seq<p::star<p::one<' '>>, p::one<'\n'>> {};

struct line_start : p::star<p::one<' '>> {};

template <typename... Inner>
struct literal : p::seq<Inner...> {};

struct interpolation : p::seq<p::string<'$', '{'>, separators, p::must<expression>, separators,
                              p::must<p::one<'}'>>> {};

struct escape : p::must<p::any> {};

} // namespace indented_string_rule

struct string_indented
    : p::seq<
          TAO_PEGTL_STRING("''"), p::opt<indented_string_rule::strip_first_line>,
          p::list<
              p::seq<indented_string_rule::line_start,
                     p::opt<p::plus<p::sor<
                         indented_string_rule::literal<p::plus<p::sor<
                             p::not_one<'$', '\'', '\n', '\r', '\0'>,
                             p::seq<p::one<'$'>, p::not_one<'{', '\'', '\n', '\r', '\0'>>,
                             p::seq<p::one<'$'>, p::at<p::one<'\n'>>>,
                             p::seq<p::one<'\''>, p::not_one<'\'', '$', '\n', '\r', '\0'>>,
                             p::seq<p::one<'\''>, p::at<p::one<'\n'>>>>>>,
                         indented_string_rule::interpolation,
                         indented_string_rule::literal<p::one<'$'>>,
                         indented_string_rule::literal<p::one<'\''>, p::not_at<p::one<'\''>>>,
                         p::seq<p::one<'\''>, indented_string_rule::literal<p::string<'\'', '\''>>>,
                         p::seq<p::string<'\'', '\''>,
                                p::sor<indented_string_rule::literal<p::one<'$'>>,
                                       p::seq<p::one<'\\'>, indented_string_rule::escape>>>>>>>,
              indented_string_rule::literal<p::one<'\n'>>>,
          p::must<TAO_PEGTL_STRING("''")>> {};

// ============================================================================
// paths
// ============================================================================

namespace path_rule {

struct path_segment : p::seq<p::star<character::path>, character::path_separator> {};
struct path_full
    : p::seq<p::star<character::path>, p::plus<character::path_separator, p::plus<character::path>>,
             p::opt<character::path_separator>> {};
struct home_path_full
    : p::seq<p::one<'~'>, p::plus<character::path_separator, p::plus<character::path>>,
             p::opt<character::path_separator>> {};

template <typename... Inner>
struct literal : p::seq<Inner...> {};

struct interpolation : p::seq<p::string<'$', '{'>, separators, p::must<expression>, separators,
                              p::must<p::one<'}'>>> {};

struct anchor : p::sor<path_full, p::seq<path_segment, p::at<TAO_PEGTL_STRING("${")>>> {};

struct home_anchor
    : p::sor<home_path_full, p::seq<TAO_PEGTL_STRING("~/"), p::at<TAO_PEGTL_STRING("${")>>> {};

struct searched_path : p::list<p::plus<character::path>, character::path_separator> {};

} // namespace path_rule

struct path
    : p::sor<p::seq<p::sor<path_rule::anchor, path_rule::home_anchor>,
                    p::star<p::sor<
                        path_rule::literal<p::sor<path_rule::path_full, path_rule::path_segment,
                                                  p::plus<character::path>>>,
                        path_rule::interpolation>>>,
             p::seq<p::one<'<'>, path_rule::searched_path, p::one<'>'>>> {};

// ============================================================================
// formals (lambda parameters)
// ============================================================================

struct formal_name : token::identifier {};

struct formal_default : p::must<expression> {};

struct formal : p::seq<formal_name, p::opt<separators, p::one<'?'>, separators, formal_default>> {};

struct formals_ellipsis : p::ellipsis {};

struct formals
    : p::seq<
          p::one<'{'>, separators,
          p::sor<p::one<'}'>, p::seq<formals_ellipsis, separators, p::must<p::one<'}'>>>,
                 p::seq<formal, separators,
                        p::if_then_else<p::at<p::one<','>>,
                                        p::seq<p::star<p::one<','>, separators, formal, separators>,
                                               p::opt<p::one<','>, separators,
                                                      p::opt<formals_ellipsis, separators>>,
                                               p::must<p::one<'}'>>>,
                                        p::one<'}'>>>>> {};

// ============================================================================
// attributes
// ============================================================================

struct attribute_simple : p::sor<token::identifier, token::keyword_or> {};

struct attribute_string : string_double_quoted {};

struct attribute_dynamic : p::seq<TAO_PEGTL_STRING("${"), separators, p::must<expression>,
                                  separators, p::must<p::one<'}'>>> {};

struct attribute : p::sor<attribute_simple, attribute_string, attribute_dynamic> {};

struct attribute_path : p::list<attribute, p::one<'.'>, token::separator> {};

// ============================================================================
// bindings
// ============================================================================

struct inherit_from : p::must<expression> {};

struct inherit_attributes : p::list<attribute, separators> {};

struct inherit_binding : p::seq<token::keyword_inherit, separators,
                                p::opt<p::one<'('>, separators, inherit_from, separators,
                                       p::must<p::one<')'>>, separators>,
                                p::opt<inherit_attributes, separators>, p::must<p::one<';'>>> {};

struct binding_path : attribute_path {};
struct binding_equal : p::one<'='> {};
struct binding_value : p::must<expression> {};

struct attribute_binding
    : p::seq<binding_path, separators, p::must<binding_equal>, separators, binding_value> {};

struct bindings
    : p::opt<p::list<
          p::sor<inherit_binding, p::seq<attribute_binding, separators, p::must<p::one<';'>>>>,
          separators>> {};

// ============================================================================
// operators
// ============================================================================

namespace op {

enum class kind {
  non_associative,
  left_associative,
  right_associative,
  unary,
};

template <typename Rule, unsigned Precedence, kind Kind = kind::left_associative>
struct operator_base : Rule {
  static constexpr unsigned precedence = Precedence;
  static constexpr op::kind associativity = Kind;
};

struct unary_minus : operator_base<token::operator_minus, 3, kind::unary> {};
struct has_attribute : operator_base<p::seq<p::one<'?'>, separators, p::must<attribute_path>>, 4> {
};
struct concatenate : operator_base<TAO_PEGTL_STRING("++"), 5, kind::right_associative> {};
struct multiply : operator_base<p::one<'*'>, 6> {};
struct divide : operator_base<token::operator_divide, 6> {};
struct add : operator_base<p::one<'+'>, 7> {};
struct subtract : operator_base<token::operator_minus, 7> {};
struct logical_not : operator_base<p::one<'!'>, 8, kind::unary> {};
struct update : operator_base<TAO_PEGTL_STRING("//"), 9, kind::right_associative> {};
struct less_equal : operator_base<TAO_PEGTL_STRING("<="), 10, kind::non_associative> {};
struct greater_equal : operator_base<TAO_PEGTL_STRING(">="), 10, kind::non_associative> {};
struct less : operator_base<p::one<'<'>, 10, kind::non_associative> {};
struct greater : operator_base<p::one<'>'>, 10, kind::non_associative> {};
struct equals : operator_base<TAO_PEGTL_STRING("=="), 11, kind::non_associative> {};
struct not_equals : operator_base<TAO_PEGTL_STRING("!="), 11, kind::non_associative> {};
struct logical_and : operator_base<TAO_PEGTL_STRING("&&"), 12> {};
struct logical_or : operator_base<TAO_PEGTL_STRING("||"), 13> {};
struct implies : operator_base<TAO_PEGTL_STRING("->"), 14, kind::right_associative> {};
struct pipe_right : operator_base<TAO_PEGTL_STRING("|>"), 15> {};
struct pipe_left : operator_base<TAO_PEGTL_STRING("<|"), 16, kind::right_associative> {};

} // namespace op

// ============================================================================
// expressions
// ============================================================================

namespace expr {

// attribute set helpers
template <template <typename...> class OpenMod = p::seq, typename... Init>
struct attrset_ : p::seq<Init..., OpenMod<p::one<'{'>>, separators, bindings, separators,
                         p::must<p::one<'}'>>> {};

// Forward declarations for mutual recursion
struct select;
struct simple;

struct identifier : token::identifier {};
struct integer : token::integer {};
struct floating : token::floating {};
struct string : string_double_quoted {};
struct indented_string : string_indented {};
struct path_expr : path {};
struct uri : token::uri {};
struct ancient_let : attrset_<p::must, token::keyword_let, separators> {};
struct recursive_set : attrset_<p::must, token::keyword_rec, separators> {};
struct attribute_set : attrset_<> {};

// list uses select as its entry type - uses forward reference
// The list_entry is just a marker for the select rule in list context
struct list_entry;

struct list : p::seq<p::one<'['>, separators, p::opt<p::list<list_entry, separators>, separators>,
                     p::must<p::one<']'>>> {};

// simple: all terminal expressions
struct simple
    : p::sor<identifier, integer, floating, string, indented_string, path_expr, uri,
             p::seq<p::one<'('>, separators, p::must<expression>, separators, p::must<p::one<')'>>>,
             ancient_let, recursive_set, attribute_set, list> {};

// select expression
struct select_head : simple {};
struct select_attr : attribute_path {};
struct select_as_app_or : token::keyword_or {};
struct select_or_default : token::keyword_or {}; // "or" in select path context (x.y or z)

struct select
    : p::seq<
          select_head, separators,
          p::opt<p::sor<p::seq<p::one<'.'>, separators, select_attr,
                               p::opt<separators, select_or_default, separators, p::must<select>>>,
                        select_as_app_or>>> {};

// Define list_entry as select now that select is complete
struct list_entry : select {};

// function application
struct app_select_or_function : select {};
struct app_first_argument : select {};
struct app_another_argument : select {};

struct application
    : p::seq<app_select_or_function,
             p::opt<separators, app_first_argument, p::star<separators, app_another_argument>>> {};

// operators
template <typename Op>
struct operator_marker : Op {};

struct binary_operator : p::sor<operator_marker<op::implies>, operator_marker<op::update>,
                                operator_marker<op::concatenate>, operator_marker<op::add>,
                                operator_marker<op::subtract>, operator_marker<op::multiply>,
                                operator_marker<op::divide>, operator_marker<op::pipe_right>,
                                operator_marker<op::pipe_left>, operator_marker<op::less_equal>,
                                operator_marker<op::greater_equal>, operator_marker<op::less>,
                                operator_marker<op::greater>, operator_marker<op::equals>,
                                operator_marker<op::not_equals>, operator_marker<op::logical_or>,
                                operator_marker<op::logical_and>> {};

struct unary
    : p::seq<p::star<p::sor<operator_marker<op::logical_not>, operator_marker<op::unary_minus>>,
                     separators>,
             application> {};

struct binary_expression
    : p::seq<unary, p::star<separators, p::sor<p::seq<binary_operator, separators, p::must<unary>>,
                                               operator_marker<op::has_attribute>>>> {};

// lambda patterns
struct lambda_argument : token::identifier {};
struct lambda_body : p::seq<p::one<':'>, separators, p::must<expression>> {};
struct lambda_must_body : p::must<p::one<':'>, separators, expression> {};

struct lambda_pattern_simple : p::seq<lambda_argument, separators, lambda_body> {};

struct lambda_pattern_attrs
    : p::sor<p::seq<lambda_argument, separators, p::one<'@'>, separators, p::must<formals>,
                    separators, lambda_must_body>,
             p::seq<formals, separators,
                    p::sor<p::seq<p::one<'@'>, separators, p::must<lambda_argument>, separators,
                                  lambda_must_body>,
                           lambda_body>>> {};

struct lambda : p::sor<lambda_pattern_simple, lambda_pattern_attrs> {};

// compound expressions
struct assert_expression
    : p::seq<token::keyword_assert, separators, p::must<expression>, separators,
             p::must<p::one<';'>>, separators, p::must<expression>> {};

struct with_expression : p::seq<token::keyword_with, separators, p::must<expression>, separators,
                                p::must<p::one<';'>>, separators, p::must<expression>> {};

struct let_expression
    : p::seq<token::keyword_let, separators,
             p::not_at<p::one<'{'>>, // exclude ancient_let
             bindings, separators, p::must<token::keyword_in>, separators, p::must<expression>> {};

struct if_expression
    : p::seq<token::keyword_if, separators, p::must<expression>, separators,
             p::must<token::keyword_then>, separators, p::must<expression>, separators,
             p::must<token::keyword_else>, separators, p::must<expression>> {};

} // namespace expr

// main expression rule
struct expression : p::sor<expr::lambda, expr::assert_expression, expr::with_expression,
                           expr::let_expression, expr::if_expression, expr::binary_expression> {};

// ============================================================================
// root rules
// ============================================================================

struct end_of_file : p::sor<p::eof, p::one<0>> {};

struct root : p::must<separators, expression, separators, end_of_file> {};

} // namespace straylight::language::parse::grammar
