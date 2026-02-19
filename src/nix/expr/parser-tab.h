// A Bison parser, made by GNU Bison 3.8.2.

// Skeleton interface for Bison LALR(1) parsers in C++

// Copyright (C) 2002-2015, 2018-2021 Free Software Foundation, Inc.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// As a special exception, you may create a larger work that contains
// part or all of the Bison parser skeleton and distribute that work
// under terms of your choice, so long as that work isn't itself a
// parser generator using the skeleton or a modified version thereof
// as a parser skeleton.  Alternatively, if you modify or redistribute
// the parser skeleton itself, you may (at your option) remove this
// special exception, which will cause the skeleton and the resulting
// Bison output files to be licensed under the GNU General Public
// License without this special exception.

// This special exception was added by the Free Software Foundation in
// version 2.2 of Bison.


/**
 ** \file parser-tab.hpp
 ** Define the  ::nix::parser ::parser class.
 */

// C++ LALR(1) parser skeleton written by Akim Demaille.

// DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
// especially those whose name start with YY_ or yy_.  They are
// private implementation details that can be changed or removed.

#ifndef YY_YY_PARSER_TAB_HPP_INCLUDED
#define YY_YY_PARSER_TAB_HPP_INCLUDED
// "%code requires" blocks.
#line 15 "parser.y"


// bison adds a bunch of switch statements with default:
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"

#ifndef BISON_HEADER
#  define BISON_HEADER

#  include <variant>

#  include "nix/expr/eval-settings.h"
#  include "nix/expr/eval.h"
#  include "nix/expr/nixexpr.h"
#  include "nix/expr/parser-state.h"
#  include "nix/util/finally.h"
#  include "nix/util/users.h"
#  include "nix/util/util.h"

#  define YY_DECL                                                                                  \
    int yylex(nix::Parser::value_type* yylval_param, nix::Parser::location_type* yylloc_param,     \
              yyscan_t yyscanner, nix::ParserState* state)

// For efficiency, we only track offsets; not line,column coordinates
#  define YYLLOC_DEFAULT(Current, Rhs, N)                                                          \
    do                                                                                             \
      if (N) {                                                                                     \
        (Current).beginOffset = YYRHSLOC(Rhs, 1).beginOffset;                                      \
        (Current).endOffset = YYRHSLOC(Rhs, N).endOffset;                                          \
      } else {                                                                                     \
        (Current).beginOffset = (Current).endOffset = YYRHSLOC(Rhs, 0).endOffset;                  \
      }                                                                                            \
    while (0)

namespace nix {

typedef boost::unordered_flat_map<pos_idx_t, DocComment, std::hash<pos_idx_t>> DocCommentMap;

Expr* parseExprFromBuf(char* text, size_t length, Pos::origin_t origin, const source_path_t& basePath,
                       Exprs& exprs, SymbolTable& symbols, const EvalSettings& settings,
                       pos_table_t& positions, DocCommentMap& docComments,
                       const ref<SourceAccessor> rootFS);

} // namespace nix

#endif


#line 113 "parser-tab.hpp"


#include <cstdlib> // std::abort
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined __cplusplus
#  define YY_CPLUSPLUS __cplusplus
#else
#  define YY_CPLUSPLUS 199711L
#endif

// Support move semantics when possible.
#if 201103L <= YY_CPLUSPLUS
#  define YY_MOVE std::move
#  define YY_MOVE_OR_COPY move
#  define YY_MOVE_REF(Type) Type&&
#  define YY_RVREF(Type) Type&&
#  define YY_COPY(Type) Type
#else
#  define YY_MOVE
#  define YY_MOVE_OR_COPY copy
#  define YY_MOVE_REF(Type) Type&
#  define YY_RVREF(Type) const Type&
#  define YY_COPY(Type) const Type&
#endif

// Support noexcept when possible.
#if 201103L <= YY_CPLUSPLUS
#  define YY_NOEXCEPT noexcept
#  define YY_NOTHROW
#else
#  define YY_NOEXCEPT
#  define YY_NOTHROW throw()
#endif

// Support constexpr when possible.
#if 201703 <= YY_CPLUSPLUS
#  define YY_CONSTEXPR constexpr
#else
#  define YY_CONSTEXPR
#endif


#ifndef YY_ATTRIBUTE_PURE
#  if defined __GNUC__ && 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
#    define YY_ATTRIBUTE_PURE __attribute__((__pure__))
#  else
#    define YY_ATTRIBUTE_PURE
#  endif
#endif

#ifndef YY_ATTRIBUTE_UNUSED
#  if defined __GNUC__ && 2 < __GNUC__ + (7 <= __GNUC_MINOR__)
#    define YY_ATTRIBUTE_UNUSED __attribute__((__unused__))
#  else
#    define YY_ATTRIBUTE_UNUSED
#  endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if !defined lint || defined __GNUC__
#  define YY_USE(E) ((void)(E))
#else
#  define YY_USE(E) /* empty */
#endif

/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
#if defined __GNUC__ && !defined __ICC && 406 <= __GNUC__ * 100 + __GNUC_MINOR__
#  if __GNUC__ * 100 + __GNUC_MINOR__ < 407
#    define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                                                    \
      _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wuninitialized\"")
#  else
#    define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                                                    \
      _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wuninitialized\"")         \
          _Pragma("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
#  endif
#  define YY_IGNORE_MAYBE_UNINITIALIZED_END _Pragma("GCC diagnostic pop")
#else
#  define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
#  define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
#  define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif

#if defined __cplusplus && defined __GNUC__ && !defined __ICC && 6 <= __GNUC__
#  define YY_IGNORE_USELESS_CAST_BEGIN                                                             \
    _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wuseless-cast\"")
#  define YY_IGNORE_USELESS_CAST_END _Pragma("GCC diagnostic pop")
#endif
#ifndef YY_IGNORE_USELESS_CAST_BEGIN
#  define YY_IGNORE_USELESS_CAST_BEGIN
#  define YY_IGNORE_USELESS_CAST_END
#endif

#ifndef YY_CAST
#  ifdef __cplusplus
#    define YY_CAST(Type, Val) static_cast<Type>(Val)
#    define YY_REINTERPRET_CAST(Type, Val) reinterpret_cast<Type>(Val)
#  else
#    define YY_CAST(Type, Val) ((Type)(Val))
#    define YY_REINTERPRET_CAST(Type, Val) ((Type)(Val))
#  endif
#endif
#ifndef YY_NULLPTR
#  if defined __cplusplus
#    if 201103L <= __cplusplus
#      define YY_NULLPTR nullptr
#    else
#      define YY_NULLPTR 0
#    endif
#  else
#    define YY_NULLPTR ((void*)0)
#  endif
#endif

/* Debug traces.  */
#ifndef YYDEBUG
#  define YYDEBUG 0
#endif

