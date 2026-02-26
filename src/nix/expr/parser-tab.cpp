// A Bison parser, made by GNU Bison 3.8.2.

// Skeleton implementation for Bison LALR(1) parsers in C++

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

// DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
// especially those whose name start with YY_ or yy_.  They are
// private implementation details that can be changed or removed.


// First part of user prologue.
#line 79 "parser.y"


/* The parser is very performance sensitive and loses out on a lot
   of performance even with basic stdlib assertions. Since those don't
   affect ABI we can disable those just for this file. */
#if defined(_GLIBCXX_ASSERTIONS) && !defined(_GLIBCXX_DEBUG)
#  undef _GLIBCXX_ASSERTIONS
#endif

#include "parser-scanner-decls.h"

YY_DECL;

#define CUR_POS state->at(yylhs.location)

void nix::parser::bison_parser_t::error(const location_type& loc_, const std::string& error) {
  auto loc = loc_;
  if (std::string_view(error).starts_with("syntax error, unexpected end of file")) {
    loc.beginOffset = loc.endOffset;
  }
  throw nix::ParseError({.msg_ = nix::hint_fmt_t(error), .pos_ = state->positions[state->at(loc)]});
}

#define SET_DOC_POS(lambda, pos) set_doc_position(state->lexer_state, lambda, state->at(pos))
static void set_doc_position(const nix::LexerState& lexer_state, nix::ExprLambda* lambda,
                             nix::pos_idx_t start) {
  auto it = lexer_state.positionToDocComment.find(start);
  if (it != lexer_state.positionToDocComment.end()) {
    lambda->setDocComment(it->second);
  }
}

static nix::expr_t* make_call(nix::Exprs& exprs, nix::pos_idx_t pos, nix::expr_t* fn,
                              nix::expr_t* arg) {
  if (auto e2 = dynamic_cast<nix::ExprCall*>(fn)) {
    e2->args->push_back(arg);
    return fn;
  }
  return exprs.add<nix::ExprCall>(pos, fn, {arg});
}


#line 89 "parser-tab.cpp"


#include "parser-tab.h"


#ifndef YY_
#  if defined YYENABLE_NLS && YYENABLE_NLS
#    if ENABLE_NLS
#      include <libintl.h> // FIXME: INFRINGES ON USER NAME SPACE.
#      define YY_(msgid) dgettext("bison-runtime", msgid)
#    endif
#  endif
#  ifndef YY_
#    define YY_(msgid) msgid
#  endif
#endif


// Whether we are compiled with exception support.
#ifndef YY_EXCEPTIONS
#  if defined __GNUC__ && !defined __EXCEPTIONS
#    define YY_EXCEPTIONS 0
#  else
#    define YY_EXCEPTIONS 1
#  endif
#endif

#define YYRHSLOC(Rhs, K) ((Rhs)[K].location)
/* YYLLOC_DEFAULT -- Set CURRENT to span from RHS[1] to RHS[N].
   If N is 0, then set CURRENT to the empty location which ends
   the previous symbol: RHS[0] (always defined).  */

#ifndef YYLLOC_DEFAULT
#  define YYLLOC_DEFAULT(Current, Rhs, N)                                                          \
    do                                                                                             \
      if (N) {                                                                                     \
        (Current).begin = YYRHSLOC(Rhs, 1).begin;                                                  \
        (Current).end = YYRHSLOC(Rhs, N).end;                                                      \
      } else {                                                                                     \
        (Current).begin = (Current).end = YYRHSLOC(Rhs, 0).end;                                    \
      }                                                                                            \
    while (false)
#endif


// Enable debugging if requested.
#if YYDEBUG

// A pseudo ostream that takes yydebug_ into account.
#  define YYCDEBUG                                                                                 \
    if (yydebug_)                                                                                  \
    (*yycdebug_)

#  define YY_SYMBOL_PRINT(Title, symbol_t)                                                         \
    do {                                                                                           \
      if (yydebug_) {                                                                              \
        *yycdebug_ << Title << ' ';                                                                \
        yy_print_(*yycdebug_, symbol_t);                                                           \
        *yycdebug_ << '\n';                                                                        \
      }                                                                                            \
    } while (false)

#  define YY_REDUCE_PRINT(Rule)                                                                    \
    do {                                                                                           \
      if (yydebug_)                                                                                \
        yy_reduce_print_(Rule);                                                                    \
    } while (false)

#  define YY_STACK_PRINT()                                                                         \
    do {                                                                                           \
      if (yydebug_)                                                                                \
        yy_stack_print_();                                                                         \
    } while (false)

#else // !YYDEBUG

#  define YYCDEBUG                                                                                 \
    if (false)                                                                                     \
    std::cerr
#  define YY_SYMBOL_PRINT(Title, symbol_t) YY_USE(symbol_t)
#  define YY_REDUCE_PRINT(Rule) static_cast<void>(0)
#  define YY_STACK_PRINT() static_cast<void>(0)

#endif // !YYDEBUG

#define yyerrok (yyerrstatus_ = 0)
#define yyclearin (yyla.clear())

#define YYACCEPT goto yyacceptlab
#define YYABORT goto yyabortlab
#define YYERROR goto yyerrorlab
#define YYRECOVERING() (!!yyerrstatus_)

