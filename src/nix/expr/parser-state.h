#pragma once
///@file

#include <limits>

#include "nix/expr/eval.h"
#include "nix/expr/static-string-data.h"
#include "nix/expr/value.h"

namespace nix {

/**
 * @note Storing a C-style `char *` and `size_t` allows us to avoid
 * having to define the special members that using string_view here
 * would implicitly delete.
 */
struct StringToken {
  const char* p;
  size_t l;
  bool hasIndentation;

  operator std::string_view() const { return {p, l}; }
};

struct ParserLocation {
  int beginOffset;
  int endOffset;

  // backup to recover from yyless(0)
  int stashedBeginOffset, stashedEndOffset;

  void stash() {
    stashedBeginOffset = beginOffset;
    stashedEndOffset = endOffset;
  }

  void unstash() {
    beginOffset = stashedBeginOffset;
    endOffset = stashedEndOffset;
  }
};

/**
 * This represents a string-like parse that possibly has yet to be constructed.
 *
 * Examples:
 * "foo"
 * ${"foo" + "bar"}
 * "foo.bar"
 * "foo-${a}"
 *
 * Using this type allows us to avoid construction altogether in cases where what we actually need
 * is the string contents. For example in foo."bar.baz", there is no need to construct an AST node
 * for "bar.baz", but we don't know that until we bubble the value up during parsing and see that
 * it's a node in an AttrPath.
 */
class ToBeStringyExpr {
private:
  using raw_t = std::variant<std::monostate, std::string_view, expr_t*>;
  raw_t raw;

public:
  ToBeStringyExpr() = default;

  ToBeStringyExpr(std::string_view v) : raw(v) {}

  ToBeStringyExpr(expr_t* expr) : raw(expr) { assert(expr); }

  /**
   * Visits the expression and invokes an overloaded functor object \ref f.
   * If the underlying expr_t has a dynamic type of ExprString the overload taking std::string_view
   * is invoked.
   *
   * Used to consistently handle simple StringExpr ${"string"} as non-dynamic attributes.
   * @see https://github.com/NixOS/nix/issues/14642
   */
  template <class F>
  void visit(F&& f) {
    std::visit(overloaded{[&](std::string_view str) { f(str); },
                          [&](expr_t* expr) {
                            ExprString* str = dynamic_cast<ExprString*>(expr);
                            if (str)
                              f(str->v.string_view());
                            else
                              f(expr);
                          },
                          [](std::monostate) { unreachable(); }},
               raw);
  }

  /**
   * Get or create an expr_t from either an existing expr_t or from a string.
   * Delays the allocation or an AST node in case the parser only cares about string contents.
   */
  expr_t* toExpr(Exprs& exprs) {
    return std::visit(overloaded{[&](std::string_view str) -> expr_t* {
                                   return exprs.add<ExprString>(exprs.alloc, str);
                                 },
                                 [&](expr_t* expr) { return expr; },
                                 [](std::monostate) -> expr_t* { unreachable(); }},
                      raw);
  }
};

struct LexerState {
  /**
   * Tracks the distance to the last doc comment, in terms of lexer tokens.
   *
   * The lexer sets this to 0 when reading a doc comment, and increments it
   * for every matched rule; see `lexer-helpers.cc`.
   * Whitespace and comment rules decrement the distance, so that they result
   * in a net 0 change in distance.
   */
  int docCommentDistance = std::numeric_limits<int>::max();

  /**
   * The location of the last doc comment.
   *
   * (stashing fields are not used)
   */
  ParserLocation lastDocCommentLoc;

  /**
   * @brief Maps some positions to a DocComment, where the comment is relevant to the location.
   */
  DocCommentMap& positionToDocComment;

  pos_table_t& positions;
  pos_table_t::origin_t origin;