#line 3 "parser.y"
namespace nix {
namespace parser {
#line 249 "parser-tab.hpp"


/// A Bison parser.
class bison_parser_t {
public:
#ifdef YYSTYPE
#  ifdef __GNUC__
#    pragma GCC message "bison: do not #define YYSTYPE in C++, use %define api.value.type"
#  endif
  typedef YYSTYPE value_type;
#else
  /// A buffer to store and retrieve objects.
  ///
  /// Sort of a variant, but does not keep track of the nature
  /// of the stored data, since that knowledge is available
  /// via the current parser state.
  class value_type {
  public:
    /// Type of *this.
    typedef value_type self_type;

    /// Empty construction.
    value_type() YY_NOEXCEPT : yyraw_() {}

    /// Construct and fill.
    template <typename T>
    value_type(YY_RVREF(T) t) {
      new (yyas_<T>()) T(YY_MOVE(t));
    }

#  if 201103L <= YY_CPLUSPLUS
    /// Non copyable.
    value_type(const self_type&) = delete;
    /// Non copyable.
    self_type& operator=(const self_type&) = delete;
#  endif

    /// Destruction, allowed only if empty.
    ~value_type() YY_NOEXCEPT {}

#  if 201103L <= YY_CPLUSPLUS
    /// Instantiate a \a T in here from \a t.
    template <typename T, typename... U>
    T& emplace(U&&... u) {
      return *new (yyas_<T>()) T(std::forward<U>(u)...);
    }
#  else
    /// Instantiate an empty \a T in here.
    template <typename T>
    T& emplace() {
      return *new (yyas_<T>()) T();
    }

    /// Instantiate a \a T in here from \a t.
    template <typename T>
    T& emplace(const T& t) {
      return *new (yyas_<T>()) T(t);
    }
#  endif

    /// Instantiate an empty \a T in here.
    /// Obsolete, use emplace.
    template <typename T>
    T& build() {
      return emplace<T>();
    }

    /// Instantiate a \a T in here from \a t.
    /// Obsolete, use emplace.
    template <typename T>
    T& build(const T& t) {
      return emplace<T>(t);
    }

    /// Accessor to a built \a T.
    template <typename T>
    T& as() YY_NOEXCEPT {
      return *yyas_<T>();
    }

    /// Const accessor to a built \a T (for %printer).
    template <typename T>
    const T& as() const YY_NOEXCEPT {
      return *yyas_<T>();
    }

    /// Swap the content with \a that, of same type.
    ///
    /// Both variants must be built beforehand, because swapping the actual
    /// data requires reading it (with as()), and this is not possible on
    /// unconstructed variants: it would require some dynamic testing, which
    /// should not be the variant's responsibility.
    /// Swapping between built and (possibly) non-built is done with
    /// self_type::move ().
    template <typename T>
    void swap(self_type& that) YY_NOEXCEPT {
      std::swap(as<T>(), that.as<T>());
    }

    /// Move the content of \a that to this.
    ///
    /// Destroys \a that.
    template <typename T>
    void move(self_type& that) {
#  if 201103L <= YY_CPLUSPLUS
      emplace<T>(std::move(that.as<T>()));
#  else
      emplace<T>();
      swap<T>(that);
#  endif
      that.destroy<T>();
    }

#  if 201103L <= YY_CPLUSPLUS
    /// Move the content of \a that to this.
    template <typename T>
    void move(self_type&& that) {
      emplace<T>(std::move(that.as<T>()));
      that.destroy<T>();
    }
#  endif

    /// Copy the content of \a that to this.
    template <typename T>
    void copy(const self_type& that) {
      emplace<T>(that.as<T>());
    }

    /// Destroy the stored \a T.
    template <typename T>
    void destroy() {
      as<T>().~T();
    }

  private:
#  if YY_CPLUSPLUS < 201103L
    /// Non copyable.
    value_type(const self_type&);
    /// Non copyable.
    self_type& operator=(const self_type&);
#  endif

    /// Accessor to raw memory as \a T.
    template <typename T>
    T* yyas_() YY_NOEXCEPT {
      void* yyp = yyraw_;
      return static_cast<T*>(yyp);
    }

    /// Const accessor to raw memory as \a T.
    template <typename T>
    const T* yyas_() const YY_NOEXCEPT {
      const void* yyp = yyraw_;
      return static_cast<const T*>(yyp);
    }

    /// An auxiliary type to compute the largest semantic type.
    union union_type {
      // start
      // expr
      // expr_function
      // expr_if
      // expr_pipe_from
      // expr_pipe_into
      // expr_op
      // expr_app
      // expr_select
      // expr_simple
      // path_start
      char dummy1[sizeof(Expr*)];

      // binds
      // binds1
      char dummy2[sizeof(ExprAttrs*)];

      // formal
      char dummy3[sizeof(Formal)];

      // formal_set
      // formals
      char dummy4[sizeof(FormalsBuilder)];

      // FLOAT_LIT
      char dummy5[sizeof(NixFloat)];

      // INT_LIT
      char dummy6[sizeof(NixInt)];

      // ID
      // STR
      // IND_STR
      // PATH
      // HPATH
      // SPATH
      // PATH_END
      // URI
      // attr
      char dummy7[sizeof(StringToken)];

      // string_parts
      // string_attr
      char dummy8[sizeof(ToBeStringyExpr)];

      // list
      char dummy9[sizeof(std::pmr::vector<Expr*>)];

      // attrpath
      char dummy10[sizeof(std::vector<AttrName>)];

      // attrs
      char dummy11[sizeof(std::vector<std::pair<AttrName, pos_idx_t>>)];

      // string_parts_interpolated
      char dummy12[sizeof(std::vector<std::pair<pos_idx_t, Expr*>>)];

      // ind_string_parts
      char dummy13[sizeof(std::vector<std::pair<pos_idx_t, std::variant<Expr*, StringToken>>>)];
    };

    /// The size of the largest semantic type.
    enum { size = sizeof(union_type) };

    /// A buffer to store semantic values.
    union {
      /// Strongest alignment constraints.
      long double yyalign_me_;
      /// A buffer large enough to store any of the semantic values.
      char yyraw_[size];
    };
  };

#endif
  /// Backward compatibility (Bison 3.8).
  typedef value_type semantic_type;

  /// Symbol locations.
  typedef ::nix::ParserLocation location_type;

  /// Syntax errors thrown from user actions.
  struct syntax_error : std::runtime_error {
    syntax_error(const location_type& l, const std::string& m)
        : std::runtime_error(m), location(l) {}

    syntax_error(const syntax_error& s) : std::runtime_error(s.what()), location(s.location) {}

    ~syntax_error() YY_NOEXCEPT YY_NOTHROW;

    location_type location;
  };