#line 3 "parser.y"
namespace nix {
namespace parser {
#line 187 "parser-tab.cpp"

/// Build a parser object.
bison_parser_t ::bison_parser_t(void* scanner_yyarg, nix::ParserState* state_yyarg)
#if YYDEBUG
    : yydebug_(false),
      yycdebug_(&std::cerr),
#else
    :
#endif
      scanner(scanner_yyarg),
      state(state_yyarg) {
}

bison_parser_t ::~bison_parser_t() {}

bison_parser_t ::syntax_error::~syntax_error() YY_NOEXCEPT YY_NOTHROW {}

/*---------.
| symbol.  |
`---------*/

// basic_symbol.
template <typename Base>
bison_parser_t ::basic_symbol<Base>::basic_symbol(const basic_symbol& that)
    : Base(that), value(), location(that.location) {
  switch (this->kind()) {
    case symbol_kind::s_start:          // start
    case symbol_kind::s_expr:           // expr
    case symbol_kind::s_expr_function:  // expr_function
    case symbol_kind::s_expr_if:        // expr_if
    case symbol_kind::s_expr_pipe_from: // expr_pipe_from
    case symbol_kind::s_expr_pipe_into: // expr_pipe_into
    case symbol_kind::s_expr_op:        // expr_op
    case symbol_kind::s_expr_app:       // expr_app
    case symbol_kind::s_expr_select:    // expr_select
    case symbol_kind::s_expr_simple:    // expr_simple
    case symbol_kind::s_path_start:     // path_start
      value.copy<expr_t*>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_binds:  // binds
    case symbol_kind::s_binds1: // binds1
      value.copy<ExprAttrs*>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_formal: // formal
      value.copy<Formal>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_formal_set: // formal_set
    case symbol_kind::s_formals:    // formals
      value.copy<FormalsBuilder>(YY_MOVE(that.value));
      break;

    case symbol_kind::S_FLOAT_LIT: // FLOAT_LIT
      value.copy<NixFloat>(YY_MOVE(that.value));
      break;

    case symbol_kind::S_INT_LIT: // INT_LIT
      value.copy<NixInt>(YY_MOVE(that.value));
      break;

    case symbol_kind::S_ID:       // ID
    case symbol_kind::S_STR:      // STR
    case symbol_kind::S_IND_STR:  // IND_STR
    case symbol_kind::S_PATH:     // PATH
    case symbol_kind::S_HPATH:    // HPATH
    case symbol_kind::S_SPATH:    // SPATH
    case symbol_kind::S_PATH_END: // PATH_END
    case symbol_kind::S_URI:      // URI
    case symbol_kind::s_attr:     // attr
      value.copy<StringToken>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_string_parts: // string_parts
    case symbol_kind::s_string_attr:  // string_attr
      value.copy<ToBeStringyExpr>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_list: // list
      value.copy<std::pmr::vector<expr_t*>>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_attrpath: // attrpath
      value.copy<std::vector<AttrName>>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_attrs: // attrs
      value.copy<std::vector<std::pair<AttrName, pos_idx_t>>>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_string_parts_interpolated: // string_parts_interpolated
      value.copy<std::vector<std::pair<pos_idx_t, expr_t*>>>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_ind_string_parts: // ind_string_parts
      value.copy<std::vector<std::pair<pos_idx_t, std::variant<expr_t*, StringToken>>>>(
          YY_MOVE(that.value));
      break;

    default:
      break;
  }
}


template <typename Base>
bison_parser_t ::symbol_kind_type
bison_parser_t ::basic_symbol<Base>::type_get() const YY_NOEXCEPT {
  return this->kind();
}


template <typename Base>
bool bison_parser_t ::basic_symbol<Base>::empty() const YY_NOEXCEPT {
  return this->kind() == symbol_kind::S_YYEMPTY;
}

template <typename Base>
void bison_parser_t ::basic_symbol<Base>::move(basic_symbol& s) {
  super_type::move(s);
  switch (this->kind()) {
    case symbol_kind::s_start:          // start
    case symbol_kind::s_expr:           // expr
    case symbol_kind::s_expr_function:  // expr_function
    case symbol_kind::s_expr_if:        // expr_if
    case symbol_kind::s_expr_pipe_from: // expr_pipe_from
    case symbol_kind::s_expr_pipe_into: // expr_pipe_into
    case symbol_kind::s_expr_op:        // expr_op
    case symbol_kind::s_expr_app:       // expr_app
    case symbol_kind::s_expr_select:    // expr_select
    case symbol_kind::s_expr_simple:    // expr_simple
    case symbol_kind::s_path_start:     // path_start
      value.move<expr_t*>(YY_MOVE(s.value));
      break;

    case symbol_kind::s_binds:  // binds
    case symbol_kind::s_binds1: // binds1
      value.move<ExprAttrs*>(YY_MOVE(s.value));
      break;

    case symbol_kind::s_formal: // formal
      value.move<Formal>(YY_MOVE(s.value));
      break;

    case symbol_kind::s_formal_set: // formal_set
    case symbol_kind::s_formals:    // formals
      value.move<FormalsBuilder>(YY_MOVE(s.value));
      break;

    case symbol_kind::S_FLOAT_LIT: // FLOAT_LIT
      value.move<NixFloat>(YY_MOVE(s.value));
      break;

    case symbol_kind::S_INT_LIT: // INT_LIT
      value.move<NixInt>(YY_MOVE(s.value));
      break;

    case symbol_kind::S_ID:       // ID
    case symbol_kind::S_STR:      // STR
    case symbol_kind::S_IND_STR:  // IND_STR
    case symbol_kind::S_PATH:     // PATH
    case symbol_kind::S_HPATH:    // HPATH
    case symbol_kind::S_SPATH:    // SPATH
    case symbol_kind::S_PATH_END: // PATH_END
    case symbol_kind::S_URI:      // URI
    case symbol_kind::s_attr:     // attr
      value.move<StringToken>(YY_MOVE(s.value));
      break;

    case symbol_kind::s_string_parts: // string_parts
    case symbol_kind::s_string_attr:  // string_attr
      value.move<ToBeStringyExpr>(YY_MOVE(s.value));
      break;

    case symbol_kind::s_list: // list
      value.move<std::pmr::vector<expr_t*>>(YY_MOVE(s.value));
      break;

    case symbol_kind::s_attrpath: // attrpath
      value.move<std::vector<AttrName>>(YY_MOVE(s.value));
      break;

    case symbol_kind::s_attrs: // attrs
      value.move<std::vector<std::pair<AttrName, pos_idx_t>>>(YY_MOVE(s.value));
      break;

    case symbol_kind::s_string_parts_interpolated: // string_parts_interpolated
      value.move<std::vector<std::pair<pos_idx_t, expr_t*>>>(YY_MOVE(s.value));
      break;

    case symbol_kind::s_ind_string_parts: // ind_string_parts
      value.move<std::vector<std::pair<pos_idx_t, std::variant<expr_t*, StringToken>>>>(
          YY_MOVE(s.value));
      break;

    default:
      break;
  }

  location = YY_MOVE(s.location);
}

// by_kind.
bison_parser_t ::by_kind::by_kind() YY_NOEXCEPT : kind_(symbol_kind::S_YYEMPTY) {}

#if 201103L <= YY_CPLUSPLUS
bison_parser_t ::by_kind::by_kind(by_kind&& that) YY_NOEXCEPT : kind_(that.kind_) {
  that.clear();
}
#endif

bison_parser_t ::by_kind::by_kind(const by_kind& that) YY_NOEXCEPT : kind_(that.kind_) {}

bison_parser_t ::by_kind::by_kind(token_kind_type t) YY_NOEXCEPT : kind_(yytranslate_(t)) {}


void bison_parser_t ::by_kind::clear() YY_NOEXCEPT {
  kind_ = symbol_kind::S_YYEMPTY;
}

void bison_parser_t ::by_kind::move(by_kind& that) {
  kind_ = that.kind_;
  that.clear();
}

bison_parser_t ::symbol_kind_type bison_parser_t ::by_kind::kind() const YY_NOEXCEPT {
  return kind_;
}


bison_parser_t ::symbol_kind_type bison_parser_t ::by_kind::type_get() const YY_NOEXCEPT {
  return this->kind();
}


// by_state.
bison_parser_t ::by_state::by_state() YY_NOEXCEPT : state(empty_state) {}

bison_parser_t ::by_state::by_state(const by_state& that) YY_NOEXCEPT : state(that.state) {}

void bison_parser_t ::by_state::clear() YY_NOEXCEPT {
  state = empty_state;
}

void bison_parser_t ::by_state::move(by_state& that) {
  state = that.state;
  that.clear();
}

bison_parser_t ::by_state::by_state(state_type s) YY_NOEXCEPT : state(s) {}

bison_parser_t ::symbol_kind_type bison_parser_t ::by_state::kind() const YY_NOEXCEPT {
  if (state == empty_state)
    return symbol_kind::S_YYEMPTY;
  else
    return YY_CAST(symbol_kind_type, yystos_[+state]);
}

bison_parser_t ::stack_symbol_type::stack_symbol_type() {}

bison_parser_t ::stack_symbol_type::stack_symbol_type(YY_RVREF(stack_symbol_type) that)
    : super_type(YY_MOVE(that.state), YY_MOVE(that.location)) {
  switch (that.kind()) {
    case symbol_kind::s_start:          // start
    case symbol_kind::s_expr:           // expr
    case symbol_kind::s_expr_function:  // expr_function
    case symbol_kind::s_expr_if:        // expr_if
    case symbol_kind::s_expr_pipe_from: // expr_pipe_from
    case symbol_kind::s_expr_pipe_into: // expr_pipe_into
    case symbol_kind::s_expr_op:        // expr_op
    case symbol_kind::s_expr_app:       // expr_app
    case symbol_kind::s_expr_select:    // expr_select
    case symbol_kind::s_expr_simple:    // expr_simple
    case symbol_kind::s_path_start:     // path_start
      value.YY_MOVE_OR_COPY<expr_t*>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_binds:  // binds
    case symbol_kind::s_binds1: // binds1
      value.YY_MOVE_OR_COPY<ExprAttrs*>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_formal: // formal
      value.YY_MOVE_OR_COPY<Formal>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_formal_set: // formal_set
    case symbol_kind::s_formals:    // formals
      value.YY_MOVE_OR_COPY<FormalsBuilder>(YY_MOVE(that.value));
      break;

    case symbol_kind::S_FLOAT_LIT: // FLOAT_LIT
      value.YY_MOVE_OR_COPY<NixFloat>(YY_MOVE(that.value));
      break;

    case symbol_kind::S_INT_LIT: // INT_LIT
      value.YY_MOVE_OR_COPY<NixInt>(YY_MOVE(that.value));
      break;

    case symbol_kind::S_ID:       // ID
    case symbol_kind::S_STR:      // STR
    case symbol_kind::S_IND_STR:  // IND_STR
    case symbol_kind::S_PATH:     // PATH
    case symbol_kind::S_HPATH:    // HPATH
    case symbol_kind::S_SPATH:    // SPATH
    case symbol_kind::S_PATH_END: // PATH_END
    case symbol_kind::S_URI:      // URI
    case symbol_kind::s_attr:     // attr
      value.YY_MOVE_OR_COPY<StringToken>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_string_parts: // string_parts
    case symbol_kind::s_string_attr:  // string_attr
      value.YY_MOVE_OR_COPY<ToBeStringyExpr>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_list: // list
      value.YY_MOVE_OR_COPY<std::pmr::vector<expr_t*>>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_attrpath: // attrpath
      value.YY_MOVE_OR_COPY<std::vector<AttrName>>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_attrs: // attrs
      value.YY_MOVE_OR_COPY<std::vector<std::pair<AttrName, pos_idx_t>>>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_string_parts_interpolated: // string_parts_interpolated
      value.YY_MOVE_OR_COPY<std::vector<std::pair<pos_idx_t, expr_t*>>>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_ind_string_parts: // ind_string_parts
      value.YY_MOVE_OR_COPY<std::vector<std::pair<pos_idx_t, std::variant<expr_t*, StringToken>>>>(
          YY_MOVE(that.value));
      break;

    default:
      break;
  }

#if 201103L <= YY_CPLUSPLUS
  // that is emptied.
  that.state = empty_state;
#endif
}

bison_parser_t ::stack_symbol_type::stack_symbol_type(state_type s, YY_MOVE_REF(symbol_type) that)
    : super_type(s, YY_MOVE(that.location)) {
  switch (that.kind()) {
    case symbol_kind::s_start:          // start
    case symbol_kind::s_expr:           // expr
    case symbol_kind::s_expr_function:  // expr_function
    case symbol_kind::s_expr_if:        // expr_if
    case symbol_kind::s_expr_pipe_from: // expr_pipe_from
    case symbol_kind::s_expr_pipe_into: // expr_pipe_into
    case symbol_kind::s_expr_op:        // expr_op
    case symbol_kind::s_expr_app:       // expr_app
    case symbol_kind::s_expr_select:    // expr_select
    case symbol_kind::s_expr_simple:    // expr_simple
    case symbol_kind::s_path_start:     // path_start
      value.move<expr_t*>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_binds:  // binds
    case symbol_kind::s_binds1: // binds1
      value.move<ExprAttrs*>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_formal: // formal
      value.move<Formal>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_formal_set: // formal_set
    case symbol_kind::s_formals:    // formals
      value.move<FormalsBuilder>(YY_MOVE(that.value));
      break;

    case symbol_kind::S_FLOAT_LIT: // FLOAT_LIT
      value.move<NixFloat>(YY_MOVE(that.value));
      break;

    case symbol_kind::S_INT_LIT: // INT_LIT
      value.move<NixInt>(YY_MOVE(that.value));
      break;

    case symbol_kind::S_ID:       // ID
    case symbol_kind::S_STR:      // STR
    case symbol_kind::S_IND_STR:  // IND_STR
    case symbol_kind::S_PATH:     // PATH
    case symbol_kind::S_HPATH:    // HPATH
    case symbol_kind::S_SPATH:    // SPATH
    case symbol_kind::S_PATH_END: // PATH_END
    case symbol_kind::S_URI:      // URI
    case symbol_kind::s_attr:     // attr
      value.move<StringToken>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_string_parts: // string_parts
    case symbol_kind::s_string_attr:  // string_attr
      value.move<ToBeStringyExpr>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_list: // list
      value.move<std::pmr::vector<expr_t*>>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_attrpath: // attrpath
      value.move<std::vector<AttrName>>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_attrs: // attrs
      value.move<std::vector<std::pair<AttrName, pos_idx_t>>>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_string_parts_interpolated: // string_parts_interpolated
      value.move<std::vector<std::pair<pos_idx_t, expr_t*>>>(YY_MOVE(that.value));
      break;

    case symbol_kind::s_ind_string_parts: // ind_string_parts
      value.move<std::vector<std::pair<pos_idx_t, std::variant<expr_t*, StringToken>>>>(
          YY_MOVE(that.value));
      break;

    default:
      break;
  }

  // that is emptied.
  that.kind_ = symbol_kind::S_YYEMPTY;
}

#if YY_CPLUSPLUS < 201103L
bison_parser_t ::stack_symbol_type&
bison_parser_t ::stack_symbol_type::operator=(const stack_symbol_type& that) {
  state = that.state;
  switch (that.kind()) {
    case symbol_kind::s_start:          // start
    case symbol_kind::s_expr:           // expr
    case symbol_kind::s_expr_function:  // expr_function
    case symbol_kind::s_expr_if:        // expr_if
    case symbol_kind::s_expr_pipe_from: // expr_pipe_from
    case symbol_kind::s_expr_pipe_into: // expr_pipe_into
    case symbol_kind::s_expr_op:        // expr_op
    case symbol_kind::s_expr_app:       // expr_app
    case symbol_kind::s_expr_select:    // expr_select
    case symbol_kind::s_expr_simple:    // expr_simple
    case symbol_kind::s_path_start:     // path_start
      value.copy<expr_t*>(that.value);
      break;

    case symbol_kind::s_binds:  // binds
    case symbol_kind::s_binds1: // binds1
      value.copy<ExprAttrs*>(that.value);
      break;

    case symbol_kind::s_formal: // formal
      value.copy<Formal>(that.value);
      break;

    case symbol_kind::s_formal_set: // formal_set
    case symbol_kind::s_formals:    // formals
      value.copy<FormalsBuilder>(that.value);
      break;

    case symbol_kind::S_FLOAT_LIT: // FLOAT_LIT
      value.copy<NixFloat>(that.value);
      break;

    case symbol_kind::S_INT_LIT: // INT_LIT
      value.copy<NixInt>(that.value);
      break;

    case symbol_kind::S_ID:       // ID
    case symbol_kind::S_STR:      // STR
    case symbol_kind::S_IND_STR:  // IND_STR
    case symbol_kind::S_PATH:     // PATH
    case symbol_kind::S_HPATH:    // HPATH
    case symbol_kind::S_SPATH:    // SPATH
    case symbol_kind::S_PATH_END: // PATH_END
    case symbol_kind::S_URI:      // URI
    case symbol_kind::s_attr:     // attr
      value.copy<StringToken>(that.value);
      break;

    case symbol_kind::s_string_parts: // string_parts
    case symbol_kind::s_string_attr:  // string_attr
      value.copy<ToBeStringyExpr>(that.value);
      break;

    case symbol_kind::s_list: // list
      value.copy<std::pmr::vector<expr_t*>>(that.value);
      break;

    case symbol_kind::s_attrpath: // attrpath
      value.copy<std::vector<AttrName>>(that.value);
      break;

    case symbol_kind::s_attrs: // attrs
      value.copy<std::vector<std::pair<AttrName, pos_idx_t>>>(that.value);
      break;

    case symbol_kind::s_string_parts_interpolated: // string_parts_interpolated
      value.copy<std::vector<std::pair<pos_idx_t, expr_t*>>>(that.value);
      break;

    case symbol_kind::s_ind_string_parts: // ind_string_parts
      value.copy<std::vector<std::pair<pos_idx_t, std::variant<expr_t*, StringToken>>>>(that.value);
      break;

    default:
      break;
  }

  location = that.location;
  return *this;
}

bison_parser_t ::stack_symbol_type&
bison_parser_t ::stack_symbol_type::operator=(stack_symbol_type& that) {
  state = that.state;
  switch (that.kind()) {
    case symbol_kind::s_start:          // start
    case symbol_kind::s_expr:           // expr
    case symbol_kind::s_expr_function:  // expr_function
    case symbol_kind::s_expr_if:        // expr_if
    case symbol_kind::s_expr_pipe_from: // expr_pipe_from
    case symbol_kind::s_expr_pipe_into: // expr_pipe_into
    case symbol_kind::s_expr_op:        // expr_op
    case symbol_kind::s_expr_app:       // expr_app
    case symbol_kind::s_expr_select:    // expr_select
    case symbol_kind::s_expr_simple:    // expr_simple
    case symbol_kind::s_path_start:     // path_start
      value.move<expr_t*>(that.value);
      break;

    case symbol_kind::s_binds:  // binds
    case symbol_kind::s_binds1: // binds1
      value.move<ExprAttrs*>(that.value);
      break;

    case symbol_kind::s_formal: // formal
      value.move<Formal>(that.value);
      break;

    case symbol_kind::s_formal_set: // formal_set
    case symbol_kind::s_formals:    // formals
      value.move<FormalsBuilder>(that.value);
      break;

    case symbol_kind::S_FLOAT_LIT: // FLOAT_LIT
      value.move<NixFloat>(that.value);
      break;

    case symbol_kind::S_INT_LIT: // INT_LIT
      value.move<NixInt>(that.value);
      break;

    case symbol_kind::S_ID:       // ID
    case symbol_kind::S_STR:      // STR
    case symbol_kind::S_IND_STR:  // IND_STR
    case symbol_kind::S_PATH:     // PATH
    case symbol_kind::S_HPATH:    // HPATH
    case symbol_kind::S_SPATH:    // SPATH
    case symbol_kind::S_PATH_END: // PATH_END
    case symbol_kind::S_URI:      // URI
    case symbol_kind::s_attr:     // attr
      value.move<StringToken>(that.value);
      break;

    case symbol_kind::s_string_parts: // string_parts
    case symbol_kind::s_string_attr:  // string_attr
      value.move<ToBeStringyExpr>(that.value);
      break;

    case symbol_kind::s_list: // list
      value.move<std::pmr::vector<expr_t*>>(that.value);
      break;

    case symbol_kind::s_attrpath: // attrpath
      value.move<std::vector<AttrName>>(that.value);
      break;

    case symbol_kind::s_attrs: // attrs
      value.move<std::vector<std::pair<AttrName, pos_idx_t>>>(that.value);
      break;

    case symbol_kind::s_string_parts_interpolated: // string_parts_interpolated
      value.move<std::vector<std::pair<pos_idx_t, expr_t*>>>(that.value);
      break;

    case symbol_kind::s_ind_string_parts: // ind_string_parts
      value.move<std::vector<std::pair<pos_idx_t, std::variant<expr_t*, StringToken>>>>(that.value);
      break;

    default:
      break;
  }

  location = that.location;
  // that is emptied.
  that.state = empty_state;
  return *this;
}
#endif

template <typename Base>
void bison_parser_t ::yy_destroy_(const char* yymsg, basic_symbol<Base>& yysym) const {
  if (yymsg)
    YY_SYMBOL_PRINT(yymsg, yysym);
}

#if YYDEBUG
template <typename Base>
void bison_parser_t ::yy_print_(std::ostream& yyo, const basic_symbol<Base>& yysym) const {
  std::ostream& yyoutput = yyo;
  YY_USE(yyoutput);
  if (yysym.empty())
    yyo << "empty symbol";
  else {
    symbol_kind_type yykind = yysym.kind();
    yyo << (yykind < YYNTOKENS ? "token" : "nterm") << ' ' << yysym.name() << " (" << yysym.location
        << ": ";
    YY_USE(yykind);
    yyo << ')';
  }
}
#endif

void bison_parser_t ::yypush_(const char* m, YY_MOVE_REF(stack_symbol_type) sym) {
  if (m)
    YY_SYMBOL_PRINT(m, sym);
  yystack_.push(YY_MOVE(sym));
}

void bison_parser_t ::yypush_(const char* m, state_type s, YY_MOVE_REF(symbol_type) sym) {
#if 201103L <= YY_CPLUSPLUS
  yypush_(m, stack_symbol_type(s, std::move(sym)));
#else
  stack_symbol_type ss(s, sym);
  yypush_(m, ss);
#endif
}

void bison_parser_t ::yypop_(int n) YY_NOEXCEPT {
  yystack_.pop(n);
}

#if YYDEBUG
std::ostream& bison_parser_t ::debug_stream() const {
  return *yycdebug_;
}

void bison_parser_t ::set_debug_stream(std::ostream& o) {
  yycdebug_ = &o;
}


bison_parser_t ::debug_level_type bison_parser_t ::debug_level() const {
  return yydebug_;
}

void bison_parser_t ::set_debug_level(debug_level_type l) {
  yydebug_ = l;
}
#endif // YYDEBUG

bison_parser_t ::state_type bison_parser_t ::yy_lr_goto_state_(state_type yystate, int yysym) {
  int yyr = yypgoto_[yysym - YYNTOKENS] + yystate;
  if (0 <= yyr && yyr <= yylast_ && yycheck_[yyr] == yystate)
    return yytable_[yyr];
  else
    return yydefgoto_[yysym - YYNTOKENS];
}

bool bison_parser_t ::yy_pact_value_is_default_(int yyvalue) YY_NOEXCEPT {
  return yyvalue == yypact_ninf_;
}

bool bison_parser_t ::yy_table_value_is_error_(int yyvalue) YY_NOEXCEPT {
  return yyvalue == yytable_ninf_;
}

int bison_parser_t ::operator()() {
  return parse();
}

int bison_parser_t ::parse() {
  int yyn;
  /// Length of the RHS of the rule being reduced.
  int yylen = 0;

  // Error handling.
  int yynerrs_ = 0;
  int yyerrstatus_ = 0;

  /// The lookahead symbol.
  symbol_type yyla;

  /// The locations where the error started and ended.
  stack_symbol_type yyerror_range[3];

  /// The return value of parse ().
  int yyresult;

#if YY_EXCEPTIONS
  try
#endif // YY_EXCEPTIONS
  {
    YYCDEBUG << "Starting parse\n";


    /* Initialize the stack.  The initial state will be set in
       yynewstate, since the latter expects the semantical and the
       location values to have been already stored, initialize these
       stacks with a primary value.  */
    yystack_.clear();
    yypush_(YY_NULLPTR, 0, YY_MOVE(yyla));

  /*-----------------------------------------------.
  | yynewstate -- push a new symbol on the stack.  |
  `-----------------------------------------------*/
  yynewstate:
    YYCDEBUG << "Entering state " << int(yystack_[0].state) << '\n';
    YY_STACK_PRINT();

    // Accept?
    if (yystack_[0].state == yyfinal_)
      YYACCEPT;

    goto yybackup;


  /*-----------.
  | yybackup.  |
  `-----------*/
  yybackup:
    // Try to take a decision without lookahead.
    yyn = yypact_[+yystack_[0].state];
    if (yy_pact_value_is_default_(yyn))
      goto yydefault;

    // Read a lookahead token.
    if (yyla.empty()) {
      YYCDEBUG << "Reading a token\n";
#if YY_EXCEPTIONS
      try
#endif // YY_EXCEPTIONS
      {
        yyla.kind_ = yytranslate_(yylex(&yyla.value, &yyla.location, scanner, state));
      }
#if YY_EXCEPTIONS
      catch (const syntax_error& yyexc) {
        YYCDEBUG << "Caught exception: " << yyexc.what() << '\n';
        error(yyexc);
        goto yyerrlab1;
      }
#endif // YY_EXCEPTIONS
    }
    YY_SYMBOL_PRINT("Next token is", yyla);

    if (yyla.kind() == symbol_kind::s_y_yerror) {
      // The scanner already issued an error message, process directly
      // to error recovery.  But do not keep the error token as
      // lookahead, it is too special and may lead us to an endless
      // loop in error recovery. */
      yyla.kind_ = symbol_kind::S_YYUNDEF;
      goto yyerrlab1;
    }

    /* If the proper action on seeing token YYLA.TYPE is to reduce or
       to detect an error, take that action.  */
    yyn += yyla.kind();
    if (yyn < 0 || yylast_ < yyn || yycheck_[yyn] != yyla.kind()) {
      goto yydefault;
    }

    // Reduce or error.
    yyn = yytable_[yyn];
    if (yyn <= 0) {
      if (yy_table_value_is_error_(yyn))
        goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }

    // Count tokens shifted since error; after three, turn off error status.
    if (yyerrstatus_)
      --yyerrstatus_;

    // Shift the lookahead token.
    yypush_("Shifting", state_type(yyn), YY_MOVE(yyla));
    goto yynewstate;


  /*-----------------------------------------------------------.
  | yydefault -- do the default action for the current state.  |
  `-----------------------------------------------------------*/
  yydefault:
    yyn = yydefact_[+yystack_[0].state];
    if (yyn == 0)
      goto yyerrlab;
    goto yyreduce;


  /*-----------------------------.
  | yyreduce -- do a reduction.  |
  `-----------------------------*/
  yyreduce:
    yylen = yyr2_[yyn];
    {
      stack_symbol_type yylhs;
      yylhs.state = yy_lr_goto_state_(yystack_[yylen].state, yyr1_[yyn]);
      /* Variants are always initialized to an empty instance of the
         correct type. The default '$$ = $1' action is NOT applied
         when using variants.  */
      switch (yyr1_[yyn]) {
        case symbol_kind::s_start:          // start
        case symbol_kind::s_expr:           // expr
        case symbol_kind::s_expr_function:  // expr_function
        case symbol_kind::s_expr_if:        // expr_if
        case symbol_kind::s_expr_pipe_from: // expr_pipe_from
        case symbol_kind::s_expr_pipe_into: // expr_pipe_into
        case symbol_kind::s_expr_op:        // expr_op
        case symbol_kind::s_expr_app:       // expr_app
        case symbol_kind::s_expr_select:    // expr_select
        case symbol_kind::s_expr_simple:    // expr_simple
        case symbol_kind::s_path_start:     // path_start
          yylhs.value.emplace<expr_t*>();
          break;

        case symbol_kind::s_binds:  // binds
        case symbol_kind::s_binds1: // binds1
          yylhs.value.emplace<ExprAttrs*>();
          break;

        case symbol_kind::s_formal: // formal
          yylhs.value.emplace<Formal>();
          break;

        case symbol_kind::s_formal_set: // formal_set
        case symbol_kind::s_formals:    // formals
          yylhs.value.emplace<FormalsBuilder>();
          break;

        case symbol_kind::S_FLOAT_LIT: // FLOAT_LIT
          yylhs.value.emplace<NixFloat>();
          break;

        case symbol_kind::S_INT_LIT: // INT_LIT
          yylhs.value.emplace<NixInt>();
          break;

        case symbol_kind::S_ID:       // ID
        case symbol_kind::S_STR:      // STR
        case symbol_kind::S_IND_STR:  // IND_STR
        case symbol_kind::S_PATH:     // PATH
        case symbol_kind::S_HPATH:    // HPATH
        case symbol_kind::S_SPATH:    // SPATH
        case symbol_kind::S_PATH_END: // PATH_END
        case symbol_kind::S_URI:      // URI
        case symbol_kind::s_attr:     // attr
          yylhs.value.emplace<StringToken>();
          break;

        case symbol_kind::s_string_parts: // string_parts
        case symbol_kind::s_string_attr:  // string_attr
          yylhs.value.emplace<ToBeStringyExpr>();
          break;

        case symbol_kind::s_list: // list
          yylhs.value.emplace<std::pmr::vector<expr_t*>>();
          break;

        case symbol_kind::s_attrpath: // attrpath
          yylhs.value.emplace<std::vector<AttrName>>();
          break;

        case symbol_kind::s_attrs: // attrs
          yylhs.value.emplace<std::vector<std::pair<AttrName, pos_idx_t>>>();
          break;

        case symbol_kind::s_string_parts_interpolated: // string_parts_interpolated
          yylhs.value.emplace<std::vector<std::pair<pos_idx_t, expr_t*>>>();
          break;

        case symbol_kind::s_ind_string_parts: // ind_string_parts
          yylhs.value
              .emplace<std::vector<std::pair<pos_idx_t, std::variant<expr_t*, StringToken>>>>();
          break;

        default:
          break;
      }


      // Default location.
      {
        stack_type::slice range(yystack_, yylen);
        YYLLOC_DEFAULT(yylhs.location, range, yylen);
        yyerror_range[1].location = yylhs.location;
      }

      // Perform the reduction.
      YY_REDUCE_PRINT(yyn);
#if YY_EXCEPTIONS
      try
#endif // YY_EXCEPTIONS
      {
        switch (yyn) {
          case 2: // start: expr
#line 171 "parser.y"
          {
            state->result = yystack_[0].value.as<expr_t*>();

            // This parser does not use yynerrs; suppress the warning.
            (void)yynerrs_;
          }
#line 1193 "parser-tab.cpp"
          break;

          case 3: // expr: expr_function
#line 178 "parser.y"
          {
            yylhs.value.as<expr_t*>() = yystack_[0].value.as<expr_t*>();
          }
#line 1199 "parser-tab.cpp"
          break;

          case 4: // expr_function: ID ':' expr_function
#line 182 "parser.y"
          {
            auto me = state->exprs.add<ExprLambda>(
                CUR_POS, state->symbols.create(yystack_[2].value.as<StringToken>()),
                yystack_[0].value.as<expr_t*>());
            yylhs.value.as<expr_t*>() = me;
            SET_DOC_POS(me, yystack_[2].location);
          }
#line 1208 "parser-tab.cpp"
          break;

          case 5: // expr_function: formal_set ':' expr_function
#line 187 "parser.y"
          {
            state->validateFormals(yystack_[2].value.as<FormalsBuilder>());
            auto me = state->exprs.add<ExprLambda>(state->positions, state->exprs.alloc, CUR_POS,
                                                   yystack_[2].value.as<FormalsBuilder>(),
                                                   yystack_[0].value.as<expr_t*>());
            yylhs.value.as<expr_t*>() = me;
            SET_DOC_POS(me, yystack_[2].location);
          }
#line 1219 "parser-tab.cpp"
          break;

          case 6: // expr_function: formal_set '@' ID ':' expr_function
#line 194 "parser.y"
          {
            auto arg = state->symbols.create(yystack_[2].value.as<StringToken>());
            state->validateFormals(yystack_[4].value.as<FormalsBuilder>(), CUR_POS, arg);
            auto me = state->exprs.add<ExprLambda>(state->positions, state->exprs.alloc, CUR_POS,
                                                   arg, yystack_[4].value.as<FormalsBuilder>(),
                                                   yystack_[0].value.as<expr_t*>());
            yylhs.value.as<expr_t*>() = me;
            SET_DOC_POS(me, yystack_[4].location);
          }
#line 1231 "parser-tab.cpp"
          break;

          case 7: // expr_function: ID '@' formal_set ':' expr_function
#line 202 "parser.y"
          {
            auto arg = state->symbols.create(yystack_[4].value.as<StringToken>());
            state->validateFormals(yystack_[2].value.as<FormalsBuilder>(), CUR_POS, arg);
            auto me = state->exprs.add<ExprLambda>(state->positions, state->exprs.alloc, CUR_POS,
                                                   arg, yystack_[2].value.as<FormalsBuilder>(),
                                                   yystack_[0].value.as<expr_t*>());
            yylhs.value.as<expr_t*>() = me;
            SET_DOC_POS(me, yystack_[4].location);
          }
#line 1243 "parser-tab.cpp"
          break;

          case 8: // expr_function: ASSERT expr ';' expr_function
#line 210 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprAssert>(
                CUR_POS, yystack_[2].value.as<expr_t*>(), yystack_[0].value.as<expr_t*>());
          }
#line 1249 "parser-tab.cpp"
          break;

          case 9: // expr_function: WITH expr ';' expr_function
#line 212 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprWith>(
                CUR_POS, yystack_[2].value.as<expr_t*>(), yystack_[0].value.as<expr_t*>());
          }
#line 1255 "parser-tab.cpp"
          break;

          case 10: // expr_function: LET binds IN_KW expr_function
#line 214 "parser.y"
          {
            if (!yystack_[2].value.as<ExprAttrs*>()->dynamicAttrs->empty())
              throw ParseError({.msg_ = hint_fmt_t("dynamic attributes not allowed in let"),
                                .pos_ = state->positions[CUR_POS]});
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprLet>(
                yystack_[2].value.as<ExprAttrs*>(), yystack_[0].value.as<expr_t*>());
          }
#line 1267 "parser-tab.cpp"
          break;

          case 11: // expr_function: expr_if
#line 221 "parser.y"
          {
            yylhs.value.as<expr_t*>() = yystack_[0].value.as<expr_t*>();
          }
#line 1273 "parser-tab.cpp"
          break;

          case 12: // expr_if: IF expr THEN expr ELSE expr
#line 225 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprIf>(
                CUR_POS, yystack_[4].value.as<expr_t*>(), yystack_[2].value.as<expr_t*>(),
                yystack_[0].value.as<expr_t*>());
          }
#line 1279 "parser-tab.cpp"
          break;

