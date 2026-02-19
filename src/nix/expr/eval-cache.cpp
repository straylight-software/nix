#include "nix/expr/eval-cache.h"

#include "nix/expr/eval-inline.h"
#include "nix/expr/eval.h"
#include "nix/store/globals.h"
#include "nix/store/sqlite.h"
#include "nix/store/store-api.h"
#include "nix/util/users.h"
// Need specialization involving `SymbolStr` just in this one module.
#include "nix/util/strings-inline.h"

namespace nix::eval_cache {

CachedEvalError::CachedEvalError(ref<AttrCursor> cursor, Symbol attr)
    : EvalError(cursor->root->state, "cached failure of attribute '%s'",
                cursor->getAttrPathStr(attr)),
      cursor(cursor),
      attr(attr) {}

void CachedEvalError::force() {
  auto& v = cursor->forceValue();

  if (v.type() == nAttrs) {
    auto a = v.attrs()->get(this->attr);

    state.forceValue(*a->value, a->pos);
  }

  // Shouldn't happen.
  throw EvalError(state, "evaluation of cached failed attribute '%s' unexpectedly succeeded",
                  cursor->getAttrPathStr(attr));
}

static const char* schema = R"sql(
create table if not exists Attributes (
    parent      integer not null,
    name        text,
    type        integer not null,
    value       text,
    context     text,
    primary key (parent, name)
);
)sql";

struct attr_db_t {
  std::atomic_bool failed{false};

  const StoreDirConfig& cfg;

  struct State {
    SQLite db;
    SQLiteStmt insert_attribute;
    SQLiteStmt insert_attribute_with_context;
    SQLiteStmt query_attribute;
    SQLiteStmt query_attributes;
    std::unique_ptr<SQLiteTxn> txn;
  };

  std::unique_ptr<sync_t<State>> _state;

  SymbolTable& symbols;

  attr_db_t(const StoreDirConfig& cfg, const Hash& fingerprint, SymbolTable& symbols)
      : cfg(cfg), _state(std::make_unique<sync_t<State>>()), symbols(symbols) {
    auto state(_state->lock());

    auto cache_dir = std::filesystem::path(get_cache_dir()) / "eval-cache-v6";
    create_dirs(cache_dir);

    auto db_path = cache_dir / (fingerprint.to_string(hash_format_t::base16, false) + ".sqlite");

    state->db = SQLite(db_path);
    state->db.isCache();
    state->db.exec(schema);

    state->insert_attribute.create(
        state->db,
        "insert or replace into Attributes(parent, name, type, value) values (?, ?, ?, ?)");

    state->insert_attribute_with_context.create(state->db,
                                             "insert or replace into Attributes(parent, name, "
                                             "type, value, context) values (?, ?, ?, ?, ?)");

    state->query_attribute.create(
        state->db,
        "select rowid, type, value, context from Attributes where parent = ? and name = ?");

    state->query_attributes.create(state->db, "select name from Attributes where parent = ?");

    state->txn = std::make_unique<SQLiteTxn>(state->db);
  }

  ~attr_db_t() {
    try {
      auto state(_state->lock());
      if (!failed && state->txn->active)
        state->txn->commit();
      state->txn.reset();
    } catch (...) {
      ignore_exception_in_destructor();
    }
  }

  template <typename F>
  AttrId do_sq_lite(F&& fun) {
    if (failed)
      return 0;
    try {
      return fun();
    } catch (SQLiteError&) {
      ignore_exception_except_interrupt();
      failed = true;
      return 0;
    }
  }

  AttrId set_attrs(AttrKey key, const std::vector<Symbol>& attrs) {
    return do_sq_lite([&]() {
      auto state(_state->lock());

      state->insert_attribute.use()(key.first)(symbols[key.second])(AttrType::FullAttrs)(0, false)
          .exec();

      AttrId row_id = state->db.getLastInsertedRowId();
      assert(row_id);

      for (auto& attr : attrs)
        state->insert_attribute.use()(row_id)(symbols[attr])(AttrType::Placeholder)(0, false).exec();

      return row_id;
    });
  }

