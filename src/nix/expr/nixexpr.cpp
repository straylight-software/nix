#include "nix/expr/nixexpr.h"

#include <cstdlib>

#include "nix/expr/eval.h"
#include "nix/expr/print.h"
#include "nix/expr/symbol-table.h"
#include "nix/util/string-ostream.h"
#include "nix/util/strings-inline.h"
#include "nix/util/util.h"

namespace nix {

Counter expr_t::nrExprs;

// FIXME: remove, because *symbols* are abstract and do not have a single
//        textual representation; see printIdentifier()
std::ostream& operator<<(std::ostream& str, const SymbolStr& symbol) {
  std::string_view s = symbol;
  return print_identifier(str, s);
}

void expr_t::show(const symbol_table_t& symbols, std::ostream& str) const {
  unreachable();
}

auto expr_t::show_str(const symbol_table_t& symbols) const -> std::string {
  string_ostream_t oss;
  show(symbols, oss);
  return oss.take();
}

void ExprInt::show(const symbol_table_t& symbols, std::ostream& str) const {
  str << v.integer();
}

void ExprFloat::show(const symbol_table_t& symbols, std::ostream& str) const {
  str << v.fpoint();
}

void ExprString::show(const symbol_table_t& symbols, std::ostream& str) const {
  print_literal_string(str, v.string_view());
}

void ExprPath::show(const symbol_table_t& symbols, std::ostream& str) const {
  str << v.pathStrView();
}

void ExprVar::show(const symbol_table_t& symbols, std::ostream& str) const {
  str << symbols[name];
}

void ExprSelect::show(const symbol_table_t& symbols, std::ostream& str) const {
  str << "(";
  e->show(symbols, str);
  str << ")." << show_attr_selection_path(symbols, getAttrPath());
  if (def) {
    str << " or (";
    def->show(symbols, str);
    str << ")";
  }
}

void ExprOpHasAttr::show(const symbol_table_t& symbols, std::ostream& str) const {
  str << "((";
  e->show(symbols, str);
  str << ") ? " << show_attr_selection_path(symbols, attr_path) << ")";
}

void ExprAttrs::showBindings(const symbol_table_t& symbols, std::ostream& str) const {
  typedef const AttrDefs::value_type* attr_t;
  std::vector<attr_t> sorted;
  for (auto& i : *attrs) {
    sorted.push_back(&i);
  }
  std::sort(sorted.begin(), sorted.end(), [&](attr_t a, attr_t b) {
    std::string_view sa = symbols[a->first], sb = symbols[b->first];
    return sa < sb;
  });
  std::vector<symbol_t> inherits;
  // We can use the displacement as a proxy for the order in which the symbols were parsed.
  // The assignment of displacements should be deterministic, so that showBindings is deterministic.
  std::map<Displacement, std::vector<symbol_t>> inheritsFrom;
  for (auto& i : sorted) {
    switch (i->second.kind) {
      case AttrDef::Kind::Plain:
        break;
      case AttrDef::Kind::Inherited:
        inherits.push_back(i->first);
        break;
      case AttrDef::Kind::InheritedFrom: {
        auto& select = dynamic_cast<ExprSelect&>(*i->second.e);
        auto& from = dynamic_cast<ExprInheritFrom&>(*select.e);
        inheritsFrom[from.displ].push_back(i->first);
        break;
      }
    }
  }
  if (!inherits.empty()) {
    str << "inherit";
    for (auto sym : inherits) {
      str << " " << symbols[sym];
    }
    str << "; ";
  }
  for (const auto& [from, syms] : inheritsFrom) {
    str << "inherit (";
    (*inheritFromExprs)[from]->show(symbols, str);
    str << ")";
    for (auto sym : syms) {
      str << " " << symbols[sym];
    }
    str << "; ";
  }
  for (auto& i : sorted) {
    if (i->second.kind == AttrDef::Kind::Plain) {
      str << symbols[i->first] << " = ";
      i->second.e->show(symbols, str);
      str << "; ";
    }
  }
  for (auto& i : *dynamicAttrs) {
    str << "\"${";
    i.nameExpr->show(symbols, str);
    str << "}\" = ";
    i.valueExpr->show(symbols, str);
    str << "; ";
  }
}

void ExprAttrs::show(const symbol_table_t& symbols, std::ostream& str) const {
  if (recursive) {
    str << "rec ";
  }
  str << "{ ";
  showBindings(symbols, str);
  str << "}";
}

void ExprList::show(const symbol_table_t& symbols, std::ostream& str) const {
  str << "[ ";
  for (auto& i : elems) {
    str << "(";
    i->show(symbols, str);
    str << ") ";
  }
  str << "]";
}

void ExprLambda::show(const symbol_table_t& symbols, std::ostream& str) const {
  str << "(";
  if (auto formals = getFormals()) {
    str << "{ ";
    bool first = true;
    // the natural symbol_t ordering is by creation time, which can lead to the
    // same expression being printed in two different ways depending on its
    // context. always use lexicographic ordering to avoid this.
    for (auto& i : formals->lexicographicOrder(symbols)) {
      if (first) {
        first = false;
      } else {
        str << ", ";
      }
      str << symbols[i.name];
      if (i.def) {
        str << " ? ";
        i.def->show(symbols, str);
      }
    }
    if (ellipsis) {
      if (!first) {
        str << ", ";
      }
      str << "...";
    }
    str << " }";
    if (arg) {
      str << " @ ";
    }
  }
  if (arg) {
    str << symbols[arg];
  }
  str << ": ";
  body->show(symbols, str);
  str << ")";
}

void ExprCall::show(const symbol_table_t& symbols, std::ostream& str) const {
  str << '(';
  fun->show(symbols, str);
  for (auto e : *args) {
    str << ' ';
    e->show(symbols, str);
  }
  str << ')';
}

void ExprLet::show(const symbol_table_t& symbols, std::ostream& str) const {
  str << "(let ";
  attrs->showBindings(symbols, str);
  str << "in ";
  body->show(symbols, str);
  str << ")";
}

void ExprWith::show(const symbol_table_t& symbols, std::ostream& str) const {
  str << "(with ";
  attrs->show(symbols, str);
  str << "; ";
  body->show(symbols, str);
  str << ")";
}

void ExprIf::show(const symbol_table_t& symbols, std::ostream& str) const {
  str << "(if ";
  cond->show(symbols, str);
  str << " then ";
  then->show(symbols, str);
  str << " else ";
  else_->show(symbols, str);
  str << ")";
}

void ExprAssert::show(const symbol_table_t& symbols, std::ostream& str) const {
  str << "assert ";
  cond->show(symbols, str);
  str << "; ";
  body->show(symbols, str);
}

void ExprOpNot::show(const symbol_table_t& symbols, std::ostream& str) const {
  str << "(! ";
  e->show(symbols, str);
  str << ")";
}

void ExprConcatStrings::show(const symbol_table_t& symbols, std::ostream& str) const {
  bool first = true;
  str << "(";
  for (auto& i : es) {
    if (first) {
      first = false;
    } else {
      str << " + ";
    }
    i.second->show(symbols, str);
  }
  str << ")";
}

void ExprPos::show(const symbol_table_t& symbols, std::ostream& str) const {
  str << "__curPos";
}

std::string show_attr_selection_path(const symbol_table_t& symbols,
                                     std::span<const AttrName> attr_path) {
  std::string result;
  bool first = true;
  for (auto& i : attr_path) {
    if (!first) {
      result += '.';
    } else {
      first = false;
    }
    if (i.symbol) {
      result += symbols[i.symbol];
    } else {
      result += "\"${";
      result += i.expr->show_str(symbols);
      result += "}\"";
    }
  }
  return result;
}

/* Computing levels/displacements for variables. */

void expr_t::bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) {
  unreachable();
}

