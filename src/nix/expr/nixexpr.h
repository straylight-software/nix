#pragma once
///@file

#include <algorithm>
#include <map>
#include <memory>
#include <memory_resource>
#include <span>
#include <vector>

#include "nix/expr/counter.h"
#include "nix/expr/eval-error.h"
#include "nix/expr/static-string-data.h"
#include "nix/expr/symbol-table.h"
#include "nix/expr/value.h"
#include "nix/util/error.h"
#include "nix/util/pos-idx.h"
#include "nix/util/pos-table.h"

namespace nix {

class eval_state_t;
class pos_table_t;
struct Env;
struct ExprWith;
struct StaticEnv;
struct value_t;

/**
 * A documentation comment, in the sense of [RFC
 * 145](https://github.com/NixOS/rfcs/blob/master/rfcs/0145-doc-strings.md)
 *
 * Note that this does not implement the following:
 *  - argument attribute names ("formals"): TBD
 *  - argument names: these are internal to the function and their names may not be optimal for
 * documentation
 *  - function arity (degree of currying or number of ':'s):
 *      - Functions returning partially applied functions have a higher arity
 *        than can be determined locally and without evaluation.
 *        We do not want to present false data.
 *      - Some functions should be thought of as transformations of other
 *        functions. For instance `overlay -> overlay -> overlay` is the simplest
 *        way to understand `composeExtensions`, but its implementation looks like
 *        `f: g: final: prev: <...>`. The parameters `final` and `prev` are part
 *        of the overlay concept, while distracting from the function's purpose.
 */
struct DocComment {
  /**
   * Start of the comment, including the opening, ie `/` and `**`.
   */
  pos_idx_t begin;

  /**
   * Position right after the final asterisk and `/` that terminate the comment.
   */
  pos_idx_t end;

  /**
   * Whether the comment is set.
   *
   * A `DocComment` is small enough that it makes sense to pass by value, and
   * therefore baking optionality into it is also useful, to avoiding the memory
   * overhead of `std::optional`.
   */
  operator bool() const { return static_cast<bool>(begin); }

  std::string getInnerText(const pos_table_t& positions) const;
};

/**
 * An attribute path is a sequence of attribute names.
 */
struct AttrName {
  symbol_t symbol;
  expr_t* expr = nullptr;
  AttrName(symbol_t s) : symbol(s) {};
  AttrName(expr_t* e) : expr(e) {};
};

static_assert(std::is_trivially_copy_constructible_v<AttrName>);

using AttrSelectionPath = std::vector<AttrName>;

std::string show_attr_selection_path(const symbol_table_t& symbols,
                                     std::span<const AttrName> attr_path);

/* Abstract syntax of Nix expressions. */

struct expr_t {
  struct AstSymbols {
    symbol_t sub, lessThan, mul, div, or_, findFile, nixPath, body;
  };

  static Counter nrExprs;

  expr_t() { nrExprs++; }

  virtual ~expr_t() {};
  virtual void show(const symbol_table_t& symbols, std::ostream& str) const;
  // Returns the expression as a string (avoids need for ostringstream at call sites)
  [[nodiscard]] auto show_str(const symbol_table_t& symbols) const -> std::string;
  virtual void bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env);

  /** normal evaluation, implemented directly by all subclasses. */
  virtual void eval(eval_state_t& state, Env& env, value_t& v);

  /**
   * Create a thunk for the delayed computation of the given expression
   * in the given environment. But if the expression is a variable,
   * then look it up right away. This significantly reduces the number
   * of thunks allocated.
   */
  virtual value_t* maybeThunk(eval_state_t& state, Env& env);
  virtual void setName(symbol_t name);
  virtual void setDocComment(DocComment doc_comment) {};

  virtual pos_idx_t getPos() const { return no_pos; }

  // These are temporary methods to be used only in parser.y
  virtual void resetCursedOr() {};
  virtual void warnIfCursedOr(const symbol_table_t& symbols, const pos_table_t& positions) {};
};

#define COMMON_METHODS                                                                             \
  void show(const symbol_table_t& symbols, std::ostream& str) const override;                      \
  void eval(eval_state_t& state, Env& env, value_t& v) override;                                   \
  void bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) override;

struct ExprInt : expr_t {
  value_t v;