          case 13: // expr_if: expr_pipe_from
#line 226 "parser.y"
          {
            yylhs.value.as<expr_t*>() = yystack_[0].value.as<expr_t*>();
          }
#line 1285 "parser-tab.cpp"
          break;

          case 14: // expr_if: expr_pipe_into
#line 227 "parser.y"
          {
            yylhs.value.as<expr_t*>() = yystack_[0].value.as<expr_t*>();
          }
#line 1291 "parser-tab.cpp"
          break;

          case 15: // expr_if: expr_op
#line 228 "parser.y"
          {
            yylhs.value.as<expr_t*>() = yystack_[0].value.as<expr_t*>();
          }
#line 1297 "parser-tab.cpp"
          break;

          case 16: // expr_pipe_from: expr_op PIPE_FROM expr_pipe_from
#line 232 "parser.y"
          {
            yylhs.value.as<expr_t*>() =
                make_call(state->exprs, state->at(yystack_[1].location),
                          yystack_[2].value.as<expr_t*>(), yystack_[0].value.as<expr_t*>());
          }
#line 1303 "parser-tab.cpp"
          break;

          case 17: // expr_pipe_from: expr_op PIPE_FROM expr_op
#line 233 "parser.y"
          {
            yylhs.value.as<expr_t*>() =
                make_call(state->exprs, state->at(yystack_[1].location),
                          yystack_[2].value.as<expr_t*>(), yystack_[0].value.as<expr_t*>());
          }
#line 1309 "parser-tab.cpp"
          break;