void ExprInt::bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) {
  if (es.debugRepl) {
    es.exprEnvs.insert(std::make_pair(this, env));
  }
}

void ExprFloat::bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) {
  if (es.debugRepl) {
    es.exprEnvs.insert(std::make_pair(this, env));
  }
}

void ExprString::bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) {
  if (es.debugRepl) {
    es.exprEnvs.insert(std::make_pair(this, env));
  }
}

void ExprPath::bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) {
  if (es.debugRepl) {
    es.exprEnvs.insert(std::make_pair(this, env));
  }
}

void ExprVar::bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) {
  if (es.debugRepl) {
    es.exprEnvs.insert(std::make_pair(this, env));
  }

  fromWith = nullptr;

  /* Check whether the variable appears in the environment.  If so,
     set its level and displacement. */
  const StaticEnv* curEnv;
  Level level;
  int withLevel = -1;
  for (curEnv = env.get(), level = 0; curEnv; curEnv = curEnv->up.get(), level++) {
    if (curEnv->isWith) {
      if (withLevel == -1) {
        withLevel = level;
      }
    } else {
      auto i = curEnv->find(name);
      if (i != curEnv->vars.end()) {
        this->level = level;
        displ = i->second;
        return;
      }
    }
  }

  /* Otherwise, the variable must be obtained from the nearest
     enclosing `with'.  If there is no `with', then we can issue an
     "undefined variable" error now. */
  if (withLevel == -1) {
    es.error<UndefinedVarError>("undefined variable '%1%'", es.symbols[name])
        .at_pos(pos)
        .debugThrow();
  }
  for (auto* e = env.get(); e && !fromWith; e = e->up.get()) {
    fromWith = e->isWith;
  }
  this->level = withLevel;
}