  ExprInt(NixInt n) { v.mkInt(n); };

  ExprInt(NixInt::Inner n) { v.mkInt(n); };

  value_t* maybeThunk(eval_state_t& state, Env& env) override;
  COMMON_METHODS
};

struct ExprFloat : expr_t {
  value_t v;

  ExprFloat(NixFloat nf) { v.mkFloat(nf); };

  value_t* maybeThunk(eval_state_t& state, Env& env) override;
  COMMON_METHODS
};

struct ExprString : expr_t {
  value_t v;

  /**
   * This is only for strings already allocated in our polymorphic allocator,
   * or that live at least that long (e.g. c++ string literals)
   */
  ExprString(const StringData& s) { v.mkStringNoCopy(s); };

  ExprString(std::pmr::polymorphic_allocator<char>& alloc, std::string_view sv) {
    if (sv.size() == 0) {
      v.mkStringNoCopy(""_sds);
      return;
    }
    v.mkStringNoCopy(StringData::make(*alloc.resource(), sv));
  };

  value_t* maybeThunk(eval_state_t& state, Env& env) override;
  COMMON_METHODS
};

struct ExprPath : expr_t {
  ref<source_accessor_t> accessor;
  value_t v;

  ExprPath(std::pmr::polymorphic_allocator<char>& alloc, ref<source_accessor_t> accessor,
           std::string_view sv)
      : accessor(accessor) {
    v.mkPath(&*accessor, StringData::make(*alloc.resource(), sv));
  }

  value_t* maybeThunk(eval_state_t& state, Env& env) override;
  COMMON_METHODS
};

using Level = uint32_t;
using Displacement = uint32_t;

struct ExprVar : expr_t {
  pos_idx_t pos;
  symbol_t name;

  /* Whether the variable comes from an environment (e.g. a rec, let
     or function argument) or from a "with".

     `nullptr`: Not from a `with`.
     Valid pointer: the nearest, innermost `with` expression to query first. */
  ExprWith* fromWith = nullptr;

  /* In the former case, the value is obtained by going `level`
     levels up from the current environment and getting the
     `displ`th value in that environment.  In the latter case, the
     value is obtained by getting the attribute named `name` from
     the set stored in the environment that is `level` levels up
     from the current one.*/
  Level level = 0;
  Displacement displ = 0;

  ExprVar(symbol_t name) : name(name) {};
  ExprVar(const pos_idx_t& pos, symbol_t name) : pos(pos), name(name) {};
  value_t* maybeThunk(eval_state_t& state, Env& env) override;

  pos_idx_t getPos() const override { return pos; }

  COMMON_METHODS
};

/**
 * A pseudo-expression for the purpose of evaluating the `from` expression in `inherit (from)`
 * syntax. Unlike normal variable references, the displacement is set during parsing, and always
 * refers to `ExprAttrs::inheritFromExprs` (by itself or in `ExprLet`), whose values are put into
 * their own `Env`.
 */
struct ExprInheritFrom : ExprVar {
  ExprInheritFrom(pos_idx_t pos, Displacement displ) : ExprVar(pos, {}) {
    this->level = 0;
    this->displ = displ;
    this->fromWith = nullptr;
  }

  void bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) override;
};

struct ExprSelect : expr_t {
  pos_idx_t pos;
  uint32_t nAttrPath;
  expr_t *e, *def;
  AttrName* attrPathStart;

  ExprSelect(std::pmr::polymorphic_allocator<char>& alloc, const pos_idx_t& pos, expr_t* e,
             std::span<const AttrName> attr_path, expr_t* def)
      : pos(pos),
        nAttrPath(attr_path.size()),
        e(e),
        def(def),
        attrPathStart(alloc.allocate_object<AttrName>(nAttrPath)) {
    std::ranges::copy(attr_path, attrPathStart);
  };

  ExprSelect(std::pmr::polymorphic_allocator<char>& alloc, const pos_idx_t& pos, expr_t* e,
             symbol_t name)
      : pos(pos), nAttrPath(1), e(e), def(0), attrPathStart((alloc.allocate_object<AttrName>())) {
    *attrPathStart = AttrName(name);
  };

  pos_idx_t getPos() const override { return pos; }

  std::span<const AttrName> getAttrPath() const { return {attrPathStart, nAttrPath}; }

