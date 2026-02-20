#include "nix/expr/get-drvs.h"

#include <cstring>
#include <regex>

#include "nix/expr/eval-inline.h"
#include "nix/store/derivations.h"
#include "nix/store/path-with-outputs.h"
#include "nix/store/store-api.h"

namespace nix {

PackageInfo::PackageInfo(eval_state_t& state, std::string attr_path, const bindings_t* attrs)
    : state(&state), attrs(attrs), attr_path(std::move(attr_path)) {}

PackageInfo::PackageInfo(eval_state_t& state, ref<store_t> store,
                         const std::string& drvPathWithOutputs)
    : state(&state), attrs(nullptr), attr_path("") {
  auto [drv_path, selectedOutputs] = parse_path_with_outputs(*store, drvPathWithOutputs);

  this->drv_path = drv_path;

  auto drv = store->derivationFromPath(drv_path);

  name = drv_path.name();

  if (selectedOutputs.size() > 1)
    throw Error("building more than one derivation output is not supported, in '%s'",
                drvPathWithOutputs);

  output_name =
      selectedOutputs.empty() ? get_or(drv.env, "outputName", "out") : *selectedOutputs.begin();

  auto i = drv.outputs.find(output_name);
  if (i == drv.outputs.end())
    throw Error("derivation '%s' does not have output '%s'", store->printStorePath(drv_path),
                output_name);
  auto& [output_name, output] = *i;

  out_path = {output.path(*store, drv.name, output_name)};
}

std::string PackageInfo::queryName() const {
  if (name == "" && attrs) {
    auto i = attrs->get(state->s.name);
    if (!i)
      state->error<TypeError>("derivation name missing").debugThrow();
    name = state->forceStringNoCtx(*i->value, no_pos,
                                   "while evaluating the 'name' attribute of a derivation");
  }
  return name;
}

std::string PackageInfo::querySystem() const {
  if (system == "" && attrs) {
    auto i = attrs->get(state->s.system);
    system = !i ? "unknown"
                : state->forceStringNoCtx(
                      *i->value, i->pos, "while evaluating the 'system' attribute of a derivation");
  }
  return system;
}

std::optional<store_path_t> PackageInfo::queryDrvPath() const {
  if (!drv_path && attrs) {
    if (auto i = attrs->get(state->s.drv_path)) {
      NixStringContext context;
      auto found = state->coerceToStorePath(
          i->pos, *i->value, context, "while evaluating the 'drvPath' attribute of a derivation");
      try {
        found.requireDerivation();
      } catch (Error& e) {
        e.add_trace(state->positions[i->pos],
                    "while evaluating the 'drvPath' attribute of a derivation");
        throw;
      }
      drv_path = {std::move(found)};
    } else
      drv_path = {std::nullopt};
  }
  return drv_path.value_or(std::nullopt);
}

store_path_t PackageInfo::requireDrvPath() const {
  if (auto drv_path = queryDrvPath())
    return *drv_path;
  throw Error("derivation does not contain a 'drvPath' attribute");
}

store_path_t PackageInfo::queryOutPath() const {
  if (!out_path && attrs) {
    auto i = attrs->get(state->s.out_path);
    NixStringContext context;
    if (i)
      out_path = state->coerceToStorePath(i->pos, *i->value, context,
                                          "while evaluating the output path of a derivation");
  }
  if (!out_path)
    throw UnimplementedError("CA derivations are not yet supported");
  return *out_path;
}

PackageInfo::Outputs PackageInfo::queryOutputs(bool withPaths, bool onlyOutputsToInstall) {
  if (outputs.empty()) {
    /* Get the ‘outputs’ list. */
    const attr_t* i;
    if (attrs && (i = attrs->get(state->s.outputs))) {
      state->forceList(*i->value, i->pos,
                       "while evaluating the 'outputs' attribute of a derivation");

      /* For each output... */
      for (auto elem : i->value->list_view()) {
        std::string output(state->forceStringNoCtx(
            *elem, i->pos, "while evaluating the name of an output of a derivation"));

        if (withPaths) {
          /* Evaluate the corresponding set. */
          auto out = attrs->get(state->symbols.create(output));
          if (!out)
            continue; // FIXME: throw error?
          state->forceAttrs(*out->value, i->pos, "while evaluating an output of a derivation");

          /* And evaluate its ‘out_path’ attribute. */
          auto out_path = out->value->attrs()->get(state->s.out_path);
          if (!out_path)
            continue; // FIXME: throw error?
          NixStringContext context;
          outputs.emplace(
              output, state->coerceToStorePath(out_path->pos, *out_path->value, context,
                                               "while evaluating an output path of a derivation"));
        } else
          outputs.emplace(output, std::nullopt);
      }
    } else
      outputs.emplace("out", withPaths ? std::optional{queryOutPath()} : std::nullopt);
  }

  if (!onlyOutputsToInstall || !attrs)
    return outputs;

  const attr_t* i;
  if (attrs && (i = attrs->get(state->s.outputSpecified)) &&
      state->forceBool(*i->value, i->pos,
                       "while evaluating the 'outputSpecified' attribute of a derivation")) {
    Outputs result;
    auto out = outputs.find(queryOutputName());
    if (out == outputs.end())
      throw Error("derivation does not have output '%s'", queryOutputName());
    result.insert(*out);
    return result;
  }

  else {
    /* Check for `meta.outputsToInstall` and return `outputs` reduced to that. */
    const value_t* outTI = queryMeta("outputsToInstall");
    if (!outTI)
      return outputs;
    auto errMsg = Error("this derivation has bad 'meta.outputsToInstall'");
    /* ^ this shows during `nix-env -i` right under the bad derivation */
    if (!outTI->isList())
      throw errMsg;
    Outputs result;
    for (auto elem : outTI->list_view()) {
      if (elem->type() != nString)
        throw errMsg;
      auto out = outputs.find(elem->string_view());
      if (out == outputs.end())
        throw errMsg;
      result.insert(*out);
    }
    return result;
  }
}

std::string PackageInfo::queryOutputName() const {
  if (output_name == "" && attrs) {
    auto i = attrs->get(state->s.output_name);
    output_name = i ? state->forceStringNoCtx(*i->value, no_pos,
                                              "while evaluating the output name of a derivation")
                    : "";
  }
  return output_name;
}

const bindings_t* PackageInfo::getMeta() {
  if (meta)
    return meta;
  if (!attrs)
    return 0;
  auto a = attrs->get(state->s.meta);
  if (!a)
    return 0;
  state->forceAttrs(*a->value, a->pos, "while evaluating the 'meta' attribute of a derivation");
  meta = a->value->attrs();
  return meta;
}

string_set_t PackageInfo::queryMetaNames() {
  string_set_t res;
  if (!getMeta())
    return res;
  for (auto& i : *meta)
    res.emplace(state->symbols[i.name]);
  return res;
}

bool PackageInfo::checkMeta(value_t& v) {
  state->forceValue(v, v.determinePos(no_pos));
  if (v.type() == nList) {
    for (auto elem : v.list_view())
      if (!checkMeta(*elem))
        return false;
    return true;
  } else if (v.type() == nAttrs) {
    if (v.attrs()->get(state->s.out_path))
      return false;
    for (auto& i : *v.attrs())
      if (!checkMeta(*i.value))
        return false;
    return true;
  } else
    return v.type() == nInt || v.type() == nBool || v.type() == nString || v.type() == nFloat;
}

value_t* PackageInfo::queryMeta(const std::string& name) {
  if (!getMeta())
    return 0;
  auto a = meta->get(state->symbols.create(name));
  if (!a || !checkMeta(*a->value))
    return 0;
  return a->value;
}

std::string PackageInfo::queryMetaString(const std::string& name) {
  value_t* v = queryMeta(name);
  if (!v || v->type() != nString)
    return "";
  return std::string{v->string_view()};
}

NixInt PackageInfo::queryMetaInt(const std::string& name, NixInt def) {
  value_t* v = queryMeta(name);
  if (!v)
    return def;
  if (v->type() == nInt)
    return v->integer();
  if (v->type() == nString) {
    /* Backwards compatibility with before we had support for
       integer meta fields. */
    if (auto n = string2_int<NixInt::Inner>(v->string_view()))
      return NixInt{*n};
  }
  return def;
}

NixFloat PackageInfo::queryMetaFloat(const std::string& name, NixFloat def) {
  value_t* v = queryMeta(name);
  if (!v)
    return def;
  if (v->type() == nFloat)
    return v->fpoint();
  if (v->type() == nString) {
    /* Backwards compatibility with before we had support for
       float meta fields. */
    if (auto n = string2_float<NixFloat>(v->string_view()))
      return *n;
  }
  return def;
}

bool PackageInfo::queryMetaBool(const std::string& name, bool def) {
  value_t* v = queryMeta(name);
  if (!v)
    return def;
  if (v->type() == nBool)
    return v->boolean();
  if (v->type() == nString) {
    /* Backwards compatibility with before we had support for
       Boolean meta fields. */
    if (v->string_view() == "true")
      return true;
    if (v->string_view() == "false")
      return false;
  }
  return def;
}

void PackageInfo::setMeta(const std::string& name, value_t* v) {
  getMeta();
  auto attrs = state->buildBindings(1 + (meta ? meta->size() : 0));
  auto sym = state->symbols.create(name);
  if (meta)
    for (auto i : *meta)
      if (i.name != sym)
        attrs.insert(i);
  if (v)
    attrs.insert(sym, v);
  meta = attrs.finish();
}

/* cache_t for already considered attrsets. */
typedef std::set<const bindings_t*> done_t;

/* Evaluate value `v'.  If it evaluates to a set of type `derivation',
   then put information about it in `drvs' (unless it's already in `done').
   The result boolean indicates whether it makes sense
   for the caller to recursively search for derivations in `v'. */
static bool get_derivation(eval_state_t& state, value_t& v, const std::string& attr_path,
                           PackageInfos& drvs, done_t& done, bool ignore_assertion_failures) {
  try {
    state.forceValue(v, v.determinePos(no_pos));
    if (!state.is_derivation(v))
      return true;

    /* Remove spurious duplicates (e.g., a set like `rec { x =
       derivation {...}; y = x;}'. */
    if (!done.insert(v.attrs()).second)
      return false;

    PackageInfo drv(state, attr_path, v.attrs());

    drv.queryName();

    drvs.push_back(drv);

    return false;

  } catch (AssertionError& e) {
    if (ignore_assertion_failures)
      return false;
    throw;
  }
}

std::optional<PackageInfo> get_derivation(eval_state_t& state, value_t& v,
                                          bool ignore_assertion_failures) {
  done_t done;
  PackageInfos drvs;
  get_derivation(state, v, "", drvs, done, ignore_assertion_failures);
  if (drvs.size() != 1)
    return {};
  return std::move(drvs.front());
}

static std::string add_to_path(const std::string& s1, std::string_view s2) {
  return s1.empty() ? std::string(s2) : s1 + "." + s2;
}

static std::regex attr_regex("[A-Za-z_][A-Za-z0-9-_+]*");

static void get_derivations(eval_state_t& state, value_t& v_in, const std::string& path_prefix,
                            bindings_t& auto_args, PackageInfos& drvs, done_t& done,
                            bool ignore_assertion_failures) {
  value_t v;
  state.autoCallFunction(auto_args, v_in, v);

  /* Process the expression. */
  if (!get_derivation(state, v, path_prefix, drvs, done, ignore_assertion_failures))
    ;

  else if (v.type() == nAttrs) {
    /* !!! undocumented hackery to support combining channels in
       nix-env.cc. */
    bool combine_channels = v.attrs()->get(state.symbols.create("_combineChannels"));

    /* Consider the attributes in sorted order to get more
       deterministic behaviour in nix-env operations (e.g. when
       there are names clashes between derivations, the derivation
       bound to the attribute with the "lower" name should take
       precedence). */
    for (auto& i : v.attrs()->lexicographicOrder(state.symbols)) {
      std::string_view symbol{state.symbols[i->name]};
      try {
        debug("evaluating attribute '%1%'", symbol);
        if (!std::regex_match(symbol.begin(), symbol.end(), attr_regex))
          continue;
        std::string pathPrefix2 = add_to_path(path_prefix, symbol);
        if (combine_channels)
          get_derivations(state, *i->value, pathPrefix2, auto_args, drvs, done,
                          ignore_assertion_failures);
        else if (get_derivation(state, *i->value, pathPrefix2, drvs, done,
                                ignore_assertion_failures)) {
          /* If the value of this attribute is itself a set,
          should we recurse into it?  => Only if it has a
          `recurseForDerivations = true' attribute. */
          if (i->value->type() == nAttrs) {
            auto j = i->value->attrs()->get(state.s.recurseForDerivations);
            if (j && state.forceBool(*j->value, j->pos,
                                     "while evaluating the attribute `recurseForDerivations`"))
              get_derivations(state, *i->value, pathPrefix2, auto_args, drvs, done,
                              ignore_assertion_failures);
          }
        }
      } catch (Error& e) {
        e.add_trace(state.positions[i->pos], "while evaluating the attribute '%s'", symbol);
        throw;
      }
    }
  }

  else if (v.type() == nList) {
    auto list_view = v.list_view();
    for (auto [n, elem] : enumerate(list_view)) {
      std::string pathPrefix2 = add_to_path(path_prefix, fmt("%d", n));
      if (get_derivation(state, *elem, pathPrefix2, drvs, done, ignore_assertion_failures))
        get_derivations(state, *elem, pathPrefix2, auto_args, drvs, done,
                        ignore_assertion_failures);
    }
  }

  else
    state
        .error<TypeError>(
            "expression does not evaluate to a derivation (or a set or list of those)")
        .debugThrow();
}

void get_derivations(eval_state_t& state, value_t& v, const std::string& path_prefix,
                     bindings_t& auto_args, PackageInfos& drvs, bool ignore_assertion_failures) {
  done_t done;
  get_derivations(state, v, path_prefix, auto_args, drvs, done, ignore_assertion_failures);
}

} // namespace nix