void ExprInheritFrom::bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) {
  if (es.debugRepl) {
    es.exprEnvs.insert(std::make_pair(this, env));
  }
}

void ExprSelect::bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) {
  if (es.debugRepl) {
    es.exprEnvs.insert(std::make_pair(this, env));
  }

  e->bindVars(es, env);
  if (def) {
    def->bindVars(es, env);
  }
  for (auto& i : getAttrPath()) {
    if (!i.symbol) {
      i.expr->bindVars(es, env);
    }
  }
}

void ExprOpHasAttr::bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) {
  if (es.debugRepl) {
    es.exprEnvs.insert(std::make_pair(this, env));
  }

  e->bindVars(es, env);
  for (auto& i : attr_path) {
    if (!i.symbol) {
      i.expr->bindVars(es, env);
    }
  }
}

std::shared_ptr<const StaticEnv>
ExprAttrs::bindInheritSources(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) {
  if (!inheritFromExprs) {
    return nullptr;
  }

  // the inherit (from) source values are inserted into an env of its own, which
  // does not introduce any variable names.
  // analysis must see an empty env, or an env that contains only entries with
  // otherwise unused names to not interfere with regular names. the parser
  // has already filled all exprs that access this env with appropriate level
  // and displacement, and nothing else is allowed to access it. ideally we'd
  // not even *have* an expr that grabs anything from this env since it's fully
  // invisible, but the evaluator does not allow for this yet.
  auto inner = std::make_shared<StaticEnv>(nullptr, env, 0);
  for (auto from : *inheritFromExprs) {
    from->bindVars(es, env);
  }

  return inner;
}

void ExprAttrs::moveDataToAllocator(std::pmr::polymorphic_allocator<char>& alloc) {
  AttrDefs newAttrs{std::move(*attrs), alloc};
  attrs.emplace(std::move(newAttrs), alloc);
  DynamicAttrDefs newDynamicAttrs{std::move(*dynamicAttrs), alloc};
  dynamicAttrs.emplace(std::move(newDynamicAttrs), alloc);
  if (inheritFromExprs) {
    inheritFromExprs =
        std::make_unique<std::pmr::vector<expr_t*>>(std::move(*inheritFromExprs), alloc);
  }
}