  /**
   * Evaluate the `a.b.c` part of `a.b.c.d`. This exists mostly for the purpose of :doc in the repl.
   *
   * @param[out] attrs The attribute set that should contain the last attribute name (if it exists).
   * @return The last attribute name in `attr_path`
   *
   * @note This does *not* evaluate the final attribute, and does not fail if that's the only
   * attribute that does not exist.
   */
  symbol_t evalExceptFinalSelect(eval_state_t& state, Env& env, value_t& attrs);

  COMMON_METHODS
};

struct ExprOpHasAttr : expr_t {
  expr_t* e;
  std::span<AttrName> attr_path;

  ExprOpHasAttr(std::pmr::polymorphic_allocator<char>& alloc, expr_t* e,
                std::span<AttrName> attr_path)
      : e(e), attr_path({alloc.allocate_object<AttrName>(attr_path.size()), attr_path.size()}) {
    std::ranges::copy(attr_path, this->attr_path.begin());
  };

  pos_idx_t getPos() const override { return e->getPos(); }

  COMMON_METHODS
};

struct ExprAttrs : expr_t {
  bool recursive;
  pos_idx_t pos;

  struct AttrDef {
    enum class Kind {
      /** `attr = expr;` */
      Plain,
      /** `inherit attr1 attrn;` */
      Inherited,
      /** `inherit (expr) attr1 attrn;` */
      InheritedFrom,
    };

    Kind kind;
    expr_t* e;
    pos_idx_t pos;
    Displacement displ = 0; // displacement
    AttrDef(expr_t* e, const pos_idx_t& pos, Kind kind = Kind::Plain)
        : kind(kind), e(e), pos(pos) {};
    AttrDef() {};

    template <typename T>
    const T& chooseByKind(const T& plain, const T& inherited, const T& inheritedFrom) const {
      switch (kind) {
        case Kind::Plain:
          return plain;
        case Kind::Inherited:
          return inherited;
        default:
        case Kind::InheritedFrom:
          return inheritedFrom;
      }
    }
  };

  typedef std::pmr::map<symbol_t, AttrDef> AttrDefs;
  /**
   * attrs will never be null. we use std::optional so that we can call emplace() to re-initialize
   * the value with a new pmr::map using a different allocator (move assignment will copy into the
   * old allocator)
   */
  std::optional<AttrDefs> attrs;
  std::unique_ptr<std::pmr::vector<expr_t*>> inheritFromExprs;

  struct DynamicAttrDef {
    expr_t *nameExpr, *valueExpr;
    pos_idx_t pos;
    DynamicAttrDef(expr_t* nameExpr, expr_t* valueExpr, const pos_idx_t& pos)
        : nameExpr(nameExpr), valueExpr(valueExpr), pos(pos) {};
  };

  typedef std::pmr::vector<DynamicAttrDef> DynamicAttrDefs;
  /**
   * dynamicAttrs will never be null. See comment on AttrDefs above.
   */
  std::optional<DynamicAttrDefs> dynamicAttrs;
  ExprAttrs(const pos_idx_t& pos)
      : recursive(false), pos(pos), attrs(AttrDefs{}), dynamicAttrs(DynamicAttrDefs{}) {};
  ExprAttrs() : recursive(false), attrs(AttrDefs{}), dynamicAttrs(DynamicAttrDefs{}) {};

  pos_idx_t getPos() const override { return pos; }

  COMMON_METHODS

  std::shared_ptr<const StaticEnv> bindInheritSources(eval_state_t& es,
                                                      const std::shared_ptr<const StaticEnv>& env);
  Env* buildInheritFromEnv(eval_state_t& state, Env& up);
  void showBindings(const symbol_table_t& symbols, std::ostream& str) const;
  void moveDataToAllocator(std::pmr::polymorphic_allocator<char>& alloc);
};

struct ExprList : expr_t {
  std::span<expr_t*> elems;

  ExprList(std::pmr::polymorphic_allocator<char>& alloc, std::span<expr_t*> exprs)
      : elems({alloc.allocate_object<expr_t*>(exprs.size()), exprs.size()}) {
    std::ranges::copy(exprs, elems.begin());
  };