  AttrId set_string(AttrKey key, std::string_view s,
                   const Value::StringWithContext::Context* context = nullptr) {
    return do_sq_lite([&]() {
      auto state(_state->lock());

      if (context) {
        std::string ctx;
        bool first = true;
        for (auto* elem : *context) {
          if (!first)
            ctx.push_back(' ');
          ctx.append(elem->view());
          first = false;
        }
        state->insert_attribute_with_context
            .use()(key.first)(symbols[key.second])(AttrType::String)(s)(ctx)
            .exec();
      } else {
        state->insert_attribute.use()(key.first)(symbols[key.second])(AttrType::String)(s).exec();
      }

      return state->db.getLastInsertedRowId();
    });
  }

  AttrId set_bool(AttrKey key, bool b) {
    return do_sq_lite([&]() {
      auto state(_state->lock());

      state->insert_attribute.use()(key.first)(symbols[key.second])(AttrType::Bool)(b ? 1 : 0)
          .exec();

      return state->db.getLastInsertedRowId();
    });
  }

  AttrId set_int(AttrKey key, int n) {
    return do_sq_lite([&]() {
      auto state(_state->lock());

      state->insert_attribute.use()(key.first)(symbols[key.second])(AttrType::Int)(n).exec();

      return state->db.getLastInsertedRowId();
    });
  }

  AttrId set_list_of_strings(AttrKey key, const std::vector<std::string>& l) {
    return do_sq_lite([&]() {
      auto state(_state->lock());

      state->insert_attribute
          .use()(key.first)(symbols[key.second])(
              AttrType::ListOfStrings)(drop_empty_init_then_concat_strings_sep("\t", l))
          .exec();

      return state->db.getLastInsertedRowId();
    });
  }

  AttrId set_placeholder(AttrKey key) {
    return do_sq_lite([&]() {
      auto state(_state->lock());

      state->insert_attribute.use()(key.first)(symbols[key.second])(AttrType::Placeholder)(0, false)
          .exec();

      return state->db.getLastInsertedRowId();
    });
  }

  AttrId set_missing(AttrKey key) {
    return do_sq_lite([&]() {
      auto state(_state->lock());

      state->insert_attribute.use()(key.first)(symbols[key.second])(AttrType::Missing)(0, false)
          .exec();

      return state->db.getLastInsertedRowId();
    });
  }

  AttrId set_misc(AttrKey key) {
    return do_sq_lite([&]() {
      auto state(_state->lock());

      state->insert_attribute.use()(key.first)(symbols[key.second])(AttrType::Misc)(0, false).exec();

      return state->db.getLastInsertedRowId();
    });
  }

  AttrId set_failed(AttrKey key) {
    return do_sq_lite([&]() {
      auto state(_state->lock());

      state->insert_attribute.use()(key.first)(symbols[key.second])(AttrType::Failed)(0, false)
          .exec();

      return state->db.getLastInsertedRowId();
    });
  }