void ExprAttrs::bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) {
  moveDataToAllocator(es.mem.exprs.alloc);

  if (es.debugRepl) {
    es.exprEnvs.insert(std::make_pair(this, env));
  }

  if (recursive) {
    auto new_env = [&]() -> std::shared_ptr<const StaticEnv> {
      auto new_env = std::make_shared<StaticEnv>(nullptr, env, attrs->size());

      Displacement displ = 0;
      for (auto& i : *attrs) {
        new_env->vars.emplace_back(i.first, i.second.displ = displ++);
      }
      return new_env;
    }();

    // No need to sort newEnv since attrs is in sorted order.

    auto inheritFromEnv = bindInheritSources(es, new_env);
    for (auto& i : *attrs) {
      i.second.e->bindVars(es, i.second.chooseByKind(new_env, env, inheritFromEnv));
    }

    for (auto& i : *dynamicAttrs) {
      i.nameExpr->bindVars(es, new_env);
      i.valueExpr->bindVars(es, new_env);
    }
  } else {
    auto inheritFromEnv = bindInheritSources(es, env);

    for (auto& i : *attrs) {
      i.second.e->bindVars(es, i.second.chooseByKind(env, env, inheritFromEnv));
    }

    for (auto& i : *dynamicAttrs) {
      i.nameExpr->bindVars(es, env);
      i.valueExpr->bindVars(es, env);
    }
  }
}

void ExprList::bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) {
  if (es.debugRepl) {
    es.exprEnvs.insert(std::make_pair(this, env));
  }

  for (auto& i : elems) {
    i->bindVars(es, env);
  }
}

void ExprLambda::bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) {
  if (es.debugRepl) {
    es.exprEnvs.insert(std::make_pair(this, env));
  }

  auto new_env = std::make_shared<StaticEnv>(
      nullptr, env, (getFormals() ? getFormals()->formals.size() : 0) + (!arg ? 0 : 1));

  Displacement displ = 0;

  if (arg) {
    new_env->vars.emplace_back(arg, displ++);
  }

  if (auto formals = getFormals()) {
    for (auto& i : formals->formals) {
      new_env->vars.emplace_back(i.name, displ++);
    }

    new_env->sort();

    for (auto& i : formals->formals) {
      if (i.def) {
        i.def->bindVars(es, new_env);
      }
    }
  }

  body->bindVars(es, new_env);
}

void ExprCall::moveDataToAllocator(std::pmr::polymorphic_allocator<char>& alloc) {
  std::pmr::vector<expr_t*> newArgs{std::move(*args), alloc};
  args.emplace(std::move(newArgs), alloc);
}

void ExprCall::bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) {
  moveDataToAllocator(es.mem.exprs.alloc);
  if (es.debugRepl) {
    es.exprEnvs.insert(std::make_pair(this, env));
  }

  fun->bindVars(es, env);
  for (auto e : *args) {
    e->bindVars(es, env);
  }
}

void ExprLet::bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) {
  attrs->moveDataToAllocator(es.mem.exprs.alloc);
  auto new_env = [&]() -> std::shared_ptr<const StaticEnv> {
    auto new_env = std::make_shared<StaticEnv>(nullptr, env, attrs->attrs->size());

    Displacement displ = 0;
    for (auto& i : *attrs->attrs) {
      new_env->vars.emplace_back(i.first, i.second.displ = displ++);
    }
    return new_env;
  }();

  // No need to sort newEnv since attrs->attrs is in sorted order.

  auto inheritFromEnv = attrs->bindInheritSources(es, new_env);
  for (auto& i : *attrs->attrs) {
    i.second.e->bindVars(es, i.second.chooseByKind(new_env, env, inheritFromEnv));
  }

  if (es.debugRepl) {
    es.exprEnvs.insert(std::make_pair(this, new_env));
  }

  body->bindVars(es, new_env);
}

void ExprWith::bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) {
  if (es.debugRepl) {
    es.exprEnvs.insert(std::make_pair(this, env));
  }

  parentWith = nullptr;
  for (auto* e = env.get(); e && !parentWith; e = e->up.get()) {
    parentWith = e->isWith;
  }

  /* Does this `with' have an enclosing `with'?  If so, record its
     level so that `lookupVar' can look up variables in the previous
     `with' if this one doesn't contain the desired attribute. */
  const StaticEnv* curEnv;
  Level level;
  prevWith = 0;
  for (curEnv = env.get(), level = 1; curEnv; curEnv = curEnv->up.get(), level++) {
    if (curEnv->isWith) {
      assert(level <= std::numeric_limits<uint32_t>::max());
      prevWith = level;
      break;
    }
  }

  attrs->bindVars(es, env);
  auto new_env = std::make_shared<StaticEnv>(this, env);
  body->bindVars(es, new_env);
}