  COMMON_METHODS
  value_t* maybeThunk(eval_state_t& state, Env& env) override;

  pos_idx_t getPos() const override { return elems.empty() ? no_pos : elems.front()->getPos(); }
};

struct Formal {
  pos_idx_t pos;
  symbol_t name;
  expr_t* def;
};

struct FormalsBuilder {
  typedef std::vector<Formal> Formals_;
  /**
   * @pre Sorted according to predicate (std::tie(a.name, a.pos) < std::tie(b.name, b.pos)).
   */
  Formals_ formals;
  bool ellipsis;

  bool has(symbol_t arg) const {
    auto it = std::lower_bound(formals.begin(), formals.end(), arg,
                               [](const Formal& f, const symbol_t& sym) { return f.name < sym; });
    return it != formals.end() && it->name == arg;
  }
};

struct Formals {
  std::span<Formal> formals;
  bool ellipsis;

  Formals(std::span<Formal> formals, bool ellipsis) : formals(formals), ellipsis(ellipsis) {};

  bool has(symbol_t arg) const {
    auto it = std::lower_bound(formals.begin(), formals.end(), arg,
                               [](const Formal& f, const symbol_t& sym) { return f.name < sym; });
    return it != formals.end() && it->name == arg;
  }

  std::vector<Formal> lexicographicOrder(const symbol_table_t& symbols) const {
    std::vector<Formal> result(formals.begin(), formals.end());
    std::sort(result.begin(), result.end(), [&](const Formal& a, const Formal& b) {
      std::string_view sa = symbols[a.name], sb = symbols[b.name];
      return sa < sb;
    });
    return result;
  }
};

struct ExprLambda : expr_t {
  pos_idx_t pos;
  symbol_t name;
  symbol_t arg;

private:
  bool hasFormals;
  bool ellipsis;
  uint16_t nFormals;
  Formal* formalsStart;

public:
  std::optional<Formals> getFormals() const {
    if (hasFormals)
      return Formals{{formalsStart, nFormals}, ellipsis};
    else
      return std::nullopt;
  }

  expr_t* body;
  DocComment doc_comment;

  ExprLambda(const pos_table_t& positions, std::pmr::polymorphic_allocator<char>& alloc,
             pos_idx_t pos, symbol_t arg, const FormalsBuilder& formals, expr_t* body)
      : pos(pos),
        arg(arg),
        hasFormals(true),
        ellipsis(formals.ellipsis),
        nFormals(formals.formals.size()),
        formalsStart(alloc.allocate_object<Formal>(nFormals)),
        body(body) {
    if (formals.formals.size() > nFormals) [[unlikely]] {
      auto err = Error("too many formal arguments, implementation supports at most %1%",
                       std::numeric_limits<decltype(nFormals)>::max());
      if (pos)
        err.at_pos(positions[pos]);
      throw err;
    }
    std::uninitialized_copy_n(formals.formals.begin(), nFormals, formalsStart);
  };

  ExprLambda(pos_idx_t pos, symbol_t arg, expr_t* body)
      : pos(pos),
        arg(arg),
        hasFormals(false),
        ellipsis(false),
        nFormals(0),
        formalsStart(nullptr),
        body(body) {};

  ExprLambda(const pos_table_t& positions, std::pmr::polymorphic_allocator<char>& alloc,
             pos_idx_t pos, const FormalsBuilder& formals, expr_t* body)
      : ExprLambda(positions, alloc, pos, symbol_t(), formals, body) {};

  void setName(symbol_t name) override;
  std::string showNamePos(const eval_state_t& state) const;

  pos_idx_t getPos() const override { return pos; }

  virtual void setDocComment(DocComment doc_comment) override;
  COMMON_METHODS
};

struct ExprCall : expr_t {
  expr_t* fun;
  /**
   * args will never be null. See comment on ExprAttrs::AttrDefs below.
   */
  std::optional<std::pmr::vector<expr_t*>> args;
  pos_idx_t pos;
  std::optional<pos_idx_t>
      cursedOrEndPos; // used during parsing to warn about https://github.com/NixOS/nix/issues/11118

  ExprCall(const pos_idx_t& pos, expr_t* fun, std::pmr::vector<expr_t*>&& args)
      : fun(fun), args(args), pos(pos), cursedOrEndPos({}) {}