          case 18: // expr_pipe_into: expr_pipe_into PIPE_INTO expr_op
#line 237 "parser.y"
          {
            yylhs.value.as<expr_t*>() =
                make_call(state->exprs, state->at(yystack_[1].location),
                          yystack_[0].value.as<expr_t*>(), yystack_[2].value.as<expr_t*>());
          }
#line 1315 "parser-tab.cpp"
          break;

          case 19: // expr_pipe_into: expr_op PIPE_INTO expr_op
#line 238 "parser.y"
          {
            yylhs.value.as<expr_t*>() =
                make_call(state->exprs, state->at(yystack_[1].location),
                          yystack_[0].value.as<expr_t*>(), yystack_[2].value.as<expr_t*>());
          }
#line 1321 "parser-tab.cpp"
          break;

          case 20: // expr_op: '!' expr_op
#line 242 "parser.y"
          {
            yylhs.value.as<expr_t*>() =
                state->exprs.add<ExprOpNot>(yystack_[0].value.as<expr_t*>());
          }
#line 1327 "parser-tab.cpp"
          break;

          case 21: // expr_op: '-' expr_op
#line 243 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprCall>(
                CUR_POS, state->exprs.add<ExprVar>(state->s.sub),
                {state->exprs.add<ExprInt>(0), yystack_[0].value.as<expr_t*>()});
          }
#line 1333 "parser-tab.cpp"
          break;

          case 22: // expr_op: expr_op EQ expr_op
#line 244 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprOpEq>(yystack_[2].value.as<expr_t*>(),
                                                                   yystack_[0].value.as<expr_t*>());
          }
#line 1339 "parser-tab.cpp"
          break;

          case 23: // expr_op: expr_op NEQ expr_op
#line 245 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprOpNEq>(
                yystack_[2].value.as<expr_t*>(), yystack_[0].value.as<expr_t*>());
          }
#line 1345 "parser-tab.cpp"
          break;

          case 24: // expr_op: expr_op '<' expr_op
#line 246 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprCall>(
                state->at(yystack_[1].location), state->exprs.add<ExprVar>(state->s.lessThan),
                {yystack_[2].value.as<expr_t*>(), yystack_[0].value.as<expr_t*>()});
          }
#line 1351 "parser-tab.cpp"
          break;

          case 25: // expr_op: expr_op LEQ expr_op
#line 247 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprOpNot>(state->exprs.add<ExprCall>(
                state->at(yystack_[1].location), state->exprs.add<ExprVar>(state->s.lessThan),
                {yystack_[0].value.as<expr_t*>(), yystack_[2].value.as<expr_t*>()}));
          }
#line 1357 "parser-tab.cpp"
          break;

          case 26: // expr_op: expr_op '>' expr_op
#line 248 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprCall>(
                state->at(yystack_[1].location), state->exprs.add<ExprVar>(state->s.lessThan),
                {yystack_[0].value.as<expr_t*>(), yystack_[2].value.as<expr_t*>()});
          }
#line 1363 "parser-tab.cpp"
          break;

          case 27: // expr_op: expr_op GEQ expr_op
#line 249 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprOpNot>(state->exprs.add<ExprCall>(
                state->at(yystack_[1].location), state->exprs.add<ExprVar>(state->s.lessThan),
                {yystack_[2].value.as<expr_t*>(), yystack_[0].value.as<expr_t*>()}));
          }
#line 1369 "parser-tab.cpp"
          break;

          case 28: // expr_op: expr_op AND expr_op
#line 250 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprOpAnd>(
                state->at(yystack_[1].location), yystack_[2].value.as<expr_t*>(),
                yystack_[0].value.as<expr_t*>());
          }
#line 1375 "parser-tab.cpp"
          break;

          case 29: // expr_op: expr_op OR expr_op
#line 251 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprOpOr>(state->at(yystack_[1].location),
                                                                   yystack_[2].value.as<expr_t*>(),
                                                                   yystack_[0].value.as<expr_t*>());
          }
#line 1381 "parser-tab.cpp"
          break;

          case 30: // expr_op: expr_op IMPL expr_op
#line 252 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprOpImpl>(
                state->at(yystack_[1].location), yystack_[2].value.as<expr_t*>(),
                yystack_[0].value.as<expr_t*>());
          }
#line 1387 "parser-tab.cpp"
          break;

          case 31: // expr_op: expr_op UPDATE expr_op
#line 253 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprOpUpdate>(
                state->at(yystack_[1].location), yystack_[2].value.as<expr_t*>(),
                yystack_[0].value.as<expr_t*>());
          }
#line 1393 "parser-tab.cpp"
          break;

          case 32: // expr_op: expr_op '?' attrpath
#line 254 "parser.y"
          {
            yylhs.value.as<expr_t*>() =
                state->exprs.add<ExprOpHasAttr>(state->exprs.alloc, yystack_[2].value.as<expr_t*>(),
                                                yystack_[0].value.as<std::vector<AttrName>>());
          }
#line 1399 "parser-tab.cpp"
          break;

          case 33: // expr_op: expr_op '+' expr_op
#line 256 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprConcatStrings>(
                state->exprs.alloc, state->at(yystack_[1].location), false,
                {{state->at(yystack_[2].location), yystack_[2].value.as<expr_t*>()},
                 {state->at(yystack_[0].location), yystack_[0].value.as<expr_t*>()}});
          }
#line 1405 "parser-tab.cpp"
          break;

          case 34: // expr_op: expr_op '-' expr_op
#line 257 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprCall>(
                state->at(yystack_[1].location), state->exprs.add<ExprVar>(state->s.sub),
                {yystack_[2].value.as<expr_t*>(), yystack_[0].value.as<expr_t*>()});
          }
#line 1411 "parser-tab.cpp"
          break;

          case 35: // expr_op: expr_op '*' expr_op
#line 258 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprCall>(
                state->at(yystack_[1].location), state->exprs.add<ExprVar>(state->s.mul),
                {yystack_[2].value.as<expr_t*>(), yystack_[0].value.as<expr_t*>()});
          }
#line 1417 "parser-tab.cpp"
          break;

          case 36: // expr_op: expr_op '/' expr_op
#line 259 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprCall>(
                state->at(yystack_[1].location), state->exprs.add<ExprVar>(state->s.div),
                {yystack_[2].value.as<expr_t*>(), yystack_[0].value.as<expr_t*>()});
          }
#line 1423 "parser-tab.cpp"
          break;

          case 37: // expr_op: expr_op CONCAT expr_op
#line 260 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprOpConcatLists>(
                state->at(yystack_[1].location), yystack_[2].value.as<expr_t*>(),
                yystack_[0].value.as<expr_t*>());
          }
#line 1429 "parser-tab.cpp"
          break;

          case 38: // expr_op: expr_app
#line 261 "parser.y"
          {
            yylhs.value.as<expr_t*>() = yystack_[0].value.as<expr_t*>();
          }
#line 1435 "parser-tab.cpp"
          break;

          case 39: // expr_app: expr_app expr_select
#line 265 "parser.y"
          {
            yylhs.value.as<expr_t*>() =
                make_call(state->exprs, CUR_POS, yystack_[1].value.as<expr_t*>(),
                          yystack_[0].value.as<expr_t*>());
            yystack_[0].value.as<expr_t*>()->warnIfCursedOr(state->symbols, state->positions);
          }
#line 1441 "parser-tab.cpp"
          break;

          case 40: // expr_app: expr_select
#line 270 "parser.y"
          {
            yylhs.value.as<expr_t*>() = yystack_[0].value.as<expr_t*>();
            yylhs.value.as<expr_t*>()->resetCursedOr();
          }
#line 1447 "parser-tab.cpp"
          break;

          case 41: // expr_select: expr_simple '.' attrpath
#line 275 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprSelect>(
                state->exprs.alloc, CUR_POS, yystack_[2].value.as<expr_t*>(),
                yystack_[0].value.as<std::vector<AttrName>>(), nullptr);
          }
#line 1453 "parser-tab.cpp"
          break;

          case 42: // expr_select: expr_simple '.' attrpath OR_KW expr_select
#line 277 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprSelect>(
                state->exprs.alloc, CUR_POS, yystack_[4].value.as<expr_t*>(),
                yystack_[2].value.as<std::vector<AttrName>>(), yystack_[0].value.as<expr_t*>());
            yystack_[0].value.as<expr_t*>()->warnIfCursedOr(state->symbols, state->positions);
          }
#line 1459 "parser-tab.cpp"
          break;

          case 43: // expr_select: expr_simple OR_KW
#line 286 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprCall>(
                CUR_POS, yystack_[1].value.as<expr_t*>(),
                {state->exprs.add<ExprVar>(CUR_POS, state->s.or_)},
                state->positions.add(state->origin, yylhs.location.endOffset));
          }
#line 1465 "parser-tab.cpp"
          break;

          case 44: // expr_select: expr_simple
#line 287 "parser.y"
          {
            yylhs.value.as<expr_t*>() = yystack_[0].value.as<expr_t*>();
          }
#line 1471 "parser-tab.cpp"
          break;

          case 45: // expr_simple: ID
#line 291 "parser.y"
          {
            std::string_view s = "__curPos";
            if (yystack_[0].value.as<StringToken>().l == s.size() &&
                strncmp(yystack_[0].value.as<StringToken>().p, s.data(), s.size()) == 0)
              yylhs.value.as<expr_t*>() = state->exprs.add<ExprPos>(CUR_POS);
            else
              yylhs.value.as<expr_t*>() = state->exprs.add<ExprVar>(
                  CUR_POS, state->symbols.create(yystack_[0].value.as<StringToken>()));
          }
#line 1483 "parser-tab.cpp"
          break;

          case 46: // expr_simple: INT_LIT
#line 298 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprInt>(yystack_[0].value.as<NixInt>());
          }
#line 1489 "parser-tab.cpp"
          break;

          case 47: // expr_simple: FLOAT_LIT
#line 299 "parser.y"
          {
            yylhs.value.as<expr_t*>() =
                state->exprs.add<ExprFloat>(yystack_[0].value.as<NixFloat>());
          }
#line 1495 "parser-tab.cpp"
          break;

          case 48: // expr_simple: '"' string_parts '"'