void ExprIf::bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) {
  if (es.debugRepl) {
    es.exprEnvs.insert(std::make_pair(this, env));
  }

  cond->bindVars(es, env);
  then->bindVars(es, env);
  else_->bindVars(es, env);
}

void ExprAssert::bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) {
  if (es.debugRepl) {
    es.exprEnvs.insert(std::make_pair(this, env));
  }

  cond->bindVars(es, env);
  body->bindVars(es, env);
}

void ExprOpNot::bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) {
  if (es.debugRepl) {
    es.exprEnvs.insert(std::make_pair(this, env));
  }

  e->bindVars(es, env);
}

void ExprConcatStrings::bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) {
  if (es.debugRepl) {
    es.exprEnvs.insert(std::make_pair(this, env));
  }

  for (auto& i : this->es) {
    i.second->bindVars(es, env);
  }
}

void ExprPos::bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) {
  if (es.debugRepl) {
    es.exprEnvs.insert(std::make_pair(this, env));
  }
}

/* Storing function names. */

void expr_t::setName(symbol_t name) {}

void ExprLambda::setName(symbol_t name) {
  this->name = name;
  body->setName(name);
}

std::string ExprLambda::showNamePos(const eval_state_t& state) const {
  std::string id(name ? concat_strings("'", state.symbols[name], "'") : "anonymous function");
  return fmt("%1% at %2%", id, state.positions[pos]);
}

void ExprLambda::setDocComment(DocComment doc_comment) {
  // RFC 145 specifies that the innermost doc comment wins.
  // See https://github.com/NixOS/rfcs/blob/master/rfcs/0145-doc-strings.md#ambiguous-placement
  if (!this->doc_comment) {
    this->doc_comment = doc_comment;

    // Curried functions are defined by putting a function directly
    // in the body of another function. To render docs for those, we
    // need to propagate the doc comment to the innermost function.
    //
    // If we have our own comment, we've already propagated it, so this
    // belongs in the same conditional.
    body->setDocComment(doc_comment);
  }
}

std::string DocComment::getInnerText(const pos_table_t& positions) const {
  auto beginPos = positions[begin];
  auto endPos = positions[end];
  auto docCommentStr = beginPos.get_snippet_up_to(endPos).value_or("");

  // Strip "/**" and "*/"
  constexpr size_t prefixLen = 3;
  constexpr size_t suffixLen = 2;
  std::string docStr =
      docCommentStr.substr(prefixLen, docCommentStr.size() - prefixLen - suffixLen);
  if (docStr.empty()) {
    return {};
  }
  // Turn the now missing "/**" into indentation
  docStr = "   " + docStr;
  // Strip indentation (for the whole, potentially multi-line string)
  docStr = strip_indentation(docStr);
  return docStr;
}

/* ‘Cursed or’ handling.
 *
 * In parser.y, every use of expr_select in a production must call one of the
 * two below functions.
 *
 * To be removed by https://github.com/NixOS/nix/pull/11121
 */

void ExprCall::resetCursedOr() {
  cursedOrEndPos.reset();
}

void ExprCall::warnIfCursedOr(const symbol_table_t& symbols, const pos_table_t& positions) {
  if (cursedOrEndPos.has_value()) {
    std::string msg =
        "at " + positions[pos].to_string() +
        ": "
        "This expression uses `or` as an identifier in a way that will change in a "
        "future Nix release.\n"
        "Wrap this entire expression in parentheses to preserve its current meaning:\n"
        "    (" +
        positions[pos]
            .get_snippet_up_to(positions[*cursedOrEndPos])
            .value_or("could not read expression") +
        ")\n"
        "Give feedback at https://github.com/NixOS/nix/pull/11121";
    warn(msg);
  }
}

} // namespace nix