  std::optional<std::pair<AttrId, AttrValue>> get_attr(AttrKey key) {
    auto state(_state->lock());

    auto query_attribute(state->query_attribute.use()(key.first)(symbols[key.second]));
    if (!query_attribute.next())
      return {};

    auto row_id = (AttrId)query_attribute.getInt(0);
    auto type = (AttrType)query_attribute.getInt(1);

    switch (type) {
      case AttrType::Placeholder:
        return {{row_id, placeholder_t()}};
      case AttrType::FullAttrs: {
        // FIXME: expensive, should separate this out.
        std::vector<Symbol> attrs;
        auto query_attributes(state->query_attributes.use()(row_id));
        while (query_attributes.next())
          attrs.emplace_back(symbols.create(query_attributes.getStr(0)));
        return {{row_id, attrs}};
      }
      case AttrType::String: {
        NixStringContext context;
        if (!query_attribute.isNull(3))
          for (auto& s : tokenize_string<std::vector<std::string>>(query_attribute.getStr(3), ";"))
            context.insert(NixStringContextElem::parse(s));
        return {{row_id, string_t{query_attribute.getStr(2), context}}};
      }
      case AttrType::Bool:
        return {{row_id, query_attribute.getInt(2) != 0}};
      case AttrType::Int:
        return {{row_id, int_t{NixInt{query_attribute.getInt(2)}}}};
      case AttrType::ListOfStrings:
        return {{row_id, tokenize_string<std::vector<std::string>>(query_attribute.getStr(2), "\t")}};
      case AttrType::Missing:
        return {{row_id, missing_t()}};
      case AttrType::Misc:
        return {{row_id, misc_t()}};
      case AttrType::Failed:
        return {{row_id, failed_t()}};
      default:
        throw Error("unexpected type in evaluation cache");
    }
  }
};

static std::shared_ptr<attr_db_t> make_attr_db(const StoreDirConfig& cfg, const Hash& fingerprint,
                                          SymbolTable& symbols) {
  try {
    return std::make_shared<attr_db_t>(cfg, fingerprint, symbols);
  } catch (SQLiteError&) {
    ignore_exception_except_interrupt();
    return nullptr;
  }
}

EvalCache::EvalCache(std::optional<std::reference_wrapper<const Hash>> useCache, EvalState& state,
                     RootLoader root_loader)
    : db(useCache ? make_attr_db(*state.store, *useCache, state.symbols) : nullptr),
      state(state),
      root_loader(root_loader) {}

Value* EvalCache::getRootValue() {
  if (!value) {
    debug("getting root value");
    value = alloc_root_value(root_loader());
  }
  return *value;
}

ref<AttrCursor> EvalCache::get_root() {
  return make_ref<AttrCursor>(ref(shared_from_this()), std::nullopt);
}

AttrCursor::AttrCursor(ref<EvalCache> root, Parent parent, Value* value,
                       std::optional<std::pair<AttrId, AttrValue>>&& cachedValue)
    : root(root), parent(parent), cachedValue(std::move(cachedValue)) {
  if (value)
    _value = alloc_root_value(value);
}

AttrKey AttrCursor::getKey() {
  if (!parent)
    return {0, root->state.s.epsilon};
  if (!parent->first->cachedValue) {
    parent->first->cachedValue = root->db->get_attr(parent->first->getKey());
    assert(parent->first->cachedValue);
  }
  return {parent->first->cachedValue->first, parent->second};
}

Value& AttrCursor::getValue() {
  if (!_value) {
    if (parent) {
      auto& vParent = parent->first->getValue();
      root->state.forceAttrs(vParent, no_pos, "while searching for an attribute");
      auto attr = vParent.attrs()->get(parent->second);
      if (!attr)
        throw Error("attribute '%s' is unexpectedly missing", getAttrPathStr());
      _value = alloc_root_value(attr->value);
    } else
      _value = alloc_root_value(root->getRootValue());
  }
  return **_value;
}

void AttrCursor::fetchCachedValue() {
  if (!cachedValue)
    cachedValue = root->db->get_attr(getKey());
  if (cachedValue && std::get_if<failed_t>(&cachedValue->second) && parent)
    throw CachedEvalError(parent->first, parent->second);
}

AttrPath AttrCursor::getAttrPath() const {
  if (parent) {
    auto attr_path = parent->first->getAttrPath();
    attr_path.push_back(parent->second);
    return attr_path;
  } else
    return {};
}

AttrPath AttrCursor::getAttrPath(Symbol name) const {
  auto attr_path = getAttrPath();
  attr_path.push_back(name);
  return attr_path;
}

std::string AttrCursor::getAttrPathStr() const {
  return getAttrPath().to_string(root->state);
}