#line 300 "parser.y"
          {
            yylhs.value.as<expr_t*>() =
                yystack_[1].value.as<ToBeStringyExpr>().toExpr(state->exprs);
          }
#line 1501 "parser-tab.cpp"
          break;

          case 49: // expr_simple: IND_STRING_OPEN ind_string_parts IND_STRING_CLOSE
#line 301 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->strip_indentation(
                CUR_POS,
                yystack_[1]
                    .value
                    .as<std::vector<std::pair<pos_idx_t, std::variant<expr_t*, StringToken>>>>());
          }
#line 1509 "parser-tab.cpp"
          break;

          case 50: // expr_simple: path_start PATH_END
#line 304 "parser.y"
          {
            yylhs.value.as<expr_t*>() = yystack_[1].value.as<expr_t*>();
          }
#line 1515 "parser-tab.cpp"
          break;

          case 51: // expr_simple: path_start string_parts_interpolated PATH_END
#line 305 "parser.y"
          {
            yystack_[1].value.as<std::vector<std::pair<pos_idx_t, expr_t*>>>().insert(
                yystack_[1].value.as<std::vector<std::pair<pos_idx_t, expr_t*>>>().begin(),
                {state->at(yystack_[2].location), yystack_[2].value.as<expr_t*>()});
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprConcatStrings>(
                state->exprs.alloc, CUR_POS, false,
                yystack_[1].value.as<std::vector<std::pair<pos_idx_t, expr_t*>>>());
          }
#line 1524 "parser-tab.cpp"
          break;

          case 52: // expr_simple: SPATH
#line 309 "parser.y"
          {
            std::string_view path(yystack_[0].value.as<StringToken>().p + 1,
                                  yystack_[0].value.as<StringToken>().l - 2);
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprCall>(
                CUR_POS, state->exprs.add<ExprVar>(state->s.findFile),
                {state->exprs.add<ExprVar>(state->s.nixPath),
                 state->exprs.add<ExprString>(state->exprs.alloc, path)});
          }
#line 1536 "parser-tab.cpp"
          break;

          case 53: // expr_simple: URI
#line 316 "parser.y"
          {
            static bool no_url_literals =
                experimental_feature_settings.is_enabled(xp_t::no_url_literals);
            if (no_url_literals)
              throw ParseError({.msg_ = hint_fmt_t("URL literals are disabled"),
                                .pos_ = state->positions[CUR_POS]});
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprString>(
                state->exprs.alloc, yystack_[0].value.as<StringToken>());
          }
#line 1550 "parser-tab.cpp"
          break;

          case 54: // expr_simple: '(' expr ')'
#line 325 "parser.y"
          {
            yylhs.value.as<expr_t*>() = yystack_[1].value.as<expr_t*>();
          }
#line 1556 "parser-tab.cpp"
          break;

          case 55: // expr_simple: LET '{' binds '}'
#line 329 "parser.y"
          {
            yystack_[1].value.as<ExprAttrs*>()->recursive = true;
            yystack_[1].value.as<ExprAttrs*>()->pos = CUR_POS;
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprSelect>(
                state->exprs.alloc, no_pos, yystack_[1].value.as<ExprAttrs*>(), state->s.body);
          }
#line 1562 "parser-tab.cpp"
          break;

          case 56: // expr_simple: REC '{' binds '}'
#line 331 "parser.y"
          {
            yystack_[1].value.as<ExprAttrs*>()->recursive = true;
            yystack_[1].value.as<ExprAttrs*>()->pos = CUR_POS;
            yylhs.value.as<expr_t*>() = yystack_[1].value.as<ExprAttrs*>();
          }
#line 1568 "parser-tab.cpp"
          break;

          case 57: // expr_simple: '{' binds1 '}'
#line 333 "parser.y"
          {
            yystack_[1].value.as<ExprAttrs*>()->pos = CUR_POS;
            yylhs.value.as<expr_t*>() = yystack_[1].value.as<ExprAttrs*>();
          }
#line 1574 "parser-tab.cpp"
          break;

          case 58: // expr_simple: '{' '}'
#line 335 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprAttrs>(CUR_POS);
          }
#line 1580 "parser-tab.cpp"
          break;

          case 59: // expr_simple: '[' list ']'
#line 336 "parser.y"
          {
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprList>(
                state->exprs.alloc, yystack_[1].value.as<std::pmr::vector<expr_t*>>());
          }
#line 1586 "parser-tab.cpp"
          break;

          case 60: // string_parts: STR
#line 340 "parser.y"
          {
            yylhs.value.as<ToBeStringyExpr>() = {yystack_[0].value.as<StringToken>()};
          }
#line 1592 "parser-tab.cpp"
          break;

          case 61: // string_parts: string_parts_interpolated
#line 341 "parser.y"
          {
            yylhs.value.as<ToBeStringyExpr>() = {state->exprs.add<ExprConcatStrings>(
                state->exprs.alloc, CUR_POS, true,
                yystack_[0].value.as<std::vector<std::pair<pos_idx_t, expr_t*>>>())};
          }
#line 1598 "parser-tab.cpp"
          break;

          case 62: // string_parts: %empty
#line 342 "parser.y"
          {
            yylhs.value.as<ToBeStringyExpr>() = {std::string_view()};
          }
#line 1604 "parser-tab.cpp"
          break;

          case 63: // string_parts_interpolated: string_parts_interpolated STR
#line 347 "parser.y"
          {
            yylhs.value.as<std::vector<std::pair<pos_idx_t, expr_t*>>>() =
                std::move(yystack_[1].value.as<std::vector<std::pair<pos_idx_t, expr_t*>>>());
            yylhs.value.as<std::vector<std::pair<pos_idx_t, expr_t*>>>().emplace_back(
                state->at(yystack_[0].location),
                state->exprs.add<ExprString>(state->exprs.alloc,
                                             yystack_[0].value.as<StringToken>()));
          }
#line 1610 "parser-tab.cpp"
          break;

          case 64: // string_parts_interpolated: string_parts_interpolated DOLLAR_CURLY expr '}'
#line 348 "parser.y"
          {
            yylhs.value.as<std::vector<std::pair<pos_idx_t, expr_t*>>>() =
                std::move(yystack_[3].value.as<std::vector<std::pair<pos_idx_t, expr_t*>>>());
            yylhs.value.as<std::vector<std::pair<pos_idx_t, expr_t*>>>().emplace_back(
                state->at(yystack_[2].location), yystack_[1].value.as<expr_t*>());
          }
#line 1616 "parser-tab.cpp"
          break;

          case 65: // string_parts_interpolated: DOLLAR_CURLY expr '}'
#line 349 "parser.y"
          {
            yylhs.value.as<std::vector<std::pair<pos_idx_t, expr_t*>>>().emplace_back(
                state->at(yystack_[2].location), yystack_[1].value.as<expr_t*>());
          }
#line 1622 "parser-tab.cpp"
          break;

          case 66: // string_parts_interpolated: STR DOLLAR_CURLY expr '}'
#line 350 "parser.y"
          {
            yylhs.value.as<std::vector<std::pair<pos_idx_t, expr_t*>>>().emplace_back(
                state->at(yystack_[3].location),
                state->exprs.add<ExprString>(state->exprs.alloc,
                                             yystack_[3].value.as<StringToken>()));
            yylhs.value.as<std::vector<std::pair<pos_idx_t, expr_t*>>>().emplace_back(
                state->at(yystack_[2].location), yystack_[1].value.as<expr_t*>());
          }
#line 1631 "parser-tab.cpp"
          break;

          case 67: // path_start: PATH
#line 357 "parser.y"
          {
            std::string_view literal(
                {yystack_[0].value.as<StringToken>().p, yystack_[0].value.as<StringToken>().l});

            /* check for short path literals */
            if (state->settings.warnShortPathLiterals && literal.front() != '/' &&
                literal.front() != '.') {
              logWarning(
                  {.msg_ = hint_fmt_t("relative path literal '%s' should be prefixed with '.' "
                                      "for clarity: './%s'. (" ANSI_BOLD
                                      "warn-short-path-literals" ANSI_NORMAL " = true)",
                                      literal, literal),
                   .pos_ = state->positions[CUR_POS]});
            }

            Path path(abs_path(literal, state->base_path.path.abs()));
            /* add back in the trailing '/' to the first segment */
            if (literal.size() > 1 && literal.back() == '/')
              path += '/';
            yylhs.value.as<expr_t*>() =
                /* Absolute paths are always interpreted relative to the
                   root filesystem accessor, rather than the accessor of the
                   current Nix expression. */
                literal.front() == '/'
                    ? state->exprs.add<ExprPath>(state->exprs.alloc, state->root_fs, path)
                    : state->exprs.add<ExprPath>(state->exprs.alloc, state->base_path.accessor,
                                                 path);
          }
#line 1659 "parser-tab.cpp"
          break;

          case 68: // path_start: HPATH
#line 380 "parser.y"
          {
            if (state->settings.pureEval) {
              throw Error("the path '%s' can not be resolved in pure mode",
                          std::string_view(yystack_[0].value.as<StringToken>().p,
                                           yystack_[0].value.as<StringToken>().l));
            }
            Path path(get_home().string() + std::string(yystack_[0].value.as<StringToken>().p + 1,
                                                        yystack_[0].value.as<StringToken>().l - 1));
            yylhs.value.as<expr_t*>() = state->exprs.add<ExprPath>(
                state->exprs.alloc, ref<source_accessor_t>(state->root_fs), path);
          }
#line 1674 "parser-tab.cpp"
          break;

          case 69: // ind_string_parts: ind_string_parts IND_STR
#line 393 "parser.y"
          {
            yylhs.value
                .as<std::vector<std::pair<pos_idx_t, std::variant<expr_t*, StringToken>>>>() =
                std::move(yystack_[1]
                              .value.as<std::vector<
                                  std::pair<pos_idx_t, std::variant<expr_t*, StringToken>>>>());
            yylhs.value.as<std::vector<std::pair<pos_idx_t, std::variant<expr_t*, StringToken>>>>()
                .emplace_back(state->at(yystack_[0].location), yystack_[0].value.as<StringToken>());
          }
#line 1680 "parser-tab.cpp"
          break;

          case 70: // ind_string_parts: ind_string_parts DOLLAR_CURLY expr '}'
#line 394 "parser.y"
          {
            yylhs.value
                .as<std::vector<std::pair<pos_idx_t, std::variant<expr_t*, StringToken>>>>() =
                std::move(yystack_[3]
                              .value.as<std::vector<
                                  std::pair<pos_idx_t, std::variant<expr_t*, StringToken>>>>());
            yylhs.value.as<std::vector<std::pair<pos_idx_t, std::variant<expr_t*, StringToken>>>>()
                .emplace_back(state->at(yystack_[2].location), yystack_[1].value.as<expr_t*>());
          }
#line 1686 "parser-tab.cpp"
          break;

          case 71: // ind_string_parts: %empty
#line 395 "parser.y"
          {
          }
#line 1692 "parser-tab.cpp"
          break;

          case 72: // binds: binds1
#line 399 "parser.y"
          {
            yylhs.value.as<ExprAttrs*>() = yystack_[0].value.as<ExprAttrs*>();
          }
#line 1698 "parser-tab.cpp"
          break;

          case 73: // binds: %empty
#line 400 "parser.y"
          {
            yylhs.value.as<ExprAttrs*>() = state->exprs.add<ExprAttrs>();
          }
#line 1704 "parser-tab.cpp"
          break;

          case 74: // binds1: binds1 attrpath '=' expr ';'
#line 405 "parser.y"
          {
            yylhs.value.as<ExprAttrs*>() = yystack_[4].value.as<ExprAttrs*>();
            state->addAttr(yylhs.value.as<ExprAttrs*>(),
                           std::move(yystack_[3].value.as<std::vector<AttrName>>()),
                           yystack_[3].location, yystack_[1].value.as<expr_t*>(),
                           yystack_[1].location);
          }
#line 1712 "parser-tab.cpp"
          break;

          case 75: // binds1: binds INHERIT attrs ';'
#line 409 "parser.y"
          {
            yylhs.value.as<ExprAttrs*>() = yystack_[3].value.as<ExprAttrs*>();
            for (auto& [i, iPos] :
                 yystack_[1].value.as<std::vector<std::pair<AttrName, pos_idx_t>>>()) {
              if (yystack_[3].value.as<ExprAttrs*>()->attrs->find(i.symbol) !=
                  yystack_[3].value.as<ExprAttrs*>()->attrs->end())
                state->dupAttr(i.symbol, iPos,
                               (*yystack_[3].value.as<ExprAttrs*>()->attrs)[i.symbol].pos);
              yystack_[3].value.as<ExprAttrs*>()->attrs->emplace(
                  i.symbol, ExprAttrs::AttrDef(state->exprs.add<ExprVar>(iPos, i.symbol), iPos,
                                               ExprAttrs::AttrDef::Kind::Inherited));
            }
          }
#line 1726 "parser-tab.cpp"
          break;

          case 76: // binds1: binds INHERIT '(' expr ')' attrs ';'