  ExprCall(const pos_idx_t& pos, expr_t* fun, std::pmr::vector<expr_t*>&& args,
           pos_idx_t&& cursedOrEndPos)
      : fun(fun), args(args), pos(pos), cursedOrEndPos(cursedOrEndPos) {}

  pos_idx_t getPos() const override { return pos; }

  virtual void resetCursedOr() override;
  virtual void warnIfCursedOr(const symbol_table_t& symbols, const pos_table_t& positions) override;
  void moveDataToAllocator(std::pmr::polymorphic_allocator<char>& alloc);
  COMMON_METHODS
};

struct ExprLet : expr_t {
  ExprAttrs* attrs;
  expr_t* body;
  ExprLet(ExprAttrs* attrs, expr_t* body) : attrs(attrs), body(body) {};
  COMMON_METHODS
};

struct ExprWith : expr_t {
  pos_idx_t pos;
  uint32_t prevWith;
  expr_t *attrs, *body;
  ExprWith* parentWith;
  ExprWith(const pos_idx_t& pos, expr_t* attrs, expr_t* body)
      : pos(pos), attrs(attrs), body(body) {};

  pos_idx_t getPos() const override { return pos; }

  COMMON_METHODS
};

struct ExprIf : expr_t {
  pos_idx_t pos;
  expr_t *cond, *then, *else_;
  ExprIf(const pos_idx_t& pos, expr_t* cond, expr_t* then, expr_t* else_)
      : pos(pos), cond(cond), then(then), else_(else_) {};

  pos_idx_t getPos() const override { return pos; }

  COMMON_METHODS
};

struct ExprAssert : expr_t {
  pos_idx_t pos;
  expr_t *cond, *body;
  ExprAssert(const pos_idx_t& pos, expr_t* cond, expr_t* body)
      : pos(pos), cond(cond), body(body) {};

  pos_idx_t getPos() const override { return pos; }

  COMMON_METHODS
};

struct ExprOpNot : expr_t {
  expr_t* e;
  ExprOpNot(expr_t* e) : e(e) {};

  pos_idx_t getPos() const override { return e->getPos(); }

  COMMON_METHODS
};

#define MakeBinOpMembers(name, s)                                                                  \
  pos_idx_t pos;                                                                                   \
  expr_t *e1, *e2;                                                                                 \
  name(expr_t* e1, expr_t* e2) : e1(e1), e2(e2){};                                                 \
  name(const pos_idx_t& pos, expr_t* e1, expr_t* e2) : pos(pos), e1(e1), e2(e2){};                 \
  void show(const symbol_table_t& symbols, std::ostream& str) const override {                     \
    str << "(";                                                                                    \
    e1->show(symbols, str);                                                                        \
    str << " " s " ";                                                                              \
    e2->show(symbols, str);                                                                        \
    str << ")";                                                                                    \
  }                                                                                                \
  void bindVars(eval_state_t& es, const std::shared_ptr<const StaticEnv>& env) override {          \
    e1->bindVars(es, env);                                                                         \
    e2->bindVars(es, env);                                                                         \
  }                                                                                                \
  void eval(eval_state_t& state, Env& env, value_t& v) override;                                   \
  pos_idx_t getPos() const override {                                                              \
    return pos;                                                                                    \
  }

#define MakeBinOp(name, s)                                                                         \
  struct name : expr_t {                                                                           \
    MakeBinOpMembers(name, s)                                                                      \
  };

MakeBinOp(ExprOpEq, "==");
MakeBinOp(ExprOpNEq, "!=");
MakeBinOp(ExprOpAnd, "&&");
MakeBinOp(ExprOpOr, "||");
MakeBinOp(ExprOpImpl, "->");
MakeBinOp(ExprOpConcatLists, "++");

struct ExprOpUpdate : expr_t {
  MakeBinOpMembers(ExprOpUpdate, "//")
};

struct ExprConcatStrings : expr_t {
  pos_idx_t pos;
  bool forceString;
  std::span<std::pair<pos_idx_t, expr_t*>> es;

  ExprConcatStrings(std::pmr::polymorphic_allocator<char>& alloc, const pos_idx_t& pos,
                    bool forceString, std::span<std::pair<pos_idx_t, expr_t*>> es)
      : pos(pos),
        forceString(forceString),
        es({alloc.allocate_object<std::pair<pos_idx_t, expr_t*>>(es.size()), es.size()}) {
    std::ranges::copy(es, this->es.begin());
  };