std::string AttrCursor::getAttrPathStr(Symbol name) const {
  return getAttrPath(name).to_string(root->state);
}

Value& AttrCursor::forceValue() {
  debug("evaluating uncached attribute '%s'", getAttrPathStr());

  auto& v = getValue();

  try {
    root->state.forceValue(v, no_pos);
  } catch (EvalError&) {
    debug("setting '%s' to failed", getAttrPathStr());
    if (root->db)
      cachedValue = {root->db->set_failed(getKey()), failed_t()};
    throw;
  }

  if (root->db && (!cachedValue || std::get_if<placeholder_t>(&cachedValue->second))) {
    if (v.type() == nString)
      cachedValue = {root->db->set_string(getKey(), v.string_view(), v.context()),
                     string_t{v.string_view(), {}}};
    else if (v.type() == nPath) {
      auto path = v.path().path;
      cachedValue = {root->db->set_string(getKey(), path.abs()), string_t{path.abs(), {}}};
    } else if (v.type() == nBool)
      cachedValue = {root->db->set_bool(getKey(), v.boolean()), v.boolean()};
    else if (v.type() == nInt)
      cachedValue = {root->db->set_int(getKey(), v.integer().value), int_t{v.integer()}};
    else if (v.type() == nAttrs)
      ; // FIXME: do something?
    else
      cachedValue = {root->db->set_misc(getKey()), misc_t()};
  }

  return v;
}

suggestions_t AttrCursor::getSuggestionsForAttr(Symbol name) {
  auto attrNames = getAttrs();
  string_set_t strAttrNames;
  for (auto& name : attrNames)
    strAttrNames.insert(std::string(root->state.symbols[name]));

  return suggestions_t::best_matches(strAttrNames, root->state.symbols[name]);
}

std::shared_ptr<AttrCursor> AttrCursor::maybeGetAttr(Symbol name) {
  if (root->db) {
    fetchCachedValue();

    if (cachedValue) {
      if (auto attrs = std::get_if<std::vector<Symbol>>(&cachedValue->second)) {
        for (auto& attr : *attrs)
          if (attr == name)
            return std::make_shared<AttrCursor>(root,
                                                std::make_pair(ref(shared_from_this()), attr));
        return nullptr;
      } else if (std::get_if<placeholder_t>(&cachedValue->second)) {
        auto attr = root->db->get_attr({cachedValue->first, name});
        if (attr) {
          if (std::get_if<missing_t>(&attr->second))
            return nullptr;
          else if (std::get_if<failed_t>(&attr->second))
            throw CachedEvalError(ref(shared_from_this()), name);
          else
            return std::make_shared<AttrCursor>(root, std::make_pair(ref(shared_from_this()), name),
                                                nullptr, std::move(attr));
        }
        // Incomplete attrset, so need to fall thru and
        // evaluate to see whether 'name' exists
      } else
        return nullptr;
      // error<TypeError>("'%s' is not an attribute set", getAttrPathStr()).debugThrow();
    }
  }

  auto& v = forceValue();

  if (v.type() != nAttrs)
    return nullptr;
  // error<TypeError>("'%s' is not an attribute set", getAttrPathStr()).debugThrow();

  auto attr = v.attrs()->get(name);

  if (!attr) {
    if (root->db) {
      if (!cachedValue)
        cachedValue = {root->db->set_placeholder(getKey()), placeholder_t()};
      root->db->set_missing({cachedValue->first, name});
    }
    return nullptr;
  }

  std::optional<std::pair<AttrId, AttrValue>> cachedValue2;
  if (root->db) {
    if (!cachedValue)
      cachedValue = {root->db->set_placeholder(getKey()), placeholder_t()};
    cachedValue2 = {root->db->set_placeholder({cachedValue->first, name}), placeholder_t()};
  }

  return make_ref<AttrCursor>(root, std::make_pair(ref(shared_from_this()), name), attr->value,
                              std::move(cachedValue2));
}