  /// Token kinds.
  struct token {
    enum token_kind_type {
      YYEMPTY = -2,
      YYEOF = 0,              // "end of file"
      YYerror = 256,          // error
      YYUNDEF = 257,          // "invalid token"
      ID = 258,               // ID
      STR = 259,              // STR
      IND_STR = 260,          // IND_STR
      INT_LIT = 261,          // INT_LIT
      FLOAT_LIT = 262,        // FLOAT_LIT
      PATH = 263,             // PATH
      HPATH = 264,            // HPATH
      SPATH = 265,            // SPATH
      PATH_END = 266,         // PATH_END
      URI = 267,              // URI
      IF = 268,               // IF
      THEN = 269,             // THEN
      ELSE = 270,             // ELSE
      ASSERT = 271,           // ASSERT
      WITH = 272,             // WITH
      LET = 273,              // LET
      IN_KW = 274,            // IN_KW
      REC = 275,              // REC
      INHERIT = 276,          // INHERIT
      EQ = 277,               // EQ
      NEQ = 278,              // NEQ
      AND = 279,              // AND
      OR = 280,               // OR
      IMPL = 281,             // IMPL
      OR_KW = 282,            // OR_KW
      PIPE_FROM = 283,        // PIPE_FROM
      PIPE_INTO = 284,        // PIPE_INTO
      DOLLAR_CURLY = 285,     // DOLLAR_CURLY
      IND_STRING_OPEN = 286,  // IND_STRING_OPEN
      IND_STRING_CLOSE = 287, // IND_STRING_CLOSE
      ELLIPSIS = 288,         // ELLIPSIS
      LEQ = 289,              // LEQ
      GEQ = 290,              // GEQ
      UPDATE = 291,           // UPDATE
      NOT = 292,              // NOT
      CONCAT = 293,           // CONCAT
      NEGATE = 294            // NEGATE
    };
    /// Backward compatibility alias (Bison 3.6).
    typedef token_kind_type yytokentype;
  };

  /// Token kind, as returned by yylex.
  typedef token::token_kind_type token_kind_type;

  /// Backward compatibility alias (Bison 3.6).
  typedef token_kind_type token_type;

  /// Symbol kinds.
  struct symbol_kind {
    enum symbol_kind_type {
      YYNTOKENS = 61, ///< Number of tokens.
      S_YYEMPTY = -2,
      S_YYEOF = 0,                      // "end of file"
      S_YYerror = 1,                    // error
      S_YYUNDEF = 2,                    // "invalid token"
      S_ID = 3,                         // ID
      S_STR = 4,                        // STR
      S_IND_STR = 5,                    // IND_STR
      S_INT_LIT = 6,                    // INT_LIT
      S_FLOAT_LIT = 7,                  // FLOAT_LIT
      S_PATH = 8,                       // PATH
      S_HPATH = 9,                      // HPATH
      S_SPATH = 10,                     // SPATH
      S_PATH_END = 11,                  // PATH_END
      S_URI = 12,                       // URI
      S_IF = 13,                        // IF
      S_THEN = 14,                      // THEN
      S_ELSE = 15,                      // ELSE
      S_ASSERT = 16,                    // ASSERT
      S_WITH = 17,                      // WITH
      S_LET = 18,                       // LET
      S_IN_KW = 19,                     // IN_KW
      S_REC = 20,                       // REC
      S_INHERIT = 21,                   // INHERIT
      S_EQ = 22,                        // EQ
      S_NEQ = 23,                       // NEQ
      S_AND = 24,                       // AND
      S_OR = 25,                        // OR
      S_IMPL = 26,                      // IMPL
      S_OR_KW = 27,                     // OR_KW
      S_PIPE_FROM = 28,                 // PIPE_FROM
      S_PIPE_INTO = 29,                 // PIPE_INTO
      S_DOLLAR_CURLY = 30,              // DOLLAR_CURLY
      S_IND_STRING_OPEN = 31,           // IND_STRING_OPEN
      S_IND_STRING_CLOSE = 32,          // IND_STRING_CLOSE
      S_ELLIPSIS = 33,                  // ELLIPSIS
      S_34_ = 34,                       // '<'
      S_35_ = 35,                       // '>'
      S_LEQ = 36,                       // LEQ
      S_GEQ = 37,                       // GEQ
      S_UPDATE = 38,                    // UPDATE
      S_NOT = 39,                       // NOT
      S_40_ = 40,                       // '+'
      S_41_ = 41,                       // '-'
      S_42_ = 42,                       // '*'
      S_43_ = 43,                       // '/'
      S_CONCAT = 44,                    // CONCAT
      S_45_ = 45,                       // '?'
      S_NEGATE = 46,                    // NEGATE
      S_47_ = 47,                       // ':'
      S_48_ = 48,                       // '@'
      S_49_ = 49,                       // ';'
      S_50_ = 50,                       // '!'
      S_51_ = 51,                       // '.'
      S_52_ = 52,                       // '"'
      S_53_ = 53,                       // '('
      S_54_ = 54,                       // ')'
      S_55_ = 55,                       // '{'
      S_56_ = 56,                       // '}'
      S_57_ = 57,                       // '['
      S_58_ = 58,                       // ']'
      S_59_ = 59,                       // '='
      S_60_ = 60,                       // ','
      S_YYACCEPT = 61,                  // $accept
      S_start = 62,                     // start
      S_expr = 63,                      // expr
      S_expr_function = 64,             // expr_function
      S_expr_if = 65,                   // expr_if
      S_expr_pipe_from = 66,            // expr_pipe_from
      S_expr_pipe_into = 67,            // expr_pipe_into
      S_expr_op = 68,                   // expr_op
      S_expr_app = 69,                  // expr_app
      S_expr_select = 70,               // expr_select
      S_expr_simple = 71,               // expr_simple
      S_string_parts = 72,              // string_parts
      S_string_parts_interpolated = 73, // string_parts_interpolated
      S_path_start = 74,                // path_start
      S_ind_string_parts = 75,          // ind_string_parts
      S_binds = 76,                     // binds
      S_binds1 = 77,                    // binds1
      S_attrs = 78,                     // attrs
      S_attrpath = 79,                  // attrpath
      S_attr = 80,                      // attr
      S_string_attr = 81,               // string_attr
      S_list = 82,                      // list
      S_formal_set = 83,                // formal_set
      S_formals = 84,                   // formals
      S_formal = 85                     // formal
    };
  };

  /// (Internal) symbol kind.
  typedef symbol_kind::symbol_kind_type symbol_kind_type;

  /// The number of tokens.
  static const symbol_kind_type YYNTOKENS = symbol_kind::YYNTOKENS;

  /// A complete symbol.
  ///
  /// Expects its Base type to provide access to the symbol kind
  /// via kind ().
  ///
  /// Provide access to semantic value and location.
  template <typename Base>
  struct basic_symbol : Base {
    /// Alias to Base.
    typedef Base super_type;

