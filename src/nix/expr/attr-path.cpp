#include "nix/expr/attr-path.h"

#include "nix/expr/eval-inline.h"
#include "nix/util/strings-inline.h"

namespace nix {

static strings_t parse_attr_path(std::string_view s) {
  strings_t res;
  std::string cur;
  auto i = s.begin();
  while (i != s.end()) {
    if (*i == '.') {
      res.push_back(cur);
      cur.clear();
    } else if (*i == '"') {
      ++i;
      while (1) {
        if (i == s.end())
          throw ParseError("missing closing quote in selection path '%1%'", s);
        if (*i == '"')
          break;
        cur.push_back(*i++);
      }
    } else
      cur.push_back(*i);
    ++i;
  }
  if (!cur.empty())
    res.push_back(cur);
  return res;
}

AttrPath AttrPath::parse(eval_state_t& state, std::string_view s) {
  AttrPath res;
  for (auto& a : parse_attr_path(s))
    res.push_back(state.symbols.create(a));
  return res;
}

std::string AttrPath::to_string(eval_state_t& state) const {
  return drop_empty_init_then_concat_strings_sep(".", state.symbols.resolve({*this}));
}

std::vector<SymbolStr> AttrPath::resolve(eval_state_t& state) const {
  return state.symbols.resolve({*this});
}

std::pair<value_t*, pos_idx_t> find_along_attr_path(eval_state_t& state,
                                                    const std::string& attr_path,
                                                    bindings_t& auto_args, value_t& v_in) {
  strings_t tokens = parse_attr_path(attr_path);

  value_t* v = &v_in;
  pos_idx_t pos = no_pos;

  for (auto& attr : tokens) {
    /* Is i an index (integer) or a normal attribute name? */
    auto attrIndex = string2_int<unsigned int>(attr);

    /* Evaluate the expression. */
    value_t* vNew = state.allocValue();
    state.autoCallFunction(auto_args, *v, *vNew);
    v = vNew;
    state.forceValue(*v, no_pos);

    /* It should evaluate to either a set or an expression,
       according to what is specified in the attr_path. */

    if (!attrIndex) {
      if (v->type() != nAttrs)
        state
            .error<TypeError>(
                "the expression selected by the selection path '%1%' should be a set but is %2%",
                attr_path, show_type(*v))
            .debugThrow();
      if (attr.empty())
        throw Error("empty attribute name in selection path '%1%'", attr_path);

      auto a = v->attrs()->get(state.symbols.create(attr));
      if (!a) {
        string_set_t attrNames;
        for (auto& attr : *v->attrs())
          attrNames.insert(std::string(state.symbols[attr.name]));

        auto suggestions = suggestions_t::best_matches(attrNames, attr);
        throw AttrPathNotFound(suggestions, "attribute '%1%' in selection path '%2%' not found",
                               attr, attr_path);
      }
      v = &*a->value;
      pos = a->pos;
    }

    else {
      if (!v->isList())
        state
            .error<TypeError>(
                "the expression selected by the selection path '%1%' should be a list but is %2%",
                attr_path, show_type(*v))
            .debugThrow();
      if (*attrIndex >= v->list_size())
        throw AttrPathNotFound("list index %1% in selection path '%2%' is out of range", *attrIndex,
                               attr_path);

      v = v->list_view()[*attrIndex];
      pos = no_pos;
    }
  }

  return {v, pos};
}

std::pair<source_path_t, uint32_t> find_package_filename(eval_state_t& state, value_t& v,
                                                         std::string what) {
  value_t* v2;
  try {
    auto& dummy_args = bindings_t::emptyBindings;
    v2 = find_along_attr_path(state, "meta.position", dummy_args, v).first;
  } catch (Error&) {
    throw NoPositionInfo("package '%s' has no source location information", what);
  }

  // FIXME: is it possible to extract the Pos object instead of doing this
  //        toString + parsing?
  NixStringContext context;
  auto path = state.coerceToPath(no_pos, *v2, context,
                                 "while evaluating the 'meta.position' attribute of a derivation");

  auto fn = path.path.abs();

  auto fail = [fn]() { throw ParseError("cannot parse 'meta.position' attribute '%s'", fn); };

  try {
    auto colon = fn.rfind(':');
    if (colon == std::string::npos)
      fail();
    auto lineno = std::stoi(std::string(fn, colon + 1, std::string::npos));
    return {source_path_t{path.accessor, canon_path_t(fn.substr(0, colon))}, lineno};
  } catch (std::invalid_argument& e) {
    fail();
    unreachable();
  }
}

} // namespace nix