  pos_idx_t at(const ParserLocation& loc);
};

struct ParserState {
  const LexerState& lexer_state;
  Exprs& exprs;
  symbol_table_t& symbols;
  pos_table_t& positions;
  expr_t* result;
  source_path_t base_path;
  pos_table_t::origin_t origin;
  const ref<source_accessor_t> root_fs;
  static constexpr expr_t::AstSymbols s = StaticEvalSymbols::create().exprSymbols;
  const eval_settings_t& settings;

  void dupAttr(const AttrSelectionPath& attr_path, const pos_idx_t pos, const pos_idx_t prevPos);
  void dupAttr(symbol_t attr, const pos_idx_t pos, const pos_idx_t prevPos);
  void addAttr(ExprAttrs* attrs, AttrSelectionPath&& attr_path, const ParserLocation& loc, expr_t* e,
               const ParserLocation& exprLoc);
  void addAttr(ExprAttrs* attrs, AttrSelectionPath& attr_path, const symbol_t& symbol,
               ExprAttrs::AttrDef&& def);
  void validateFormals(FormalsBuilder& formals, pos_idx_t pos = no_pos, symbol_t arg = {});
  expr_t* strip_indentation(const pos_idx_t pos,
                         std::span<std::pair<pos_idx_t, std::variant<expr_t*, StringToken>>> es);
  pos_idx_t at(const ParserLocation& loc);
};

inline void ParserState::dupAttr(const AttrSelectionPath& attr_path, const pos_idx_t pos,
                                 const pos_idx_t prevPos) {
  throw ParseError({.msg = hint_fmt_t("attribute '%1%' already defined at %2%",
                                   show_attr_selection_path(symbols, attr_path), positions[prevPos]),
                    .pos = positions[pos]});
}

inline void ParserState::dupAttr(symbol_t attr, const pos_idx_t pos, const pos_idx_t prevPos) {
  throw ParseError(
      {.msg = hint_fmt_t("attribute '%1%' already defined at %2%", symbols[attr], positions[prevPos]),
       .pos = positions[pos]});
}

inline void ParserState::addAttr(ExprAttrs* attrs, AttrSelectionPath&& attr_path,
                                 const ParserLocation& loc, expr_t* e,
                                 const ParserLocation& exprLoc) {
  AttrSelectionPath::iterator i;
  // All attrpaths have at least one attr
  assert(!attr_path.empty());
  auto pos = at(loc);
  // Checking attrPath validity.
  // ===========================
  for (i = attr_path.begin(); i + 1 < attr_path.end(); i++) {
    ExprAttrs* nested;
    if (i->symbol) {
      ExprAttrs::AttrDefs::iterator j = attrs->attrs->find(i->symbol);
      if (j != attrs->attrs->end()) {
        nested = dynamic_cast<ExprAttrs*>(j->second.e);
        if (!nested) {
          attr_path.erase(i + 1, attr_path.end());
          dupAttr(attr_path, pos, j->second.pos);
        }
      } else {
        nested = exprs.add<ExprAttrs>();
        (*attrs->attrs)[i->symbol] = ExprAttrs::AttrDef(nested, pos);
      }
    } else {
      nested = exprs.add<ExprAttrs>();
      attrs->dynamicAttrs->push_back(ExprAttrs::DynamicAttrDef(i->expr, nested, pos));
    }
    attrs = nested;
  }
  // expr_t insertion.
  // ==========================
  if (i->symbol) {
    addAttr(attrs, attr_path, i->symbol, ExprAttrs::AttrDef(e, pos));
  } else {
    attrs->dynamicAttrs->push_back(ExprAttrs::DynamicAttrDef(i->expr, e, pos));
  }

  auto it = lexer_state.positionToDocComment.find(pos);
  if (it != lexer_state.positionToDocComment.end()) {
    e->setDocComment(it->second);
    lexer_state.positionToDocComment.emplace(at(exprLoc), it->second);
  }
}

/**
 * Precondition: attr_path is used for error messages and should already contain
 * symbol as its last element.
 */
inline void ParserState::addAttr(ExprAttrs* attrs, AttrSelectionPath& attr_path,
                                 const symbol_t& symbol, ExprAttrs::AttrDef&& def) {
  ExprAttrs::AttrDefs::iterator j = attrs->attrs->find(symbol);
  if (j != attrs->attrs->end()) {
    // This attr path is already defined. However, if both
    // e and the expr pointed by the attr path are two attribute sets,
    // we want to merge them.
    // Otherwise, throw an error.
    auto ae = dynamic_cast<ExprAttrs*>(def.e);
    auto jAttrs = dynamic_cast<ExprAttrs*>(j->second.e);

    // N.B. In a world in which we are less bound by our past mistakes, we
    // would also test that jAttrs and ae are not recursive. The effect of
    // not doing so is that any `rec` marker on ae is discarded, and any
    // `rec` marker on jAttrs will apply to the attributes in ae.
    // See https://github.com/NixOS/nix/issues/9020.
    if (jAttrs && ae) {
      if (ae->inheritFromExprs && !jAttrs->inheritFromExprs)
        jAttrs->inheritFromExprs = std::make_unique<std::pmr::vector<expr_t*>>();
      for (auto& ad : *ae->attrs) {
        if (ad.second.kind == ExprAttrs::AttrDef::Kind::InheritedFrom) {
          auto& sel = dynamic_cast<ExprSelect&>(*ad.second.e);
          auto& from = dynamic_cast<ExprInheritFrom&>(*sel.e);
          from.displ += jAttrs->inheritFromExprs->size();
        }
        attr_path.emplace_back(AttrName(ad.first));
        addAttr(jAttrs, attr_path, ad.first, std::move(ad.second));
        attr_path.pop_back();
      }
      ae->attrs->clear();
      jAttrs->dynamicAttrs->insert(jAttrs->dynamicAttrs->end(),
                                   std::make_move_iterator(ae->dynamicAttrs->begin()),
                                   std::make_move_iterator(ae->dynamicAttrs->end()));
      ae->dynamicAttrs->clear();
      if (ae->inheritFromExprs) {
        jAttrs->inheritFromExprs->insert(jAttrs->inheritFromExprs->end(),
                                         std::make_move_iterator(ae->inheritFromExprs->begin()),
                                         std::make_move_iterator(ae->inheritFromExprs->end()));
        ae->inheritFromExprs = nullptr;
      }
    } else {
      dupAttr(attr_path, def.pos, j->second.pos);
    }
  } else {
    // This attr path is not defined. Let's create it.
    attrs->attrs->emplace(symbol, def);
    def.e->setName(symbol);
  }
}

inline void ParserState::validateFormals(FormalsBuilder& formals, pos_idx_t pos, symbol_t arg) {
  std::sort(formals.formals.begin(), formals.formals.end(), [](const auto& a, const auto& b) {
    return std::tie(a.name, a.pos) < std::tie(b.name, b.pos);
  });

  std::optional<std::pair<symbol_t, pos_idx_t>> duplicate;
  for (size_t i = 0; i + 1 < formals.formals.size(); i++) {
    if (formals.formals[i].name != formals.formals[i + 1].name)
      continue;
    std::pair thisDup{formals.formals[i].name, formals.formals[i + 1].pos};
    duplicate = std::min(thisDup, duplicate.value_or(thisDup));
  }
  if (duplicate)
    throw ParseError(
        {.msg = hint_fmt_t("duplicate formal function argument '%1%'", symbols[duplicate->first]),
         .pos = positions[duplicate->second]});

  if (arg && formals.has(arg))
    throw ParseError({.msg = hint_fmt_t("duplicate formal function argument '%1%'", symbols[arg]),
                      .pos = positions[pos]});
}

inline expr_t*
ParserState::strip_indentation(const pos_idx_t pos,
                              std::span<std::pair<pos_idx_t, std::variant<expr_t*, StringToken>>> es) {
  if (es.empty())
    return exprs.add<ExprString>(""_sds);

  /* Figure out the minimum indentation.  Note that by design
     whitespace-only final lines are not taken into account.  (So
     the " " in "\n ''" is ignored, but the " " in "\n foo''" is.) */
  bool at_start_of_line = true; /* = seen only whitespace in the current line */
  size_t min_indent = 1000000;
  size_t cur_indent = 0;
  for (auto& [i_pos, i] : es) {
    auto* str = std::get_if<StringToken>(&i);
    if (!str || !str->hasIndentation) {
      /* Anti-quotations and escaped characters end the current start-of-line whitespace. */
      if (at_start_of_line) {
        at_start_of_line = false;
        if (cur_indent < min_indent)
          min_indent = cur_indent;
      }
      continue;
    }
    for (size_t j = 0; j < str->l; ++j) {
      if (at_start_of_line) {
        if (str->p[j] == ' ')
          cur_indent++;
        else if (str->p[j] == '\n') {
          /* Empty line, doesn't influence minimum
             indentation. */
          cur_indent = 0;
        } else {
          at_start_of_line = false;
          if (cur_indent < min_indent)
            min_indent = cur_indent;
        }
      } else if (str->p[j] == '\n') {
        at_start_of_line = true;
        cur_indent = 0;
      }
    }
  }

  /* Strip spaces from each line. */
  std::vector<std::pair<pos_idx_t, expr_t*>> es2{};
  at_start_of_line = true;
  size_t curDropped = 0;
  size_t n = es.size();
  auto i = es.begin();
  const auto trimExpr = [&](expr_t* e) {
    at_start_of_line = false;
    curDropped = 0;
    es2.emplace_back(i->first, e);
  };
  const auto trimString = [&](const StringToken& t) {
    std::string s2;
    for (size_t j = 0; j < t.l; ++j) {
      if (at_start_of_line) {
        if (t.p[j] == ' ') {
          if (curDropped++ >= min_indent)
            s2 += t.p[j];
        } else if (t.p[j] == '\n') {
          curDropped = 0;
          s2 += t.p[j];
        } else {
          at_start_of_line = false;
          curDropped = 0;
          s2 += t.p[j];
        }
      } else {
        s2 += t.p[j];
        if (t.p[j] == '\n')
          at_start_of_line = true;
      }
    }

    /* Remove the last line if it is empty and consists only of
       spaces. */
    if (n == 1) {
      std::string::size_type p = s2.find_last_of('\n');
      if (p != std::string::npos && s2.find_first_not_of(' ', p + 1) == std::string::npos)
        s2 = std::string(s2, 0, p + 1);
    }

    // Ignore empty strings for a minor optimisation and AST simplification
    if (s2 != "") {
      es2.emplace_back(i->first, exprs.add<ExprString>(exprs.alloc, s2));
    }
  };
  for (; i != es.end(); ++i, --n) {
    std::visit(overloaded{trimExpr, trimString}, i->second);
  }

  // If there is nothing at all, return the empty string directly.
  // This also ensures that equivalent empty strings result in the same ast, which is helpful when
  // testing formatters.
  if (es2.size() == 0) {
    auto* const result = exprs.add<ExprString>(""_sds);
    return result;
  }

  /* If this is a single string, then don't do a concatenation. */
  if (es2.size() == 1 && dynamic_cast<ExprString*>((es2)[0].second)) {
    auto* const result = (es2)[0].second;
    return result;
  }
  return exprs.add<ExprConcatStrings>(exprs.alloc, pos, true, es2);
}

inline pos_idx_t LexerState::at(const ParserLocation& loc) {
  return positions.add(origin, loc.beginOffset);
}

inline pos_idx_t ParserState::at(const ParserLocation& loc) {
  return positions.add(origin, loc.beginOffset);
}

} // namespace nix