    /// Default constructor.
    basic_symbol() YY_NOEXCEPT : value(), location() {}

#if 201103L <= YY_CPLUSPLUS
    /// Move constructor.
    basic_symbol(basic_symbol&& that)
        : Base(std::move(that)), value(), location(std::move(that.location)) {
      switch (this->kind()) {
        case symbol_kind::S_start:          // start
        case symbol_kind::S_expr:           // expr
        case symbol_kind::S_expr_function:  // expr_function
        case symbol_kind::S_expr_if:        // expr_if
        case symbol_kind::S_expr_pipe_from: // expr_pipe_from
        case symbol_kind::S_expr_pipe_into: // expr_pipe_into
        case symbol_kind::S_expr_op:        // expr_op
        case symbol_kind::S_expr_app:       // expr_app
        case symbol_kind::S_expr_select:    // expr_select
        case symbol_kind::S_expr_simple:    // expr_simple
        case symbol_kind::S_path_start:     // path_start
          value.move<Expr*>(std::move(that.value));
          break;

        case symbol_kind::S_binds:  // binds
        case symbol_kind::S_binds1: // binds1
          value.move<ExprAttrs*>(std::move(that.value));
          break;

        case symbol_kind::S_formal: // formal
          value.move<Formal>(std::move(that.value));
          break;

        case symbol_kind::S_formal_set: // formal_set
        case symbol_kind::S_formals:    // formals
          value.move<FormalsBuilder>(std::move(that.value));
          break;

        case symbol_kind::S_FLOAT_LIT: // FLOAT_LIT
          value.move<NixFloat>(std::move(that.value));
          break;

        case symbol_kind::S_INT_LIT: // INT_LIT
          value.move<NixInt>(std::move(that.value));
          break;

        case symbol_kind::S_ID:       // ID
        case symbol_kind::S_STR:      // STR
        case symbol_kind::S_IND_STR:  // IND_STR
        case symbol_kind::S_PATH:     // PATH
        case symbol_kind::S_HPATH:    // HPATH
        case symbol_kind::S_SPATH:    // SPATH
        case symbol_kind::S_PATH_END: // PATH_END
        case symbol_kind::S_URI:      // URI
        case symbol_kind::S_attr:     // attr
          value.move<StringToken>(std::move(that.value));
          break;

        case symbol_kind::S_string_parts: // string_parts
        case symbol_kind::S_string_attr:  // string_attr
          value.move<ToBeStringyExpr>(std::move(that.value));
          break;

        case symbol_kind::S_list: // list
          value.move<std::pmr::vector<Expr*>>(std::move(that.value));
          break;

        case symbol_kind::S_attrpath: // attrpath
          value.move<std::vector<AttrName>>(std::move(that.value));
          break;

        case symbol_kind::S_attrs: // attrs
          value.move<std::vector<std::pair<AttrName, pos_idx_t>>>(std::move(that.value));
          break;

        case symbol_kind::S_string_parts_interpolated: // string_parts_interpolated
          value.move<std::vector<std::pair<pos_idx_t, Expr*>>>(std::move(that.value));
          break;

        case symbol_kind::S_ind_string_parts: // ind_string_parts
          value.move<std::vector<std::pair<pos_idx_t, std::variant<Expr*, StringToken>>>>(
              std::move(that.value));
          break;

        default:
          break;
      }
    }
#endif

    /// Copy constructor.
    basic_symbol(const basic_symbol& that);

    /// Constructors for typed symbols.
#if 201103L <= YY_CPLUSPLUS
    basic_symbol(typename Base::kind_type t, location_type&& l) : Base(t), location(std::move(l)) {}
#else
    basic_symbol(typename Base::kind_type t, const location_type& l) : Base(t), location(l) {}
#endif

#if 201103L <= YY_CPLUSPLUS
    basic_symbol(typename Base::kind_type t, Expr*&& v, location_type&& l)
        : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
    basic_symbol(typename Base::kind_type t, const Expr*& v, const location_type& l)
        : Base(t), value(v), location(l) {}
#endif

#if 201103L <= YY_CPLUSPLUS
    basic_symbol(typename Base::kind_type t, ExprAttrs*&& v, location_type&& l)
        : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
    basic_symbol(typename Base::kind_type t, const ExprAttrs*& v, const location_type& l)
        : Base(t), value(v), location(l) {}
#endif

#if 201103L <= YY_CPLUSPLUS
    basic_symbol(typename Base::kind_type t, Formal&& v, location_type&& l)
        : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
    basic_symbol(typename Base::kind_type t, const Formal& v, const location_type& l)
        : Base(t), value(v), location(l) {}
#endif

#if 201103L <= YY_CPLUSPLUS
    basic_symbol(typename Base::kind_type t, FormalsBuilder&& v, location_type&& l)
        : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
    basic_symbol(typename Base::kind_type t, const FormalsBuilder& v, const location_type& l)
        : Base(t), value(v), location(l) {}
#endif

#if 201103L <= YY_CPLUSPLUS
    basic_symbol(typename Base::kind_type t, NixFloat&& v, location_type&& l)
        : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
    basic_symbol(typename Base::kind_type t, const NixFloat& v, const location_type& l)
        : Base(t), value(v), location(l) {}
#endif

#if 201103L <= YY_CPLUSPLUS
    basic_symbol(typename Base::kind_type t, NixInt&& v, location_type&& l)
        : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
    basic_symbol(typename Base::kind_type t, const NixInt& v, const location_type& l)
        : Base(t), value(v), location(l) {}
#endif

#if 201103L <= YY_CPLUSPLUS
    basic_symbol(typename Base::kind_type t, StringToken&& v, location_type&& l)
        : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
    basic_symbol(typename Base::kind_type t, const StringToken& v, const location_type& l)
        : Base(t), value(v), location(l) {}
#endif

#if 201103L <= YY_CPLUSPLUS
    basic_symbol(typename Base::kind_type t, ToBeStringyExpr&& v, location_type&& l)
        : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
    basic_symbol(typename Base::kind_type t, const ToBeStringyExpr& v, const location_type& l)
        : Base(t), value(v), location(l) {}
#endif

#if 201103L <= YY_CPLUSPLUS
    basic_symbol(typename Base::kind_type t, std::pmr::vector<Expr*>&& v, location_type&& l)
        : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
    basic_symbol(typename Base::kind_type t, const std::pmr::vector<Expr*>& v,
                 const location_type& l)
        : Base(t), value(v), location(l) {}
#endif

#if 201103L <= YY_CPLUSPLUS
    basic_symbol(typename Base::kind_type t, std::vector<AttrName>&& v, location_type&& l)
        : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
    basic_symbol(typename Base::kind_type t, const std::vector<AttrName>& v, const location_type& l)
        : Base(t), value(v), location(l) {}
#endif

#if 201103L <= YY_CPLUSPLUS
    basic_symbol(typename Base::kind_type t, std::vector<std::pair<AttrName, pos_idx_t>>&& v,
                 location_type&& l)
        : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
    basic_symbol(typename Base::kind_type t, const std::vector<std::pair<AttrName, pos_idx_t>>& v,
                 const location_type& l)
        : Base(t), value(v), location(l) {}
#endif

#if 201103L <= YY_CPLUSPLUS
    basic_symbol(typename Base::kind_type t, std::vector<std::pair<pos_idx_t, Expr*>>&& v,
                 location_type&& l)
        : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
    basic_symbol(typename Base::kind_type t, const std::vector<std::pair<pos_idx_t, Expr*>>& v,
                 const location_type& l)
        : Base(t), value(v), location(l) {}
#endif

#if 201103L <= YY_CPLUSPLUS
    basic_symbol(typename Base::kind_type t,
                 std::vector<std::pair<pos_idx_t, std::variant<Expr*, StringToken>>>&& v,
                 location_type&& l)
        : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
    basic_symbol(typename Base::kind_type t,
                 const std::vector<std::pair<pos_idx_t, std::variant<Expr*, StringToken>>>& v,
                 const location_type& l)
        : Base(t), value(v), location(l) {}
#endif