#line 419 "parser.y"
          {
            yylhs.value.as<ExprAttrs*>() = yystack_[6].value.as<ExprAttrs*>();
            if (!yystack_[6].value.as<ExprAttrs*>()->inheritFromExprs)
              yystack_[6].value.as<ExprAttrs*>()->inheritFromExprs =
                  std::make_unique<std::pmr::vector<expr_t*>>();
            yystack_[6].value.as<ExprAttrs*>()->inheritFromExprs->push_back(
                yystack_[3].value.as<expr_t*>());
            auto from = state->exprs.add<ExprInheritFrom>(
                state->at(yystack_[3].location),
                yystack_[6].value.as<ExprAttrs*>()->inheritFromExprs->size() - 1);
            for (auto& [i, iPos] :
                 yystack_[1].value.as<std::vector<std::pair<AttrName, pos_idx_t>>>()) {
              if (yystack_[6].value.as<ExprAttrs*>()->attrs->find(i.symbol) !=
                  yystack_[6].value.as<ExprAttrs*>()->attrs->end())
                state->dupAttr(i.symbol, iPos,
                               (*yystack_[6].value.as<ExprAttrs*>()->attrs)[i.symbol].pos);
              yystack_[6].value.as<ExprAttrs*>()->attrs->emplace(
                  i.symbol, ExprAttrs::AttrDef(state->exprs.add<ExprSelect>(state->exprs.alloc,
                                                                            iPos, from, i.symbol),
                                               iPos, ExprAttrs::AttrDef::Kind::InheritedFrom));
            }
          }
#line 1747 "parser-tab.cpp"
          break;

          case 77: // binds1: attrpath '=' expr ';'
#line 436 "parser.y"
          {
            yylhs.value.as<ExprAttrs*>() = state->exprs.add<ExprAttrs>();
            state->addAttr(yylhs.value.as<ExprAttrs*>(),
                           std::move(yystack_[3].value.as<std::vector<AttrName>>()),
                           yystack_[3].location, yystack_[1].value.as<expr_t*>(),
                           yystack_[1].location);
          }
#line 1755 "parser-tab.cpp"
          break;

          case 78: // attrs: attrs attr
#line 442 "parser.y"
          {
            yylhs.value.as<std::vector<std::pair<AttrName, pos_idx_t>>>() =
                std::move(yystack_[1].value.as<std::vector<std::pair<AttrName, pos_idx_t>>>());
            yylhs.value.as<std::vector<std::pair<AttrName, pos_idx_t>>>().emplace_back(
                state->symbols.create(yystack_[0].value.as<StringToken>()),
                state->at(yystack_[0].location));
          }
#line 1761 "parser-tab.cpp"
          break;

          case 79: // attrs: attrs string_attr
#line 444 "parser.y"
          {
            yylhs.value.as<std::vector<std::pair<AttrName, pos_idx_t>>>() =
                std::move(yystack_[1].value.as<std::vector<std::pair<AttrName, pos_idx_t>>>());
            yystack_[0].value.as<ToBeStringyExpr>().visit(overloaded{
                [&](std::string_view str) {
                  yylhs.value.as<std::vector<std::pair<AttrName, pos_idx_t>>>().emplace_back(
                      state->symbols.create(str), state->at(yystack_[0].location));
                },
                [&](expr_t* expr) {
                  throw ParseError({.msg_ = hint_fmt_t("dynamic attributes not allowed in inherit"),
                                    .pos_ = state->positions[state->at(yystack_[0].location)]});
                }});
          }
#line 1777 "parser-tab.cpp"
          break;

          case 80: // attrs: %empty
#line 455 "parser.y"
          {
          }
#line 1783 "parser-tab.cpp"
          break;

          case 81: // attrpath: attrpath '.' attr
#line 459 "parser.y"
          {
            yylhs.value.as<std::vector<AttrName>>() =
                std::move(yystack_[2].value.as<std::vector<AttrName>>());
            yylhs.value.as<std::vector<AttrName>>().emplace_back(
                state->symbols.create(yystack_[0].value.as<StringToken>()));
          }
#line 1789 "parser-tab.cpp"
          break;

          case 82: // attrpath: attrpath '.' string_attr
#line 461 "parser.y"
          {
            yylhs.value.as<std::vector<AttrName>>() =
                std::move(yystack_[2].value.as<std::vector<AttrName>>());
            yystack_[0].value.as<ToBeStringyExpr>().visit(overloaded{
                [&](std::string_view str) {
                  yylhs.value.as<std::vector<AttrName>>().emplace_back(state->symbols.create(str));
                },
                [&](expr_t* expr) { yylhs.value.as<std::vector<AttrName>>().emplace_back(expr); }});
          }
#line 1800 "parser-tab.cpp"
          break;

          case 83: // attrpath: attr
#line 467 "parser.y"
          {
            yylhs.value.as<std::vector<AttrName>>().emplace_back(
                state->symbols.create(yystack_[0].value.as<StringToken>()));
          }
#line 1806 "parser-tab.cpp"
          break;

          case 84: // attrpath: string_attr
#line 469 "parser.y"
          {
            yystack_[0].value.as<ToBeStringyExpr>().visit(overloaded{
                [&](std::string_view str) {
                  yylhs.value.as<std::vector<AttrName>>().emplace_back(state->symbols.create(str));
                },
                [&](expr_t* expr) { yylhs.value.as<std::vector<AttrName>>().emplace_back(expr); }});
          }
#line 1816 "parser-tab.cpp"
          break;

          case 85: // attr: ID
#line 477 "parser.y"
          {
            yylhs.value.as<StringToken>() = yystack_[0].value.as<StringToken>();
          }
#line 1822 "parser-tab.cpp"
          break;

          case 86: // attr: OR_KW
#line 478 "parser.y"
          {
            yylhs.value.as<StringToken>() = {"or", 2};
          }
#line 1828 "parser-tab.cpp"
          break;

          case 87: // string_attr: '"' string_parts '"'
#line 482 "parser.y"
          {
            yylhs.value.as<ToBeStringyExpr>() = std::move(yystack_[1].value.as<ToBeStringyExpr>());
          }
#line 1834 "parser-tab.cpp"
          break;

          case 88: // string_attr: DOLLAR_CURLY expr '}'
#line 483 "parser.y"
          {
            yylhs.value.as<ToBeStringyExpr>() = {yystack_[1].value.as<expr_t*>()};
          }
#line 1840 "parser-tab.cpp"
          break;

          case 89: // list: list expr_select
#line 487 "parser.y"
          {
            yylhs.value.as<std::pmr::vector<expr_t*>>() =
                std::move(yystack_[1].value.as<std::pmr::vector<expr_t*>>());
            yylhs.value.as<std::pmr::vector<expr_t*>>().push_back(
                yystack_[0].value.as<expr_t*>()); /* !!! dangerous */
            ;
            yystack_[0].value.as<expr_t*>()->warnIfCursedOr(state->symbols, state->positions);
          }
#line 1846 "parser-tab.cpp"
          break;

          case 90: // list: %empty
#line 488 "parser.y"
          {
          }
#line 1852 "parser-tab.cpp"
          break;

          case 91: // formal_set: '{' formals ',' ELLIPSIS '}'
#line 492 "parser.y"
          {
            yylhs.value.as<FormalsBuilder>() = std::move(yystack_[3].value.as<FormalsBuilder>());
            yylhs.value.as<FormalsBuilder>().ellipsis = true;
          }
#line 1858 "parser-tab.cpp"
          break;

          case 92: // formal_set: '{' ELLIPSIS '}'
#line 493 "parser.y"
          {
            yylhs.value.as<FormalsBuilder>().ellipsis = true;
          }
#line 1864 "parser-tab.cpp"
          break;

          case 93: // formal_set: '{' formals ',' '}'
#line 494 "parser.y"
          {
            yylhs.value.as<FormalsBuilder>() = std::move(yystack_[2].value.as<FormalsBuilder>());
            yylhs.value.as<FormalsBuilder>().ellipsis = false;
          }
#line 1870 "parser-tab.cpp"
          break;

          case 94: // formal_set: '{' formals '}'
#line 495 "parser.y"
          {
            yylhs.value.as<FormalsBuilder>() = std::move(yystack_[1].value.as<FormalsBuilder>());
            yylhs.value.as<FormalsBuilder>().ellipsis = false;
          }
#line 1876 "parser-tab.cpp"
          break;

          case 95: // formal_set: '{' '}'
#line 496 "parser.y"
          {
            yylhs.value.as<FormalsBuilder>().ellipsis = false;
          }
#line 1882 "parser-tab.cpp"
          break;

          case 96: // formals: formals ',' formal
#line 501 "parser.y"
          {
            yylhs.value.as<FormalsBuilder>() = std::move(yystack_[2].value.as<FormalsBuilder>());
            yylhs.value.as<FormalsBuilder>().formals.emplace_back(
                std::move(yystack_[0].value.as<Formal>()));
          }
#line 1888 "parser-tab.cpp"
          break;

          case 97: // formals: formal
#line 503 "parser.y"
          {
            yylhs.value.as<FormalsBuilder>().formals.emplace_back(
                std::move(yystack_[0].value.as<Formal>()));
          }
#line 1894 "parser-tab.cpp"
          break;

          case 98: // formal: ID
#line 507 "parser.y"
          {
            yylhs.value.as<Formal>() =
                Formal{CUR_POS, state->symbols.create(yystack_[0].value.as<StringToken>()), 0};
          }
#line 1900 "parser-tab.cpp"
          break;

          case 99: // formal: ID '?' expr
#line 508 "parser.y"
          {
            yylhs.value.as<Formal>() =
                Formal{CUR_POS, state->symbols.create(yystack_[2].value.as<StringToken>()),
                       yystack_[0].value.as<expr_t*>()};
          }
#line 1906 "parser-tab.cpp"
          break;


#line 1910 "parser-tab.cpp"

          default:
            break;
        }
      }
#if YY_EXCEPTIONS
      catch (const syntax_error& yyexc) {
        YYCDEBUG << "Caught exception: " << yyexc.what() << '\n';
        error(yyexc);
        YYERROR;
      }
#endif // YY_EXCEPTIONS
      YY_SYMBOL_PRINT("-> $$ =", yylhs);
      yypop_(yylen);
      yylen = 0;

      // Shift the result of the reduction.
      yypush_(YY_NULLPTR, YY_MOVE(yylhs));
    }
    goto yynewstate;


  /*--------------------------------------.
  | yyerrlab -- here on detecting error.  |
  `--------------------------------------*/
  yyerrlab:
    // If not already recovering from an error, report this error.
    if (!yyerrstatus_) {
      ++yynerrs_;
      context yyctx(*this, yyla);
      std::string msg = yysyntax_error_(yyctx);
      error(yyla.location, YY_MOVE(msg));
    }


    yyerror_range[1].location = yyla.location;
    if (yyerrstatus_ == 3) {
      /* If just tried and failed to reuse lookahead token after an
         error, discard it.  */

      // Return failure if at end of input.
      if (yyla.kind() == symbol_kind::S_YYEOF)
        YYABORT;
      else if (!yyla.empty()) {
        yy_destroy_("Error: discarding", yyla);
        yyla.clear();
      }
    }

    // Else will try to reuse lookahead token after shifting the error token.
    goto yyerrlab1;


  /*---------------------------------------------------.
  | yyerrorlab -- error raised explicitly by YYERROR.  |
  `---------------------------------------------------*/
  yyerrorlab:
    /* Pacify compilers when the user code never invokes YYERROR and
       the label yyerrorlab therefore never appears in user code.  */
    if (false)
      YYERROR;

    /* Do not reclaim the symbols of the rule whose action triggered
       this YYERROR.  */
    yypop_(yylen);
    yylen = 0;
    YY_STACK_PRINT();
    goto yyerrlab1;


  /*-------------------------------------------------------------.
  | yyerrlab1 -- common code for both syntax error and YYERROR.  |
  `-------------------------------------------------------------*/
  yyerrlab1:
    yyerrstatus_ = 3; // Each real token shifted decrements this.
    // Pop stack until we find a state that shifts the error token.
    for (;;) {
      yyn = yypact_[+yystack_[0].state];
      if (!yy_pact_value_is_default_(yyn)) {
        yyn += symbol_kind::s_y_yerror;
        if (0 <= yyn && yyn <= yylast_ && yycheck_[yyn] == symbol_kind::s_y_yerror) {
          yyn = yytable_[yyn];
          if (0 < yyn)
            break;
        }
      }

      // Pop the current state because it cannot handle the error token.
      if (yystack_.size() == 1)
        YYABORT;

      yyerror_range[1].location = yystack_[0].location;
      yy_destroy_("Error: popping", yystack_[0]);
      yypop_();
      YY_STACK_PRINT();
    }
    {
      stack_symbol_type error_token;

      yyerror_range[2].location = yyla.location;
      YYLLOC_DEFAULT(error_token.location, yyerror_range, 2);

      // Shift the error token.
      error_token.state = state_type(yyn);
      yypush_("Shifting", YY_MOVE(error_token));
    }
    goto yynewstate;


  /*-------------------------------------.
  | yyacceptlab -- YYACCEPT comes here.  |
  `-------------------------------------*/
  yyacceptlab:
    yyresult = 0;
    goto yyreturn;


  /*-----------------------------------.
  | yyabortlab -- YYABORT comes here.  |
  `-----------------------------------*/
  yyabortlab:
    yyresult = 1;
    goto yyreturn;


  /*-----------------------------------------------------.
  | yyreturn -- parsing is finished, return the result.  |
  `-----------------------------------------------------*/
  yyreturn:
    if (!yyla.empty())
      yy_destroy_("Cleanup: discarding lookahead", yyla);

    /* Do not reclaim the symbols of the rule whose action triggered
       this YYABORT or YYACCEPT.  */
    yypop_(yylen);
    YY_STACK_PRINT();
    while (1 < yystack_.size()) {
      yy_destroy_("Cleanup: popping", yystack_[0]);
      yypop_();
    }

    return yyresult;
  }