std::shared_ptr<AttrCursor> AttrCursor::maybeGetAttr(std::string_view name) {
  return maybeGetAttr(root->state.symbols.create(name));
}

ref<AttrCursor> AttrCursor::get_attr(Symbol name) {
  auto p = maybeGetAttr(name);
  if (!p)
    throw Error("attribute '%s' does not exist", getAttrPathStr(name));
  return ref(p);
}

ref<AttrCursor> AttrCursor::get_attr(std::string_view name) {
  return get_attr(root->state.symbols.create(name));
}

or_suggestions_t<ref<AttrCursor>> AttrCursor::find_along_attr_path(const AttrPath& attr_path) {
  auto res = shared_from_this();
  for (auto& attr : attr_path) {
    auto child = res->maybeGetAttr(attr);
    if (!child) {
      auto suggestions = res->getSuggestionsForAttr(attr);
      return or_suggestions_t<ref<AttrCursor>>::failed(suggestions);
    }
    res = child;
  }
  return ref(res);
}

std::string AttrCursor::get_string() {
  if (root->db) {
    fetchCachedValue();
    if (cachedValue && !std::get_if<placeholder_t>(&cachedValue->second)) {
      if (auto s = std::get_if<string_t>(&cachedValue->second)) {
        debug("using cached string attribute '%s'", getAttrPathStr());
        return s->first;
      } else
        root->state.error<TypeError>("'%s' is not a string", getAttrPathStr()).debugThrow();
    }
  }

  auto& v = forceValue();

  if (v.type() != nString && v.type() != nPath)
    root->state.error<TypeError>("'%s' is not a string but %s", getAttrPathStr(), show_type(v))
        .debugThrow();

  return v.type() == nString ? std::string(v.string_view()) : v.path().to_string();
}

string_t AttrCursor::getStringWithContext() {
  if (root->db) {
    fetchCachedValue();
    if (cachedValue && !std::get_if<placeholder_t>(&cachedValue->second)) {
      if (auto s = std::get_if<string_t>(&cachedValue->second)) {
        bool valid = true;
        for (auto& c : s->second) {
          const StorePath* path = std::visit(
              overloaded{
                  [&](const NixStringContextElem::DrvDeep& d) -> const StorePath* {
                    return &d.drv_path;
                  },
                  [&](const NixStringContextElem::Built& b) -> const StorePath* {
                    return &b.drv_path->getBaseStorePath();
                  },
                  [&](const NixStringContextElem::opaque_t& o) -> const StorePath* {
                    return &o.path;
                  },
                  [&](const NixStringContextElem::Path& p) -> const StorePath* { return nullptr; },
              },
              c.raw);
          if (!path || !root->state.store->isValidPath(*path)) {
            valid = false;
            break;
          }
        }
        if (valid) {
          debug("using cached string attribute '%s'", getAttrPathStr());
          return *s;
        }
      } else
        root->state.error<TypeError>("'%s' is not a string", getAttrPathStr()).debugThrow();
    }
  }

  auto& v = forceValue();

  if (v.type() == nString) {
    NixStringContext context;
    copy_context(v, context);
    return {std::string{v.string_view()}, std::move(context)};
  } else if (v.type() == nPath)
    return {v.path().to_string(), {}};
  else
    root->state.error<TypeError>("'%s' is not a string but %s", getAttrPathStr(), show_type(v))
        .debugThrow();
}

bool AttrCursor::getBool() {
  if (root->db) {
    fetchCachedValue();
    if (cachedValue && !std::get_if<placeholder_t>(&cachedValue->second)) {
      if (auto b = std::get_if<bool>(&cachedValue->second)) {
        debug("using cached Boolean attribute '%s'", getAttrPathStr());
        return *b;
      } else
        root->state.error<TypeError>("'%s' is not a Boolean", getAttrPathStr()).debugThrow();
    }
  }

  auto& v = forceValue();

  if (v.type() != nBool)
    root->state.error<TypeError>("'%s' is not a Boolean", getAttrPathStr()).debugThrow();

  return v.boolean();
}