    /// Destroy the symbol.
    ~basic_symbol() { clear(); }


    /// Destroy contents, and record that is empty.
    void clear() YY_NOEXCEPT {
      // User destructor.
      symbol_kind_type yykind = this->kind();
      basic_symbol<Base>& yysym = *this;
      (void)yysym;
      switch (yykind) {
        default:
          break;
      }

      // Value type destructor.
      switch (yykind) {
        case symbol_kind::S_start:          // start
        case symbol_kind::S_expr:           // expr
        case symbol_kind::S_expr_function:  // expr_function
        case symbol_kind::S_expr_if:        // expr_if
        case symbol_kind::S_expr_pipe_from: // expr_pipe_from
        case symbol_kind::S_expr_pipe_into: // expr_pipe_into
        case symbol_kind::S_expr_op:        // expr_op
        case symbol_kind::S_expr_app:       // expr_app
        case symbol_kind::S_expr_select:    // expr_select
        case symbol_kind::S_expr_simple:    // expr_simple
        case symbol_kind::S_path_start:     // path_start
          value.template destroy<Expr*>();
          break;

        case symbol_kind::S_binds:  // binds
        case symbol_kind::S_binds1: // binds1
          value.template destroy<ExprAttrs*>();
          break;

        case symbol_kind::S_formal: // formal
          value.template destroy<Formal>();
          break;

        case symbol_kind::S_formal_set: // formal_set
        case symbol_kind::S_formals:    // formals
          value.template destroy<FormalsBuilder>();
          break;

        case symbol_kind::S_FLOAT_LIT: // FLOAT_LIT
          value.template destroy<NixFloat>();
          break;

        case symbol_kind::S_INT_LIT: // INT_LIT
          value.template destroy<NixInt>();
          break;

        case symbol_kind::S_ID:       // ID
        case symbol_kind::S_STR:      // STR
        case symbol_kind::S_IND_STR:  // IND_STR
        case symbol_kind::S_PATH:     // PATH
        case symbol_kind::S_HPATH:    // HPATH
        case symbol_kind::S_SPATH:    // SPATH
        case symbol_kind::S_PATH_END: // PATH_END
        case symbol_kind::S_URI:      // URI
        case symbol_kind::S_attr:     // attr
          value.template destroy<StringToken>();
          break;

        case symbol_kind::S_string_parts: // string_parts
        case symbol_kind::S_string_attr:  // string_attr
          value.template destroy<ToBeStringyExpr>();
          break;

        case symbol_kind::S_list: // list
          value.template destroy<std::pmr::vector<Expr*>>();
          break;

        case symbol_kind::S_attrpath: // attrpath
          value.template destroy<std::vector<AttrName>>();
          break;

        case symbol_kind::S_attrs: // attrs
          value.template destroy<std::vector<std::pair<AttrName, pos_idx_t>>>();
          break;

        case symbol_kind::S_string_parts_interpolated: // string_parts_interpolated
          value.template destroy<std::vector<std::pair<pos_idx_t, Expr*>>>();
          break;

        case symbol_kind::S_ind_string_parts: // ind_string_parts
          value
              .template destroy<std::vector<std::pair<pos_idx_t, std::variant<Expr*, StringToken>>>>();
          break;

        default:
          break;
      }

      Base::clear();
    }

    /// The user-facing name of this symbol.
    std::string name() const YY_NOEXCEPT { return bison_parser_t ::symbol_name(this->kind()); }

    /// Backward compatibility (Bison 3.6).
    symbol_kind_type type_get() const YY_NOEXCEPT;

    /// Whether empty.
    bool empty() const YY_NOEXCEPT;

    /// Destructive move, \a s is emptied into this.
    void move(basic_symbol& s);

    /// The semantic value.
    value_type value;

    /// The location.
    location_type location;

  private:
#if YY_CPLUSPLUS < 201103L
    /// Assignment operator.
    basic_symbol& operator=(const basic_symbol& that);
#endif
  };

  /// Type access provider for token (enum) based symbols.
  struct by_kind {
    /// The symbol kind as needed by the constructor.
    typedef token_kind_type kind_type;

    /// Default constructor.
    by_kind() YY_NOEXCEPT;

#if 201103L <= YY_CPLUSPLUS
    /// Move constructor.
    by_kind(by_kind&& that) YY_NOEXCEPT;
#endif

    /// Copy constructor.
    by_kind(const by_kind& that) YY_NOEXCEPT;

    /// Constructor from (external) token numbers.
    by_kind(kind_type t) YY_NOEXCEPT;


    /// Record that this symbol is empty.
    void clear() YY_NOEXCEPT;

    /// Steal the symbol kind from \a that.
    void move(by_kind& that);

    /// The (internal) type number (corresponding to \a type).
    /// \a empty when empty.
    symbol_kind_type kind() const YY_NOEXCEPT;

    /// Backward compatibility (Bison 3.6).
    symbol_kind_type type_get() const YY_NOEXCEPT;

    /// The symbol kind.
    /// \a S_YYEMPTY when empty.
    symbol_kind_type kind_;
  };

  /// Backward compatibility for a private implementation detail (Bison 3.6).
  typedef by_kind by_type;

  /// "External" symbols: returned by the scanner.
  struct symbol_type : basic_symbol<by_kind> {
    /// Superclass.
    typedef basic_symbol<by_kind> super_type;

    /// Empty symbol.
    symbol_type() YY_NOEXCEPT {}

    /// Constructor for valueless symbols, and symbols from each type.
#if 201103L <= YY_CPLUSPLUS
    symbol_type(int tok, location_type l)
        : super_type(token_kind_type(tok), std::move(l))
#else
    symbol_type(int tok, const location_type& l)
        : super_type(token_kind_type(tok), l)
#endif
    {
    }
#if 201103L <= YY_CPLUSPLUS
    symbol_type(int tok, NixFloat v, location_type l)
        : super_type(token_kind_type(tok), std::move(v), std::move(l))
#else
    symbol_type(int tok, const NixFloat& v, const location_type& l)
        : super_type(token_kind_type(tok), v, l)
#endif
    {
    }
#if 201103L <= YY_CPLUSPLUS
    symbol_type(int tok, NixInt v, location_type l)
        : super_type(token_kind_type(tok), std::move(v), std::move(l))
#else
    symbol_type(int tok, const NixInt& v, const location_type& l)
        : super_type(token_kind_type(tok), v, l)
#endif
    {
    }
#if 201103L <= YY_CPLUSPLUS
    symbol_type(int tok, StringToken v, location_type l)
        : super_type(token_kind_type(tok), std::move(v), std::move(l))
#else
    symbol_type(int tok, const StringToken& v, const location_type& l)
        : super_type(token_kind_type(tok), v, l)
#endif
    {
    }
  };