  ExprConcatStrings(std::pmr::polymorphic_allocator<char>& alloc, const pos_idx_t& pos,
                    bool forceString, std::initializer_list<std::pair<pos_idx_t, expr_t*>> es)
      : pos(pos),
        forceString(forceString),
        es({alloc.allocate_object<std::pair<pos_idx_t, expr_t*>>(es.size()), es.size()}) {
    std::ranges::copy(es, this->es.begin());
  };

  pos_idx_t getPos() const override { return pos; }

  COMMON_METHODS
};

struct ExprPos : expr_t {
  pos_idx_t pos;
  ExprPos(const pos_idx_t& pos) : pos(pos) {};

  pos_idx_t getPos() const override { return pos; }

  COMMON_METHODS
};

class Exprs {
  // FIXME: use std::pmr::monotonic_buffer_resource when parallel
  // eval is disabled?
  std::pmr::synchronized_pool_resource buffer;

public:
  std::pmr::polymorphic_allocator<char> alloc{&buffer};

  template <class C>
  [[gnu::always_inline]]
  C* add(auto&&... args) {
    return alloc.new_object<C>(std::forward<decltype(args)>(args)...);
  }

  // we define some calls to add explicitly so that the argument can be passed in as initializer
  // lists
  template <class C>
  [[gnu::always_inline]]
  C* add(const pos_idx_t& pos, expr_t* fun, std::pmr::vector<expr_t*>&& args)
    requires(std::same_as<C, ExprCall>)
  {
    return alloc.new_object<C>(pos, fun, std::move(args));
  }

  template <class C>
  [[gnu::always_inline]]
  C* add(const pos_idx_t& pos, expr_t* fun, std::pmr::vector<expr_t*>&& args,
         pos_idx_t&& cursedOrEndPos)
    requires(std::same_as<C, ExprCall>)
  {
    return alloc.new_object<C>(pos, fun, std::move(args), std::move(cursedOrEndPos));
  }

  template <class C>
  [[gnu::always_inline]]
  C* add(std::pmr::polymorphic_allocator<char>& alloc, const pos_idx_t& pos, bool forceString,
         std::span<std::pair<pos_idx_t, expr_t*>> es)
    requires(std::same_as<C, ExprConcatStrings>)
  {
    return alloc.new_object<C>(alloc, pos, forceString, es);
  }

  template <class C>
  [[gnu::always_inline]]
  C* add(std::pmr::polymorphic_allocator<char>& alloc, const pos_idx_t& pos, bool forceString,
         std::initializer_list<std::pair<pos_idx_t, expr_t*>> es)
    requires(std::same_as<C, ExprConcatStrings>)
  {
    return alloc.new_object<C>(alloc, pos, forceString, es);
  }
};

/* Static environments are used to map variable names onto (level,
   displacement) pairs used to obtain the value of the variable at
   runtime. */
struct StaticEnv {
  ExprWith* isWith;
  std::shared_ptr<const StaticEnv> up;

  // Note: these must be in sorted order.
  typedef std::vector<std::pair<symbol_t, Displacement>> Vars;
  Vars vars;

  StaticEnv(ExprWith* isWith, std::shared_ptr<const StaticEnv> up, size_t expectedSize = 0)
      : isWith(isWith), up(std::move(up)) {
    vars.reserve(expectedSize);
  };

  void sort() {
    std::stable_sort(
        vars.begin(), vars.end(),
        [](const Vars::value_type& a, const Vars::value_type& b) { return a.first < b.first; });
  }

  void deduplicate() {
    auto it = vars.begin(), jt = it, end = vars.end();
    while (jt != end) {
      *it = *jt++;
      while (jt != end && it->first == jt->first)
        *it = *jt++;
      it++;
    }
    vars.erase(it, end);
  }

  Vars::const_iterator find(symbol_t name) const {
    Vars::value_type key(name, 0);
    auto i = std::lower_bound(vars.begin(), vars.end(), key);
    if (i != vars.end() && i->first == name)
      return i;
    return vars.end();
  }
};

} // namespace nix