NixInt AttrCursor::getInt() {
  if (root->db) {
    fetchCachedValue();
    if (cachedValue && !std::get_if<placeholder_t>(&cachedValue->second)) {
      if (auto i = std::get_if<int_t>(&cachedValue->second)) {
        debug("using cached integer attribute '%s'", getAttrPathStr());
        return i->x;
      } else
        root->state.error<TypeError>("'%s' is not an integer", getAttrPathStr()).debugThrow();
    }
  }

  auto& v = forceValue();

  if (v.type() != nInt)
    root->state.error<TypeError>("'%s' is not an integer", getAttrPathStr()).debugThrow();

  return v.integer();
}

std::vector<std::string> AttrCursor::getListOfStrings() {
  if (root->db) {
    fetchCachedValue();
    if (cachedValue && !std::get_if<placeholder_t>(&cachedValue->second)) {
      if (auto l = std::get_if<std::vector<std::string>>(&cachedValue->second)) {
        debug("using cached list of strings attribute '%s'", getAttrPathStr());
        return *l;
      } else
        root->state.error<TypeError>("'%s' is not a list of strings", getAttrPathStr())
            .debugThrow();
    }
  }

  debug("evaluating uncached attribute '%s'", getAttrPathStr());

  auto& v = getValue();
  root->state.forceValue(v, no_pos);

  if (v.type() != nList)
    root->state.error<TypeError>("'%s' is not a list", getAttrPathStr()).debugThrow();

  std::vector<std::string> res;

  for (auto elem : v.list_view())
    res.push_back(std::string(
        root->state.forceStringNoCtx(*elem, no_pos, "while evaluating an attribute for caching")));

  if (root->db)
    cachedValue = {root->db->set_list_of_strings(getKey(), res), res};

  return res;
}

std::vector<Symbol> AttrCursor::getAttrs() {
  if (root->db) {
    fetchCachedValue();
    if (cachedValue && !std::get_if<placeholder_t>(&cachedValue->second)) {
      if (auto attrs = std::get_if<std::vector<Symbol>>(&cachedValue->second)) {
        debug("using cached attrset attribute '%s'", getAttrPathStr());
        return *attrs;
      } else
        root->state.error<TypeError>("'%s' is not an attribute set", getAttrPathStr()).debugThrow();
    }
  }

  auto& v = forceValue();

  if (v.type() != nAttrs)
    root->state.error<TypeError>("'%s' is not an attribute set", getAttrPathStr()).debugThrow();

  std::vector<Symbol> attrs;
  for (auto& attr : *getValue().attrs())
    attrs.push_back(attr.name);
  std::sort(attrs.begin(), attrs.end(), [&](Symbol a, Symbol b) {
    std::string_view sa = root->state.symbols[a], sb = root->state.symbols[b];
    return sa < sb;
  });

  if (root->db)
    cachedValue = {root->db->set_attrs(getKey(), attrs), attrs};

  return attrs;
}

bool AttrCursor::is_derivation() {
  auto aType = maybeGetAttr("type");
  return aType && aType->get_string() == "derivation";
}

StorePath AttrCursor::forceDerivation() {
  auto aDrvPath = get_attr(root->state.s.drv_path);
  auto drv_path = root->state.store->parseStorePath(aDrvPath->get_string());
  drv_path.requireDerivation();
  if (!root->state.store->isValidPath(drv_path) && !settings.readOnlyMode) {
    /* The eval cache contains 'drvPath', but the actual path has
       been garbage-collected. So force it to be regenerated. */
    aDrvPath->forceValue();
    root->state.waitForPath(drv_path);
    if (!root->state.store->isValidPath(drv_path))
      throw Error("don't know how to recreate store derivation '%s'!",
                  root->state.store->printStorePath(drv_path));
  }
  return drv_path;
}

} // namespace nix::eval_cache