#if YY_EXCEPTIONS
  catch (...) {
    YYCDEBUG << "Exception caught: cleaning lookahead and stack\n";
    // Do not try to display the values of the reclaimed symbols,
    // as their printers might throw an exception.
    if (!yyla.empty())
      yy_destroy_(YY_NULLPTR, yyla);

    while (1 < yystack_.size()) {
      yy_destroy_(YY_NULLPTR, yystack_[0]);
      yypop_();
    }
    throw;
  }
#endif // YY_EXCEPTIONS
}

void bison_parser_t ::error(const syntax_error& yyexc) {
  error(yyexc.location, yyexc.what());
}

/* Return YYSTR after stripping away unnecessary quotes and
   backslashes, so that it's suitable for yyerror.  The heuristic is
   that double-quoting is unnecessary unless the string contains an
   apostrophe, a comma, or backslash (other than backslash-backslash).
   YYSTR is taken from yytname.  */
std::string bison_parser_t ::yytnamerr_(const char* yystr) {
  if (*yystr == '"') {
    std::string yyr;
    char const* yyp = yystr;

    for (;;)
      switch (*++yyp) {
        case '\'':
        case ',':
          goto do_not_strip_quotes;

        case '\\':
          if (*++yyp != '\\')
            goto do_not_strip_quotes;
          else
            goto append;

        append:
        default:
          yyr += *yyp;
          break;

        case '"':
          return yyr;
      }
  do_not_strip_quotes:;
  }

  return yystr;
}

std::string bison_parser_t ::symbol_name(symbol_kind_type yysymbol) {
  return yytnamerr_(yytname_[yysymbol]);
}


//  BisonParser ::context.
bison_parser_t ::context::context(const bison_parser_t& yyparser, const symbol_type& yyla)
    : yyparser_(yyparser), yyla_(yyla) {}

int bison_parser_t ::context::expected_tokens(symbol_kind_type yyarg[], int yyargn) const {
  // Actual number of expected tokens
  int yycount = 0;

  const int yyn = yypact_[+yyparser_.yystack_[0].state];
  if (!yy_pact_value_is_default_(yyn)) {
    /* Start YYX at -YYN if negative to avoid negative indexes in
       YYCHECK.  In other words, skip the first -YYN actions for
       this state because they are default actions.  */
    const int yyxbegin = yyn < 0 ? -yyn : 0;
    // Stay within bounds of both yycheck and yytname.
    const int yychecklim = yylast_ - yyn + 1;
    const int yyxend = yychecklim < YYNTOKENS ? yychecklim : YYNTOKENS;
    for (int yyx = yyxbegin; yyx < yyxend; ++yyx)
      if (yycheck_[yyx + yyn] == yyx && yyx != symbol_kind::s_y_yerror &&
          !yy_table_value_is_error_(yytable_[yyx + yyn])) {
        if (!yyarg)
          ++yycount;
        else if (yycount == yyargn)
          return 0;
        else
          yyarg[yycount++] = YY_CAST(symbol_kind_type, yyx);
      }
  }

  if (yyarg && yycount == 0 && 0 < yyargn)
    yyarg[0] = symbol_kind::S_YYEMPTY;
  return yycount;
}


int bison_parser_t ::yy_syntax_error_arguments_(const context& yyctx, symbol_kind_type yyarg[],
                                                int yyargn) const {
  /* There are many possibilities here to consider:
     - If this state is a consistent state with a default action, then
       the only way this function was invoked is if the default action
       is an error action.  In that case, don't check for expected
       tokens because there are none.
     - The only way there can be no lookahead present (in yyla) is
       if this state is a consistent state with a default action.
       Thus, detecting the absence of a lookahead is sufficient to
       determine that there is no unexpected or expected token to
       report.  In that case, just report a simple "syntax error".
     - Don't assume there isn't a lookahead just because this state is
       a consistent state with a default action.  There might have
       been a previous inconsistent state, consistent state with a
       non-default action, or user semantic action that manipulated
       yyla.  (However, yyla is currently not documented for users.)
     - Of course, the expected token list depends on states to have
       correct lookahead information, and it depends on the parser not
       to perform extra reductions after fetching a lookahead from the
       scanner and before detecting a syntax error.  Thus, state merging
       (from LALR or IELR) and default reductions corrupt the expected
       token list.  However, the list is correct for canonical LR with
       one exception: it will still contain any token that will not be
       accepted due to an error action in a later state.
  */

  if (!yyctx.lookahead().empty()) {
    if (yyarg)
      yyarg[0] = yyctx.token();
    int yyn = yyctx.expected_tokens(yyarg ? yyarg + 1 : yyarg, yyargn - 1);
    return yyn + 1;
  }
  return 0;
}

// Generate an error message.
std::string bison_parser_t ::yysyntax_error_(const context& yyctx) const {
  // Its maximum.
  enum { YYARGS_MAX = 5 };
  // Arguments of yyformat.
  symbol_kind_type yyarg[YYARGS_MAX];
  int yycount = yy_syntax_error_arguments_(yyctx, yyarg, YYARGS_MAX);

  char const* yyformat = YY_NULLPTR;
  switch (yycount) {
#define YYCASE_(N, S)                                                                              \
  case N:                                                                                          \
    yyformat = S;                                                                                  \
    break
    default: // Avoid compiler warnings.
      YYCASE_(0, YY_("syntax error"));
      YYCASE_(1, YY_("syntax error, unexpected %s"));
      YYCASE_(2, YY_("syntax error, unexpected %s, expecting %s"));
      YYCASE_(3, YY_("syntax error, unexpected %s, expecting %s or %s"));
      YYCASE_(4, YY_("syntax error, unexpected %s, expecting %s or %s or %s"));
      YYCASE_(5, YY_("syntax error, unexpected %s, expecting %s or %s or %s or %s"));
#undef YYCASE_
  }

  std::string yyres;
  // Argument number.
  std::ptrdiff_t yyi = 0;
  for (char const* yyp = yyformat; *yyp; ++yyp)
    if (yyp[0] == '%' && yyp[1] == 's' && yyi < yycount) {
      yyres += symbol_name(yyarg[yyi++]);
      ++yyp;
    } else
      yyres += *yyp;
  return yyres;
}


const signed char bison_parser_t ::yypact_ninf_ = -103;

const signed char bison_parser_t ::yytable_ninf_ = -99;

const short bison_parser_t ::yypact_[] = {
    156,  -22,  -103, -103, -103, -103, -103, -103, 156,  156,  156,  50,   -39,  -103, 174,  174,
    65,   156,  10,   -103, 20,   -103, -103, -103, -103, 1,    230,  192,  -103, 36,   85,   72,
    156,  -7,   53,   21,   37,   -103, -103, 156,  65,   94,   135,  94,   -41,  -103, -103, 94,
    9,    -103, 46,   55,   -103, 265,  74,   156,  62,   88,   93,   -33,  104,  103,  140,  61,
    -49,  -103, 26,   -103, 174,  174,  174,  174,  174,  174,  174,  174,  174,  174,  174,  174,
    174,  174,  174,  174,  174,  174,  94,   -103, -103, 94,   74,   -103, 221,  156,  106,  -103,
    12,   120,  156,  156,  156,  114,  123,  0,    156,  125,  57,   94,   156,  3,    -103, 156,
    -103, -103, 156,  129,  -103, -103, 156,  -103, 156,  -103, -103, -103, 19,   -103, -103, 278,
    326,  326,  350,  302,  278,  -103, 254,  278,  362,  362,  362,  362,  289,  176,  176,  113,
    113,  113,  128,  71,   -103, -103, 141,  144,  -103, 156,  178,  -103, -103, -103, -103, -103,
    -103, 156,  96,   156,  -103, -103, 142,  -103, 147,  151,  -103, 160,  -103, 166,  -103, -103,
    192,  156,  -103, 156,  179,  -103, -103, -103, 181,  -103, -103, -103, -103, -103, -103, -103,
    -103, -103, -103, 187,  -103};

const signed char bison_parser_t ::yydefact_[] = {
    0,  45, 46, 47, 67, 68, 52, 53, 0,  0,  0,  73, 0,  71, 0,  0,  62, 0,  73, 90, 0,  2,
    3,  11, 13, 14, 15, 38, 40, 44, 0,  0,  0,  0,  0,  0,  0,  85, 86, 0,  62, 73, 0,  72,
    0,  83, 84, 73, 0,  45, 0,  73, 21, 20, 60, 0,  0,  61, 0,  85, 0,  58, 0,  72, 0,  97,
    0,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  39,
    43, 0,  0,  50, 0,  0,  0,  4,  0,  0,  0,  0,  0,  0,  0,  0,  0,  80, 0,  0,  0,  0,
    69, 0,  49, 58, 0,  0,  48, 63, 0,  54, 0,  92, 57, 94, 0,  59, 89, 18, 22, 23, 28, 29,
    30, 16, 17, 19, 24, 26, 25, 27, 31, 33, 34, 35, 36, 37, 32, 41, 51, 5,  0,  98, 95, 0,
    0,  8,  9,  88, 87, 55, 10, 0,  0,  0,  81, 82, 0,  56, 0,  0,  65, 0,  99, 0,  93, 96,
    0,  0,  7,  0,  0,  75, 78, 79, 0,  77, 70, 66, 64, 91, 42, 6,  12, 80, 74, 0,  76};

const short bison_parser_t ::yypgoto_[] = {-103, -103, -8,   -28,  -103, 122,  -103, 59, -103,
                                           -24,  -103, 188,  204,  -103, -103, 8,    -1, 42,
                                           -35,  -102, -101, -103, 205,  -103, 116};

const unsigned char bison_parser_t ::yydefgoto_[] = {0,  20, 21, 22, 23, 24, 25, 26, 27,
                                                     28, 29, 56, 57, 30, 48, 62, 43, 162,
                                                     44, 45, 46, 66, 31, 64, 65};

const short bison_parser_t ::yytable_[] = {
    34,  35,  36,  87,  95,  164, 165, 123, 106, 58,  107, 124, 120, 59,  110, 151, 47,  63,  108,
    42,  67,  105, 151, -98, 105, 32,  33,  -98, 106, 49,  68,  101, 2,   3,   4,   5,   6,   38,
    7,   111, 39,  112, 126, 60,  50,  60,  12,  115, 96,  103, 63,  146, 173, 37,  147, 109, 159,
    13,  37,  167, 182, 183, 40,  88,  37,  149, 61,  98,  152, 54,  99,  155, 156, 52,  53,  174,
    160, 38,  16,  17,  39,  51,  38,  19,  125, 39,  100, 89,  38,  90,  154, 39,  117, 182, 183,
    55,  91,  37,  176, 37,  166, 41,  40,  168, 114, 41,  169, 40,  107, 150, 171, 113, 172, 40,
    116, 55,  163, 122, 118, 93,  94,  38,  107, 38,  39,  178, 39,  127, 128, 129, 130, 131, 132,
    134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 181, 40,  119, 40,  191, -95, -95,
    190, 180, 104, 184, 105, 85,  86,  1,   121, 105, 2,   3,   4,   5,   6,   153, 7,   8,   157,
    192, 9,   10,  11,  158, 12,  49,  161, 107, 2,   3,   4,   5,   6,   170, 7,   13,  177, 120,
    37,  185, 50,  179, 12,  49,  133, 14,  2,   3,   4,   5,   6,   186, 7,   13,  15,  187, 16,
    17,  50,  18,  12,  19,  38,  14,  188, 39,  83,  84,  85,  86,  189, 13,  15,  117, 16,  17,
    102, 51,  194, 19,  148, 193, 92,  195, 196, 0,   97,  40,  175, 0,   0,   0,   16,  17,  0,
    51,  0,   19,  0,   118, 69,  70,  71,  72,  73,  0,   74,  75,  0,   0,   0,   0,   76,  77,
    78,  79,  80,  0,   81,  82,  83,  84,  85,  86,  69,  70,  71,  72,  73,  0,   74,  0,   0,
    0,   0,   0,   76,  77,  78,  79,  80,  0,   81,  82,  83,  84,  85,  86,  69,  70,  71,  72,
    73,  81,  82,  83,  84,  85,  86,  0,   76,  77,  78,  79,  80,  0,   81,  82,  83,  84,  85,
    86,  69,  70,  71,  80,  0,   81,  82,  83,  84,  85,  86,  0,   76,  77,  78,  79,  80,  0,
    81,  82,  83,  84,  85,  86,  -99, -99, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   76,
    77,  78,  79,  80,  0,   81,  82,  83,  84,  85,  86,  69,  70,  0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   76,  77,  78,  79,  80,  0,   81,  82,  83,  84,  85,  86,  -99, -99, -99,
    -99, 80,  0,   81,  82,  83,  84,  85,  86};