  /// Build a parser object.
  bison_parser_t(void* scanner_yyarg, nix::ParserState* state_yyarg);
  virtual ~bison_parser_t();

#if 201103L <= YY_CPLUSPLUS
  /// Non copyable.
  bison_parser_t(const bison_parser_t&) = delete;
  /// Non copyable.
  bison_parser_t& operator=(const bison_parser_t&) = delete;
#endif

  /// Parse.  An alias for parse ().
  /// \returns  0 iff parsing succeeded.
  int operator()();

  /// Parse.
  /// \returns  0 iff parsing succeeded.
  virtual int parse();

#if YYDEBUG
  /// The current debugging stream.
  std::ostream& debug_stream() const YY_ATTRIBUTE_PURE;
  /// Set the current debugging stream.
  void set_debug_stream(std::ostream&);

  /// Type for debugging levels.
  typedef int debug_level_type;
  /// The current debugging level.
  debug_level_type debug_level() const YY_ATTRIBUTE_PURE;
  /// Set the current debugging level.
  void set_debug_level(debug_level_type l);
#endif

  /// Report a syntax error.
  /// \param loc    where the syntax error is found.
  /// \param msg    a description of the syntax error.
  virtual void error(const location_type& loc, const std::string& msg);

  /// Report a syntax error.
  void error(const syntax_error& err);

  /// The user-facing name of the symbol whose (internal) number is
  /// YYSYMBOL.  No bounds checking.
  static std::string symbol_name(symbol_kind_type yysymbol);

  // Implementation of make_symbol for each token kind.
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_YYEOF(location_type l) { return symbol_type(token::YYEOF, std::move(l)); }
#else
  static symbol_type make_YYEOF(const location_type& l) { return symbol_type(token::YYEOF, l); }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_YYerror(location_type l) {
    return symbol_type(token::YYerror, std::move(l));
  }
#else
  static symbol_type make_YYerror(const location_type& l) { return symbol_type(token::YYerror, l); }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_YYUNDEF(location_type l) {
    return symbol_type(token::YYUNDEF, std::move(l));
  }
#else
  static symbol_type make_YYUNDEF(const location_type& l) { return symbol_type(token::YYUNDEF, l); }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_ID(StringToken v, location_type l) {
    return symbol_type(token::ID, std::move(v), std::move(l));
  }
#else
  static symbol_type make_ID(const StringToken& v, const location_type& l) {
    return symbol_type(token::ID, v, l);
  }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_STR(StringToken v, location_type l) {
    return symbol_type(token::STR, std::move(v), std::move(l));
  }
#else
  static symbol_type make_STR(const StringToken& v, const location_type& l) {
    return symbol_type(token::STR, v, l);
  }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_IND_STR(StringToken v, location_type l) {
    return symbol_type(token::IND_STR, std::move(v), std::move(l));
  }
#else
  static symbol_type make_IND_STR(const StringToken& v, const location_type& l) {
    return symbol_type(token::IND_STR, v, l);
  }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_INT_LIT(NixInt v, location_type l) {
    return symbol_type(token::INT_LIT, std::move(v), std::move(l));
  }
#else
  static symbol_type make_INT_LIT(const NixInt& v, const location_type& l) {
    return symbol_type(token::INT_LIT, v, l);
  }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_FLOAT_LIT(NixFloat v, location_type l) {
    return symbol_type(token::FLOAT_LIT, std::move(v), std::move(l));
  }
#else
  static symbol_type make_FLOAT_LIT(const NixFloat& v, const location_type& l) {
    return symbol_type(token::FLOAT_LIT, v, l);
  }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_PATH(StringToken v, location_type l) {
    return symbol_type(token::PATH, std::move(v), std::move(l));
  }
#else
  static symbol_type make_PATH(const StringToken& v, const location_type& l) {
    return symbol_type(token::PATH, v, l);
  }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_HPATH(StringToken v, location_type l) {
    return symbol_type(token::HPATH, std::move(v), std::move(l));
  }
#else
  static symbol_type make_HPATH(const StringToken& v, const location_type& l) {
    return symbol_type(token::HPATH, v, l);
  }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_SPATH(StringToken v, location_type l) {
    return symbol_type(token::SPATH, std::move(v), std::move(l));
  }
#else
  static symbol_type make_SPATH(const StringToken& v, const location_type& l) {
    return symbol_type(token::SPATH, v, l);
  }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_PATH_END(StringToken v, location_type l) {
    return symbol_type(token::PATH_END, std::move(v), std::move(l));
  }
#else
  static symbol_type make_PATH_END(const StringToken& v, const location_type& l) {
    return symbol_type(token::PATH_END, v, l);
  }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_URI(StringToken v, location_type l) {
    return symbol_type(token::URI, std::move(v), std::move(l));
  }
#else
  static symbol_type make_URI(const StringToken& v, const location_type& l) {
    return symbol_type(token::URI, v, l);
  }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_IF(location_type l) { return symbol_type(token::IF, std::move(l)); }
#else
  static symbol_type make_IF(const location_type& l) { return symbol_type(token::IF, l); }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_THEN(location_type l) { return symbol_type(token::THEN, std::move(l)); }
#else
  static symbol_type make_THEN(const location_type& l) { return symbol_type(token::THEN, l); }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_ELSE(location_type l) { return symbol_type(token::ELSE, std::move(l)); }
#else
  static symbol_type make_ELSE(const location_type& l) { return symbol_type(token::ELSE, l); }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_ASSERT(location_type l) {
    return symbol_type(token::ASSERT, std::move(l));
  }
#else
  static symbol_type make_ASSERT(const location_type& l) { return symbol_type(token::ASSERT, l); }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_WITH(location_type l) { return symbol_type(token::WITH, std::move(l)); }
#else
  static symbol_type make_WITH(const location_type& l) { return symbol_type(token::WITH, l); }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_LET(location_type l) { return symbol_type(token::LET, std::move(l)); }
#else
  static symbol_type make_LET(const location_type& l) { return symbol_type(token::LET, l); }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_IN_KW(location_type l) { return symbol_type(token::IN_KW, std::move(l)); }
#else
  static symbol_type make_IN_KW(const location_type& l) { return symbol_type(token::IN_KW, l); }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_REC(location_type l) { return symbol_type(token::REC, std::move(l)); }
#else
  static symbol_type make_REC(const location_type& l) { return symbol_type(token::REC, l); }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_INHERIT(location_type l) {
    return symbol_type(token::INHERIT, std::move(l));
  }
#else
  static symbol_type make_INHERIT(const location_type& l) { return symbol_type(token::INHERIT, l); }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_EQ(location_type l) { return symbol_type(token::EQ, std::move(l)); }
#else
  static symbol_type make_EQ(const location_type& l) { return symbol_type(token::EQ, l); }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_NEQ(location_type l) { return symbol_type(token::NEQ, std::move(l)); }
#else
  static symbol_type make_NEQ(const location_type& l) { return symbol_type(token::NEQ, l); }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_AND(location_type l) { return symbol_type(token::AND, std::move(l)); }
#else
  static symbol_type make_AND(const location_type& l) { return symbol_type(token::AND, l); }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_OR(location_type l) { return symbol_type(token::OR, std::move(l)); }
#else
  static symbol_type make_OR(const location_type& l) { return symbol_type(token::OR, l); }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_IMPL(location_type l) { return symbol_type(token::IMPL, std::move(l)); }
#else
  static symbol_type make_IMPL(const location_type& l) { return symbol_type(token::IMPL, l); }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_OR_KW(location_type l) { return symbol_type(token::OR_KW, std::move(l)); }
#else
  static symbol_type make_OR_KW(const location_type& l) { return symbol_type(token::OR_KW, l); }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_PIPE_FROM(location_type l) {
    return symbol_type(token::PIPE_FROM, std::move(l));
  }
#else
  static symbol_type make_PIPE_FROM(const location_type& l) {
    return symbol_type(token::PIPE_FROM, l);
  }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_PIPE_INTO(location_type l) {
    return symbol_type(token::PIPE_INTO, std::move(l));
  }
#else
  static symbol_type make_PIPE_INTO(const location_type& l) {
    return symbol_type(token::PIPE_INTO, l);
  }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_DOLLAR_CURLY(location_type l) {
    return symbol_type(token::DOLLAR_CURLY, std::move(l));
  }
#else
  static symbol_type make_DOLLAR_CURLY(const location_type& l) {
    return symbol_type(token::DOLLAR_CURLY, l);
  }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_IND_STRING_OPEN(location_type l) {
    return symbol_type(token::IND_STRING_OPEN, std::move(l));
  }
#else
  static symbol_type make_IND_STRING_OPEN(const location_type& l) {
    return symbol_type(token::IND_STRING_OPEN, l);
  }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_IND_STRING_CLOSE(location_type l) {
    return symbol_type(token::IND_STRING_CLOSE, std::move(l));
  }
#else
  static symbol_type make_IND_STRING_CLOSE(const location_type& l) {
    return symbol_type(token::IND_STRING_CLOSE, l);
  }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_ELLIPSIS(location_type l) {
    return symbol_type(token::ELLIPSIS, std::move(l));
  }
#else
  static symbol_type make_ELLIPSIS(const location_type& l) {
    return symbol_type(token::ELLIPSIS, l);
  }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_LEQ(location_type l) { return symbol_type(token::LEQ, std::move(l)); }
#else
  static symbol_type make_LEQ(const location_type& l) { return symbol_type(token::LEQ, l); }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_GEQ(location_type l) { return symbol_type(token::GEQ, std::move(l)); }
#else
  static symbol_type make_GEQ(const location_type& l) { return symbol_type(token::GEQ, l); }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_UPDATE(location_type l) {
    return symbol_type(token::UPDATE, std::move(l));
  }
#else
  static symbol_type make_UPDATE(const location_type& l) { return symbol_type(token::UPDATE, l); }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_NOT(location_type l) { return symbol_type(token::NOT, std::move(l)); }
#else
  static symbol_type make_NOT(const location_type& l) { return symbol_type(token::NOT, l); }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_CONCAT(location_type l) {
    return symbol_type(token::CONCAT, std::move(l));
  }
#else
  static symbol_type make_CONCAT(const location_type& l) { return symbol_type(token::CONCAT, l); }
#endif
#if 201103L <= YY_CPLUSPLUS
  static symbol_type make_NEGATE(location_type l) {
    return symbol_type(token::NEGATE, std::move(l));
  }
#else
  static symbol_type make_NEGATE(const location_type& l) { return symbol_type(token::NEGATE, l); }
#endif