const short bison_parser_t ::yycheck_[] = {
    8,   9,   10, 27,  32, 107, 107, 56, 43, 17,  51,  60,  45,  3,   5,   3,   55,  18, 59, 11,
    0,   21,  3,  56,  21, 47,  48,  60, 63, 3,   29,  39,  6,   7,   8,   9,   10,  27, 12, 30,
    30,  32,  66, 33,  18, 33,  20,  55, 55, 41,  51,  86,  33,  3,   89,  47,  56,  31, 3,  56,
    162, 162, 52, 27,  3,  93,  56,  14, 56, 4,   49,  99,  100, 14,  15,  56,  104, 27, 52, 53,
    30,  55,  27, 57,  58, 30,  49,  51, 27, 4,   98,  30,  4,   195, 195, 30,  11,  3,  27, 3,
    108, 55,  52, 111, 30, 55,  114, 52, 51, 3,   118, 56,  120, 52,  52,  30,  59,  56, 30, 47,
    48,  27,  51, 27,  30, 153, 30,  68, 69, 70,  71,  72,  73,  74,  75,  76,  77,  78, 79, 80,
    81,  82,  83, 84,  85, 49,  52,  54, 52, 177, 47,  48,  176, 161, 19,  163, 21,  44, 45, 3,
    56,  21,  6,  7,   8,  9,   10,  47, 12, 13,  56,  179, 16,  17,  18,  52,  20,  3,  53, 51,
    6,   7,   8,  9,   10, 56,  12,  31, 47, 45,  3,   49,  18,  15,  20,  3,   74,  41, 6,  7,
    8,   9,   10, 56,  12, 31,  50,  56, 52, 53,  18,  55,  20,  57,  27,  41,  56,  30, 42, 43,
    44,  45,  56, 31,  50, 4,   52,  53, 40, 55,  49,  57,  11,  54,  30,  193, 49,  -1, 33, 52,
    124, -1,  -1, -1,  52, 53,  -1,  55, -1, 57,  -1,  30,  22,  23,  24,  25,  26,  -1, 28, 29,
    -1,  -1,  -1, -1,  34, 35,  36,  37, 38, -1,  40,  41,  42,  43,  44,  45,  22,  23, 24, 25,
    26,  -1,  28, -1,  -1, -1,  -1,  -1, 34, 35,  36,  37,  38,  -1,  40,  41,  42,  43, 44, 45,
    22,  23,  24, 25,  26, 40,  41,  42, 43, 44,  45,  -1,  34,  35,  36,  37,  38,  -1, 40, 41,
    42,  43,  44, 45,  22, 23,  24,  38, -1, 40,  41,  42,  43,  44,  45,  -1,  34,  35, 36, 37,
    38,  -1,  40, 41,  42, 43,  44,  45, 22, 23,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1, -1, -1,
    34,  35,  36, 37,  38, -1,  40,  41, 42, 43,  44,  45,  22,  23,  -1,  -1,  -1,  -1, -1, -1,
    -1,  -1,  -1, -1,  34, 35,  36,  37, 38, -1,  40,  41,  42,  43,  44,  45,  34,  35, 36, 37,
    38,  -1,  40, 41,  42, 43,  44,  45};

const signed char bison_parser_t ::yystos_[] = {
    0,  3,  6,  7,  8,  9,  10, 12, 13, 16, 17, 18, 20, 31, 41, 50, 52, 53, 55, 57, 62, 63,
    64, 65, 66, 67, 68, 69, 70, 71, 74, 83, 47, 48, 63, 63, 63, 3,  27, 30, 52, 55, 76, 77,
    79, 80, 81, 55, 75, 3,  18, 55, 68, 68, 4,  30, 72, 73, 63, 3,  33, 56, 76, 77, 84, 85,
    82, 0,  29, 22, 23, 24, 25, 26, 28, 29, 34, 35, 36, 37, 38, 40, 41, 42, 43, 44, 45, 70,
    27, 51, 4,  11, 73, 47, 48, 64, 55, 83, 14, 49, 49, 63, 72, 76, 19, 21, 79, 51, 59, 76,
    5,  30, 32, 56, 30, 63, 52, 4,  30, 54, 45, 56, 56, 56, 60, 58, 70, 68, 68, 68, 68, 68,
    68, 66, 68, 68, 68, 68, 68, 68, 68, 68, 68, 68, 68, 68, 79, 79, 11, 64, 3,  3,  56, 47,
    63, 64, 64, 56, 52, 56, 64, 53, 78, 59, 80, 81, 63, 56, 63, 63, 56, 63, 63, 33, 56, 85,
    27, 47, 64, 15, 63, 49, 80, 81, 63, 49, 56, 56, 56, 56, 70, 64, 63, 54, 49, 78, 49};

const signed char bison_parser_t ::yyr1_[] = {
    0,  61, 62, 63, 64, 64, 64, 64, 64, 64, 64, 64, 65, 65, 65, 65, 66, 66, 67, 67,
    68, 68, 68, 68, 68, 68, 68, 68, 68, 68, 68, 68, 68, 68, 68, 68, 68, 68, 68, 69,
    69, 70, 70, 70, 70, 71, 71, 71, 71, 71, 71, 71, 71, 71, 71, 71, 71, 71, 71, 71,
    72, 72, 72, 73, 73, 73, 73, 74, 74, 75, 75, 75, 76, 76, 77, 77, 77, 77, 78, 78,
    78, 79, 79, 79, 79, 80, 80, 81, 81, 82, 82, 83, 83, 83, 83, 83, 84, 84, 85, 85};

const signed char bison_parser_t ::yyr2_[] = {
    0, 2, 1, 1, 3, 3, 5, 5, 4, 4, 4, 1, 6, 1, 1, 1, 3, 3, 3, 3, 2, 2, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1, 2, 1, 3, 5, 2, 1, 1, 1, 1, 3, 3,
    2, 3, 1, 1, 3, 4, 4, 3, 2, 3, 1, 1, 0, 2, 4, 3, 4, 1, 1, 2, 4, 0, 1, 0, 5,
    4, 7, 4, 2, 2, 0, 3, 3, 1, 1, 1, 1, 3, 3, 2, 0, 5, 3, 4, 3, 2, 3, 1, 1, 3};


#if YYDEBUG || 1
// YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
// First, the terminals, then, starting at \a YYNTOKENS, nonterminals.
const char* const bison_parser_t ::yytname_[] = {"\"end of file\"",
                                                 "error",
                                                 "\"invalid token\"",
                                                 "ID",
                                                 "STR",
                                                 "IND_STR",
                                                 "INT_LIT",
                                                 "FLOAT_LIT",
                                                 "PATH",
                                                 "HPATH",
                                                 "SPATH",
                                                 "PATH_END",
                                                 "URI",
                                                 "IF",
                                                 "THEN",
                                                 "ELSE",
                                                 "ASSERT",
                                                 "WITH",
                                                 "LET",
                                                 "IN_KW",
                                                 "REC",
                                                 "INHERIT",
                                                 "EQ",
                                                 "NEQ",
                                                 "AND",
                                                 "OR",
                                                 "IMPL",
                                                 "OR_KW",
                                                 "PIPE_FROM",
                                                 "PIPE_INTO",
                                                 "DOLLAR_CURLY",
                                                 "IND_STRING_OPEN",
                                                 "IND_STRING_CLOSE",
                                                 "ELLIPSIS",
                                                 "'<'",
                                                 "'>'",
                                                 "LEQ",
                                                 "GEQ",
                                                 "UPDATE",
                                                 "NOT",
                                                 "'+'",
                                                 "'-'",
                                                 "'*'",
                                                 "'/'",
                                                 "CONCAT",
                                                 "'?'",
                                                 "NEGATE",
                                                 "':'",
                                                 "'@'",
                                                 "';'",
                                                 "'!'",
                                                 "'.'",
                                                 "'\"'",
                                                 "'('",
                                                 "')'",
                                                 "'{'",
                                                 "'}'",
                                                 "'['",
                                                 "']'",
                                                 "'='",
                                                 "','",
                                                 "$accept",
                                                 "start",
                                                 "expr",
                                                 "expr_function",
                                                 "expr_if",
                                                 "expr_pipe_from",
                                                 "expr_pipe_into",
                                                 "expr_op",
                                                 "expr_app",
                                                 "expr_select",
                                                 "expr_simple",
                                                 "string_parts",
                                                 "string_parts_interpolated",
                                                 "path_start",
                                                 "ind_string_parts",
                                                 "binds",
                                                 "binds1",
                                                 "attrs",
                                                 "attrpath",
                                                 "attr",
                                                 "string_attr",
                                                 "list",
                                                 "formal_set",
                                                 "formals",
                                                 "formal",
                                                 YY_NULLPTR};
#endif


#if YYDEBUG
const short bison_parser_t ::yyrline_[] = {
    0,   171, 171, 178, 181, 186, 193, 201, 209, 211, 213, 221, 225, 226, 227, 228, 232,
    233, 237, 238, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255,
    257, 258, 259, 260, 261, 265, 270, 274, 276, 285, 287, 291, 298, 299, 300, 301, 304,
    305, 309, 316, 325, 328, 330, 332, 334, 336, 340, 341, 342, 346, 348, 349, 350, 357,
    380, 393, 394, 395, 399, 400, 404, 408, 418, 435, 442, 443, 455, 459, 460, 467, 468,
    477, 478, 482, 483, 487, 488, 492, 493, 494, 495, 496, 500, 502, 507, 508};

void bison_parser_t ::yy_stack_print_() const {
  *yycdebug_ << "Stack now";
  for (stack_type::const_iterator i = yystack_.begin(), i_end = yystack_.end(); i != i_end; ++i)
    *yycdebug_ << ' ' << int(i->state);
  *yycdebug_ << '\n';
}

void bison_parser_t ::yy_reduce_print_(int yyrule) const {
  int yylno = yyrline_[yyrule];
  int yynrhs = yyr2_[yyrule];
  // Print the symbols being reduced, and their result.
  *yycdebug_ << "Reducing stack by rule " << yyrule - 1 << " (line " << yylno << "):\n";
  // The symbols being reduced.
  for (int yyi = 0; yyi < yynrhs; yyi++)
    YY_SYMBOL_PRINT("   $" << yyi + 1 << " =", yystack_[(yynrhs) - (yyi + 1)]);
}
#endif // YYDEBUG

bison_parser_t ::symbol_kind_type bison_parser_t ::yytranslate_(int t) YY_NOEXCEPT {
  // YYTRANSLATE[TOKEN-NUM] -- symbol_t number corresponding to
  // TOKEN-NUM as returned by yylex.
  static const signed char translate_table[] = {
      0,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
      2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  50, 52, 2,  2,  2,  2,  2,  53, 54, 42, 40, 60, 41,
      51, 43, 2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  47, 49, 34, 59, 35, 45, 48, 2,  2,  2,  2,
      2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  57,
      2,  58, 2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
      2,  2,  2,  2,  2,  2,  2,  2,  55, 2,  56, 2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
      2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
      2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
      2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
      2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
      2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
      2,  2,  2,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
      21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 36, 37, 38, 39, 44, 46};
  // Last valid token kind.
  const int code_max = 294;

  if (t <= 0)
    return symbol_kind::S_YYEOF;
  else if (t <= code_max)
    return static_cast<symbol_kind_type>(translate_table[t]);
  else
    return symbol_kind::S_YYUNDEF;
}

#line 3 "parser.y"
} // namespace parser
} // namespace nix
#line 2599 "parser-tab.cpp"

#line 511 "parser.y"


#include "nix/expr/eval.h"


namespace nix {

expr_t* parse_expr_from_buf(char* text, size_t length, pos_t::origin_t origin,
                            const source_path_t& base_path, Exprs& exprs, symbol_table_t& symbols,
                            const eval_settings_t& settings, pos_table_t& positions,
                            DocCommentMap& doc_comments, const ref<source_accessor_t> root_fs) {
  yyscan_t scanner;
  LexerState lexer_state{
      .positionToDocComment = doc_comments,
      .positions = positions,
      .origin = positions.add_origin(origin, length),
  };
  ParserState state{
      .lexer_state = lexer_state,
      .exprs = exprs,
      .symbols = symbols,
      .positions = positions,
      .base_path = base_path,
      .origin = lexer_state.origin,
      .root_fs = root_fs,
      .settings = settings,
  };

  yylex_init_extra(&lexer_state, &scanner);
  finally_t _destroy([&] { yylex_destroy(scanner); });

  yy_scan_buffer(text, length, scanner);
  Parser parser(scanner, &state);
  parser.parse();

  return state.result;
}


} // namespace nix
#pragma GCC diagnostic pop // end ignored "-Wswitch-enum"