  class context {
  public:
    context(const bison_parser_t& yyparser, const symbol_type& yyla);
    const symbol_type& lookahead() const YY_NOEXCEPT { return yyla_; }
    symbol_kind_type token() const YY_NOEXCEPT { return yyla_.kind(); }
    const location_type& location() const YY_NOEXCEPT { return yyla_.location; }

    /// Put in YYARG at most YYARGN of the expected tokens, and return the
    /// number of tokens stored in YYARG.  If YYARG is null, return the
    /// number of expected tokens (guaranteed to be less than YYNTOKENS).
    int expected_tokens(symbol_kind_type yyarg[], int yyargn) const;

  private:
    const bison_parser_t& yyparser_;
    const symbol_type& yyla_;
  };

private:
#if YY_CPLUSPLUS < 201103L
  /// Non copyable.
  bison_parser_t(const bison_parser_t&);
  /// Non copyable.
  bison_parser_t& operator=(const bison_parser_t&);
#endif


  /// Stored state numbers (used for stacks).
  typedef unsigned char state_type;

  /// The arguments of the error message.
  int yy_syntax_error_arguments_(const context& yyctx, symbol_kind_type yyarg[], int yyargn) const;

  /// Generate an error message.
  /// \param yyctx     the context in which the error occurred.
  virtual std::string yysyntax_error_(const context& yyctx) const;
  /// Compute post-reduction state.
  /// \param yystate   the current state
  /// \param yysym     the nonterminal to push on the stack
  static state_type yy_lr_goto_state_(state_type yystate, int yysym);

  /// Whether the given \c yypact_ value indicates a defaulted state.
  /// \param yyvalue   the value to check
  static bool yy_pact_value_is_default_(int yyvalue) YY_NOEXCEPT;

  /// Whether the given \c yytable_ value indicates a syntax error.
  /// \param yyvalue   the value to check
  static bool yy_table_value_is_error_(int yyvalue) YY_NOEXCEPT;

  static const signed char yypact_ninf_;
  static const signed char yytable_ninf_;

  /// Convert a scanner token kind \a t to a symbol kind.
  /// In theory \a t should be a token_kind_type, but character literals
  /// are valid, yet not members of the token_kind_type enum.
  static symbol_kind_type yytranslate_(int t) YY_NOEXCEPT;

  /// Convert the symbol name \a n to a form suitable for a diagnostic.
  static std::string yytnamerr_(const char* yystr);

  /// For a symbol, its name in clear.
  static const char* const yytname_[];


  // Tables.
  // YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
  // STATE-NUM.
  static const short yypact_[];

  // YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
  // Performed when YYTABLE does not specify something else to do.  Zero
  // means the default is an error.
  static const signed char yydefact_[];

  // YYPGOTO[NTERM-NUM].
  static const short yypgoto_[];

  // YYDEFGOTO[NTERM-NUM].
  static const unsigned char yydefgoto_[];

  // YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
  // positive, shift that token.  If negative, reduce the rule whose
  // number is the opposite.  If YYTABLE_NINF, syntax error.
  static const short yytable_[];

  static const short yycheck_[];

  // YYSTOS[STATE-NUM] -- The symbol kind of the accessing symbol of
  // state STATE-NUM.
  static const signed char yystos_[];

  // YYR1[RULE-NUM] -- Symbol kind of the left-hand side of rule RULE-NUM.
  static const signed char yyr1_[];

  // YYR2[RULE-NUM] -- Number of symbols on the right-hand side of rule RULE-NUM.
  static const signed char yyr2_[];


#if YYDEBUG
  // YYRLINE[YYN] -- Source line where rule number YYN was defined.
  static const short yyrline_[];
  /// Report on the debug stream that the rule \a r is going to be reduced.
  virtual void yy_reduce_print_(int r) const;
  /// Print the state stack on the debug stream.
  virtual void yy_stack_print_() const;

  /// Debugging level.
  int yydebug_;
  /// Debug stream.
  std::ostream* yycdebug_;

  /// \brief Display a symbol kind, value and location.
  /// \param yyo    The output stream.
  /// \param yysym  The symbol.
  template <typename Base>
  void yy_print_(std::ostream& yyo, const basic_symbol<Base>& yysym) const;
#endif

  /// \brief Reclaim the memory associated to a symbol.
  /// \param yymsg     Why this token is reclaimed.
  ///                  If null, print nothing.
  /// \param yysym     The symbol.
  template <typename Base>
  void yy_destroy_(const char* yymsg, basic_symbol<Base>& yysym) const;

private:
  /// Type access provider for state based symbols.
  struct by_state {
    /// Default constructor.
    by_state() YY_NOEXCEPT;

    /// The symbol kind as needed by the constructor.
    typedef state_type kind_type;

    /// Constructor.
    by_state(kind_type s) YY_NOEXCEPT;

    /// Copy constructor.
    by_state(const by_state& that) YY_NOEXCEPT;

    /// Record that this symbol is empty.
    void clear() YY_NOEXCEPT;

    /// Steal the symbol kind from \a that.
    void move(by_state& that);

    /// The symbol kind (corresponding to \a state).
    /// \a symbol_kind::S_YYEMPTY when empty.
    symbol_kind_type kind() const YY_NOEXCEPT;

    /// The state number used to denote an empty symbol.
    /// We use the initial state, as it does not have a value.
    enum { empty_state = 0 };

    /// The state.
    /// \a empty when empty.
    state_type state;
  };

  /// "Internal" symbol: element of the stack.
  struct stack_symbol_type : basic_symbol<by_state> {
    /// Superclass.
    typedef basic_symbol<by_state> super_type;
    /// Construct an empty symbol.
    stack_symbol_type();
    /// Move or copy construction.
    stack_symbol_type(YY_RVREF(stack_symbol_type) that);
    /// Steal the contents from \a sym to build this.
    stack_symbol_type(state_type s, YY_MOVE_REF(symbol_type) sym);
#if YY_CPLUSPLUS < 201103L
    /// Assignment, needed by push_back by some old implementations.
    /// Moves the contents of that.
    stack_symbol_type& operator=(stack_symbol_type& that);

    /// Assignment, needed by push_back by other implementations.
    /// Needed by some other old implementations.
    stack_symbol_type& operator=(const stack_symbol_type& that);
#endif
  };

  /// A stack with random access from its top.
  template <typename T, typename S = std::vector<T>>
  class stack {
  public:
    // Hide our reversed order.
    typedef typename S::iterator iterator;
    typedef typename S::const_iterator const_iterator;
    typedef typename S::size_type size_type;
    typedef typename std::ptrdiff_t index_type;

    stack(size_type n = 200) YY_NOEXCEPT : seq_(n) {}

#if 201103L <= YY_CPLUSPLUS
    /// Non copyable.
    stack(const stack&) = delete;
    /// Non copyable.
    stack& operator=(const stack&) = delete;
#endif

    /// Random access.
    ///
    /// Index 0 returns the topmost element.
    const T& operator[](index_type i) const { return seq_[size_type(size() - 1 - i)]; }

    /// Random access.
    ///
    /// Index 0 returns the topmost element.
    T& operator[](index_type i) { return seq_[size_type(size() - 1 - i)]; }

    /// Steal the contents of \a t.
    ///
    /// Close to move-semantics.
    void push(YY_MOVE_REF(T) t) {
      seq_.push_back(T());
      operator[](0).move(t);
    }

    /// Pop elements from the stack.
    void pop(std::ptrdiff_t n = 1) YY_NOEXCEPT {
      for (; 0 < n; --n)
        seq_.pop_back();
    }

    /// Pop all elements from the stack.
    void clear() YY_NOEXCEPT { seq_.clear(); }

    /// Number of elements on the stack.
    index_type size() const YY_NOEXCEPT { return index_type(seq_.size()); }

    /// Iterator on top of the stack (going downwards).
    const_iterator begin() const YY_NOEXCEPT { return seq_.begin(); }

    /// Bottom of the stack.
    const_iterator end() const YY_NOEXCEPT { return seq_.end(); }

    /// Present a slice of the top of a stack.
    class slice {
    public:
      slice(const stack& stack, index_type range) YY_NOEXCEPT : stack_(stack), range_(range) {}

      const T& operator[](index_type i) const { return stack_[range_ - i]; }

    private:
      const stack& stack_;
      index_type range_;
    };

  private:
#if YY_CPLUSPLUS < 201103L
    /// Non copyable.
    stack(const stack&);
    /// Non copyable.
    stack& operator=(const stack&);
#endif
    /// The wrapped container.
    S seq_;
  };


  /// Stack type.
  typedef stack<stack_symbol_type> stack_type;

  /// The stack.
  stack_type yystack_;

  /// Push a new state on the stack.
  /// \param m    a debug message to display
  ///             if null, no trace is output.
  /// \param sym  the symbol
  /// \warning the contents of \a s.value is stolen.
  void yypush_(const char* m, YY_MOVE_REF(stack_symbol_type) sym);

  /// Push a new look ahead token on the state on the stack.
  /// \param m    a debug message to display
  ///             if null, no trace is output.
  /// \param s    the state
  /// \param sym  the symbol (for its value and location).
  /// \warning the contents of \a sym.value is stolen.
  void yypush_(const char* m, state_type s, YY_MOVE_REF(symbol_type) sym);

  /// Pop \a n symbols from the stack.
  void yypop_(int n = 1) YY_NOEXCEPT;

  /// Constants.
  enum {
    yylast_ = 407, ///< Last index in yytable_.
    yynnts_ = 25,  ///< Number of nonterminal symbols.
    yyfinal_ = 67  ///< Termination state number.
  };


  // User arguments.
  void* scanner;
  nix::ParserState* state;
};


#line 3 "parser.y"
} // namespace parser
} // namespace nix
#line 2218 "parser-tab.hpp"


#endif // !YY_YY_PARSER_TAB_HPP_INCLUDED
