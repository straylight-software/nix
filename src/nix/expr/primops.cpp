#include "nix/expr/primops.h"

#include <algorithm>
#include <cstring>
#include <regex>
#include <sstream>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <boost/container/small_vector.hpp>
#include <boost/unordered/concurrent_flat_map.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <nlohmann/json.hpp>

#include "nix/expr/eval-inline.h"
#include "nix/expr/eval-settings.h"
#include "nix/expr/eval.h"
#include "nix/expr/gc-small-vector.h"
#include "nix/expr/json-to-value.h"
#include "nix/expr/static-string-data.h"
#include "nix/expr/value-to-json.h"
#include "nix/expr/value-to-xml.h"
#include "nix/fetchers/fetch-to-store.h"
#include "nix/store/derivations.h"
#include "nix/store/downstream-placeholder.h"
#include "nix/store/globals.h"
#include "nix/store/names.h"
#include "nix/store/path-references.h"
#include "nix/store/store-api.h"
#include "nix/util/mounted-source-accessor.h"
#include "nix/util/processes.h"
#include "nix/util/sort.h"
#include "nix/util/util.h"

#ifndef _WIN32
#  include <dlfcn.h>
#endif

#include <cmath>

namespace nix {

RegisterPrimOp::PrimOps& RegisterPrimOp::primOps() {
  static RegisterPrimOp::PrimOps primOps;
  return primOps;
}

/*************************************************************
 * Miscellaneous
 *************************************************************/

static inline value_t* mk_string(eval_state_t& state, const std::csub_match& match) {
  value_t* v = state.allocValue();
  v->mk_string({match.first, match.second}, state.mem);
  return v;
}

std::string eval_state_t::realiseString(value_t& s, store_path_set_t* storePathsOutMaybe,
                                        bool isIFD, const pos_idx_t pos) {
  nix::NixStringContext stringContext;
  auto rawStr = coerceToString(pos, s, stringContext, "while realising a string").to_owned();
  auto rewrites = realiseContext(stringContext, storePathsOutMaybe, isIFD);

  return nix::rewrite_strings(rawStr, rewrites);
}

string_map_t eval_state_t::realiseContext(const NixStringContext& context,
                                          store_path_set_t* maybePathsOut, bool isIFD) {
  std::vector<derived_path_t::Built> drvs;
  string_map_t res;

  for (auto& c : context) {
    auto ensureValid = [&](const store_path_t& p) {
      waitForPath(p);
      if (!store->isValidPath(p)) {
        error<InvalidPathError>(store->printStorePath(p)).debugThrow();
      }
    };
    std::visit(overloaded{
                   [&](const NixStringContextElem::Built& b) {
                     drvs.push_back(derived_path_t::Built{
                         .drv_path = b.drv_path,
                         .outputs = OutputsSpec::Names{b.output},
                     });
                     ensureValid(b.drv_path->getBaseStorePath());
                   },
                   [&](const NixStringContextElem::opaque_t& o) {
                     // We consider virtual store paths valid here. They'll
                     // be devirtualized if needed elsewhere.
                     if (!storeFS->get_mount(canon_path_t(store->printStorePath(o.path)))) {
                       ensureValid(o.path);
                     }
                     if (maybePathsOut) {
                       maybePathsOut->emplace(o.path);
                     }
                   },
                   [&](const NixStringContextElem::DrvDeep& d) {
                     /* Treat same as opaque_t */
                     ensureValid(d.drv_path);
                     if (maybePathsOut) {
                       maybePathsOut->emplace(d.drv_path);
                     }
                   },
                   [&](const NixStringContextElem::Path& p) {
                     // FIXME: do something?
                   },
               },
               c.raw);
  }

  if (drvs.empty()) {
    return {};
  }

  if (isIFD) {
    if (!settings.enableImportFromDerivation) {
      error<IFDError>("cannot build '%1%' during evaluation because the option "
                      "'allow-import-from-derivation' is disabled",
                      drvs.begin()->to_string(*store))
          .debugThrow();
    }

    if (settings.traceImportFromDerivation) {
      warn("built '%1%' during evaluation due to an import from derivation",
           drvs.begin()->to_string(*store));
    }
  }

  /* Build/substitute the context. */
  std::vector<derived_path_t> buildReqs;
  buildReqs.reserve(drvs.size());
  for (auto& d : drvs) {
    buildReqs.emplace_back(derived_path_t{d});
  }
  buildStore->build_paths(buildReqs, bmNormal, store);

  store_path_set_t outputsToCopyAndAllow;

  for (auto& drv : drvs) {
    auto outputs = resolve_derived_path(*buildStore, drv, &*store);
    for (auto& [output_name, output_path] : outputs) {
      outputsToCopyAndAllow.insert(output_path);
      if (maybePathsOut) {
        maybePathsOut->emplace(output_path);
      }

      /* Get all the output paths corresponding to the placeholders we had */
      if (experimental_feature_settings.is_enabled(xp_t::ca_derivations)) {
        res.insert_or_assign(
            DownstreamPlaceholder::fromSingleDerivedPathBuilt(SingleDerivedPath::Built{
                                                                  .drv_path = drv.drv_path,
                                                                  .output = output_name,
                                                              })
                .render(),
            buildStore->printStorePath(output_path));
      }
    }
  }

  if (store != buildStore) {
    copy_closure(*buildStore, *store, outputsToCopyAndAllow);
  }

  if (isIFD) {
    /* Allow access to the output closures of this derivation. */
    for (auto& output_path : outputsToCopyAndAllow) {
      allowClosure(output_path);
    }
  }

  return res;
}

source_path_t
realise_path(eval_state_t& state, const pos_idx_t pos, value_t& v,
             std::optional<symlink_resolution_t> resolve_symlinks = symlink_resolution_t::full) {
  NixStringContext context;

  auto path = state.coerceToPath(no_pos, v, context, "while realising the context of a path");

  try {
    if (!context.empty() && path.accessor == state.root_fs) {
      auto rewrites = state.realiseContext(context);
      path = {path.accessor, canon_path_t(rewrite_strings(path.path.abs(), rewrites))};
    }
    return resolve_symlinks ? path.resolve_symlinks(*resolve_symlinks) : path;
  } catch (Error& e) {
    e.add_trace(state.positions[pos], "while realising the context of path '%s'", path);
    throw;
  }
}

/**
 * Add and attribute to the given attribute map from the output name to
 * the output path, or a placeholder.
 *
 * Where possible the path is used, but for floating CA derivations we
 * may not know it. For sake of determinism we always assume we don't
 * and instead put in a place holder. In either case, however, the
 * string context will contain the drv path and output name, so
 * downstream derivations will have the proper dependency, and in
 * addition, before building, the placeholder will be rewritten to be
 * the actual path.
 *
 * The 'drv' and 'drvPath' outputs must correspond.
 */
static void mk_output_string(eval_state_t& state, BindingsBuilder& attrs,
                             const store_path_t& drv_path,
                             const std::pair<std::string, derivation_output_t>& o) {
  state.mk_output_string(
      attrs.alloc(o.first),
      SingleDerivedPath::Built{
          .drv_path = makeConstantStorePathRef(drv_path),
          .output = o.first,
      },
      o.second.path(*state.store, derivation_t::nameFromPath(drv_path), o.first));
}

/**
 * `import` will parse a derivation when it imports a `.drv` file from the store.
 *
 * @param state The evaluation state.
 * @param pos The position of the `import` call.
 * @param path The path to the `.drv` to import.
 * @param store_path The path to the `.drv` to import.
 * @param v Return value
 */
void derivation_to_value(eval_state_t& state, const pos_idx_t pos, const source_path_t& path,
                         const store_path_t& store_path, value_t& v) {
  auto path2 = path.path.abs();
  derivation_t drv = state.store->read_derivation(store_path);
  auto attrs = state.buildBindings(3 + drv.outputs.size());
  attrs.alloc(state.s.drv_path)
      .mk_string(path2,
                 {
                     NixStringContextElem::DrvDeep{.drv_path = store_path},
                 },
                 state.mem);
  attrs.alloc(state.s.name).mk_string(drv.env["name"], state.mem);

  auto list = state.buildList(drv.outputs.size());
  for (const auto& [i, o] : enumerate(drv.outputs)) {
    mk_output_string(state, attrs, store_path, o);
    (list[i] = state.allocValue())->mk_string(o.first, state.mem);
  }
  attrs.alloc(state.s.outputs).mkList(list);

  auto w = state.allocValue();
  w->mkAttrs(attrs);

  if (!state.vImportedDrvToDerivation) {
    state.vImportedDrvToDerivation = alloc_root_value(state.allocValue());
    state.eval(state.parseExprFromString(
#include "imported-drv-to-derivation.nix.gen.h"
                   , state.root_path(canon_path_t::root)),
               **state.vImportedDrvToDerivation);
  }

  state.forceFunction(**state.vImportedDrvToDerivation, pos,
                      "while evaluating imported-drv-to-derivation.nix.gen.h");
  v.mkApp(*state.vImportedDrvToDerivation, w);
  state.forceAttrs(v, pos, "while calling imported-drv-to-derivation.nix.gen.h");
}

/**
 * Import a Nix file with an alternate base scope, as `builtins.scoped_import` does.
 *
 * @param state The evaluation state.
 * @param pos The position of the import call.
 * @param path The path to the file to import.
 * @param v_scope The base scope to use for the import.
 * @param v Return value
 */
static void scoped_import(eval_state_t& state, const pos_idx_t pos, source_path_t& path,
                          value_t* v_scope, value_t& v) {
  state.forceAttrs(*v_scope, pos,
                   "while evaluating the first argument passed to builtins.scopedImport");

  Env* env = &state.mem.allocEnv(v_scope->attrs()->size());
  env->up = &state.baseEnv;

  auto static_env =
      std::make_shared<StaticEnv>(nullptr, state.staticBaseEnv, v_scope->attrs()->size());

  unsigned int displ = 0;
  for (auto& attr : *v_scope->attrs()) {
    static_env->vars.emplace_back(attr.name, displ);
    env->values[displ++] = attr.value;
  }

  // No need to call staticEnv.sort(), because
  // args[0]->attrs is already sorted.

  printTalkative("evaluating file '%1%'", path);
  expr_t* e = state.parseExprFromFile(resolve_expr_path(path), static_env);

  e->eval(state, *env, v);
}

/* Load and evaluate an expression from path specified by the
   argument. */
static void import(eval_state_t& state, const pos_idx_t pos, value_t& v_path, value_t* v_scope,
                   value_t& v) {
  auto path = realise_path(state, pos, v_path, std::nullopt);
  auto path2 = path.path.abs();

  // FIXME
  auto is_valid_derivation_in_store = [&]() -> std::optional<store_path_t> {
    if (!state.store->isStorePath(path2)) {
      return std::nullopt;
    }
    auto store_path = state.store->parseStorePath(path2);
    state.waitForPath(store_path);
    if (!(state.store->isValidPath(store_path) && is_derivation(path2))) {
      return std::nullopt;
    }
    return store_path;
  };

  if (auto store_path = is_valid_derivation_in_store()) {
    derivation_to_value(state, pos, path, *store_path, v);
  } else if (v_scope) {
    scoped_import(state, pos, path, v_scope, v);
  } else {
    state.evalFile(path, v);
  }
}

static RegisterPrimOp
    primop_scoped_import({.name = "scopedImport",
                          .args = {"scope", "path"},
                          .doc = R"(
      Load, parse, and return the Nix expression in the file *path*, with the attributes from *scope* available as variables in the lexical scope of the imported file.

      This function is similar to [`import`](#builtins-import), but allows you to provide additional variables that will be available in the scope of the imported expression.
      The *scope* argument must be an attribute set; each attribute becomes a variable available in the imported file.
      Built-in functions and values remain accessible unless shadowed by *scope* attributes.

      > **Note**
      >
      > Variables from *scope* shadow built-ins with the same name, allowing you to override built-ins for the imported expression.

      > **Note**
      >
      > Unlike [`import`](#builtins-import), `scoped_import` does not memoize evaluation results.
      > While the parsing result may be reused, each call produces a distinct value.
      > This is observable through performance and side effects such as [`builtins.trace`](#builtins-trace).

      The *path* argument must meet the same criteria as an [interpolated expression](@docroot@/language/string-interpolation.md#interpolated-expression).

      If *path* is a directory, the file `default.nix` in that directory is used if it exists.

      > **Example**
      >
      > Create a file `greet.nix`:
      >
      > ```nix
      > # greet.nix
      > "${greeting}, ${name}!"
      > ```
      >
      > Import it with additional variables in scope:
      >
      > ```nix
      > scoped_import { greeting = "Hello"; name = "World"; } ./greet.nix
      > ```
      >
      >     "Hello, World!"

      Evaluation aborts if the file doesn't exist or contains an invalid Nix expression.
    )",
                          .fun = [](eval_state_t& state, const pos_idx_t pos, value_t** args,
                                    value_t& v) { import(state, pos, *args[1], args[0], v); }});

static RegisterPrimOp
    primop_import({.name = "import",
                   .args = {"path"},
                   // TODO turn "normal path values" into link below
                   .doc = R"(
      Load, parse, and return the Nix expression in the file *path*.

      > **Note**
      >
      > Unlike some languages, `import` is a regular function in Nix.

      The *path* argument must meet the same criteria as an [interpolated expression](@docroot@/language/string-interpolation.md#interpolated-expression).

      If *path* is a directory, the file `default.nix` in that directory is used if it exists.

      > **Example**
      >
      > ```console
      > $ echo 123 > default.nix
      > ```
      >
      > Import `default.nix` from the current directory.
      >
      > ```nix
      > import ./.
      > ```
      >
      >     123

      Evaluation aborts if the file doesn’t exist or contains an invalid Nix expression.

      A Nix expression loaded by `import` must not contain any *free variables*, that is, identifiers that are not defined in the Nix expression itself and are not built-in.
      Therefore, it cannot refer to variables that are in scope at the call site.

      > **Example**
      >
      > If you have a calling expression
      >
      > ```nix
      > rec {
      >   x = 123;
      >   y = import ./foo.nix;
      > }
      > ```
      >
      >  then the following `foo.nix` throws an error:
      >
      >  ```nix
      >  # foo.nix
      >  x + 456
      >  ```
      >
      >  since `x` is not in scope in `foo.nix`.
      > If you want `x` to be available in `foo.nix`, pass it as a function argument:
      >
      >  ```nix
      >  rec {
      >    x = 123;
      >    y = import ./foo.nix x;
      >  }
      >  ```
      >
      >  and
      >
      >  ```nix
      >  # foo.nix
      >  x: x + 456
      >  ```
      >
      >  The function argument doesn’t have to be called `x` in `foo.nix`; any name would work.
    )",
                   .fun = [](eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
                     import(state, pos, *args[0], nullptr, v);
                   }});

#ifndef _WIN32 // TODO implement via DLL loading on Windows

/* Want reasonable symbol names, so extern C */
/* !!! Should we pass the pos_t or the file name too? */
extern "C" typedef void (*value_initializer_t)(eval_state_t& state, value_t& v);

/* Load a value_initializer_t from a DSO and return whatever it initializes */
void prim_import_native(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  auto path = realise_path(state, pos, *args[0]);

  std::string sym(state.forceStringNoCtx(
      *args[1], pos, "while evaluating the second argument passed to builtins.importNative"));

  void* handle = dlopen(path.path.c_str(), RTLD_LAZY | RTLD_LOCAL);
  if (!handle) {
    state.error<EvalError>("could not open '%1%': %2%", path, dlerror()).debugThrow();
  }

  dlerror();
  value_initializer_t func = (value_initializer_t)dlsym(handle, sym.c_str());
  if (!func) {
    char* message = dlerror();
    if (message) {
      state.error<EvalError>("could not load symbol '%1%' from '%2%': %3%", sym, path, message)
          .debugThrow();
    } else {
      state
          .error<EvalError>(
              "symbol '%1%' from '%2%' resolved to NULL when a function pointer was expected", sym,
              path)
          .debugThrow();
    }
  }

  (func)(state, v);

  /* We don't dlclose because v may be a primop referencing a function in the shared object file */
}

/* Execute a program and parse its output */
void prim_exec(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceList(*args[0], pos, "while evaluating the first argument passed to builtins.exec");
  auto elems = args[0]->list_view();
  auto count = args[0]->list_size();
  if (count == 0) {
    state.error<EvalError>("at least one argument to 'exec' required").at_pos(pos).debugThrow();
  }
  NixStringContext context;
  auto program =
      state
          .coerceToString(
              pos, *elems[0], context,
              "while evaluating the first element of the argument passed to builtins.exec", false,
              false)
          .to_owned();
  strings_t command_args;
  for (size_t i = 1; i < count; ++i) {
    command_args.push_back(
        state
            .coerceToString(pos, *elems[i], context,
                            "while evaluating an element of the argument passed to builtins.exec",
                            false, false)
            .to_owned());
  }
  try {
    auto _ = state.realiseContext(context); // FIXME: Handle CA derivations
  } catch (InvalidPathError& e) {
    state.error<EvalError>("cannot execute '%1%', since path '%2%' is not valid", program, e.path)
        .at_pos(pos)
        .debugThrow();
  }

  auto output = run_program(program, true, command_args);
  expr_t* parsed;
  try {
    parsed = state.parseExprFromString(std::move(output), state.root_path(canon_path_t::root));
  } catch (Error& e) {
    e.add_trace(state.positions[pos], "while parsing the output from '%1%'", program);
    throw;
  }
  try {
    state.eval(parsed, v);
  } catch (Error& e) {
    e.add_trace(state.positions[pos], "while evaluating the output from '%1%'", program);
    throw;
  }
}

#endif

/* Return a string representing the type of the expression. */
static void prim_type_of(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceValue(*args[0], pos);
  switch (args[0]->type()) {
    case nInt:
      v.mkStringNoCopy("int"_sds);
      break;
    case nBool:
      v.mkStringNoCopy("bool"_sds);
      break;
    case nString:
      v.mkStringNoCopy("string"_sds);
      break;
    case nPath:
      v.mkStringNoCopy("path"_sds);
      break;
    case nNull:
      v.mkStringNoCopy("null"_sds);
      break;
    case nAttrs:
      v.mkStringNoCopy("set"_sds);
      break;
    case nList:
      v.mkStringNoCopy("list"_sds);
      break;
    case nFunction:
      v.mkStringNoCopy("lambda"_sds);
      break;
    case nExternal:
      v.mk_string(args[0]->external()->typeOf(), state.mem);
      break;
    case nFloat:
      v.mkStringNoCopy("float"_sds);
      break;
    case nThunk:
    case nFailed:
      unreachable();
  }
}

static RegisterPrimOp primop_type_of({
    .name = "__typeOf",
    .args = {"e"},
    .doc = R"(
      Return a string representing the type of the value *e*, namely
      `"int"`, `"bool"`, `"string"`, `"path"`, `"null"`, `"set"`,
      `"list"`, `"lambda"` or `"float"`.
    )",
    .fun = prim_type_of,
});

/* Determine whether the argument is the null value. */
static void prim_is_null(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceValue(*args[0], pos);
  v.mkBool(args[0]->type() == nNull);
}

static RegisterPrimOp primop_is_null({
    .name = "isNull",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to `null`, and `false` otherwise.

      This is equivalent to `e == null`.
    )",
    .fun = prim_is_null,
});

/* Determine whether the argument is a function. */
static void prim_is_function(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceValue(*args[0], pos);
  v.mkBool(args[0]->type() == nFunction);
}

static RegisterPrimOp primop_is_function({
    .name = "__isFunction",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to a function, and `false` otherwise.
    )",
    .fun = prim_is_function,
});

/* Determine whether the argument is an integer. */
static void prim_is_int(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceValue(*args[0], pos);
  v.mkBool(args[0]->type() == nInt);
}

static RegisterPrimOp primop_is_int({
    .name = "__isInt",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to an integer, and `false` otherwise.
    )",
    .fun = prim_is_int,
});

/* Determine whether the argument is a float. */
static void prim_is_float(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceValue(*args[0], pos);
  v.mkBool(args[0]->type() == nFloat);
}

static RegisterPrimOp primop_is_float({
    .name = "__isFloat",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to a float, and `false` otherwise.
    )",
    .fun = prim_is_float,
});

/* Determine whether the argument is a string. */
static void prim_is_string(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceValue(*args[0], pos);
  v.mkBool(args[0]->type() == nString);
}

static RegisterPrimOp primop_is_string({
    .name = "__isString",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to a string, and `false` otherwise.
    )",
    .fun = prim_is_string,
});

/* Determine whether the argument is a Boolean. */
static void prim_is_bool(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceValue(*args[0], pos);
  v.mkBool(args[0]->type() == nBool);
}

static RegisterPrimOp primop_is_bool({
    .name = "__isBool",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to a bool, and `false` otherwise.
    )",
    .fun = prim_is_bool,
});

/* Determine whether the argument is a path. */
static void prim_is_path(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceValue(*args[0], pos);
  v.mkBool(args[0]->type() == nPath);
}

static RegisterPrimOp primop_is_path({
    .name = "__isPath",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to a path, and `false` otherwise.
    )",
    .fun = prim_is_path,
});

template <typename Callable>
static inline void with_exception_context(trace_t trace, Callable&& func) {
  try {
    func();
  } catch (Error& e) {
    e.push_trace(trace);
    throw;
  }
}

struct compare_values_t {
  eval_state_t& state;
  const pos_idx_t pos;
  const std::string_view error_ctx;

  compare_values_t(eval_state_t& state, const pos_idx_t pos, const std::string_view&& error_ctx)
      : state(state), pos(pos), error_ctx(error_ctx) {};

  bool operator()(value_t* v1, value_t* v2) const { return (*this)(v1, v2, error_ctx); }

  bool operator()(value_t* v1, value_t* v2, std::string_view error_ctx) const {
    try {
      if (v1->type() == nFloat && v2->type() == nInt) {
        return v1->fpoint() < v2->integer().value;
      }
      if (v1->type() == nInt && v2->type() == nFloat) {
        return v1->integer().value < v2->fpoint();
      }
      if (v1->type() != v2->type()) {
        state
            .error<EvalError>("cannot compare %s with %s; values are %s and %s", show_type(*v1),
                              show_type(*v2), ValuePrinter(state, *v1, errorPrintOptions),
                              ValuePrinter(state, *v2, errorPrintOptions))
            .debugThrow();
      }
// Allow selecting a subset of enum values
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
      switch (v1->type()) {
        case nInt:
          return v1->integer() < v2->integer();
        case nFloat:
          return v1->fpoint() < v2->fpoint();
        case nString:
          return v1->string_view() < v2->string_view();
        case nPath:
          // Note: we don't take the accessor into account
          // since it's not obvious how to compare them in a
          // reproducible way.
          return v1->pathStrView() < v2->pathStrView();
        case nList:
          // Lexicographic comparison
          for (size_t i = 0;; i++) {
            if (i == v2->list_size()) {
              return false;
            } else if (i == v1->list_size()) {
              return true;
            } else if (!state.eqValues(*v1->list_view()[i], *v2->list_view()[i], pos, error_ctx)) {
              return (*this)(v1->list_view()[i], v2->list_view()[i],
                             "while comparing two list elements");
            }
          }
        default:
          state
              .error<EvalError>("cannot compare %s with %s; values of that type are incomparable "
                                "(values are %s and %s)",
                                show_type(*v1), show_type(*v2),
                                ValuePrinter(state, *v1, errorPrintOptions),
                                ValuePrinter(state, *v2, errorPrintOptions))
              .debugThrow();
#pragma GCC diagnostic pop
      }
    } catch (Error& e) {
      if (!error_ctx.empty()) {
        e.add_trace(nullptr, error_ctx);
      }
      throw;
    }
  }
};

typedef std::list<value_t*, gc_allocator<value_t*>> ValueList;

static void prim_generic_closure(eval_state_t& state, const pos_idx_t pos, value_t** args,
                                 value_t& v) {
  state.forceAttrs(*args[0], no_pos,
                   "while evaluating the first argument passed to builtins.genericClosure");

  /* Get the start set. */
  auto start_set = state.get_attr(state.s.start_set, args[0]->attrs(),
                                  "in the attrset passed as argument to builtins.genericClosure");

  state.forceList(
      *start_set->value, no_pos,
      "while evaluating the 'startSet' attribute passed as argument to builtins.genericClosure");

  ValueList work_set;
  for (auto elem : start_set->value->list_view()) {
    work_set.push_back(elem);
  }

  if (start_set->value->list_size() == 0) {
    v = *start_set->value;
    return;
  }

  /* Get the operator. */
  auto op = state.get_attr(state.s.operator_, args[0]->attrs(),
                           "in the attrset passed as argument to builtins.genericClosure");
  state.forceFunction(
      *op->value, no_pos,
      "while evaluating the 'operator' attribute passed as argument to builtins.genericClosure");

  /* Construct the closure by applying the operator to elements of
     `work_set', adding the result to `workSet', continuing until
     no new elements are found. */
  ValueList res;
  // Track which element each key came from
  auto cmp = compare_values_t(state, no_pos, "");
  std::map<value_t*, value_t*, decltype(cmp)> keyToElem(cmp);
  while (!work_set.empty()) {
    value_t* e = *(work_set.begin());
    work_set.pop_front();

    try {
      state.forceAttrs(*e, no_pos, "");
    } catch (Error& err) {
      err.add_trace(nullptr, "in genericClosure element %s",
                    ValuePrinter(state, *e, errorPrintOptions));
      throw;
    }

    const attr_t* key;
    try {
      key = state.get_attr(state.s.key, e->attrs(), "");
    } catch (Error& err) {
      err.add_trace(nullptr, "in genericClosure element %s",
                    ValuePrinter(state, *e, errorPrintOptions));
      throw;
    }
    state.forceValue(*key->value, no_pos);

    try {
      auto [it, inserted] = keyToElem.insert({key->value, e});
      if (!inserted) {
        continue;
      }
    } catch (Error& err) {
      // Try to find which element we're comparing against
      value_t* other_elem = nullptr;
      for (auto& [otherKey, elem] : keyToElem) {
        try {
          cmp(key->value, otherKey);
        } catch (Error&) {
          // Found the element we're comparing against
          other_elem = elem;
          break;
        }
      }
      if (other_elem) {
        // Traces are printed in reverse order; pre-swap them.
        err.add_trace(nullptr, "with element %s",
                      ValuePrinter(state, *other_elem, errorPrintOptions));
        err.add_trace(nullptr, "while comparing element %s",
                      ValuePrinter(state, *e, errorPrintOptions));
      } else {
        // Couldn't find the specific element, just show current
        err.add_trace(nullptr, "while checking key of element %s",
                      ValuePrinter(state, *e, errorPrintOptions));
      }
      throw;
    }
    res.push_back(e);

    /* Call the `operator' function with `e' as argument. */
    value_t new_elements;
    try {
      state.callFunction(*op->value, {&e, 1}, new_elements, no_pos);
      state.forceList(
          new_elements, no_pos,
          "while evaluating the return value of the `operator` passed to builtins.genericClosure");

      /* Add the values returned by the operator to the work set. */
      for (auto elem : new_elements.list_view()) {
        state.forceValue(*elem, no_pos); // "while evaluating one one of the elements returned by
                                         // the `operator` passed to builtins.genericClosure");
        work_set.push_back(elem);
      }
    } catch (Error& err) {
      err.add_trace(nullptr, "while calling %s on genericClosure element %s",
                    state.symbols[state.s.operator_], ValuePrinter(state, *e, errorPrintOptions));
      throw;
    }
  }

  /* Create the result list. */
  auto list = state.buildList(res.size());
  for (const auto& [n, i] : enumerate(res)) {
    list[n] = i;
  }
  v.mkList(list);
}

static RegisterPrimOp primop_generic_closure(PrimOp{
    .name = "__genericClosure",
    .args = {"attrset"},
    .arity = 1,
    .doc = R"(
      `builtins.genericClosure` iteratively computes the transitive closure over an arbitrary relation defined by a function.

      It takes *attrset* with two attributes named `start_set` and `operator`, and returns a list of attribute sets:

      - `start_set`:
        The initial list of attribute sets.

      - `operator`:
        A function that takes an attribute set and returns a list of attribute sets.
        It defines how each item in the current set is processed and expanded into more items.

      Each attribute set in the list `start_set` and the list returned by `operator` must have an attribute `key`, which must support equality comparison.
      The value of `key` can be one of the following types:

      - [Int](@docroot@/language/types.md#type-int)
      - [Float](@docroot@/language/types.md#type-float)
      - [Boolean](@docroot@/language/types.md#type-bool)
      - [String](@docroot@/language/types.md#type-string)
      - [Path](@docroot@/language/types.md#type-path)
      - [List](@docroot@/language/types.md#type-list)

      The result is produced by calling the `operator` on each `item` that has not been called yet, including newly added items, until no new items are added.
      Items are compared by their `key` attribute.

      common_t usages are:

      - Generating unique collections of items, such as dependency graphs.
      - Traversing through structures that may contain cycles or loops.
      - Processing data structures with complex internal relationships.

      > **Example**
      >
      > ```nix
      > builtins.genericClosure {
      >   start_set = [ {key = 5;} ];
      >   operator = item: [{
      >     key = if (item.key / 2 ) * 2 == item.key
      >          then item.key / 2
      >          else 3 * item.key + 1;
      >   }];
      > }
      > ```
      >
      > evaluates to
      >
      > ```nix
      > [ { key = 5; } { key = 16; } { key = 8; } { key = 4; } { key = 2; } { key = 1; } ]
      > ```
      )",
    .fun = prim_generic_closure,
});

static RegisterPrimOp
    primop_break({.name = "break",
                  .args = {"v"},
                  .doc = R"(
      In debug mode (enabled using `--debugger`), pause Nix expression evaluation and enter the REPL.
      Otherwise, return the argument `v`.
    )",
                  .fun = [](eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
                    if (state.canDebug()) {
                      auto error = Error(error_info_t{
                          .level_ = lvl_info,
                          .msg_ = hint_fmt_t("breakpoint reached"),
                          .pos_ = state.positions[pos],
                      });

                      state.runDebugRepl(&error);
                    }

                    // Return the value we were passed.
                    v = *args[0];
                  }});

static RegisterPrimOp primop_abort(
    {.name = "abort",
     .args = {"s"},
     .doc = R"(
      Abort Nix expression evaluation and print the error message *s*.
    )",
     .fun = [](eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
       NixStringContext context;
       auto s = state
                    .coerceToString(pos, *args[0], context,
                                    "while evaluating the error message passed to builtins.abort")
                    .to_owned();
       state.error<Abort>("evaluation aborted with the following error message: '%1%'", s)
           .setIsFromExpr()
           .debugThrow();
     }});

static RegisterPrimOp primop_throw(
    {.name = "throw",
     .args = {"s"},
     .doc = R"(
      Throw an error message *s*. This usually aborts Nix expression
      evaluation, but in `nix-env -qa` and other commands that try to
      evaluate a set of derivations to get information about those
      derivations, a derivation that throws an error is silently skipped
      (which is not the case for `abort`).
    )",
     .fun = [](eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
       NixStringContext context;
       auto s = state
                    .coerceToString(pos, *args[0], context,
                                    "while evaluating the error message passed to builtin.throw")
                    .to_owned();
       state.error<ThrownError>(s).setIsFromExpr().debugThrow();
     }});

static void prim_add_error_context(eval_state_t& state, const pos_idx_t pos, value_t** args,
                                   value_t& v) {
  try {
    state.forceValue(*args[1], pos);
    v = *args[1];
  } catch (Error& e) {
    NixStringContext context;
    auto message =
        state
            .coerceToString(pos, *args[0], context,
                            "while evaluating the error message passed to builtins.addErrorContext",
                            false, false)
            .to_owned();
    e.add_trace(nullptr, hint_fmt_t(message), trace_print_t::always);
    throw;
  }
}

static RegisterPrimOp primop_add_error_context(PrimOp{
    .name = "__addErrorContext",
    .arity = 2,
    // The normal trace item is redundant
    .add_trace = false,
    .fun = prim_add_error_context,
});

static void prim_ceil(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  auto value = state.forceFloat(*args[0], args[0]->determinePos(pos),
                                "while evaluating the first argument passed to builtins.ceil");
  auto ceil_value = ceil(value);
  bool is_int = args[0]->type() == nInt;
  constexpr NixFloat int_min =
      std::numeric_limits<NixInt::Inner>::min(); // power of 2, so that no rounding occurs
  if (ceil_value >= int_min && ceil_value < -int_min) {
    v.mkInt(ceil_value);
  } else if (is_int) {
    // a NixInt, e.g. INT64_MAX, can be rounded to -int_min due to the cast to NixFloat
    state
        .error<EvalError>("Due to a bug (see https://github.com/NixOS/nix/issues/12899) the NixInt "
                          "argument %1% caused undefined behavior in previous Nix "
                          "versions.\n\tFuture Nix versions might implement the correct behavior.",
                          args[0]->integer().value)
        .at_pos(pos)
        .debugThrow();
  } else {
    state.error<EvalError>("NixFloat argument %1% is not in the range of NixInt", args[0]->fpoint())
        .at_pos(pos)
        .debugThrow();
  }
  // `forceFloat` casts NixInt to NixFloat, but instead NixInt args shall be returned unmodified
  if (is_int) {
    auto arg = args[0]->integer();
    auto res = v.integer();
    if (arg != res) {
      state
          .error<EvalError>(
              "Due to a bug (see https://github.com/NixOS/nix/issues/12899) a loss of precision "
              "occurred in previous Nix versions because the NixInt argument %1% was rounded to "
              "%2%.\n\tFuture Nix versions might implement the correct behavior.",
              arg, res)
          .at_pos(pos)
          .debugThrow();
    }
  }
}

static RegisterPrimOp primop_ceil({
    .name = "__ceil",
    .args = {"number"},
    .doc = R"(
        Rounds and converts *number* to the next higher NixInt value if possible, i.e. `ceil *number* >= *number*` and
        `ceil *number* - *number* < 1`.

        An evaluation error is thrown, if there exists no such NixInt value `ceil *number*`.
        Due to bugs in previous Nix versions an evaluation error might be thrown, if the datatype of *number* is
        a NixInt and if `*number* < -9007199254740992` or `*number* > 9007199254740992`.

        If the datatype of *number* is neither a NixInt (signed 64-bit integer) nor a NixFloat
        (IEEE-754 double-precision floating-point number), an evaluation error is thrown.
    )",
    .fun = prim_ceil,
});

static void prim_floor(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  auto value = state.forceFloat(*args[0], args[0]->determinePos(pos),
                                "while evaluating the first argument passed to builtins.floor");
  auto floor_value = floor(value);
  bool is_int = args[0]->type() == nInt;
  constexpr NixFloat int_min =
      std::numeric_limits<NixInt::Inner>::min(); // power of 2, so that no rounding occurs
  if (floor_value >= int_min && floor_value < -int_min) {
    v.mkInt(floor_value);
  } else if (is_int) {
    // a NixInt, e.g. INT64_MAX, can be rounded to -int_min due to the cast to NixFloat
    state
        .error<EvalError>("Due to a bug (see https://github.com/NixOS/nix/issues/12899) the NixInt "
                          "argument %1% caused undefined behavior in previous Nix "
                          "versions.\n\tFuture Nix versions might implement the correct behavior.",
                          args[0]->integer().value)
        .at_pos(pos)
        .debugThrow();
  } else {
    state.error<EvalError>("NixFloat argument %1% is not in the range of NixInt", args[0]->fpoint())
        .at_pos(pos)
        .debugThrow();
  }
  // `forceFloat` casts NixInt to NixFloat, but instead NixInt args shall be returned unmodified
  if (is_int) {
    auto arg = args[0]->integer();
    auto res = v.integer();
    if (arg != res) {
      state
          .error<EvalError>(
              "Due to a bug (see https://github.com/NixOS/nix/issues/12899) a loss of precision "
              "occurred in previous Nix versions because the NixInt argument %1% was rounded to "
              "%2%.\n\tFuture Nix versions might implement the correct behavior.",
              arg, res)
          .at_pos(pos)
          .debugThrow();
    }
  }
}

static RegisterPrimOp primop_floor({
    .name = "__floor",
    .args = {"number"},
    .doc = R"(
        Rounds and converts *number* to the next lower NixInt value if possible, i.e. `floor *number* <= *number*` and
        `*number* - floor *number* < 1`.

        An evaluation error is thrown, if there exists no such NixInt value `floor *number*`.
        Due to bugs in previous Nix versions an evaluation error might be thrown, if the datatype of *number* is
        a NixInt and if `*number* < -9007199254740992` or `*number* > 9007199254740992`.

        If the datatype of *number* is neither a NixInt (signed 64-bit integer) nor a NixFloat
        (IEEE-754 double-precision floating-point number), an evaluation error is thrown.
    )",
    .fun = prim_floor,
});

/* Try evaluating the argument. Success => {success=true; value=something;},
 * else => {success=false; value=false;} */
static void prim_try_eval(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  auto attrs = state.buildBindings(2);

  /* increment state.trylevel, and decrement it when this function returns. */
  maintain_count_t trylevel(state.trylevel);

  ReplExitStatus (*savedDebugRepl)(ref<eval_state_t> es, const ValMap& extraEnv) = nullptr;
  if (state.debugRepl && state.settings.ignoreExceptionsDuringTry) {
    /* to prevent starting the repl from exceptions within a tryEval, null it. */
    savedDebugRepl = state.debugRepl;
    state.debugRepl = nullptr;
  }

  try {
    state.forceValue(*args[0], pos);
    attrs.insert(state.s.value, args[0]);
    attrs.insert(state.symbols.create("success"), &value_t::vTrue);
  } catch (AssertionError& e) {
    // `value = false;` is unfortunate but removing it is a breaking change.
    attrs.insert(state.s.value, &value_t::vFalse);
    attrs.insert(state.symbols.create("success"), &value_t::vFalse);
  }

  // restore the debugRepl pointer if we saved it earlier.
  if (savedDebugRepl) {
    state.debugRepl = savedDebugRepl;
  }

  v.mkAttrs(attrs);
}

static RegisterPrimOp primop_try_eval({
    .name = "__tryEval",
    .args = {"e"},
    .doc = R"(
      Try to shallowly evaluate *e*. Return a set containing the
      attributes `success` (`true` if *e* evaluated successfully,
      `false` if an error was thrown) and `value`, equalling *e* if
      successful and `false` otherwise. `tryEval` only prevents
      errors created by `throw` or `assert` from being thrown.
      Errors that `tryEval` doesn't catch are, for example, those created
      by `abort` and type errors generated by builtins. Also note that
      this doesn't evaluate *e* deeply, so `let e = { x = throw ""; };
      in (builtins.tryEval e).success` is `true`. Using
      `builtins.deepSeq` one can get the expected result:
      `let e = { x = throw ""; }; in
      (builtins.tryEval (builtins.deepSeq e e)).success` is
      `false`.

      `tryEval` intentionally does not return the error message, because that risks bringing non-determinism into the evaluation result, and it would become very difficult to improve error reporting without breaking existing expressions.
      Instead, use [`builtins.addErrorContext`](@docroot@/language/builtins.md#builtins-addErrorContext) to add context to the error message, and use a Nix unit testing tool for testing.
    )",
    .fun = prim_try_eval,
});

/* Return an environment variable.  use with care. */
static void prim_get_env(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  std::string name(state.forceStringNoCtx(
      *args[0], pos, "while evaluating the first argument passed to builtins.getEnv"));
  v.mk_string(state.settings.restrictEval || state.settings.pureEval ? ""
                                                                     : get_env(name).value_or(""),
              state.mem);
}

static RegisterPrimOp primop_get_env({
    .name = "__getEnv",
    .args = {"s"},
    .doc = R"(
      `get_env` returns the value of the environment variable *s*, or an
      empty string if the variable doesn’t exist. This function should be
      used with care, as it can introduce all sorts of nasty environment
      dependencies in your Nix expression.

      `get_env` is used in Nix Packages to locate the file
      `~/.nixpkgs/config.nix`, which contains user-local settings for Nix
      Packages. (That is, it does a `get_env "HOME"` to locate the user’s
      home directory.)
    )",
    .fun = prim_get_env,
});

/* Evaluate the first argument, then return the second argument. */
static void prim_seq(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceValue(*args[0], pos);
  state.forceValue(*args[1], pos);
  v = *args[1];
}

static RegisterPrimOp primop_seq({
    .name = "__seq",
    .args = {"e1", "e2"},
    .doc = R"(
      Evaluate *e1*, then evaluate and return *e2*. This ensures that a
      computation is strict in the value of *e1*.
    )",
    .fun = prim_seq,
});

/* Evaluate the first argument deeply (i.e. recursing into lists and
   attrsets), then return the second argument. */
static void prim_deep_seq(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceValueDeep(*args[0]);
  state.forceValue(*args[1], pos);
  v = *args[1];
}

static RegisterPrimOp primop_deep_seq({
    .name = "__deepSeq",
    .args = {"e1", "e2"},
    .doc = R"(
      This is like `seq e1 e2`, except that *e1* is evaluated *deeply*:
      if it’s a list or set, its elements or attributes are also
      evaluated recursively.
    )",
    .fun = prim_deep_seq,
});

/* Evaluate the first expression and print it on standard error.  Then
   return the second expression.  Useful for debugging. */
static void prim_trace(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceValue(*args[0], pos);
  if (args[0]->type() == nString) {
    printError("trace: %1%", args[0]->string_view());
  } else {
    printError("trace: %1%", ValuePrinter(state, *args[0]));
  }
  if (state.settings.builtinsTraceDebugger) {
    state.runDebugRepl(nullptr);
  }
  state.forceValue(*args[1], pos);
  v = *args[1];
}

static RegisterPrimOp primop_trace({
    .name = "__trace",
    .args = {"e1", "e2"},
    .doc = R"(
      Evaluate *e1* and print its abstract syntax representation on
      standard error. Then return *e2*. This function is useful for
      debugging.

      If the
      [`debugger-on-trace`](@docroot@/command-ref/conf-file.md#conf-debugger-on-trace)
      option is set to `true` and the `--debugger` flag is given, the
      interactive debugger is started when `trace` is called (like
      [`break`](@docroot@/language/builtins.md#builtins-break)).
    )",
    .fun = prim_trace,
});

static void prim_warn(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  // We only accept a string argument for now. The use case for pretty printing a value is covered
  // by `trace`. By rejecting non-strings we allow future versions to add more features without
  // breaking existing code.
  auto msg_str = state.forceString(
      *args[0], pos, "while evaluating the first argument; the message passed to builtins.warn");

  {
    base_error_t msg(std::string{msg_str});
    msg.at_pos(state.positions[pos]);
    auto info = msg.info();
    info.level_ = lvl_warn;
    info.is_from_expr_ = true;
    logWarning(info);
  }

  if (state.settings.builtinsAbortOnWarn) {
    // Not an EvalError or subclass, which would cause the error to be stored in the eval cache.
    state.error<EvalBaseError>("aborting to reveal stack trace of warning, as abort-on-warn is set")
        .setIsFromExpr()
        .debugThrow();
  }
  if (state.settings.builtinsTraceDebugger || state.settings.builtinsDebuggerOnWarn) {
    state.runDebugRepl(nullptr);
  }
  state.forceValue(*args[1], pos);
  v = *args[1];
}

static RegisterPrimOp primop_warn({
    .name = "__warn",
    .args = {"e1", "e2"},
    .doc = R"(
      Evaluate *e1*, which must be a string, and print it on standard error as a warning.
      Then return *e2*.
      This function is useful for non-critical situations where attention is advisable.

      If the
      [`debugger-on-trace`](@docroot@/command-ref/conf-file.md#conf-debugger-on-trace)
      or [`debugger-on-warn`](@docroot@/command-ref/conf-file.md#conf-debugger-on-warn)
      option is set to `true` and the `--debugger` flag is given, the
      interactive debugger is started when `warn` is called (like
      [`break`](@docroot@/language/builtins.md#builtins-break)).

      If the
      [`abort-on-warn`](@docroot@/command-ref/conf-file.md#conf-abort-on-warn)
      option is set, the evaluation is aborted after the warning is printed.
      This is useful to reveal the stack trace of the warning, when the context is non-interactive and a debugger can not be launched.
    )",
    .fun = prim_warn,
});

/* Takes two arguments and evaluates to the second one. Used as the
 * builtins.traceVerbose implementation when --trace-verbose is not enabled
 */
static void prim_second(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceValue(*args[1], pos);
  v = *args[1];
}

/*************************************************************
 * Derivations
 *************************************************************/

static void derivation_strict_internal(eval_state_t& state, std::string_view name,
                                       const bindings_t* attrs, value_t& v);

/* Construct (as a unobservable side effect) a Nix derivation
   expression that performs the derivation described by the argument
   set.  Returns the original set extended with the following
   attributes: `out_path' containing the primary output path of the
   derivation; `drv_path' containing the path of the Nix expression;
   and `type' set to `derivation' to indicate that this is a
   derivation. */
static void prim_derivation_strict(eval_state_t& state, const pos_idx_t pos, value_t** args,
                                   value_t& v) {
  state.forceAttrs(*args[0], pos,
                   "while evaluating the argument passed to builtins.derivationStrict");

  auto attrs = args[0]->attrs();

  /* Figure out the name first (for stack backtraces). */
  auto name_attr = state.get_attr(state.s.name, attrs,
                                  "in the attrset passed as argument to builtins.derivationStrict");

  std::string_view drv_name;
  try {
    drv_name = state.forceStringNoCtx(
        *name_attr->value, pos,
        "while evaluating the `name` attribute passed to builtins.derivationStrict");
  } catch (Error& e) {
    e.add_trace(state.positions[name_attr->pos],
                "while evaluating the derivation attribute 'name'");
    throw;
  }

  try {
    derivation_strict_internal(state, drv_name, attrs, v);
  } catch (Error& e) {
    pos_t pos = state.positions[name_attr->pos];
    /*
     * Here we make two abuses of the error system
     *
     * 1. We print the location as a string to avoid a code snippet being
     * printed. While the location of the name attribute is a good hint, the
     * exact code there is irrelevant.
     *
     * 2. We mark this trace as a frame trace, meaning that we stop printing
     * less important traces from now on. In particular, this prevents the
     * display of the automatic "while calling builtins.derivationStrict"
     * trace, which is of little use for the public we target here.
     *
     * Please keep in mind that error reporting is done on a best-effort
     * basis in nix. There is no accurate location for a derivation, as it
     * often results from the composition of several functions
     * (derivationStrict, derivation, mkDerivation, mkPythonModule, etc.)
     */
    e.add_trace(nullptr, hint_fmt_t("while evaluating derivation '%s'\n"
                                    "  whose name attribute is located at %s",
                                    drv_name, pos));
    throw;
  }
}

/**
 * Early validation for the derivation name, for better error message.
 * It is checked again when constructing store paths.
 *
 * @todo Check that the `.drv` suffix also fits.
 */
static void check_derivation_name(eval_state_t& state, std::string_view drv_name) {
  try {
    check_name(drv_name);
  } catch (BadStorePathName& e) {
    // "Please pass a different name": Users may not be aware that they can
    //     pass a different one, in functions like `fetchurl` where the name
    //     is optional.
    // Note that Nixpkgs generally won't trigger this, because `mkDerivation`
    // sanitizes the name.
    state
        .error<EvalError>("invalid derivation name: %s. Please pass a different '%s'.",
                          uncolored_t(e.message()), "name")
        .debugThrow();
  }
}

static void derivation_strict_internal(eval_state_t& state, std::string_view drv_name,
                                       const bindings_t* attrs, value_t& v) {
  check_derivation_name(state, drv_name);

  /* Check whether attributes should be passed as a JSON file. */
  using nlohmann::json;
  std::optional<StructuredAttrs> json_object;
  auto pos = v.determinePos(no_pos);
  auto attr = attrs->get(state.s.structured_attrs);
  if (attr && state.forceBool(*attr->value, pos,
                              "while evaluating the `__structuredAttrs` "
                              "attribute passed to builtins.derivationStrict")) {
    json_object = StructuredAttrs{};
  }

  /* Check whether null attributes should be ignored. */
  bool ignore_nulls = false;
  attr = attrs->get(state.s.ignore_nulls);
  if (attr) {
    ignore_nulls = state.forceBool(*attr->value, pos,
                                   "while evaluating the `__ignoreNulls` attribute "
                                   "passed to builtins.derivationStrict");
  }

  /* Build the derivation expression by processing the attributes. */
  derivation_t drv;
  drv.name = drv_name;

  NixStringContext context;

  bool content_addressed = false;
  bool is_impure = false;
  std::optional<std::string> outputHash;
  std::optional<hash_algorithm_t> outputHashAlgo;
  std::optional<content_address_method_t> ingestionMethod;

  string_set_t outputs;
  outputs.insert("out");

  for (auto& i : attrs->lexicographicOrder(state.symbols)) {
    if (i->name == state.s.ignore_nulls) {
      continue;
    }
    auto key = state.symbols[i->name];
    vomit("processing attribute '%1%'", key);

    auto handleHashMode = [&](const std::string_view s) {
      if (s == "recursive") {
        // back compat, new name is "nar"
        ingestionMethod = content_address_method_t::raw_t::nix_archive;
      } else {
        try {
          ingestionMethod = content_address_method_t::parse(s);
        } catch (UsageError&) {
          state.error<EvalError>("invalid value '%s' for 'outputHashMode' attribute", s)
              .at_pos(v)
              .debugThrow();
        }
      }
      if (ingestionMethod == content_address_method_t::raw_t::Text) {
        experimental_feature_settings.require(
            xp_t::dynamic_derivations,
            fmt("text-hashed derivation '%s', outputHashMode = \"text\"", drv_name));
      }
      if (ingestionMethod == content_address_method_t::raw_t::git) {
        experimental_feature_settings.require(xp_t::git_hashing);
      }
    };

    auto handleOutputs = [&](const strings_t& ss) {
      outputs.clear();
      for (auto& j : ss) {
        if (outputs.find(j) != outputs.end()) {
          state.error<EvalError>("duplicate derivation output '%1%'", j).at_pos(v).debugThrow();
        }
        /* !!! Check whether j is a valid attribute
           name. */
        /* Derivations cannot be named ‘drv_path’, because
           we already have an attribute ‘drv_path’ in
           the resulting set (see state.sDrvPath). */
        if (j == "drvPath") {
          state.error<EvalError>("invalid derivation output name 'drvPath'").at_pos(v).debugThrow();
        }
        outputs.insert(j);
      }
      if (outputs.empty()) {
        state.error<EvalError>("derivation cannot have an empty set of outputs")
            .at_pos(v)
            .debugThrow();
      }
    };

    try {
      // This try-catch block adds context for most errors.
      // Use this empty error context to signify that we defer to it.
      const std::string_view context_below("");

      if (ignore_nulls) {
        state.forceValue(*i->value, pos);
        if (i->value->type() == nNull) {
          continue;
        }
      }

      switch (i->name.getId()) {
        case eval_state_t::s.content_addressed.getId():
          if (state.forceBool(*i->value, pos, context_below)) {
            content_addressed = true;
            experimental_feature_settings.require(xp_t::ca_derivations);
          }
          break;
        case eval_state_t::s.impure.getId():
          if (state.forceBool(*i->value, pos, context_below)) {
            is_impure = true;
            experimental_feature_settings.require(xp_t::impure_derivations);
          }
          break;
        /* The `args' attribute is special: it supplies the
           command-line arguments to the builder. */
        case eval_state_t::s.args.getId():
          state.forceList(*i->value, pos, context_below);
          for (auto elem : i->value->list_view()) {
            auto s = state
                         .coerceToString(pos, *elem, context,
                                         "while evaluating an element of the argument list", true)
                         .to_owned();
            drv.args.push_back(s);
          }
          break;
        /* All other attributes are passed to the builder through
           the environment. */
        default:

          if (json_object) {
            if (i->name == state.s.structured_attrs) {
              continue;
            }

            json_object->structured_attrs.emplace(
                key, print_value_as_json(state, true, *i->value, pos, context));

            switch (i->name.getId()) {
              case eval_state_t::s.builder.getId():
                drv.builder = state.forceString(*i->value, context, pos, context_below);
                break;
              case eval_state_t::s.system.getId():
                drv.platform = state.forceStringNoCtx(*i->value, pos, context_below);
                break;
              case eval_state_t::s.outputHash.getId():
                outputHash = state.forceStringNoCtx(*i->value, pos, context_below);
                break;
              case eval_state_t::s.outputHashAlgo.getId():
                outputHashAlgo =
                    parse_hash_algo_opt(state.forceStringNoCtx(*i->value, pos, context_below));
                break;
              case eval_state_t::s.outputHashMode.getId():
                handleHashMode(state.forceStringNoCtx(*i->value, pos, context_below));
                break;
              case eval_state_t::s.outputs.getId(): {
                /* Require 'outputs' to be a list of strings. */
                state.forceList(*i->value, pos, context_below);
                strings_t ss;
                for (auto elem : i->value->list_view()) {
                  ss.emplace_back(state.forceStringNoCtx(*elem, pos, context_below));
                }
                handleOutputs(ss);
                break;
              }
              default:
                break;
            }

            switch (i->name.getId()) {
              case eval_state_t::s.allowedReferences.getId():
                warn("In a derivation named '%s', 'structuredAttrs' disables the effect of the "
                     "derivation attribute 'allowedReferences'; use "
                     "'outputChecks.<output>.allowedReferences' instead",
                     drv_name);
                break;
              case eval_state_t::s.allowedRequisites.getId():
                warn("In a derivation named '%s', 'structuredAttrs' disables the effect of the "
                     "derivation attribute 'allowedRequisites'; use "
                     "'outputChecks.<output>.allowedRequisites' instead",
                     drv_name);
                break;
              case eval_state_t::s.disallowedReferences.getId():
                warn("In a derivation named '%s', 'structuredAttrs' disables the effect of the "
                     "derivation attribute 'disallowedReferences'; use "
                     "'outputChecks.<output>.disallowedReferences' instead",
                     drv_name);
                break;
              case eval_state_t::s.disallowedRequisites.getId():
                warn("In a derivation named '%s', 'structuredAttrs' disables the effect of the "
                     "derivation attribute 'disallowedRequisites'; use "
                     "'outputChecks.<output>.disallowedRequisites' instead",
                     drv_name);
                break;
              case eval_state_t::s.max_size.getId():
                warn("In a derivation named '%s', 'structuredAttrs' disables the effect of the "
                     "derivation attribute 'maxSize'; use 'outputChecks.<output>.maxSize' instead",
                     drv_name);
                break;
              case eval_state_t::s.maxClosureSize.getId():
                warn("In a derivation named '%s', 'structuredAttrs' disables the effect of the "
                     "derivation attribute 'maxClosureSize'; use "
                     "'outputChecks.<output>.maxClosureSize' instead",
                     drv_name);
                break;
              default:
                break;
            }

          } else {
            auto s = state.coerceToString(pos, *i->value, context, context_below, true).to_owned();
            if (i->name == state.s.json) {
              warn("In derivation '%s': setting structured attributes via '__json' is deprecated, "
                   "and may be disallowed in future versions of Nix. Set '__structuredAttrs = "
                   "true' instead.",
                   drv_name);
              drv.structured_attrs = StructuredAttrs::parse(s);
            } else {
              drv.env.emplace(key, s);
              switch (i->name.getId()) {
                case eval_state_t::s.builder.getId():
                  drv.builder = std::move(s);
                  break;
                case eval_state_t::s.system.getId():
                  drv.platform = std::move(s);
                  break;
                case eval_state_t::s.outputHash.getId():
                  outputHash = std::move(s);
                  break;
                case eval_state_t::s.outputHashAlgo.getId():
                  outputHashAlgo = parse_hash_algo_opt(s);
                  break;
                case eval_state_t::s.outputHashMode.getId():
                  handleHashMode(s);
                  break;
                case eval_state_t::s.outputs.getId():
                  handleOutputs(tokenize_string<strings_t>(s));
                  break;
                default:
                  break;
              }
            }
          }
          break;
      }

    } catch (Error& e) {
      e.add_trace(
          state.positions[i->pos],
          hint_fmt_t("while evaluating attribute '%1%' of derivation '%2%'", key, drv_name));
      throw;
    }
  }

  if (json_object) {
    /* The only other way `drv.structured_attrs` can be set is when
       `json_object` is not set. */
    assert(!drv.structured_attrs);
    drv.structured_attrs = std::move(*json_object);
  }

  /* Everything in the context of the strings in the derivation
     attributes should be added as dependencies of the resulting
     derivation. */
  string_map_t rewrites;

  std::optional<std::string> drvS;

  for (auto& c : context) {
    std::visit(overloaded{
                   /* Since this allows the builder to gain access to every
                      path in the dependency graph of the derivation (including
                      all outputs), all paths in the graph must be added to
                      this derivation's list of inputs to ensure that they are
                      available when the builder runs. */
                   [&](const NixStringContextElem::DrvDeep& d) {
                     /* !!! This doesn't work if readOnlyMode is set. */
                     store_path_set_t refs;
                     // FIXME: don't need to wait, we only need the references.
                     state.waitForPath(d.drv_path);
                     state.store->computeFSClosure(d.drv_path, refs);
                     for (auto& j : refs) {
                       drv.input_srcs.insert(j);
                       if (j.is_derivation()) {
                         drv.input_drvs.map[j].value =
                             state.store->read_derivation(j).outputNames();
                       }
                     }
                   },
                   [&](const NixStringContextElem::Built& b) {
                     drv.input_drvs.ensureSlot(*b.drv_path).value.insert(b.output);
                   },
                   [&](const NixStringContextElem::opaque_t& o) {
                     drv.input_srcs.insert(state.devirtualize(o.path, &rewrites));
                   },
                   [&](const NixStringContextElem::Path& p) {
                     if (!drvS) {
                       drvS = drv.unparse(*state.store, true);
                     }
                     if (drvS->find(p.store_path.to_string()) != drvS->npos) {
                       auto devirtualized = state.devirtualize(p.store_path, &rewrites);
                       warn("Using 'builtins.derivation' to create a derivation named '%s' that "
                            "references the store path '%s' without a proper context. "
                            "The resulting derivation will not have a correct store reference, so "
                            "this is unreliable and may stop working in the future.",
                            drv_name, state.store->printStorePath(devirtualized));
                     }
                   },
               },
               c.raw);
  }

  drv.applyRewrites(rewrites);

  /* Do we have all required attributes? */
  if (drv.builder == "") {
    state.error<EvalError>("required attribute 'builder' missing").at_pos(v).debugThrow();
  }

  if (drv.platform == "") {
    state.error<EvalError>("required attribute 'system' missing").at_pos(v).debugThrow();
  }

  /* Check whether the derivation name is valid. */
  if (is_derivation(drv_name) && !(ingestionMethod == content_address_method_t::raw_t::Text &&
                                   outputs.size() == 1 && *(outputs.begin()) == "out")) {
    state
        .error<EvalError>("derivation names are allowed to end in '%s' only if they produce a "
                          "single derivation file",
                          drvExtension)
        .at_pos(v)
        .debugThrow();
  }

  if (outputHash) {
    /* Handle fixed-output derivations.

       Ignore `__contentAddressed` because fixed output derivations are
       already content addressed. */
    if (outputs.size() != 1 || *(outputs.begin()) != "out") {
      state.error<EvalError>("multiple outputs are not supported in fixed-output derivations")
          .at_pos(v)
          .debugThrow();
    }

    auto h = new_hash_allow_empty(*outputHash, outputHashAlgo);

    auto method = ingestionMethod.value_or(content_address_method_t::raw_t::flat);

    derivation_output_t::CAFixed dof{
        .ca =
            content_address_t{
                .method = std::move(method),
                .hash = std::move(h),
            },
    };

    drv.env["out"] = state.store->printStorePath(dof.path(*state.store, drv_name, "out"));
    drv.outputs.insert_or_assign("out", std::move(dof));
  }

  else if (content_addressed || is_impure) {
    if (content_addressed && is_impure) {
      state.error<EvalError>("derivation cannot be both content-addressed and impure")
          .at_pos(v)
          .debugThrow();
    }

    auto ha = outputHashAlgo.value_or(hash_algorithm_t::SHA256);
    auto method = ingestionMethod.value_or(content_address_method_t::raw_t::nix_archive);

    for (auto& i : outputs) {
      drv.env[i] = hash_placeholder(i);
      if (is_impure) {
        drv.outputs.insert_or_assign(i, derivation_output_t::Impure{
                                            .method = method,
                                            .hash_algo = ha,
                                        });
      } else {
        drv.outputs.insert_or_assign(i, derivation_output_t::CAFloating{
                                            .method = method,
                                            .hash_algo = ha,
                                        });
      }
    }
  }

  else {
    /* Compute a hash over the "masked" store derivation, which is
       the final one except that in the list of outputs, the
       output paths are empty strings, and the corresponding
       environment variables have an empty value.  This ensures
       that changes in the set of output names do get reflected in
       the hash. */
    for (auto& i : outputs) {
      drv.env[i] = "";
      drv.outputs.insert_or_assign(i, derivation_output_t::Deferred{});
    }

    drv.fillInOutputPaths(*state.store);
  }

  /* Write the resulting term into the Nix store directory. */
  auto drv_path = write_derivation(*state.store, *state.async_path_writer, drv, state.repair);
  auto drv_path_s = state.store->printStorePath(drv_path);

  printMsg(lvl_chatty, "instantiated '%1%' -> '%2%'", drv_name, drv_path_s);

  /* Optimisation, but required in read-only mode! because in that
     case we don't actually write store derivations, so we can't
     read them later. */
  {
    auto h = hash_derivation_modulo(*state.store, drv, false);
    drv_hashes.insert_or_assign(drv_path, std::move(h));
  }

  auto result = state.buildBindings(1 + drv.outputs.size());
  result.alloc(state.s.drv_path)
      .mk_string(drv_path_s,
                 {
                     NixStringContextElem::DrvDeep{.drv_path = drv_path},
                 },
                 state.mem);
  for (auto& i : drv.outputs) {
    mk_output_string(state, result, drv_path, i);
  }

  v.mkAttrs(result);
}

static RegisterPrimOp primop_derivation_strict(PrimOp{
    .name = "derivationStrict",
    .arity = 1,
    .fun = prim_derivation_strict,
});

/* Return a placeholder string for the specified output that will be
   substituted by the corresponding output path at build time. For
   example, 'placeholder "out"' returns the string
   /1rz4g4znpzjwh1xymhjpm42vipw92pr73vdgl6xs1hycac8kf2n9. At build
   time, any occurrence of this string in an derivation attribute will
   be replaced with the concrete path in the Nix store of the output
   ‘out’. */
static void prim_placeholder(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  v.mk_string(
      hash_placeholder(state.forceStringNoCtx(
          *args[0], pos, "while evaluating the first argument passed to builtins.placeholder")),
      state.mem);
}

static RegisterPrimOp primop_placeholder({
    .name = "placeholder",
    .args = {"output"},
    .doc = R"(
      Return an
      [output placeholder string](@docroot@/store/derivation/index.md#output-placeholder)
      for the specified *output* that will be substituted by the corresponding
      [output path](@docroot@/glossary.md#gloss-output-path)
      at build time.

      Typical outputs would be `"out"`, `"bin"` or `"dev"`.
    )",
    .fun = prim_placeholder,
});

/*************************************************************
 * Paths
 *************************************************************/

/* Convert the argument to a path and then to a string (confusing,
   eh?).  !!! obsolete? */
static void prim_to_path(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  NixStringContext context;
  auto path = state.coerceToPath(pos, *args[0], context,
                                 "while evaluating the first argument passed to builtins.toPath");
  v.mk_string(path.path.abs(), context, state.mem);
}

static RegisterPrimOp primop_to_path({
    .name = "__toPath",
    .args = {"s"},
    .doc = R"(
      **DEPRECATED.** use `/. + "/path"` to convert a string into an absolute
      path. For relative paths, use `./. + "/path"`.
    )",
    .fun = prim_to_path,
});

/* Allow a valid store path to be used in an expression.  This is
   useful in some generated expressions such as in nix-push, which
   generates a call to a function with an already existing store path
   as argument.  You don't want to use `toPath' here because it copies
   the path to the Nix store, which yields a copy like
   /nix/store/newhash-oldhash-oldname.  In the past, `to_path' had
   special case behaviour for store paths, but that created weird
   corner cases. */
static void prim_store_path(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  if (state.settings.pureEval) {
    state.error<EvalError>("'%s' is not allowed in pure evaluation mode", "builtins.storePath")
        .at_pos(pos)
        .debugThrow();
  }

  NixStringContext context;
  auto path =
      state
          .coerceToPath(pos, *args[0], context,
                        "while evaluating the first argument passed to 'builtins.storePath'")
          .path;
  /* Resolve symlinks in ‘path’, unless ‘path’ itself is a symlink
     directly in the store.  The latter condition is necessary so
     e.g. nix-push does the right thing. */
  if (!state.store->isStorePath(path.abs())) {
    path = canon_path_t(canon_path(path.abs(), true));
  }
  if (!state.store->isInStore(path.abs())) {
    state.error<EvalError>("path '%1%' is not in the Nix store", path).at_pos(pos).debugThrow();
  }
  auto path2 = state.store->toStorePath(path.abs()).first;
  if (!settings.readOnlyMode) {
    state.store->ensure_path(path2);
  }
  context.insert(NixStringContextElem::opaque_t{.path = path2});
  v.mk_string(path.abs(), context, state.mem);
}

static RegisterPrimOp primop_store_path({
    .name = "__storePath",
    .args = {"path"},
    .doc = R"(
      This function allows you to define a dependency on an already
      existing store path. For example, the derivation attribute `src
      = builtins.store_path /nix/store/f1d18v1y…-source` causes the
      derivation to depend on the specified path, which must exist or
      be substitutable. Note that this differs from a plain path
      (e.g. `src = /nix/store/f1d18v1y…-source`) in that the latter
      causes the path to be *copied* again to the Nix store, resulting
      in a new path (e.g. `/nix/store/ld01dnzc…-source-source`).

      Not available in [pure evaluation mode](@docroot@/command-ref/conf-file.md#conf-pure-eval).

      See also [`builtins.fetchClosure`](#builtins-fetchClosure).
    )",
    .fun = prim_store_path,
});

static void prim_path_exists(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  try {
    auto& arg = *args[0];

    /* source_path_t doesn't know about trailing slash. */
    state.forceValue(arg, pos);
    auto must_be_dir = arg.type() == nString &&
                       (arg.string_view().ends_with("/") || arg.string_view().ends_with("/."));

    auto symlink_resolution =
        must_be_dir ? symlink_resolution_t::full : symlink_resolution_t::ancestors;
    auto path = realise_path(state, pos, arg, symlink_resolution);

    auto st = path.maybe_lstat();
    auto exists = st && (!must_be_dir || st->type == source_accessor_t::t_directory);
    v.mkBool(exists);
  } catch (RestrictedPathError& e) {
    v.mkBool(false);
  }
}

static RegisterPrimOp primop_path_exists({
    .name = "__pathExists",
    .args = {"path"},
    .doc = R"(
      Return `true` if the path *path* exists at evaluation time, and
      `false` otherwise.
    )",
    .fun = prim_path_exists,
});

// Ideally, all trailing slashes should have been removed, but it's been like this for
// almost a decade as of writing. Changing it will affect reproducibility.
static std::string_view legacy_base_name_of(std::string_view path) {
  if (path.empty()) {
    return "";
  }

  auto last = path.size() - 1;
  if (path[last] == '/' && last > 0) {
    last -= 1;
  }

  auto pos = path.rfind('/', last);
  if (pos == path.npos) {
    pos = 0;
  } else {
    pos += 1;
  }

  return path.substr(pos, last - pos + 1);
}

/* Return the base name of the given string, i.e., everything
   following the last slash. */
static void prim_base_name_of(eval_state_t& state, const pos_idx_t pos, value_t** args,
                              value_t& v) {
  NixStringContext context;
  v.mk_string(
      legacy_base_name_of(*state.coerceToString(
          pos, *args[0], context,
          "while evaluating the first argument passed to builtins.baseNameOf", false, false)),
      context, state.mem);
}

static RegisterPrimOp primop_base_name_of({
    .name = "baseNameOf",
    .args = {"x"},
    .doc = R"(
      Return the *base name* of either a [path value](@docroot@/language/types.md#type-path) *x* or a string *x*, depending on which type is passed, and according to the following rules.

      For a path value, the *base name* is considered to be the part of the path after the last directory separator, including any file extensions.
      This is the simple case, as path values don't have trailing slashes.

      When the argument is a string, a more involved logic applies. If the string ends with a `/`, only this one final slash is removed.

      After this, the *base name* is returned as previously described, assuming `/` as the directory separator. (Note that evaluation must be platform independent.)

      This is somewhat similar to the [GNU `basename`](https://www.gnu.org/software/coreutils/manual/html_node/basename-invocation.html) command, but GNU `basename` strips any number of trailing slashes.
    )",
    .fun = prim_base_name_of,
});

/* Return the directory of the given path, i.e., everything before the
   last slash.  Return either a path or a string depending on the type
   of the argument. */
static void prim_dir_of(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceValue(*args[0], pos);
  if (args[0]->type() == nPath) {
    auto path = args[0]->path();
    v.mkPath(path.path.is_root() ? path : path.parent(), state.mem);
  } else {
    NixStringContext context;
    auto path = state.coerceToString(
        pos, *args[0], context, "while evaluating the first argument passed to 'builtins.dirOf'",
        false, false);
    auto pos = path->rfind('/');
    if (pos == path->npos) {
      v.mkStringMove("."_sds, context, state.mem);
    } else if (pos == 0) {
      v.mkStringMove("/"_sds, context, state.mem);
    } else {
      v.mk_string(path->substr(0, pos), context, state.mem);
    }
  }
}

static RegisterPrimOp primop_dir_of({
    .name = "dirOf",
    .args = {"s"},
    .doc = R"(
      Return the directory part of the string *s*, that is, everything
      before the final slash in the string. This is similar to the GNU
      `dirname` command.
    )",
    .fun = prim_dir_of,
});

/* Return the contents of a file as a string. */
static void prim_read_file(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  auto path = realise_path(state, pos, *args[0]);
  auto s = path.read_file();
  if (s.find((char)0) != std::string::npos) {
    state
        .error<EvalError>("the contents of the file '%1%' cannot be represented as a Nix string",
                          path)
        .at_pos(pos)
        .debugThrow();
  }
  store_path_set_t refs;
  if (state.store->isInStore(path.path.abs())) {
    auto store_path = state.store->toStorePath(path.path.abs()).first;
    // Skip virtual paths since they don't have references and
    // don't exist anyway.
    if (!state.storeFS->get_mount(canon_path_t(state.store->printStorePath(store_path)))) {
      if (auto info =
              state.store->maybeQueryPathInfo(state.store->toStorePath(path.path.abs()).first)) {
        // Re-scan references to filter down to just the ones that actually occur in the file.
        auto refs_sink = PathRefScanSink::fromPaths(info->references);
        refs_sink << s;
        refs = refs_sink.getResultPaths();
      }
    }
  }
  NixStringContext context;
  for (auto&& p : std::move(refs)) {
    context.insert(NixStringContextElem::opaque_t{
        .path = std::move((store_path_t&&)p),
    });
  }
  v.mk_string(s, context, state.mem);
}

static RegisterPrimOp primop_read_file({
    .name = "__readFile",
    .args = {"path"},
    .doc = R"(
      Return the contents of the file *path* as a string.
    )",
    .fun = prim_read_file,
});

/* Find a file in the Nix search path. Used to implement <x> paths,
   which are desugared to 'findFile __nixPath "x"'. */
static void prim_find_file(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceList(*args[0], pos, "while evaluating the first argument passed to builtins.findFile");

  LookupPath lookup_path;

  for (auto v2 : args[0]->list_view()) {
    state.forceAttrs(*v2, pos,
                     "while evaluating an element of the list passed to builtins.findFile");

    std::string prefix;
    auto i = v2->attrs()->get(state.s.prefix);
    if (i) {
      prefix = state.forceStringNoCtx(*i->value, pos,
                                      "while evaluating the `prefix` attribute of an element of "
                                      "the list passed to builtins.findFile");
    }

    i = state.get_attr(state.s.path, v2->attrs(), "in an element of the __nixPath");

    NixStringContext context;
    auto path = state
                    .coerceToString(pos, *i->value, context,
                                    "while evaluating the `path` attribute of an element of the "
                                    "list passed to builtins.findFile",
                                    false, false)
                    .to_owned();

    try {
      auto rewrites = state.realiseContext(context);
      path = rewrite_strings(std::move(path), rewrites);
    } catch (InvalidPathError& e) {
      state.error<EvalError>("cannot find '%1%', since path '%2%' is not valid", path, e.path)
          .at_pos(pos)
          .debugThrow();
    }

    lookup_path.elements.emplace_back(LookupPath::Elem{
        .prefix = LookupPath::Prefix{.s = std::move(prefix)},
        .path = LookupPath::Path{.s = std::move(path)},
    });
  }

  auto path = state.forceStringNoCtx(
      *args[1], pos, "while evaluating the second argument passed to builtins.findFile");

  v.mkPath(state.findFile(lookup_path, path, pos), state.mem);
}

static RegisterPrimOp primop_find_file(PrimOp{
    .name = "__findFile",
    .args = {"search-path", "lookup-path"},
    .doc = R"(
      Find *lookup-path* in *search-path*.

      [Lookup path](@docroot@/language/constructs/lookup-path.md) expressions are [desugared](https://en.wikipedia.org/wiki/Syntactic_sugar) using this and [`builtins.nixPath`](#builtins-nixPath):

      ```nix
      <nixpkgs>
      ```

      is equivalent to:

      ```nix
      builtins.findFile builtins.nixPath "nixpkgs"
      ```

      A search path is represented as a list of [attribute sets](./types.md#type-attrs) with two attributes:
      - `prefix` is a relative path.
      - `path` denotes a file system location

      Examples of search path attribute sets:

      - ```
        {
          prefix = "";
          path = "/nix/var/nix/profiles/per-user/root/channels";
        }
        ```
      - ```
        {
          prefix = "nixos-config";
          path = "/etc/nixos/configuration.nix";
        }
        ```
      - ```
        {
          prefix = "nixpkgs";
          path = "https://github.com/NixOS/nixpkgs/tarballs/master";
        }
        ```
      - ```
        {
          prefix = "nixpkgs";
          path = "channel:nixpkgs-unstable";
        }
        ```
      - ```
        {
          prefix = "flake-compat";
          path = "flake:github:edolstra/flake-compat";
        }
        ```

      The lookup algorithm checks each entry until a match is found, returning a [path value](@docroot@/language/types.md#type-path) of the match:

      - If a prefix of `lookup-path` matches `prefix`, then the remainder of *lookup-path* (the "suffix") is searched for within the directory denoted by `path`.
        The contents of `path` may need to be downloaded at this point to look inside.

      - If the suffix is found inside that directory, then the entry is a match.
        The combined absolute path of the directory (now downloaded if need be) and the suffix is returned.

      > **Example**
      >
      > A *search-path* value
      >
      > ```
      > [
      >   {
      >     prefix = "";
      >     path = "/home/eelco/Dev";
      >   }
      >   {
      >     prefix = "nixos-config";
      >     path = "/etc/nixos";
      >   }
      > ]
      > ```
      >
      > and a *lookup-path* value `"nixos-config"` causes Nix to try `/home/eelco/Dev/nixos-config` and `/etc/nixos` in that order and return the first path that exists.

      If `path` starts with `http://` or `https://`, it is interpreted as the URL of a tarball to be downloaded and unpacked to a temporary location.
      The tarball must consist of a single top-level directory.

      The URLs of the tarballs from the official `nixos.org` channels can be abbreviated as `channel:<channel-name>`.
      See [documentation on `nix-channel`](@docroot@/command-ref/nix-channel.md) for details about channels.

      > **Example**
      >
      > These two search path entries are equivalent:
      >
      > - ```
      >   {
      >     prefix = "nixpkgs";
      >     path = "channel:nixpkgs-unstable";
      >   }
      >   ```
      > - ```
      >   {
      >     prefix = "nixpkgs";
      >     path = "https://channels.nixos.org/nixos-unstable/nixexprs.tar.xz";
      >   }
      >   ```

      Search paths can also point to source trees using [flake URLs](@docroot@/command-ref/new-cli/nix3-flake.md#url-like-syntax).


      > **Example**
      >
      > The search path entry
      >
      > ```
      > {
      >   prefix = "nixpkgs";
      >   path = "flake:nixpkgs";
      > }
      > ```
      > specifies that the prefix `nixpkgs` shall refer to the source tree downloaded from the `nixpkgs` entry in the flake registry.
      >
      > Similarly
      >
      > ```
      > {
      >   prefix = "nixpkgs";
      >   path = "flake:github:nixos/nixpkgs/nixos-22.05";
      > }
      > ```
      >
      > makes `<nixpkgs>` refer to a particular branch of the `NixOS/nixpkgs` repository on GitHub.
    )",
    .fun = prim_find_file,
});

/* Return the cryptographic hash of a file in base-16. */
static void prim_hash_file(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  auto algo = state.forceStringNoCtx(
      *args[0], pos, "while evaluating the first argument passed to builtins.hashFile");
  std::optional<hash_algorithm_t> ha = parse_hash_algo(algo);
  if (!ha) {
    state.error<EvalError>("unknown hash algorithm '%1%'", algo).at_pos(pos).debugThrow();
  }

  auto path = realise_path(state, pos, *args[1]);

  v.mk_string(hash_string(*ha, path.read_file()).to_string(hash_format_t::base16, false),
              state.mem);
}

static RegisterPrimOp primop_hash_file({
    .name = "__hashFile",
    .args = {"type", "p"},
    .doc = R"(
      Return a base-16 representation of the cryptographic hash of the
      file at path *p*. The hash algorithm specified by *type* must be one
      of `"md5"`, `"sha1"`, `"sha256"` or `"sha512"`.
    )",
    .fun = prim_hash_file,
});

static const value_t& file_type_to_string(eval_state_t& state, source_accessor_t::Type type) {
  struct constants_t {
    value_t regular;
    value_t directory;
    value_t symlink;
    value_t unknown;
  };

  static const constants_t string_values = []() {
    constants_t res;
    res.regular.mkStringNoCopy("regular"_sds);
    res.directory.mkStringNoCopy("directory"_sds);
    res.symlink.mkStringNoCopy("symlink"_sds);
    res.unknown.mkStringNoCopy("unknown"_sds);
    return res;
  }();

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
  using enum source_accessor_t::Type;
  switch (type) {
    case t_regular:
      return string_values.regular;
    case t_directory:
      return string_values.directory;
    case t_symlink:
      return string_values.symlink;
    default:
      return string_values.unknown;
  }
}

static void prim_read_file_type(eval_state_t& state, const pos_idx_t pos, value_t** args,
                                value_t& v) {
  auto path = realise_path(state, pos, *args[0], std::nullopt);
  /* Retrieve the directory entry type and stringize it. */
  v = file_type_to_string(state, path.lstat().type);
}

static RegisterPrimOp primop_read_file_type({
    .name = "__readFileType",
    .args = {"p"},
    .doc = R"(
      Determine the directory entry type of a filesystem node, being
      one of `"directory"`, `"regular"`, `"symlink"`, or `"unknown"`.
    )",
    .fun = prim_read_file_type,
});

/* Read a directory (without . or ..) */
static void prim_read_dir(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  auto path = realise_path(state, pos, *args[0]);

  // Retrieve directory entries for all nodes in a directory.
  // This is similar to `getFileType` but is optimized to reduce system calls
  // on many systems.
  auto entries = path.read_directory();
  auto attrs = state.buildBindings(entries.size());

  // If we hit unknown directory entry types we may need to fallback to
  // using `getFileType` on some systems.
  // In order to reduce system calls we make each lookup lazy by using
  // `builtins.readFileType` application.
  value_t* read_file_type = nullptr;

  for (auto& [name, type] : entries) {
    if (!type) {
      auto& attr = attrs.alloc(name);
      // Some filesystems or operating systems may not be able to return
      // detailed node info quickly in this case we produce a thunk to
      // query the file type lazily.
      auto epath = state.allocValue();
      epath->mkPath(path / name, state.mem);
      if (!read_file_type) {
        read_file_type = &state.getBuiltin("readFileType");
      }
      attr.mkApp(read_file_type, epath);
    } else {
      // This branch of the conditional is much more likely.
      // Here we just stringize the directory entry type.
      // N.B. const_cast here is ok, because these values will never be modified, since
      // only thunks are mutable - other types do not change once constructed.
      attrs.insert(state.symbols.create(name),
                   const_cast<value_t*>(&file_type_to_string(state, *type)));
    }
  }

  v.mkAttrs(attrs);
}

static RegisterPrimOp primop_read_dir({
    .name = "__readDir",
    .args = {"path"},
    .doc = R"(
      Return the contents of the directory *path* as a set mapping
      directory entries to the corresponding file type. For instance, if
      directory `A` contains a regular file `B` and another directory
      `C`, then `builtins.readDir ./A` returns the set

      ```nix
      { B = "regular"; C = "directory"; }
      ```

      The possible values for the file type are `"regular"`,
      `"directory"`, `"symlink"` and `"unknown"`.
    )",
    .fun = prim_read_dir,
});

/* Extend single element string context with another output. */
static void prim_output_of(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  SingleDerivedPath drv_path = state.coerceToSingleDerivedPath(
      pos, *args[0], "while evaluating the first argument to builtins.outputOf");

  OutputNameView output_name = state.forceStringNoCtx(
      *args[1], pos, "while evaluating the second argument to builtins.outputOf");

  state.mkSingleDerivedPathString(
      SingleDerivedPath::Built{
          .drv_path = make_ref<SingleDerivedPath>(drv_path),
          .output = std::string{output_name},
      },
      v);
}

static RegisterPrimOp primop_output_of({
    .name = "__outputOf",
    .args = {"derivation-reference", "output-name"},
    .doc = R"(
      Return the output path of a derivation, literally or using an
      [input placeholder string](@docroot@/store/derivation/index.md#input-placeholder)
      if needed.

      If the derivation has a statically-known output path (i.e. the derivation output is input-addressed, or fixed content-addressed), the output path is returned.
      But if the derivation is content-addressed or if the derivation is itself not-statically produced (i.e. is the output of another derivation), an input placeholder is returned instead.

      *`derivation reference`* must be a string that may contain a regular store path to a derivation, or may be an input placeholder reference.
      If the derivation is produced by a derivation, you must explicitly select `drv.out_path`.
      This primop can be chained arbitrarily deeply.
      For instance,

      ```nix
      builtins.outputOf
        (builtins.outputOf myDrv "out")
        "out"
      ```

      returns an input placeholder for the output of the output of `myDrv`.

      This primop corresponds to the `^` sigil for [deriving paths](@docroot@/glossary.md#gloss-deriving-path), e.g. as part of installable syntax on the command line.
    )",
    .fun = prim_output_of,
    .experimental_feature = xp_t::dynamic_derivations,
});

/*************************************************************
 * Creating files
 *************************************************************/

/* Convert the argument (which can be any Nix expression) to an XML
   representation returned in a string.  Not all Nix expressions can
   be sensibly or completely represented (e.g., functions). */
static void prim_to_xml(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  std::ostringstream out;
  NixStringContext context;
  print_value_as_xml(state, true, false, *args[0], out, context, pos);
  v.mk_string(out.str(), context, state.mem);
}

static RegisterPrimOp primop_to_xml({
    .name = "__toXML",
    .args = {"e"},
    .doc = R"(
      Return a string containing an XML representation of *e*. The main
      application for `toXML` is to communicate information with the
      builder in a more structured format than plain environment
      variables.

      Here is an example where this is the case:

      ```nix
      { stdenv, fetchurl, libxslt, jira, uberwiki }:

      stdenv.mkDerivation (rec {
        name = "web-server";

        buildInputs = [ libxslt ];

        builder = builtins.toFile "builder.sh" "
          source $stdenv/setup
          mkdir $out
          echo "$servlets" | xsltproc ${stylesheet} - > $out/server-conf.xml ①
        ";

        stylesheet = builtins.toFile "stylesheet.xsl" ②
         "<?xml version='1.0' encoding='UTF-8'?>
          <xsl:stylesheet xmlns:xsl='http://www.w3.org/1999/XSL/Transform' version='1.0'>
            <xsl:template match='/'>
              <Configure>
                <xsl:for-each select='/expr/list/attrs'>
                  <Call name='addWebApplication'>
                    <Arg><xsl:value-of select=\"attr[@name = 'path']/string/@value\" /></Arg>
                    <Arg><xsl:value-of select=\"attr[@name = 'war']/path/@value\" /></Arg>
                  </Call>
                </xsl:for-each>
              </Configure>
            </xsl:template>
          </xsl:stylesheet>
        ";

        servlets = builtins.toXML [ ③
          { path = "/bugtracker"; war = jira + "/lib/atlassian-jira.war"; }
          { path = "/wiki"; war = uberwiki + "/uberwiki.war"; }
        ];
      })
      ```

      The builder is supposed to generate the configuration file for a
      [Jetty servlet container](http://jetty.mortbay.org/). A servlet
      container contains a number of servlets (`*.war` files) each
      exported under a specific URI prefix. So the servlet configuration
      is a list of sets containing the `path` and `war` of the servlet
      (①). This kind of information is difficult to communicate with the
      normal method of passing information through an environment
      variable, which just concatenates everything together into a
      string (which might just work in this case, but wouldn’t work if
      fields are optional or contain lists themselves). Instead the Nix
      expression is converted to an XML representation with `toXML`,
      which is unambiguous and can easily be processed with the
      appropriate tools. For instance, in the example an XSLT stylesheet
      (at point ②) is applied to it (at point ①) to generate the XML
      configuration file for the Jetty server. The XML representation
      produced at point ③ by `toXML` is as follows:

      ```xml
      <?xml version='1.0' encoding='utf-8'?>
      <expr>
        <list>
          <attrs>
            <attr name="path">
              <string value="/bugtracker" />
            </attr>
            <attr name="war">
              <path value="/nix/store/d1jh9pasa7k2...-jira/lib/atlassian-jira.war" />
            </attr>
          </attrs>
          <attrs>
            <attr name="path">
              <string value="/wiki" />
            </attr>
            <attr name="war">
              <path value="/nix/store/y6423b1yi4sx...-uberwiki/uberwiki.war" />
            </attr>
          </attrs>
        </list>
      </expr>
      ```

      Note that we used the `toFile` built-in to write the builder and
      the stylesheet “inline” in the Nix expression. The path of the
      stylesheet is spliced into the builder using the syntax `xsltproc
      ${stylesheet}`.
    )",
    .fun = prim_to_xml,
});

/* Convert the argument (which can be any Nix expression) to a JSON
   string.  Not all Nix expressions can be sensibly or completely
   represented (e.g., functions). */
static void prim_to_json(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  std::ostringstream out;
  NixStringContext context;
  print_value_as_json(state, true, *args[0], pos, out, context);
  v.mk_string(out.str(), context, state.mem);
}

static RegisterPrimOp primop_to_json({
    .name = "__toJSON",
    .args = {"e"},
    .doc = R"(
      Return a string containing a JSON representation of *e*. strings_t,
      integers, floats, booleans, nulls and lists are mapped to their JSON
      equivalents. Sets (except derivations) are represented as objects.
      Derivations are translated to a JSON string containing the
      derivation’s output path. Paths are copied to the store and
      represented as a JSON string of the resulting store path.
    )",
    .fun = prim_to_json,
});

/* Parse a JSON string to a value. */
static void prim_from_json(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  auto s = state.forceStringNoCtx(
      *args[0], pos, "while evaluating the first argument passed to builtins.fromJSON");
  try {
    parse_json(state, s, v);
  } catch (JSONParseError& e) {
    e.add_trace(state.positions[pos], "while decoding a JSON string");
    throw;
  }
}

static RegisterPrimOp primop_from_json({
    .name = "__fromJSON",
    .args = {"e"},
    .doc = R"(
      Convert a JSON string to a Nix value. For example,

      ```nix
      builtins.from_json ''{"x": [1, 2, 3], "y": null}''
      ```

      returns the value `{ x = [ 1 2 3 ]; y = null; }`.
    )",
    .fun = prim_from_json,
});

/* store_t a string in the Nix store as a source file that can be used
   as an input by derivations. */
static void prim_to_file(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  NixStringContext context;
  auto name = state.forceStringNoCtx(
      *args[0], pos, "while evaluating the first argument passed to builtins.toFile");
  std::string contents(state.forceString(
      *args[1], context, pos, "while evaluating the second argument passed to builtins.toFile"));

  store_path_set_t refs;
  string_map_t rewrites;

  for (auto c : context) {
    if (auto p = std::get_if<NixStringContextElem::opaque_t>(&c.raw)) {
      refs.insert(p->path);
    } else if (auto p = std::get_if<NixStringContextElem::Path>(&c.raw)) {
      if (contents.find(p->store_path.to_string()) != contents.npos) {
        auto devirtualized = state.devirtualize(p->store_path, &rewrites);
        warn("Using 'builtins.toFile' to create a file named '%s' that references the store path "
             "'%s' without a proper context. "
             "The resulting file will not have a correct store reference, so this is unreliable "
             "and may stop working in the future.",
             name, state.store->printStorePath(devirtualized));
      }
    } else {
      state
          .error<EvalError>(
              "files created by %1% may not reference derivations, but %2% references %3%",
              "builtins.toFile", name, c.to_string())
          .at_pos(pos)
          .debugThrow();
    }
  }

  contents = rewrite_strings(contents, rewrites);

  auto store_path =
      settings.readOnlyMode
          ? state.store->makeFixedOutputPathFromCA(
                name,
                TextInfo{
                    .hash = hash_string(hash_algorithm_t::SHA256, contents),
                    .references = std::move(refs),
                })
          : ({
              string_source_t s{contents};
              state.store->add_to_store_from_dump(s, name, file_serialisation_method_t::flat,
                                                  content_address_method_t::raw_t::Text,
                                                  hash_algorithm_t::SHA256, refs, state.repair);
            });

  /* Note: we don't need to add `context' to the context of the
     result, since `store_path' itself has references to the paths
     used in args[1]. */

  /* Add the output of this to the allowed paths. */
  state.allowAndSetStorePathString(store_path, v);
}

static RegisterPrimOp primop_to_file({
    .name = "__toFile",
    .args = {"name", "s"},
    .doc = R"(
      store_t the string *s* in a file in the Nix store and return its
      path.  The file has suffix *name*. This file can be used as an
      input to derivations. One application is to write builders
      “inline”. For instance, the following Nix expression combines the
      Nix expression for GNU Hello and its build script into one file:

      ```nix
      { stdenv, fetchurl, perl }:

      stdenv.mkDerivation {
        name = "hello-2.1.1";

        builder = builtins.toFile "builder.sh" "
          source $stdenv/setup

          PATH=$perl/bin:$PATH

          tar xvfz $src
          cd hello-*
          ./configure --prefix=$out
          make
          make install
        ";

        src = fetchurl {
          url = "http://ftp.nluug.nl/pub/gnu/hello/hello-2.1.1.tar.gz";
          sha256 = "1md7jsfd8pa45z73bz1kszpp01yw6x5ljkjk2hx7wl800any6465";
        };
        inherit perl;
      }
      ```

      It is even possible for one file to refer to another, e.g.,

      ```nix
      builder = let
        configFile = builtins.toFile "foo.conf" "
          # This is some dummy configuration file.
          ...
        ";
      in builtins.toFile "builder.sh" "
        source $stdenv/setup
        ...
        cp ${configFile} $out/etc/foo.conf
      ";
      ```

      Note that `${configFile}` is a
      [string interpolation](@docroot@/language/types.md#type-string), so the result of the
      expression `configFile`
      (i.e., a path like `/nix/store/m7p7jfny445k...-foo.conf`) will be
      spliced into the resulting string.

      It is however *not* allowed to have files mutually referring to each
      other, like so:

      ```nix
      let
        foo = builtins.toFile "foo" "...${bar}...";
        bar = builtins.toFile "bar" "...${foo}...";
      in foo
      ```

      This is not allowed because it would cause a cyclic dependency in
      the computation of the cryptographic hashes for `foo` and `bar`.

      It is also not possible to reference the result of a derivation. If
      you are using Nixpkgs, the `writeTextFile` function is able to do
      that.
    )",
    .fun = prim_to_file,
});

bool eval_state_t::callPathFilter(value_t* filter_fun, const source_path_t& path, pos_idx_t pos) {
  auto st = path.lstat();

  /* Call the filter function.  The first argument is the path, the
     second is a string indicating the type of the file. */
  value_t arg1;
  arg1.mk_string(path.path.abs(), mem);

  // assert that type is not "unknown"
  value_t* args[]{&arg1, const_cast<value_t*>(&file_type_to_string(*this, st.type))};
  value_t res;
  callFunction(*filter_fun, args, res, pos);

  return forceBool(res, pos, "while evaluating the return value of the path filter function");
}

static void add_path(eval_state_t& state, const pos_idx_t pos, std::string_view name,
                     source_path_t path, value_t* filter_fun, content_address_method_t method,
                     const std::optional<Hash> expected_hash, value_t& v,
                     const NixStringContext& context) {
  try {
    store_path_set_t refs;

    if (path.accessor == state.root_fs && state.store->isInStore(path.path.abs()) &&
        !context.empty()) {
      // FIXME: handle CA derivation outputs (where path needs to
      // be rewritten to the actual output).
      auto rewrites = state.realiseContext(context);
      path = {path.accessor, canon_path_t(rewrite_strings(path.path.abs(), rewrites))};
      auto [store_path, subPath] = state.store->toStorePath(path.path.abs());
      try {
        refs = state.store->queryPathInfo(store_path)->references;
      } catch (Error&) { // FIXME: should be InvalidPathError
      }
    }

    std::unique_ptr<path_filter_t> filter;
    if (filter_fun) {
      filter = std::make_unique<path_filter_t>([&](const Path& p) {
        auto p2 = canon_path_t(p);
        return state.callPathFilter(filter_fun, {path.accessor, p2}, pos);
      });
    }

    std::optional<store_path_t> expectedStorePath;
    if (expected_hash) {
      expectedStorePath = state.store->makeFixedOutputPathFromCA(
          name, ContentAddressWithReferences::fromParts(method, *expected_hash, {refs}));
    }

    if (!expected_hash || !state.store->isValidPath(*expectedStorePath)) {
      // FIXME: make this lazy?
      // FIXME: support refs in fetchToStore()?
      auto dst_path =
          refs.empty() ? fetch_to_store(state.fetch_settings, *state.store, path.resolve_symlinks(),
                                        settings.readOnlyMode ? FetchMode::DryRun : FetchMode::Copy,
                                        name, method, filter.get(), state.repair)
                       : state.store->add_to_store(
                             name, path.resolve_symlinks(), method, hash_algorithm_t::SHA256, refs,
                             filter ? *filter.get() : default_path_filter, state.repair);
      if (expected_hash && expectedStorePath != dst_path) {
        state
            .error<EvalError>("store path mismatch in (possibly filtered) path added from '%s'",
                              path)
            .at_pos(pos)
            .debugThrow();
      }
      state.allowAndSetStorePathString(dst_path, v);
    } else {
      state.allowAndSetStorePathString(*expectedStorePath, v);
    }
  } catch (Error& e) {
    e.add_trace(state.positions[pos], "while adding path '%s'", path);
    throw;
  }
}

static void prim_filter_source(eval_state_t& state, const pos_idx_t pos, value_t** args,
                               value_t& v) {
  NixStringContext context;
  auto path = state.coerceToPath(pos, *args[1], context,
                                 "while evaluating the second argument (the path to filter) passed "
                                 "to 'builtins.filterSource'");
  state.forceFunction(*args[0], pos,
                      "while evaluating the first argument passed to builtins.filterSource");

  add_path(state, pos, state.computeBaseName(path, pos), path, args[0],
           content_address_method_t::raw_t::nix_archive, std::nullopt, v, context);
}

static RegisterPrimOp primop_filter_source({
    .name = "__filterSource",
    .args = {"e1", "e2"},
    .doc = R"(
      > **Warning**
      >
      > `filterSource` should not be used to filter store paths. Since
      > `filterSource` uses the name of the input directory while naming
      > the output directory, doing so produces a directory name in
      > the form of `<hash2>-<hash>-<name>`, where `<hash>-<name>` is
      > the name of the input directory. Since `<hash>` depends on the
      > unfiltered directory, the name of the output directory
      > indirectly depends on files that are filtered out by the
      > function. This triggers a rebuild even when a filtered out
      > file is changed. use `builtins.path` instead, which allows
      > specifying the name of the output directory.

      This function allows you to copy sources into the Nix store while
      filtering certain files. For instance, suppose that you want to use
      the directory `source-dir` as an input to a Nix expression, e.g.

      ```nix
      stdenv.mkDerivation {
        ...
        src = ./source-dir;
      }
      ```

      However, if `source-dir` is a Subversion working copy, then all of
      those annoying `.svn` subdirectories are also copied to the
      store. Worse, the contents of those directories may change a lot,
      causing lots of spurious rebuilds. With `filterSource` you can
      filter out the `.svn` directories:

      ```nix
      src = builtins.filterSource
        (path: type: type != "directory" || base_name_of path != ".svn")
        ./source-dir;
      ```

      Thus, the first argument *e1* must be a predicate function that is
      called for each regular file, directory or symlink in the source
      tree *e2*. If the function returns `true`, the file is copied to the
      Nix store, otherwise it is omitted. The function is called with two
      arguments. The first is the full path of the file. The second is a
      string that identifies the type of the file, which is either
      `"regular"`, `"directory"`, `"symlink"` or `"unknown"` (for other
      kinds of files such as device nodes or fifos — but note that those
      cannot be copied to the Nix store, so if the predicate returns
      `true` for them, the copy fails). If you exclude a directory,
      the entire corresponding subtree of *e2* is excluded.
    )",
    .fun = prim_filter_source,
});

static void prim_path(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  std::optional<source_path_t> path;
  std::string_view name;
  value_t* filter_fun = nullptr;
  auto method = content_address_method_t::raw_t::nix_archive;
  std::optional<Hash> expected_hash;
  NixStringContext context;

  state.forceAttrs(*args[0], pos, "while evaluating the argument passed to 'builtins.path'");

  for (auto& attr : *args[0]->attrs()) {
    auto n = state.symbols[attr.name];
    if (n == "path") {
      path.emplace(
          state.coerceToPath(attr.pos, *attr.value, context,
                             "while evaluating the 'path' attribute passed to 'builtins.path'"));
    } else if (attr.name == state.s.name) {
      name = state.forceStringNoCtx(
          *attr.value, attr.pos, "while evaluating the `name` attribute passed to builtins.path");
    } else if (n == "filter") {
      state.forceFunction(*(filter_fun = attr.value), attr.pos,
                          "while evaluating the `filter` parameter passed to builtins.path");
    } else if (n == "recursive") {
      method = state.forceBool(*attr.value, attr.pos,
                               "while evaluating the `recursive` attribute passed to builtins.path")
                   ? content_address_method_t::raw_t::nix_archive
                   : content_address_method_t::raw_t::flat;
    } else if (n == "sha256") {
      expected_hash = new_hash_allow_empty(
          state.forceStringNoCtx(*attr.value, attr.pos,
                                 "while evaluating the `sha256` attribute passed to builtins.path"),
          hash_algorithm_t::SHA256);
    } else {
      state
          .error<EvalError>("unsupported argument '%1%' to 'builtins.path'",
                            state.symbols[attr.name])
          .at_pos(attr.pos)
          .debugThrow();
    }
  }
  if (!path) {
    state
        .error<EvalError>(
            "missing required 'path' attribute in the first argument to 'builtins.path'")
        .at_pos(pos)
        .debugThrow();
  }
  if (name.empty()) {
    name = path->base_name();
  }

  add_path(state, pos, name, *path, filter_fun, method, expected_hash, v, context);
}

static RegisterPrimOp primop_path({
    .name = "__path",
    .args = {"args"},
    .doc = R"(
      An enrichment of the built-in path type, based on the attributes
      present in *args*. All are optional except `path`:

        - path\
          The underlying path.

        - name\
          The name of the path when added to the store. This can used to
          reference paths that have nix-illegal characters in their names,
          like `@`.

        - filter\
          A function of the type expected by [`builtins.filterSource`](#builtins-filterSource),
          with the same semantics.

        - recursive\
          When `false`, when `path` is added to the store it is with a
          flat hash, rather than a hash of the NAR serialization of the
          file. Thus, `path` must refer to a regular file, not a
          directory. This allows similar behavior to `fetchurl`. Defaults
          to `true`.

        - sha256\
          When provided, this is the expected hash of the file at the
          path. Evaluation fails if the hash is incorrect, and
          providing a hash allows `builtins.path` to be used even when the
          `pure-eval` nix config option is on.
    )",
    .fun = prim_path,
});

/*************************************************************
 * Sets
 *************************************************************/

/* Return the names of the attributes in a set as a sorted list of
   strings. */
static void prim_attr_names(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceAttrs(*args[0], pos, "while evaluating the argument passed to builtins.attrNames");

  auto list = state.buildList(args[0]->attrs()->size());

  for (const auto& [n, i] : enumerate(*args[0]->attrs())) {
    list[n] = value_t::toPtr(state.symbols[i.name]);
  }

  std::sort(list.begin(), list.end(),
            [](value_t* v1, value_t* v2) { return v1->string_view() < v2->string_view(); });

  v.mkList(list);
}

static RegisterPrimOp primop_attr_names({
    .name = "__attrNames",
    .args = {"set"},
    .doc = R"(
      Return the names of the attributes in the set *set* in an
      alphabetically sorted list. For instance, `builtins.attrNames { y
      = 1; x = "foo"; }` evaluates to `[ "x" "y" ]`.
    )",
    .fun = prim_attr_names,
});

/* Return the values of the attributes in a set as a list, in the same
   order as attrNames. */
static void prim_attr_values(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceAttrs(*args[0], pos, "while evaluating the argument passed to builtins.attrValues");

  auto list = state.buildList(args[0]->attrs()->size());

  for (const auto& [n, i] : enumerate(*args[0]->attrs())) {
    list[n] = (value_t*)&i;
  }

  std::sort(list.begin(), list.end(), [&](value_t* v1, value_t* v2) {
    std::string_view s1 = state.symbols[((attr_t*)v1)->name],
                     s2 = state.symbols[((attr_t*)v2)->name];
    return s1 < s2;
  });

  for (auto& v : list) {
    v = ((attr_t*)v)->value;
  }

  v.mkList(list);
}

static RegisterPrimOp primop_attr_values({
    .name = "__attrValues",
    .args = {"set"},
    .doc = R"(
      Return the values of the attributes in the set *set* in the order
      corresponding to the sorted attribute names.
    )",
    .fun = prim_attr_values,
});

/* Dynamic version of the `.' operator. */
void prim_get_attr(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  auto attr = state.forceStringNoCtx(
      *args[0], pos, "while evaluating the first argument passed to builtins.getAttr");
  state.forceAttrs(*args[1], pos,
                   "while evaluating the second argument passed to builtins.getAttr");
  auto i = state.get_attr(state.symbols.create(attr), args[1]->attrs(),
                          "in the attribute set under consideration");
  // !!! add to stack trace?
  if (state.countCalls && i->pos) {
    state.attrSelects[i->pos]++;
  }
  state.forceValue(*i->value, pos);
  v = *i->value;
}

static RegisterPrimOp primop_get_attr({
    .name = "__getAttr",
    .args = {"s", "set"},
    .doc = R"(
      `get_attr` returns the attribute named *s* from *set*. Evaluation
      aborts if the attribute doesn’t exist. This is a dynamic version of
      the `.` operator, since *s* is an expression rather than an
      identifier.
    )",
    .fun = prim_get_attr,
});

/* Return position information of the specified attribute. */
static void prim_unsafe_get_attr_pos(eval_state_t& state, const pos_idx_t pos, value_t** args,
                                     value_t& v) {
  auto attr = state.forceStringNoCtx(
      *args[0], pos, "while evaluating the first argument passed to builtins.unsafeGetAttrPos");
  state.forceAttrs(*args[1], pos,
                   "while evaluating the second argument passed to builtins.unsafeGetAttrPos");
  auto i = args[1]->attrs()->get(state.symbols.create(attr));
  if (!i) {
    v.mkNull();
  } else {
    state.mkPos(v, i->pos);
  }
}

static RegisterPrimOp primop_unsafe_get_attr_pos(PrimOp{
    .name = "__unsafeGetAttrPos",
    .args = {"s", "set"},
    .arity = 2,
    .doc = R"(
      `unsafeGetAttrPos` returns the position of the attribute named *s*
      from *set*. This is used by Nixpkgs to provide location information
      in error messages.
    )",
    .fun = prim_unsafe_get_attr_pos,
});

// access to exact position information (ie, line and column numbers) is deferred
// due to the cost associated with calculating that information and how rarely
// it is used in practice. this is achieved by creating thunks to otherwise
// inaccessible primops that are not exposed as __op or under builtins to turn
// the internal PosIdx back into a line and column number, respectively. exposing
// these primops in any way would at best be not useful and at worst create wildly
// indeterministic eval results depending on parse order of files.
//
// in a simpler world this would instead be implemented as another kind of thunk,
// but each type of thunk has an associated runtime cost in the current evaluator.
// as with black holes this cost is too high to justify another thunk type to check
// for in the very hot path that is forceValue.
static struct lazy_pos_accessors_t {
  PrimOp primop_line_of_pos{
      .arity = 1, .fun = [](eval_state_t& state, pos_idx_t pos, value_t** args, value_t& v) {
        v.mkInt(state.positions[pos_idx_t(args[0]->integer().value)].line);
      }};
  PrimOp primop_column_of_pos{
      .arity = 1, .fun = [](eval_state_t& state, pos_idx_t pos, value_t** args, value_t& v) {
        v.mkInt(state.positions[pos_idx_t(args[0]->integer().value)].column);
      }};

  value_t line_of_pos, column_of_pos;

  lazy_pos_accessors_t() {
    line_of_pos.mkPrimOp(&primop_line_of_pos);
    column_of_pos.mkPrimOp(&primop_column_of_pos);
  }

  void operator()(eval_state_t& state, const pos_idx_t pos, value_t& line, value_t& column) {
    value_t* pos_v = state.allocValue();
    pos_v->mkInt(pos.id);
    line.mkApp(&line_of_pos, pos_v);
    column.mkApp(&column_of_pos, pos_v);
  }
} make_lazy_pos_accessors;

void make_position_thunks(eval_state_t& state, const pos_idx_t pos, value_t& line,
                          value_t& column) {
  make_lazy_pos_accessors(state, pos, line, column);
}

/* Dynamic version of the `?' operator. */
static void prim_has_attr(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  auto attr = state.forceStringNoCtx(
      *args[0], pos, "while evaluating the first argument passed to builtins.hasAttr");
  state.forceAttrs(*args[1], pos,
                   "while evaluating the second argument passed to builtins.hasAttr");
  v.mkBool(args[1]->attrs()->get(state.symbols.create(attr)));
}

static RegisterPrimOp primop_has_attr({
    .name = "__hasAttr",
    .args = {"s", "set"},
    .doc = R"(
      `hasAttr` returns `true` if *set* has an attribute named *s*, and
      `false` otherwise. This is a dynamic version of the `?` operator,
      since *s* is an expression rather than an identifier.
    )",
    .fun = prim_has_attr,
});

/* Determine whether the argument is a set. */
static void prim_is_attrs(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceValue(*args[0], pos);
  v.mkBool(args[0]->type() == nAttrs);
}

static RegisterPrimOp primop_is_attrs({
    .name = "__isAttrs",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to a set, and `false` otherwise.
    )",
    .fun = prim_is_attrs,
});

static void prim_remove_attrs(eval_state_t& state, const pos_idx_t pos, value_t** args,
                              value_t& v) {
  state.forceAttrs(*args[0], pos,
                   "while evaluating the first argument passed to builtins.removeAttrs");
  state.forceList(*args[1], pos,
                  "while evaluating the second argument passed to builtins.removeAttrs");

  /* Get the attribute names to be removed.
     We keep them as Attrs instead of Symbols so std::set_difference
     can be used to remove them from attrs[0]. */
  // 64: large enough to fit the attributes of a derivation
  boost::container::small_vector<attr_t, 64> names;
  names.reserve(args[1]->list_size());
  for (auto elem : args[1]->list_view()) {
    state.forceStringNoCtx(
        *elem, pos,
        "while evaluating the values of the second argument passed to builtins.removeAttrs");
    names.emplace_back(state.symbols.create(elem->string_view()), nullptr);
  }
  std::sort(names.begin(), names.end());

  /* Copy all attributes not in that set.  Note that we don't need
     to sort v.attrs because it's a subset of an already sorted
     vector. */
  auto attrs = state.buildBindings(args[0]->attrs()->size());
  std::set_difference(args[0]->attrs()->begin(), args[0]->attrs()->end(), names.begin(),
                      names.end(), std::back_inserter(attrs));
  v.mkAttrs(attrs.alreadySorted());
}

static RegisterPrimOp primop_remove_attrs({
    .name = "removeAttrs",
    .args = {"set", "list"},
    .doc = R"(
      Remove the attributes listed in *list* from *set*. The attributes
      don’t have to exist in *set*. For instance,

      ```nix
      removeAttrs { x = 1; y = 2; z = 3; } [ "a" "x" "z" ]
      ```

      evaluates to `{ y = 2; }`.
    )",
    .fun = prim_remove_attrs,
});

/* Builds a set from a list specifying (name, value) pairs.  To be
   precise, a list [{name = "name1"; value = value1;} ... {name =
   "nameN"; value = valueN;}] is transformed to {name1 = value1;
   ... nameN = valueN;}.  In case of duplicate occurrences of the same
   name, the first takes precedence. */
static void prim_list_to_attrs(eval_state_t& state, const pos_idx_t pos, value_t** args,
                               value_t& v) {
  state.forceList(*args[0], pos, "while evaluating the argument passed to builtins.listToAttrs");

  // Step 1. Sort the name-value attrsets in place using the memory we allocate for the result
  auto list_view = args[0]->list_view();
  size_t list_size = list_view.size();
  auto& bindings = *state.mem.allocBindings(list_size);
  using ElemPtr = decltype(&bindings[0].value);

  for (const auto& [n, v2] : enumerate(list_view)) {
    state.forceAttrs(*v2, pos,
                     "while evaluating an element of the list passed to builtins.listToAttrs");

    auto j = state.get_attr(state.s.name, v2->attrs(), "in a {name=...; value=...;} pair");

    auto name = state.forceStringNoCtx(*j->value, j->pos,
                                       "while evaluating the `name` attribute of an element of the "
                                       "list passed to builtins.listToAttrs");
    auto sym = state.symbols.create(name);

    // (ab)use attr_t to store a value_t * * instead of a value_t *, so that we can stabilize the
    // sort using the value_t * *
    bindings[n] = attr_t(sym, std::bit_cast<value_t*>(&v2));
  }

  std::sort(&bindings[0], &bindings[list_size], [](const attr_t& a, const attr_t& b) {
    // Note that .value is actually a value_t * * that corresponds to the position in the list
    return a < b || (!(a > b) && std::bit_cast<ElemPtr>(a.value) < std::bit_cast<ElemPtr>(b.value));
  });

  // Step 2. Unpack the bindings in place and skip name-value pairs with duplicate names
  symbol_t prev;
  for (size_t n = 0; n < list_size; n++) {
    auto attr = bindings[n];
    if (prev == attr.name) {
      continue;
    }
    // Note that .value is actually a value_t * *; see earlier comments
    value_t* v2 = *std::bit_cast<ElemPtr>(attr.value);

    auto j = state.get_attr(state.s.value, v2->attrs(), "in a {name=...; value=...;} pair");
    prev = attr.name;
    bindings.push_back({prev, j->value, j->pos});
  }
  // help GC and clear end of allocated array
  for (size_t n = bindings.size(); n < list_size; n++) {
    bindings[n] = attr_t{};
  }
  v.mkAttrs(&bindings);
}

static RegisterPrimOp primop_list_to_attrs({
    .name = "__listToAttrs",
    .args = {"e"},
    .doc = R"(
      Construct a set from a list specifying the names and values of each
      attribute. Each element of the list should be a set consisting of a
      string-valued attribute `name` specifying the name of the attribute,
      and an attribute `value` specifying its value.

      In case of duplicate occurrences of the same name, the first
      takes precedence.

      Example:

      ```nix
      builtins.listToAttrs
        [ { name = "foo"; value = 123; }
          { name = "bar"; value = 456; }
          { name = "bar"; value = 420; }
        ]
      ```

      evaluates to

      ```nix
      { foo = 123; bar = 456; }
      ```
    )",
    .fun = prim_list_to_attrs,
});

static void prim_intersect_attrs(eval_state_t& state, const pos_idx_t pos, value_t** args,
                                 value_t& v) {
  state.forceAttrs(*args[0], pos,
                   "while evaluating the first argument passed to builtins.intersectAttrs");
  state.forceAttrs(*args[1], pos,
                   "while evaluating the second argument passed to builtins.intersectAttrs");

  auto& left = *args[0]->attrs();
  auto& right = *args[1]->attrs();

  auto attrs = state.buildBindings(std::min(left.size(), right.size()));

  // The current implementation has good asymptotic complexity and is reasonably
  // simple. Further optimization may be possible, but does not seem productive,
  // considering the state of eval performance in 2022.
  //
  // I have looked for reusable and/or standard solutions and these are my
  // findings:
  //
  // STL
  // ===
  // std::set_intersection is not suitable, as it only performs a simultaneous
  // linear scan; not taking advantage of random access. This is O(n + m), so
  // linear in the largest set, which is not acceptable for callPackage in Nixpkgs.
  //
  // Simultaneous scan, with alternating simple binary search
  // ===
  // One alternative algorithm scans the attrsets simultaneously, jumping
  // forward using `lower_bound` in case of inequality. This should perform
  // well on very similar sets, having a local and predictable access pattern.
  // On dissimilar sets, it seems to need more comparisons than the current
  // algorithm, as few consecutive attrs match. `lower_bound` could take
  // advantage of the decreasing remaining search space, but this causes
  // the medians to move, which can mean that they don't stay in the cache
  // like they would with the current naive `find`.
  //
  // Double binary search
  // ===
  // The optimal algorithm may be "Double binary search", which doesn't
  // scan at all, but rather divides both sets simultaneously.
  // See "Fast Intersection Algorithms for Sorted Sequences" by Baeza-Yates et al.
  // https://cs.uwaterloo.ca/~ajsaling/papers/intersection_alg_app10.pdf
  // The only downsides I can think of are not having a linear access pattern
  // for similar sets, and having to maintain a more intricate algorithm.
  //
  // Adaptive
  // ===
  // Finally one could run try a simultaneous scan, count misses and fall back
  // to double binary search when the counter hit some threshold and/or ratio.

  if (left.size() < right.size()) {
    for (auto& l : left) {
      auto r = right.get(l.name);
      if (r) {
        attrs.insert(*r);
      }
    }
  } else {
    for (auto& r : right) {
      auto l = left.get(r.name);
      if (l) {
        attrs.insert(r);
      }
    }
  }

  v.mkAttrs(attrs.alreadySorted());
}

static RegisterPrimOp primop_intersect_attrs({
    .name = "__intersectAttrs",
    .args = {"e1", "e2"},
    .doc = R"(
      Return a set consisting of the attributes in the set *e2* which have the
      same name as some attribute in *e1*.

      Performs in O(*n* log *m*) where *n* is the size of the smaller set and *m* the larger set's size.
    )",
    .fun = prim_intersect_attrs,
});

static void prim_cat_attrs(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  auto attr_name = state.symbols.create(state.forceStringNoCtx(
      *args[0], pos, "while evaluating the first argument passed to builtins.catAttrs"));
  state.forceList(*args[1], pos,
                  "while evaluating the second argument passed to builtins.catAttrs");

  SmallValueVector<nonRecursiveStackReservation> res(args[1]->list_size());
  size_t found = 0;

  for (auto v2 : args[1]->list_view()) {
    state.forceAttrs(
        *v2, pos,
        "while evaluating an element in the list passed as second argument to builtins.catAttrs");
    if (auto i = v2->attrs()->get(attr_name)) {
      res[found++] = i->value;
    }
  }

  auto list = state.buildList(found);
  for (size_t n = 0; n < found; ++n) {
    list[n] = res[n];
  }
  v.mkList(list);
}

static RegisterPrimOp primop_cat_attrs({
    .name = "__catAttrs",
    .args = {"attr", "list"},
    .doc = R"(
      Collect each attribute named *attr* from a list of attribute
      sets.  Attrsets that don't contain the named attribute are
      ignored. For example,

      ```nix
      builtins.catAttrs "a" [{a = 1;} {b = 0;} {a = 2;}]
      ```

      evaluates to `[1 2]`.
    )",
    .fun = prim_cat_attrs,
});

static void prim_function_args(eval_state_t& state, const pos_idx_t pos, value_t** args,
                               value_t& v) {
  state.forceValue(*args[0], pos);
  if (args[0]->isPrimOpApp() || args[0]->isPrimOp()) {
    v.mkAttrs(&bindings_t::emptyBindings);
    return;
  }
  if (!args[0]->isLambda()) {
    state.error<TypeError>("'functionArgs' requires a function").at_pos(pos).debugThrow();
  }

  if (const auto& formals = args[0]->lambda().fun->getFormals()) {
    auto attrs = state.buildBindings(formals->formals.size());
    for (auto& i : formals->formals) {
      attrs.insert(i.name, state.getBool(i.def), i.pos);
    }
    /* Optimization: avoid sorting bindings. `formals` must already be sorted according to
       (std::tie(a.name, a.pos) < std::tie(b.name, b.pos)) predicate, so the following assertion
       always holds:
       assert(std::is_sorted(attrs.alreadySorted()->begin(), attrs.alreadySorted()->end()));
       .*/
    v.mkAttrs(attrs.alreadySorted());
  } else {
    v.mkAttrs(&bindings_t::emptyBindings);
    return;
  }
}

static RegisterPrimOp primop_function_args({
    .name = "__functionArgs",
    .args = {"f"},
    .doc = R"(
      Return a set containing the names of the formal arguments expected
      by the function *f*. The value of each attribute is a Boolean
      denoting whether the corresponding argument has a default value. For
      instance, `functionArgs ({ x, y ? 123}: ...) = { x = false; y =
      true; }`.

      "Formal argument" here refers to the attributes pattern-matched by
      the function. Plain lambdas are not included, e.g. `functionArgs (x:
      ...) = { }`.
    )",
    .fun = prim_function_args,
});

/*  */
static void prim_map_attrs(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceAttrs(*args[1], pos,
                   "while evaluating the second argument passed to builtins.mapAttrs");

  auto attrs = state.buildBindings(args[1]->attrs()->size());

  for (auto& i : *args[1]->attrs()) {
    value_t* vName = value_t::toPtr(state.symbols[i.name]);
    value_t* vFun2 = state.allocValue();
    vFun2->mkApp(args[0], vName);
    attrs.alloc(i.name).mkApp(vFun2, i.value);
  }

  v.mkAttrs(attrs.alreadySorted());
}

static RegisterPrimOp primop_map_attrs({
    .name = "__mapAttrs",
    .args = {"f", "attrset"},
    .doc = R"(
      Apply function *f* to every element of *attrset*. For example,

      ```nix
      builtins.mapAttrs (name: value: value * 10) { a = 1; b = 2; }
      ```

      evaluates to `{ a = 10; b = 20; }`.
    )",
    .fun = prim_map_attrs,
});

static void prim_filter_attrs(eval_state_t& state, const pos_idx_t pos, value_t** args,
                              value_t& v) {
  state.forceAttrs(*args[1], pos,
                   "while evaluating the second argument passed to builtins.filterAttrs");

  if (args[1]->attrs()->empty()) {
    v = *args[1];
    return;
  }

  state.forceFunction(*args[0], pos,
                      "while evaluating the first argument passed to builtins.filterAttrs");

  auto attrs = state.buildBindings(args[1]->attrs()->size());

  for (auto& i : *args[1]->attrs()) {
    value_t* vName = value_t::toPtr(state.symbols[i.name]);
    value_t* callArgs[] = {vName, i.value};
    value_t res;
    state.callFunction(*args[0], callArgs, res, no_pos);
    if (state.forceBool(res, pos,
                        "while evaluating the return value of the filtering function passed to "
                        "builtins.filterAttrs")) {
      attrs.insert(i.name, i.value);
    }
  }

  v.mkAttrs(attrs.alreadySorted());
}

static RegisterPrimOp primop_filter_attrs({
    .name = "__filterAttrs",
    .args = {"f", "attrset"},
    .doc = R"(
      Return an attribute set consisting of the attributes in *attrset* for which
      the function *f* returns `true`. The function *f* is called with two arguments:
      the name of the attribute and the value of the attribute. For example,

      ```nix
      builtins.filterAttrs (name: value: name == "foo") { foo = 1; bar = 2; }
      ```

      evaluates to `{ foo = 1; }`.
    )",
    .fun = prim_filter_attrs,
});

static void prim_zip_attrs_with(eval_state_t& state, const pos_idx_t pos, value_t** args,
                                value_t& v) {
  // we will first count how many values are present for each given key.
  // we then allocate a single attrset and pre-populate it with lists of
  // appropriate sizes, stash the pointers to the list elements of each,
  // and populate the lists. after that we replace the list in the every
  // attribute with the merge function application. this way we need not
  // use (slightly slower) temporary storage the GC does not know about.

  struct Item {
    size_t size = 0;
    size_t pos = 0;
    std::optional<ListBuilder> list;
  };

  std::map<symbol_t, Item, std::less<symbol_t>,
           traceable_allocator<std::pair<const symbol_t, Item>>>
      attrsSeen;

  state.forceFunction(*args[0], pos,
                      "while evaluating the first argument passed to builtins.zipAttrsWith");
  state.forceList(*args[1], pos,
                  "while evaluating the second argument passed to builtins.zipAttrsWith");
  const auto list_items = args[1]->list_view();

  for (auto& v_elem : list_items) {
    state.forceAttrs(
        *v_elem, no_pos,
        "while evaluating a value of the list passed as second argument to builtins.zipAttrsWith");
    for (auto& attr : *v_elem->attrs()) {
      attrsSeen.try_emplace(attr.name).first->second.size++;
    }
  }

  for (auto& [sym, elem] : attrsSeen) {
    elem.list.emplace(state.buildList(elem.size));
  }

  for (auto& v_elem : list_items) {
    for (auto& attr : *v_elem->attrs()) {
      auto& item = attrsSeen.at(attr.name);
      (*item.list)[item.pos++] = attr.value;
    }
  }

  auto attrs = state.buildBindings(attrsSeen.size());

  for (auto& [sym, elem] : attrsSeen) {
    auto name = value_t::toPtr(state.symbols[sym]);
    auto call1 = state.allocValue();
    call1->mkApp(args[0], name);
    auto call2 = state.allocValue();
    auto arg = state.allocValue();
    arg->mkList(*elem.list);
    call2->mkApp(call1, arg);
    attrs.insert(sym, call2);
  }

  v.mkAttrs(attrs.alreadySorted());
}

static RegisterPrimOp primop_zip_attrs_with({
    .name = "__zipAttrsWith",
    .args = {"f", "list"},
    .doc = R"(
      Transpose a list of attribute sets into an attribute set of lists,
      then apply `mapAttrs`.

      `f` receives two arguments: the attribute name and a non-empty
      list of all values encountered for that attribute name.

      The result is an attribute set where the attribute names are the
      union of the attribute names in each element of `list`. The attribute
      values are the return values of `f`.

      ```nix
      builtins.zipAttrsWith
        (name: values: { inherit name values; })
        [ { a = "x"; } { a = "y"; b = "z"; } ]
      ```

      evaluates to

      ```
      {
        a = { name = "a"; values = [ "x" "y" ]; };
        b = { name = "b"; values = [ "z" ]; };
      }
      ```
    )",
    .fun = prim_zip_attrs_with,
});

/*************************************************************
 * Lists
 *************************************************************/

/* Determine whether the argument is a list. */
static void prim_is_list(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceValue(*args[0], pos);
  v.mkBool(args[0]->type() == nList);
}

static RegisterPrimOp primop_is_list({
    .name = "__isList",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to a list, and `false` otherwise.
    )",
    .fun = prim_is_list,
});

/* Return the n-1'th element of a list. */
static void prim_elem_at(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  NixInt::Inner n =
      state
          .forceInt(*args[1], pos,
                    "while evaluating the second argument passed to 'builtins.elemAt'")
          .value;
  state.forceList(*args[0], pos, "while evaluating the first argument passed to 'builtins.elemAt'");
  if (n < 0 || std::make_unsigned_t<NixInt::Inner>(n) >= args[0]->list_size()) {
    state
        .error<EvalError>("'builtins.elemAt' called with index %d on a list of size %d", n,
                          args[0]->list_size())
        .at_pos(pos)
        .debugThrow();
  }
  state.forceValue(*args[0]->list_view()[n], pos);
  v = *args[0]->list_view()[n];
}

static RegisterPrimOp primop_elem_at({
    .name = "__elemAt",
    .args = {"xs", "n"},
    .doc = R"(
      Return element *n* from the list *xs*. Elements are counted starting
      from 0. A fatal error occurs if the index is out of bounds.
    )",
    .fun = prim_elem_at,
});

/* Return the first element of a list. */
static void prim_head(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceList(*args[0], pos, "while evaluating the first argument passed to 'builtins.head'");
  if (args[0]->list_size() == 0) {
    state.error<EvalError>("'builtins.head' called on an empty list").at_pos(pos).debugThrow();
  }
  state.forceValue(*args[0]->list_view()[0], pos);
  v = *args[0]->list_view()[0];
}

static RegisterPrimOp primop_head({
    .name = "__head",
    .args = {"list"},
    .doc = R"(
      Return the first element of a list; abort evaluation if the argument
      isn’t a list or is an empty list. You can test whether a list is
      empty by comparing it with `[]`.
    )",
    .fun = prim_head,
});

/* Return a list consisting of everything but the first element of
   a list.  Warning: this function takes O(n) time, so you probably
   don't want to use it!  */
static void prim_tail(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceList(*args[0], pos, "while evaluating the first argument passed to 'builtins.tail'");
  if (args[0]->list_size() == 0) {
    state.error<EvalError>("'builtins.tail' called on an empty list").at_pos(pos).debugThrow();
  }

  auto list = state.buildList(args[0]->list_size() - 1);
  for (const auto& [n, v] : enumerate(list)) {
    v = args[0]->list_view()[n + 1];
  }
  v.mkList(list);
}

static RegisterPrimOp primop_tail({
    .name = "__tail",
    .args = {"list"},
    .doc = R"(
      Return the list without its first item; abort evaluation if
      the argument isn’t a list or is an empty list.

      > **Warning**
      >
      > This function should generally be avoided since it's inefficient:
      > unlike Haskell's `tail`, it takes O(n) time, so recursing over a
      > list by repeatedly calling `tail` takes O(n^2) time.
    )",
    .fun = prim_tail,
});

/* Apply a function to every element of a list. */
static void prim_map(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceList(*args[1], pos, "while evaluating the second argument passed to builtins.map");

  if (args[1]->list_size() == 0) {
    v = *args[1];
    return;
  }

  state.forceFunction(*args[0], pos, "while evaluating the first argument passed to builtins.map");

  auto list = state.buildList(args[1]->list_size());
  for (const auto& [n, v] : enumerate(list)) {
    (v = state.allocValue())->mkApp(args[0], args[1]->list_view()[n]);
  }
  v.mkList(list);
}

static RegisterPrimOp primop_map({
    .name = "map",
    .args = {"f", "list"},
    .doc = R"(
      Apply the function *f* to each element in the list *list*. For
      example,

      ```nix
      map (x: "foo" + x) [ "bar" "bla" "abc" ]
      ```

      evaluates to `[ "foobar" "foobla" "fooabc" ]`.
    )",
    .fun = prim_map,
});

/* Filter a list using a predicate; that is, return a list containing
   every element from the list for which the predicate function
   returns true. */
static void prim_filter(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceList(*args[1], pos, "while evaluating the second argument passed to builtins.filter");

  if (args[1]->list_size() == 0) {
    v = *args[1];
    return;
  }

  state.forceFunction(*args[0], pos,
                      "while evaluating the first argument passed to builtins.filter");

  auto len = args[1]->list_size();
  SmallValueVector<nonRecursiveStackReservation> vs(len);
  size_t k = 0;

  bool same = true;
  for (size_t n = 0; n < len; ++n) {
    value_t res;
    state.callFunction(*args[0], *args[1]->list_view()[n], res, no_pos);
    if (state.forceBool(res, pos,
                        "while evaluating the return value of the filtering function passed to "
                        "builtins.filter")) {
      vs[k++] = args[1]->list_view()[n];
    } else {
      same = false;
    }
  }

  if (same) {
    v = *args[1];
  } else {
    auto list = state.buildList(k);
    for (const auto& [n, v] : enumerate(list)) {
      v = vs[n];
    }
    v.mkList(list);
  }
}

static RegisterPrimOp primop_filter({
    .name = "__filter",
    .args = {"f", "list"},
    .doc = R"(
      Return a list consisting of the elements of *list* for which the
      function *f* returns `true`.
    )",
    .fun = prim_filter,
});

/* Return true if a list contains a given element. */
static void prim_elem(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  bool res = false;
  state.forceList(*args[1], pos, "while evaluating the second argument passed to builtins.elem");
  for (auto elem : args[1]->list_view()) {
    if (state.eqValues(*args[0], *elem, pos,
                       "while searching for the presence of the given element in the list")) {
      res = true;
      break;
    }
  }
  v.mkBool(res);
}

static RegisterPrimOp primop_elem({
    .name = "__elem",
    .args = {"x", "xs"},
    .doc = R"(
      Return `true` if a value equal to *x* occurs in the list *xs*, and
      `false` otherwise.
    )",
    .fun = prim_elem,
});

/* Concatenate a list of lists. */
static void prim_concat_lists(eval_state_t& state, const pos_idx_t pos, value_t** args,
                              value_t& v) {
  state.forceList(*args[0], pos,
                  "while evaluating the first argument passed to builtins.concatLists");
  auto list_view = args[0]->list_view();
  state.concatLists(v, args[0]->list_size(), list_view.data(), pos,
                    "while evaluating a value of the list passed to builtins.concatLists");
}

static RegisterPrimOp primop_concat_lists({
    .name = "__concatLists",
    .args = {"lists"},
    .doc = R"(
      Concatenate a list of lists into a single list.
    )",
    .fun = prim_concat_lists,
});

/* Return the length of a list.  This is an O(1) time operation. */
static void prim_length(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceList(*args[0], pos, "while evaluating the first argument passed to builtins.length");
  v.mkInt(args[0]->list_size());
}

static RegisterPrimOp primop_length({
    .name = "__length",
    .args = {"e"},
    .doc = R"(
      Return the length of the list *e*.
    )",
    .fun = prim_length,
});

/* Reduce a list by applying a binary operator, from left to
   right. The operator is applied strictly. */
static void prim_foldl_strict(eval_state_t& state, const pos_idx_t pos, value_t** args,
                              value_t& v) {
  state.forceFunction(*args[0], pos,
                      "while evaluating the first argument passed to builtins.foldlStrict");
  state.forceList(*args[2], pos,
                  "while evaluating the third argument passed to builtins.foldlStrict");

  if (args[2]->list_size()) {
    value_t* v_cur = args[1];

    auto list_view = args[2]->list_view();
    for (auto [n, elem] : enumerate(list_view)) {
      value_t* vs[]{v_cur, elem};
      v_cur = n == args[2]->list_size() - 1 ? &v : state.allocValue();
      state.callFunction(*args[0], vs, *v_cur, pos);
    }
    state.forceValue(v, pos);
  } else {
    state.forceValue(*args[1], pos);
    v = *args[1];
  }
}

static RegisterPrimOp primop_foldl_strict({
    .name = "__foldl'",
    .args = {"op", "nul", "list"},
    .doc = R"(
      Reduce a list by applying a binary operator, from left to right,
      e.g. `foldl' op nul [x0 x1 x2 ...] = op (op (op nul x0) x1) x2)
      ...`.

      For example, `foldl' (acc: elem: acc + elem) 0 [1 2 3]` evaluates
      to `6` and `foldl' (acc: elem: { "${elem}" = elem; } // acc) {}
      ["a" "b"]` evaluates to `{ a = "a"; b = "b"; }`.

      The first argument of `op` is the accumulator whereas the second
      argument is the current element being processed. The return value
      of each application of `op` is evaluated immediately, even for
      intermediate values.
    )",
    .fun = prim_foldl_strict,
});

static void any_or_all(bool any, eval_state_t& state, const pos_idx_t pos, value_t** args,
                       value_t& v) {
  state.forceFunction(*args[0], pos,
                      std::string("while evaluating the first argument passed to builtins.") +
                          (any ? "any" : "all"));
  state.forceList(*args[1], pos,
                  std::string("while evaluating the second argument passed to builtins.") +
                      (any ? "any" : "all"));

  std::string_view error_ctx =
      any ? "while evaluating the return value of the function passed to builtins.any"
          : "while evaluating the return value of the function passed to builtins.all";

  for (auto elem : args[1]->list_view()) {
    value_t vTmp;
    state.callFunction(*args[0], *elem, vTmp, pos);
    bool res = state.forceBool(vTmp, pos, error_ctx);
    if (res == any) {
      v.mkBool(any);
      return;
    }
  }

  v.mkBool(!any);
}

static void prim_any(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  any_or_all(true, state, pos, args, v);
}

static RegisterPrimOp primop_any({
    .name = "__any",
    .args = {"pred", "list"},
    .doc = R"(
      Return `true` if the function *pred* returns `true` for at least one
      element of *list*, and `false` otherwise.
    )",
    .fun = prim_any,
});

static void prim_all(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  any_or_all(false, state, pos, args, v);
}

static RegisterPrimOp primop_all({
    .name = "__all",
    .args = {"pred", "list"},
    .doc = R"(
      Return `true` if the function *pred* returns `true` for all elements
      of *list*, and `false` otherwise.
    )",
    .fun = prim_all,
});

static void prim_gen_list(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  auto len_ = state
                  .forceInt(*args[1], pos,
                            "while evaluating the second argument passed to builtins.genList")
                  .value;

  if (len_ < 0 || std::make_unsigned_t<NixInt::Inner>(len_) > std::numeric_limits<size_t>::max()) {
    state.error<EvalError>("cannot create list of size %1%", len_).at_pos(pos).debugThrow();
  }

  size_t len = size_t(len_);

  // More strict than strictly (!) necessary, but acceptable
  // as evaluating map without accessing any values makes little sense.
  state.forceFunction(*args[0], no_pos,
                      "while evaluating the first argument passed to builtins.genList");

  auto list = state.buildList(len);
  for (const auto& [n, v] : enumerate(list)) {
    auto arg = state.allocValue();
    arg->mkInt(n);
    (v = state.allocValue())->mkApp(args[0], arg);
  }
  v.mkList(list);
}

static RegisterPrimOp primop_gen_list({
    .name = "__genList",
    .args = {"generator", "length"},
    .doc = R"(
      Generate list of size *length*, with each element *i* equal to the
      value returned by *generator* `i`. For example,

      ```nix
      builtins.genList (x: x * x) 5
      ```

      returns the list `[ 0 1 4 9 16 ]`.
    )",
    .fun = prim_gen_list,
});

static void prim_less_than(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v);

static void prim_sort(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceList(*args[1], pos, "while evaluating the second argument passed to builtins.sort");

  auto len = args[1]->list_size();
  if (len == 0) {
    v = *args[1];
    return;
  }

  state.forceFunction(*args[0], pos, "while evaluating the first argument passed to builtins.sort");

  auto list = state.buildList(len);
  for (const auto& [n, v] : enumerate(list)) {
    state.forceValue(*(v = args[1]->list_view()[n]), pos);
  }

  auto comparator = [&](value_t* a, value_t* b) {
    /* Optimization: if the comparator is lessThan, bypass
       callFunction. */
    if (args[0]->isPrimOp()) {
      auto ptr = args[0]->prim_op()->fun.target<decltype(&prim_less_than)>();
      if (ptr && *ptr == prim_less_than) {
        return compare_values_t(
            state, no_pos, "while evaluating the ordering function passed to builtins.sort")(a, b);
      }
    }

    value_t* vs[] = {a, b};
    value_t v_bool;
    state.callFunction(*args[0], vs, v_bool, no_pos);
    return state.forceBool(
        v_bool, pos,
        "while evaluating the return value of the sorting function passed to builtins.sort");
  };

  /* NOTE: Using custom implementation because std::sort and std::stable_sort
     are not resilient to comparators that violate strict weak ordering. Diagnosing
     incorrect implementations is a O(n^3) problem, so doing the checks is much more
     expensive that doing the sorting. For this reason we choose to use sorting algorithms
     that are can't be broken by invalid comprators. peek_sort (mergesort)
     doesn't misbehave when any of the strict weak order properties is
     violated - output is always a reordering of the input. */
  peek_sort(list.begin(), list.end(), comparator);

  v.mkList(list);
}

static RegisterPrimOp primop_sort({
    .name = "__sort",
    .args = {"comparator", "list"},
    .doc = R"(
      Return *list* in sorted order. It repeatedly calls the function
      *comparator* with two elements. The comparator should return `true`
      if the first element is less than the second, and `false` otherwise.
      For example,

      ```nix
      builtins.sort builtins.lessThan [ 483 249 526 147 42 77 ]
      ```

      produces the list `[ 42 77 147 249 483 526 ]`.

      This is a stable sort: it preserves the relative order of elements
      deemed equal by the comparator.

      *comparator* must impose a strict weak ordering on the set of values
      in the *list*. This means that for any elements *a*, *b* and *c* from the
      *list*, *comparator* must satisfy the following relations:

        1. Transitivity

        ```nix
        comparator a b && comparator b c -> comparator a c
        ```

        1. Irreflexivity

        ```nix
        comparator a a == false
        ```

        1. Transitivity of equivalence

        ```nix
        let equiv = a: b: (!comparator a b && !comparator b a); in
        equiv a b && equiv b c -> equiv a c
        ```

      If the *comparator* violates any of these properties, then `builtins.sort`
      reorders elements in an unspecified manner.
    )",
    .fun = prim_sort,
});

static void prim_partition(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceFunction(*args[0], pos,
                      "while evaluating the first argument passed to builtins.partition");
  state.forceList(*args[1], pos,
                  "while evaluating the second argument passed to builtins.partition");

  auto len = args[1]->list_size();

  ValueVector right, wrong;

  for (size_t n = 0; n < len; ++n) {
    auto v_elem = args[1]->list_view()[n];
    state.forceValue(*v_elem, pos);
    value_t res;
    state.callFunction(*args[0], *v_elem, res, pos);
    if (state.forceBool(res, pos,
                        "while evaluating the return value of the partition function passed to "
                        "builtins.partition")) {
      right.push_back(v_elem);
    } else {
      wrong.push_back(v_elem);
    }
  }

  auto attrs = state.buildBindings(2);

  auto rsize = right.size();
  auto rlist = state.buildList(rsize);
  if (rsize) {
    memcpy(rlist.elems, right.data(), sizeof(value_t*) * rsize);
  }
  attrs.alloc(state.s.right).mkList(rlist);

  auto wsize = wrong.size();
  auto wlist = state.buildList(wsize);
  if (wsize) {
    memcpy(wlist.elems, wrong.data(), sizeof(value_t*) * wsize);
  }
  attrs.alloc(state.s.wrong).mkList(wlist);

  v.mkAttrs(attrs);
}

static RegisterPrimOp primop_partition({
    .name = "__partition",
    .args = {"pred", "list"},
    .doc = R"(
      Given a predicate function *pred*, this function returns an
      attrset containing a list named `right`, containing the elements
      in *list* for which *pred* returned `true`, and a list named
      `wrong`, containing the elements for which it returned
      `false`. For example,

      ```nix
      builtins.partition (x: x > 10) [1 23 9 3 42]
      ```

      evaluates to

      ```nix
      { right = [ 23 42 ]; wrong = [ 1 9 3 ]; }
      ```
    )",
    .fun = prim_partition,
});

static void prim_group_by(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceFunction(*args[0], pos,
                      "while evaluating the first argument passed to builtins.groupBy");
  state.forceList(*args[1], pos, "while evaluating the second argument passed to builtins.groupBy");

  ValueVectorMap attrs;

  for (auto v_elem : args[1]->list_view()) {
    value_t res;
    state.callFunction(*args[0], *v_elem, res, pos);
    auto name = state.forceStringNoCtx(
        res, pos,
        "while evaluating the return value of the grouping function passed to builtins.groupBy");
    auto sym = state.symbols.create(name);
    auto vector = attrs.try_emplace<ValueVector>(sym, {}).first;
    vector->second.push_back(v_elem);
  }

  auto attrs2 = state.buildBindings(attrs.size());

  for (auto& i : attrs) {
    auto size = i.second.size();
    auto list = state.buildList(size);
    memcpy(list.elems, i.second.data(), sizeof(value_t*) * size);
    attrs2.alloc(i.first).mkList(list);
  }

  v.mkAttrs(attrs2.alreadySorted());
}

static RegisterPrimOp primop_group_by({
    .name = "__groupBy",
    .args = {"f", "list"},
    .doc = R"(
      Groups elements of *list* together by the string returned from the
      function *f* called on each element. It returns an attribute set
      where each attribute value contains the elements of *list* that are
      mapped to the same corresponding attribute name returned by *f*.

      For example,

      ```nix
      builtins.groupBy (builtins.substring 0 1) ["foo" "bar" "baz"]
      ```

      evaluates to

      ```nix
      { b = [ "bar" "baz" ]; f = [ "foo" ]; }
      ```
    )",
    .fun = prim_group_by,
});

static void prim_concat_map(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceFunction(*args[0], pos,
                      "while evaluating the first argument passed to builtins.concatMap");
  state.forceList(*args[1], pos,
                  "while evaluating the second argument passed to builtins.concatMap");
  auto nr_lists = args[1]->list_size();

  // List of returned lists before concatenation. References to these Values must NOT be persisted.
  SmallTemporaryValueVector<conservativeStackReservation> lists(nr_lists);
  size_t len = 0;

  for (size_t n = 0; n < nr_lists; ++n) {
    value_t* v_elem = args[1]->list_view()[n];
    state.callFunction(*args[0], *v_elem, lists[n], pos);
    state.forceList(
        lists[n], lists[n].determinePos(args[0]->determinePos(pos)),
        "while evaluating the return value of the function passed to builtins.concatMap");
    len += lists[n].list_size();
  }

  auto list = state.buildList(len);
  auto out = list.elems;
  for (size_t n = 0, pos = 0; n < nr_lists; ++n) {
    auto list_view = lists[n].list_view();
    auto l = list_view.size();
    if (l) {
      memcpy(out + pos, list_view.data(), l * sizeof(value_t*));
    }
    pos += l;
  }
  v.mkList(list);
}

static RegisterPrimOp primop_concat_map({
    .name = "__concatMap",
    .args = {"f", "list"},
    .doc = R"(
      This function is equivalent to `builtins.concatLists (map f list)`
      but is more efficient.
    )",
    .fun = prim_concat_map,
});

/*************************************************************
 * Integer arithmetic
 *************************************************************/

static void prim_add(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceValue(*args[0], pos);
  state.forceValue(*args[1], pos);
  if (args[0]->type() == nFloat || args[1]->type() == nFloat) {
    v.mkFloat(
        state.forceFloat(*args[0], pos, "while evaluating the first argument of the addition") +
        state.forceFloat(*args[1], pos, "while evaluating the second argument of the addition"));
  } else {
    auto i1 = state.forceInt(*args[0], pos, "while evaluating the first argument of the addition");
    auto i2 = state.forceInt(*args[1], pos, "while evaluating the second argument of the addition");

    auto result_ = i1 + i2;
    if (auto result = result_.valueChecked(); result.has_value()) {
      v.mkInt(*result);
    } else {
      state.error<EvalError>("integer overflow in adding %1% + %2%", i1, i2)
          .at_pos(pos)
          .debugThrow();
    }
  }
}

static RegisterPrimOp primop_add({
    .name = "__add",
    .args = {"e1", "e2"},
    .doc = R"(
      Return the sum of the numbers *e1* and *e2*.
    )",
    .fun = prim_add,
});

static void prim_sub(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceValue(*args[0], pos);
  state.forceValue(*args[1], pos);
  if (args[0]->type() == nFloat || args[1]->type() == nFloat) {
    v.mkFloat(
        state.forceFloat(*args[0], pos, "while evaluating the first argument of the subtraction") -
        state.forceFloat(*args[1], pos, "while evaluating the second argument of the subtraction"));
  } else {
    auto i1 =
        state.forceInt(*args[0], pos, "while evaluating the first argument of the subtraction");
    auto i2 =
        state.forceInt(*args[1], pos, "while evaluating the second argument of the subtraction");

    auto result_ = i1 - i2;

    if (auto result = result_.valueChecked(); result.has_value()) {
      v.mkInt(*result);
    } else {
      state.error<EvalError>("integer overflow in subtracting %1% - %2%", i1, i2)
          .at_pos(pos)
          .debugThrow();
    }
  }
}

static RegisterPrimOp primop_sub({
    .name = "__sub",
    .args = {"e1", "e2"},
    .doc = R"(
      Return the difference between the numbers *e1* and *e2*.
    )",
    .fun = prim_sub,
});

static void prim_mul(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceValue(*args[0], pos);
  state.forceValue(*args[1], pos);
  if (args[0]->type() == nFloat || args[1]->type() == nFloat) {
    v.mkFloat(state.forceFloat(*args[0], pos, "while evaluating the first of the multiplication") *
              state.forceFloat(*args[1], pos,
                               "while evaluating the second argument of the multiplication"));
  } else {
    auto i1 =
        state.forceInt(*args[0], pos, "while evaluating the first argument of the multiplication");
    auto i2 =
        state.forceInt(*args[1], pos, "while evaluating the second argument of the multiplication");

    auto result_ = i1 * i2;

    if (auto result = result_.valueChecked(); result.has_value()) {
      v.mkInt(*result);
    } else {
      state.error<EvalError>("integer overflow in multiplying %1% * %2%", i1, i2)
          .at_pos(pos)
          .debugThrow();
    }
  }
}

static RegisterPrimOp primop_mul({
    .name = "__mul",
    .args = {"e1", "e2"},
    .doc = R"(
      Return the product of the numbers *e1* and *e2*.
    )",
    .fun = prim_mul,
});

static void prim_div(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceValue(*args[0], pos);
  state.forceValue(*args[1], pos);

  NixFloat f2 =
      state.forceFloat(*args[1], pos, "while evaluating the second operand of the division");
  if (f2 == 0) {
    state.error<EvalError>("division by zero").at_pos(pos).debugThrow();
  }

  if (args[0]->type() == nFloat || args[1]->type() == nFloat) {
    v.mkFloat(
        state.forceFloat(*args[0], pos, "while evaluating the first operand of the division") / f2);
  } else {
    NixInt i1 = state.forceInt(*args[0], pos, "while evaluating the first operand of the division");
    NixInt i2 =
        state.forceInt(*args[1], pos, "while evaluating the second operand of the division");
    /* Avoid division overflow as it might raise SIGFPE. */
    auto result_ = i1 / i2;
    if (auto result = result_.valueChecked(); result.has_value()) {
      v.mkInt(*result);
    } else {
      state.error<EvalError>("integer overflow in dividing %1% / %2%", i1, i2)
          .at_pos(pos)
          .debugThrow();
    }
  }
}

static RegisterPrimOp primop_div({
    .name = "__div",
    .args = {"e1", "e2"},
    .doc = R"(
      Return the quotient of the numbers *e1* and *e2*.
    )",
    .fun = prim_div,
});

static void prim_bit_and(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  auto i1 = state.forceInt(*args[0], pos,
                           "while evaluating the first argument passed to builtins.bitAnd");
  auto i2 = state.forceInt(*args[1], pos,
                           "while evaluating the second argument passed to builtins.bitAnd");
  v.mkInt(i1.value & i2.value);
}

static RegisterPrimOp primop_bit_and({
    .name = "__bitAnd",
    .args = {"e1", "e2"},
    .doc = R"(
      Return the bitwise AND of the integers *e1* and *e2*.
    )",
    .fun = prim_bit_and,
});

static void prim_bit_or(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  auto i1 =
      state.forceInt(*args[0], pos, "while evaluating the first argument passed to builtins.bitOr");
  auto i2 = state.forceInt(*args[1], pos,
                           "while evaluating the second argument passed to builtins.bitOr");

  v.mkInt(i1.value | i2.value);
}

static RegisterPrimOp primop_bit_or({
    .name = "__bitOr",
    .args = {"e1", "e2"},
    .doc = R"(
      Return the bitwise OR of the integers *e1* and *e2*.
    )",
    .fun = prim_bit_or,
});

static void prim_bit_xor(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  auto i1 = state.forceInt(*args[0], pos,
                           "while evaluating the first argument passed to builtins.bitXor");
  auto i2 = state.forceInt(*args[1], pos,
                           "while evaluating the second argument passed to builtins.bitXor");

  v.mkInt(i1.value ^ i2.value);
}

static RegisterPrimOp primop_bit_xor({
    .name = "__bitXor",
    .args = {"e1", "e2"},
    .doc = R"(
      Return the bitwise XOR of the integers *e1* and *e2*.
    )",
    .fun = prim_bit_xor,
});

static void prim_less_than(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  state.forceValue(*args[0], pos);
  state.forceValue(*args[1], pos);
  // pos is exact here, no need for a message.
  compare_values_t comp(state, no_pos, "");
  v.mkBool(comp(args[0], args[1]));
}

static RegisterPrimOp primop_less_than({
    .name = "__lessThan",
    .args = {"e1", "e2"},
    .doc = R"(
      Return `true` if the value *e1* is less than the value *e2*, and `false` otherwise.
      Evaluation aborts if either *e1* or *e2* does not evaluate to a number, string or path.
      Furthermore, it aborts if *e2* does not match *e1*'s type according to the aforementioned classification of number, string or path.
    )",
    .fun = prim_less_than,
});

/*************************************************************
 * String manipulation
 *************************************************************/

/* Convert the argument to a string.  Paths are *not* copied to the
   store, so `toString /foo/bar' yields `"/foo/bar"', not
   `"/nix/store/whatever..."'. */
static void prim_to_string(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  NixStringContext context;
  auto s = state.coerceToString(pos, *args[0], context,
                                "while evaluating the first argument passed to builtins.toString",
                                true, false);
  v.mk_string(*s, context, state.mem);
}

static RegisterPrimOp primop_to_string({
    .name = "toString",
    .args = {"e"},
    .doc = R"(
      Convert the expression *e* to a string. *e* can be:

        - A string (in which case the string is returned unmodified).

        - A path (e.g., `toString /foo/bar` yields `"/foo/bar"`.

        - A set containing `{ __toString = self: ...; }` or `{ out_path = ...; }`.

        - An integer.

        - A list, in which case the string representations of its elements
          are joined with spaces.

        - A Boolean (`false` yields `""`, `true` yields `"1"`).

        - `null`, which yields the empty string.
    )",
    .fun = prim_to_string,
});

/* `substring start len str' returns the substring of `str' starting
   at byte position `min(start, stringLength str)' inclusive and
   ending at `min(start + len, stringLength str)'.  `start' must be
   non-negative. */
static void prim_substring(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  using NixUInt = std::make_unsigned_t<NixInt::Inner>;
  NixInt::Inner start =
      state
          .forceInt(
              *args[0], pos,
              "while evaluating the first argument (the start offset) passed to builtins.substring")
          .value;

  if (start < 0) {
    state.error<EvalError>("negative start position in 'substring'").at_pos(pos).debugThrow();
  }

  NixInt::Inner len = state
                          .forceInt(*args[1], pos,
                                    "while evaluating the second argument (the substring length) "
                                    "passed to builtins.substring")
                          .value;

  // Negative length may be idiomatically passed to builtins.substring to get
  // the tail of the string.
  auto _len = std::numeric_limits<std::string::size_type>::max();

  // Special-case on empty substring to avoid O(n) strlen
  // This allows for the use of empty substrings to efficiently capture string context
  if (len == 0) {
    state.forceValue(*args[2], pos);
    if (args[2]->type() == nString) {
      v.mkStringNoCopy(""_sds, args[2]->context());
      return;
    }
  }

  if (len >= 0 && NixUInt(len) < _len) {
    _len = len;
  }

  NixStringContext context;
  auto s = state.coerceToString(
      pos, *args[2], context,
      "while evaluating the third argument (the string) passed to builtins.substring");

  v.mk_string(NixUInt(start) >= s->size() ? "" : s->substr(start, _len), context, state.mem);
}

static RegisterPrimOp primop_substring({
    .name = "__substring",
    .args = {"start", "len", "s"},
    .doc = R"(
      Return the substring of *s* from byte position *start*
      (zero-based) up to but not including *start + len*. If *start* is
      greater than the length of the string, an empty string is returned.
      If *start + len* lies beyond the end of the string or *len* is `-1`,
      only the substring up to the end of the string is returned.
      *start* must be non-negative.
      For example,

      ```nix
      builtins.substring 0 3 "nixos"
      ```

      evaluates to `"nix"`.
    )",
    .fun = prim_substring,
});

static void prim_string_length(eval_state_t& state, const pos_idx_t pos, value_t** args,
                               value_t& v) {
  NixStringContext context;
  auto s = state.coerceToString(pos, *args[0], context,
                                "while evaluating the argument passed to builtins.stringLength");
  v.mkInt(NixInt::Inner(s->size()));
}

static RegisterPrimOp primop_string_length({
    .name = "__stringLength",
    .args = {"e"},
    .doc = R"(
      Return the number of bytes of the string *e*. If *e* is not a string,
      evaluation is aborted.
    )",
    .fun = prim_string_length,
});

/* Return the cryptographic hash of a string in base-16. */
static void prim_hash_string(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  auto algo = state.forceStringNoCtx(
      *args[0], pos, "while evaluating the first argument passed to builtins.hashString");
  std::optional<hash_algorithm_t> ha = parse_hash_algo(algo);
  if (!ha) {
    state.error<EvalError>("unknown hash algorithm '%1%'", algo).at_pos(pos).debugThrow();
  }

  NixStringContext context; // discarded
  auto s = state.forceString(*args[1], context, pos,
                             "while evaluating the second argument passed to builtins.hashString");

  v.mk_string(hash_string(*ha, s).to_string(hash_format_t::base16, false), state.mem);
}

static RegisterPrimOp primop_hash_string({
    .name = "__hashString",
    .args = {"type", "s"},
    .doc = R"(
      Return a base-16 representation of the cryptographic hash of string
      *s*. The hash algorithm specified by *type* must be one of `"md5"`,
      `"sha1"`, `"sha256"` or `"sha512"`.
    )",
    .fun = prim_hash_string,
});

static void prim_convert_hash(eval_state_t& state, const pos_idx_t pos, value_t** args,
                              value_t& v) {
  state.forceAttrs(*args[0], pos,
                   "while evaluating the first argument passed to builtins.convertHash");
  auto input_attrs = args[0]->attrs();

  auto iterator_hash = state.get_attr(state.symbols.create("hash"), input_attrs,
                                      "while locating the attribute 'hash'");
  auto hash =
      state.forceStringNoCtx(*iterator_hash->value, pos, "while evaluating the attribute 'hash'");

  auto iterator_hash_algo = input_attrs->get(state.symbols.create("hashAlgo"));
  std::optional<hash_algorithm_t> ha = std::nullopt;
  if (iterator_hash_algo) {
    ha = parse_hash_algo(state.forceStringNoCtx(*iterator_hash_algo->value, pos,
                                                "while evaluating the attribute 'hashAlgo'"));
  }

  auto iterator_to_hash_format =
      state.get_attr(state.symbols.create("toHashFormat"), args[0]->attrs(),
                     "while locating the attribute 'toHashFormat'");
  hash_format_t hf = parse_hash_format(state.forceStringNoCtx(
      *iterator_to_hash_format->value, pos, "while evaluating the attribute 'toHashFormat'"));

  v.mk_string(Hash::parse_any(hash, ha).to_string(hf, hf == hash_format_t::sri), state.mem);
}

static RegisterPrimOp primop_convert_hash({
    .name = "__convertHash",
    .args = {"args"},
    .doc = R"(
      Return the specified representation of a hash string, based on the attributes presented in *args*:

      - `hash`

        The hash to be converted.
        The hash format is detected automatically.

      - `hash_algo`

        The algorithm used to create the hash. Must be one of
        - `"md5"`
        - `"sha1"`
        - `"sha256"`
        - `"sha512"`

        The attribute may be omitted when `hash` is an [SRI hash](https://www.w3.org/TR/SRI/#the-integrity-attribute) or when the hash is prefixed with the hash algorithm name followed by a colon.
        That `<hash_algo>:<hashBody>` syntax is supported for backwards compatibility with existing tooling.

      - `toHashFormat`

        The format of the resulting hash. Must be one of
        - `"base16"`
        - `"nix32"`
        - `"base32"` (deprecated alias for `"nix32"`)
        - `"base64"`
        - `"sri"`

      The result hash is the *toHashFormat* representation of the hash *hash*.

      > **Example**
      >
      >   Convert a SHA256 hash in base16 to SRI:
      >
      > ```nix
      > builtins.convertHash {
      >   hash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
      >   toHashFormat = "sri";
      >   hash_algo = "sha256";
      > }
      > ```
      >
      >     "sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU="

      > **Example**
      >
      >   Convert a SHA256 hash in SRI to base16:
      >
      > ```nix
      > builtins.convertHash {
      >   hash = "sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=";
      >   toHashFormat = "base16";
      > }
      > ```
      >
      >     "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

      > **Example**
      >
      >   Convert a hash in the form `<hash_algo>:<hashBody>` in base16 to SRI:
      >
      > ```nix
      > builtins.convertHash {
      >   hash = "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
      >   toHashFormat = "sri";
      > }
      > ```
      >
      >     "sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU="
    )",
    .fun = prim_convert_hash,
});

struct regex_cache_t {
  boost::concurrent_flat_map<std::string, std::regex, string_view_hash_t, std::equal_to<>> cache;

  std::regex get(std::string_view re) {
    std::regex regex;
    /* No std::regex constructor overload from std::string_view, but can be constructed
       from a pointer + size or an iterator range. */
    cache.try_emplace_and_cvisit(
        re,
        /*s=*/re.data(),
        /*count=*/re.size(), std::regex::extended, [&regex](const auto& kv) { regex = kv.second; },
        [&regex](const auto& kv) { regex = kv.second; });
    return regex;
  }
};

ref<regex_cache_t> make_regex_cache() {
  return make_ref<regex_cache_t>();
}

void prim_match(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  auto re = state.forceStringNoCtx(*args[0], pos,
                                   "while evaluating the first argument passed to builtins.match");

  try {
    auto regex = state.regexCache->get(re);

    NixStringContext context;
    const auto str = state.forceString(
        *args[1], context, pos, "while evaluating the second argument passed to builtins.match");

    std::cmatch match;
    if (!std::regex_match(str.begin(), str.end(), match, regex)) {
      v.mkNull();
      return;
    }

    // the first match is the whole string
    auto list = state.buildList(match.size() - 1);
    for (const auto& [i, v2] : enumerate(list)) {
      if (!match[i + 1].matched) {
        v2 = &value_t::vNull;
      } else {
        v2 = mk_string(state, match[i + 1]);
      }
    }
    v.mkList(list);

  } catch (std::regex_error& e) {
    if (e.code() == std::regex_constants::error_space) {
      // limit is _GLIBCXX_REGEX_STATE_LIMIT for libstdc++
      state.error<EvalError>("memory limit exceeded by regular expression '%s'", re)
          .at_pos(pos)
          .debugThrow();
    } else {
      state.error<EvalError>("invalid regular expression '%s'", re).at_pos(pos).debugThrow();
    }
  }
}

static RegisterPrimOp primop_match({
    .name = "__match",
    .args = {"regex", "str"},
    .doc = R"s(
      Returns a list if the [extended POSIX regular
      expression](http://pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap09.html#tag_09_04)
      *regex* matches *str* precisely, otherwise returns `null`. Each item
      in the list is a regex group.

      ```nix
      builtins.match "ab" "abc"
      ```

      Evaluates to `null`.

      ```nix
      builtins.match "abc" "abc"
      ```

      Evaluates to `[ ]`.

      ```nix
      builtins.match "a(b)(c)" "abc"
      ```

      Evaluates to `[ "b" "c" ]`.

      ```nix
      builtins.match "[[:space:]]+([[:upper:]]+)[[:space:]]+" "  FOO   "
      ```

      Evaluates to `[ "FOO" ]`.
    )s",
    .fun = prim_match,
});

/* Split a string with a regular expression, and return a list of the
   non-matching parts interleaved by the lists of the matching groups. */
void prim_split(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v) {
  auto re = state.forceStringNoCtx(*args[0], pos,
                                   "while evaluating the first argument passed to builtins.split");

  try {
    auto regex = state.regexCache->get(re);

    NixStringContext context;
    const auto str = state.forceString(
        *args[1], context, pos, "while evaluating the second argument passed to builtins.split");

    auto begin = std::cregex_iterator(str.begin(), str.end(), regex);
    auto end = std::cregex_iterator();

    // Any matches results are surrounded by non-matching results.
    const size_t len = std::distance(begin, end);
    auto list = state.buildList(2 * len + 1);
    size_t idx = 0;

    if (len == 0) {
      list[0] = args[1];
      v.mkList(list);
      return;
    }

    for (auto i = begin; i != end; ++i) {
      assert(idx <= 2 * len + 1 - 3);
      const auto& match = *i;

      // Add a string for non-matched characters.
      list[idx++] = mk_string(state, match.prefix());

      // Add a list for matched substrings.
      const size_t slen = match.size() - 1;

      // Start at 1, because the first match is the whole string.
      auto list2 = state.buildList(slen);
      for (const auto& [si, v2] : enumerate(list2)) {
        if (!match[si + 1].matched) {
          v2 = &value_t::vNull;
        } else {
          v2 = mk_string(state, match[si + 1]);
        }
      }

      (list[idx++] = state.allocValue())->mkList(list2);

      // Add a string for non-matched suffix characters.
      if (idx == 2 * len) {
        list[idx++] = mk_string(state, match.suffix());
      }
    }

    assert(idx == 2 * len + 1);

    v.mkList(list);

  } catch (std::regex_error& e) {
    if (e.code() == std::regex_constants::error_space) {
      // limit is _GLIBCXX_REGEX_STATE_LIMIT for libstdc++
      state.error<EvalError>("memory limit exceeded by regular expression '%s'", re)
          .at_pos(pos)
          .debugThrow();
    } else {
      state.error<EvalError>("invalid regular expression '%s'", re).at_pos(pos).debugThrow();
    }
  }
}

static RegisterPrimOp primop_split({
    .name = "__split",
    .args = {"regex", "str"},
    .doc = R"s(
      Returns a list composed of non matched strings interleaved with the
      lists of the [extended POSIX regular
      expression](http://pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap09.html#tag_09_04)
      *regex* matches of *str*. Each item in the lists of matched
      sequences is a regex group.

      ```nix
      builtins.split "(a)b" "abc"
      ```

      Evaluates to `[ "" [ "a" ] "c" ]`.

      ```nix
      builtins.split "([ac])" "abc"
      ```

      Evaluates to `[ "" [ "a" ] "b" [ "c" ] "" ]`.

      ```nix
      builtins.split "(a)|(c)" "abc"
      ```

      Evaluates to `[ "" [ "a" null ] "b" [ null "c" ] "" ]`.

      ```nix
      builtins.split "([[:upper:]]+)" " FOO "
      ```

      Evaluates to `[ " " [ "FOO" ] " " ]`.
    )s",
    .fun = prim_split,
});

static void prim_concat_strings_sep(eval_state_t& state, const pos_idx_t pos, value_t** args,
                                    value_t& v) {
  NixStringContext context;

  auto sep = state.forceString(*args[0], context, pos,
                               "while evaluating the first argument (the separator string) passed "
                               "to builtins.concatStringsSep");
  state.forceList(*args[1], pos,
                  "while evaluating the second argument (the list of strings to concat) passed to "
                  "builtins.concatStringsSep");

  std::string res;
  res.reserve((args[1]->list_size() + 32) * sep.size());
  bool first = true;

  for (auto elem : args[1]->list_view()) {
    if (first) {
      first = false;
    } else {
      res += sep;
    }
    res += *state.coerceToString(pos, *elem, context,
                                 "while evaluating one element of the list of strings to concat "
                                 "passed to builtins.concatStringsSep");
  }

  v.mk_string(res, context, state.mem);
}

static RegisterPrimOp primop_concat_strings_sep({
    .name = "__concatStringsSep",
    .args = {"separator", "list"},
    .doc = R"(
      Concatenate a list of strings with a separator between each
      element, e.g. `concat_strings_sep "/" ["usr" "local" "bin"] ==
      "usr/local/bin"`.
    )",
    .fun = prim_concat_strings_sep,
});

static void prim_replace_strings(eval_state_t& state, const pos_idx_t pos, value_t** args,
                                 value_t& v) {
  state.forceList(*args[0], pos,
                  "while evaluating the first argument passed to builtins.replaceStrings");
  state.forceList(*args[1], pos,
                  "while evaluating the second argument passed to builtins.replaceStrings");
  if (args[0]->list_size() != args[1]->list_size()) {
    state
        .error<EvalError>(
            "'from' and 'to' arguments passed to builtins.replaceStrings have different lengths")
        .at_pos(pos)
        .debugThrow();
  }

  std::vector<std::string_view> from;
  from.reserve(args[0]->list_size());
  for (auto elem : args[0]->list_view()) {
    from.emplace_back(state.forceString(
        *elem, pos,
        "while evaluating one of the strings to replace passed to builtins.replaceStrings"));
  }

  boost::unordered_flat_map<size_t, std::string_view> cache;
  auto to = args[1]->list_view();

  NixStringContext context;
  auto s =
      state.forceString(*args[2], context, pos,
                        "while evaluating the third argument passed to builtins.replaceStrings");

  std::string res;
  // Loops one past last character to handle the case where 'from' contains an empty string.
  for (size_t p = 0; p <= s.size();) {
    bool found = false;
    auto i = from.begin();
    auto j = to.begin();
    size_t j_index = 0;
    for (; i != from.end(); ++i, ++j, ++j_index) {
      if (s.compare(p, i->size(), *i) == 0) {
        found = true;
        auto v = cache.find(j_index);
        if (v == cache.end()) {
          NixStringContext ctx;
          auto ts = state.forceString(
              **j, ctx, pos,
              "while evaluating one of the replacement strings passed to builtins.replaceStrings");
          v = (cache.emplace(j_index, ts)).first;
          for (auto& path : ctx) {
            context.insert(path);
          }
        }
        res += v->second;
        if (i->empty()) {
          if (p < s.size()) {
            res += s[p];
          }
          p++;
        } else {
          p += i->size();
        }
        break;
      }
    }
    if (!found) {
      if (p < s.size()) {
        res += s[p];
      }
      p++;
    }
  }

  v.mk_string(res, context, state.mem);
}

static RegisterPrimOp primop_replace_strings({
    .name = "__replaceStrings",
    .args = {"from", "to", "s"},
    .doc = R"(
      Given string *s*, replace every occurrence of the strings in *from*
      with the corresponding string in *to*.

      The argument *to* is lazy, that is, it is only evaluated when its corresponding pattern in *from* is matched in the string *s*

      Example:

      ```nix
      builtins.replace_strings ["oo" "a"] ["a" "i"] "foobar"
      ```

      evaluates to `"fabir"`.
    )",
    .fun = prim_replace_strings,
});

/*************************************************************
 * Versions
 *************************************************************/

static void prim_parse_drv_name(eval_state_t& state, const pos_idx_t pos, value_t** args,
                                value_t& v) {
  auto name = state.forceStringNoCtx(
      *args[0], pos, "while evaluating the first argument passed to builtins.parseDrvName");
  DrvName parsed(name);
  auto attrs = state.buildBindings(2);
  attrs.alloc(state.s.name).mk_string(parsed.name, state.mem);
  attrs.alloc("version").mk_string(parsed.version, state.mem);
  v.mkAttrs(attrs);
}

static RegisterPrimOp primop_parse_drv_name({
    .name = "__parseDrvName",
    .args = {"s"},
    .doc = R"(
      Split the string *s* into a package name and version. The package
      name is everything up to but not including the first dash not followed
      by a letter, and the version is everything following that dash. The
      result is returned in a set `{ name, version }`. Thus,
      `builtins.parseDrvName "nix-0.12pre12876"` returns `{ name =
      "nix"; version = "0.12pre12876"; }`.
    )",
    .fun = prim_parse_drv_name,
});

static void prim_compare_versions(eval_state_t& state, const pos_idx_t pos, value_t** args,
                                  value_t& v) {
  auto version1 = state.forceStringNoCtx(
      *args[0], pos, "while evaluating the first argument passed to builtins.compareVersions");
  auto version2 = state.forceStringNoCtx(
      *args[1], pos, "while evaluating the second argument passed to builtins.compareVersions");
  auto result = compare_versions(version1, version2);
  v.mkInt(result < 0 ? -1 : result > 0 ? 1 : 0);
}

static RegisterPrimOp primop_compare_versions({
    .name = "__compareVersions",
    .args = {"s1", "s2"},
    .doc = R"(
      Compare two strings representing versions and return `-1` if
      version *s1* is older than version *s2*, `0` if they are the same,
      and `1` if *s1* is newer than *s2*. The version comparison
      algorithm is the same as the one used by [`nix-env
      -u`](../command-ref/nix-env/upgrade.md).
    )",
    .fun = prim_compare_versions,
});

static void prim_split_version(eval_state_t& state, const pos_idx_t pos, value_t** args,
                               value_t& v) {
  auto version = state.forceStringNoCtx(
      *args[0], pos, "while evaluating the first argument passed to builtins.splitVersion");
  auto iter = version.cbegin();
  strings_t components;
  while (iter != version.cend()) {
    auto component = next_component(iter, version.cend());
    if (component.empty()) {
      break;
    }
    components.emplace_back(component);
  }
  auto list = state.buildList(components.size());
  for (const auto& [n, component] : enumerate(components)) {
    (list[n] = state.allocValue())->mk_string(std::move(component), state.mem);
  }
  v.mkList(list);
}

static RegisterPrimOp primop_split_version({
    .name = "__splitVersion",
    .args = {"s"},
    .doc = R"(
      Split a string representing a version into its components, by the
      same version splitting logic underlying the version comparison in
      [`nix-env -u`](../command-ref/nix-env/upgrade.md).
    )",
    .fun = prim_split_version,
});

/*************************************************************
 * Primop registration
 *************************************************************/

RegisterPrimOp::RegisterPrimOp(PrimOp&& prim_op) {
  primOps().push_back(std::move(prim_op));
}

void eval_state_t::createBaseEnv(const eval_settings_t& eval_settings) {
  baseEnv.up = 0;

  /* Add global constants such as `true' to the base environment. */
  value_t v;

  /* `builtins' must be first! */
  v.mkAttrs(buildBindings(128).finish());
  addConstant("builtins", v,
              {
                  .type = nAttrs,
                  .doc = R"(
          Contains all the built-in functions and values.

          Since built-in functions were added over time, [testing for attributes](./operators.md#has-attribute) in `builtins` can be used for graceful fallback on older Nix installations:

          ```nix
          # if has_context is not available, we assume `s` has a context
          if builtins ? has_context then builtins.has_context s else true
          ```
        )",
              });

  v.mkBool(true);
  addConstant("true", v,
              {
                  .type = nBool,
                  .doc = R"(
          Primitive value.

          It can be returned by
          [comparison operators](@docroot@/language/operators.md#comparison)
          and used in
          [conditional expressions](@docroot@/language/syntax.md#conditionals).

          The name `true` is not special, and can be shadowed:

          ```nix-repl
          nix-repl> let true = 1; in true
          1
          ```
        )",
              });

  v.mkBool(false);
  addConstant("false", v,
              {
                  .type = nBool,
                  .doc = R"(
          Primitive value.

          It can be returned by
          [comparison operators](@docroot@/language/operators.md#comparison)
          and used in
          [conditional expressions](@docroot@/language/syntax.md#conditionals).

          The name `false` is not special, and can be shadowed:

          ```nix-repl
          nix-repl> let false = 1; in false
          1
          ```
        )",
              });

  addConstant("null", &value_t::vNull,
              {
                  .type = nNull,
                  .doc = R"(
          Primitive value.

          The name `null` is not special, and can be shadowed:

          ```nix-repl
          nix-repl> let null = 1; in null
          1
          ```
        )",
              });

  v.mkInt(time(0));
  addConstant("__currentTime", v,
              {
                  .type = nInt,
                  .doc = R"(
          Return the [Unix time](https://en.wikipedia.org/wiki/Unix_time) at first evaluation.
          Repeated references to that name re-use the initially obtained value.

          Example:

          ```console
          $ nix repl
          Welcome to Nix 2.15.1 Type :? for help.

          nix-repl> builtins.currentTime
          1683705525

          nix-repl> builtins.currentTime
          1683705525
          ```

          The [store path](@docroot@/store/store-path.md) of a derivation depending on `currentTime` differs for each evaluation, unless both evaluate `builtins.currentTime` in the same second.
        )",
                  .impureOnly = true,
              });

  v.mk_string(settings.getCurrentSystem(), mem);
  addConstant("__currentSystem", v,
              {
                  .type = nString,
                  .doc = R"(
          The value of the
          [`eval-system`](@docroot@/command-ref/conf-file.md#conf-eval-system)
          or else
          [`system`](@docroot@/command-ref/conf-file.md#conf-system)
          configuration option.

          It can be used to set the `system` attribute for [`builtins.derivation`](@docroot@/language/derivations.md) such that the resulting derivation can be built on the same system that evaluates the Nix expression:

          ```nix
           builtins.derivation {
             # ...
             system = builtins.currentSystem;
          }
          ```

          It can be overridden in order to create derivations for different system than the current one:

          ```console
          $ nix-instantiate --system "mips64-linux" --eval --expr 'builtins.currentSystem'
          "mips64-linux"
          ```
        )",
                  .impureOnly = true,
              });

  v.mk_string(nix_version, mem);
  addConstant("__nixVersion", v,
              {
                  .type = nString,
                  .doc = R"(
          The version of Nix.

          For example, where the command line returns the current Nix version,

          ```shell-session
          $ nix --version
          nix (Nix) 2.16.0
          ```

          the Nix language evaluator returns the same value:

          ```nix-repl
          nix-repl> builtins.nix_version
          "2.16.0"
          ```
        )",
              });

  v.mk_string(store->store_dir, mem);
  addConstant("__storeDir", v,
              {
                  .type = nString,
                  .doc = R"(
          Logical file system location of the [Nix store](@docroot@/glossary.md#gloss-store) currently in use.

          This value is determined by the `store` parameter in [store_t URLs](@docroot@/store/types/index.md#store-url-format):

          ```shell-session
          $ nix-instantiate --store 'dummy://?store=/blah' --eval --expr builtins.storeDir
          "/blah"
          ```
        )",
              });

  /* Language version.  This should be increased every time a new
     language feature gets added.  It's not necessary to increase it
     when primops get added, because you can just use `builtins ?
     prim_op' to check. */
  v.mkInt(6);
  addConstant("__langVersion", v,
              {
                  .type = nInt,
                  .doc = R"(
          The current version of the Nix language.
        )",
              });

#ifndef _WIN32 // TODO implement on Windows
  // Miscellaneous
  if (settings.enableNativeCode) {
    addPrimOp({
        .name = "__importNative",
        .arity = 2,
        .fun = prim_import_native,
    });
    addPrimOp({
        .name = "__exec",
        .arity = 1,
        .fun = prim_exec,
    });
  }
#endif

  addPrimOp({
      .name = "__traceVerbose",
      .args = {"e1", "e2"},
      .arity = 2,
      .doc = R"(
          Evaluate *e1* and print its abstract syntax representation on standard
          error if `--trace-verbose` is enabled. Then return *e2*. This function
          is useful for debugging.
        )",
      .fun = settings.traceVerbose ? prim_trace : prim_second,
  });

  /* Add a value containing the current Nix expression search path. */
  auto list = buildList(lookup_path.elements.size());
  for (const auto& [n, i] : enumerate(lookup_path.elements)) {
    auto attrs = buildBindings(2);
    attrs.alloc("path").mk_string(i.path.s, mem);
    attrs.alloc("prefix").mk_string(i.prefix.s, mem);
    (list[n] = allocValue())->mkAttrs(attrs);
  }
  v.mkList(list);
  addConstant("__nixPath", v,
              {
                  .type = nList,
                  .doc = R"(
          A list of search path entries used to resolve [lookup paths](@docroot@/language/constructs/lookup-path.md).
          Its value is primarily determined by the [`nix-path` configuration setting](@docroot@/command-ref/conf-file.md#conf-nix-path), which are
          - Overridden by the [`NIX_PATH`](@docroot@/command-ref/env-common.md#env-NIX_PATH) environment variable or the `--nix-path` option
          - Extended by the [`-I` option](@docroot@/command-ref/opt-common.md#opt-I) or `--extra-nix-path`

          > **Example**
          >
          > ```bash
          > $ NIX_PATH= nix-instantiate --eval --expr "builtins.nixPath" -I foo=bar --no-pure-eval
          > [ { path = "bar"; prefix = "foo"; } ]
          > ```

          Lookup path expressions are [desugared](https://en.wikipedia.org/wiki/Syntactic_sugar) using this and
          [`builtins.findFile`](./builtins.html#builtins-findFile):

          ```nix
          <nixpkgs>
          ```

          is equivalent to:

          ```nix
          builtins.findFile builtins.nixPath "nixpkgs"
          ```
        )",
              });

  for (auto& prim_op : RegisterPrimOp::primOps()) {
    if (experimental_feature_settings.is_enabled(prim_op.experimental_feature)) {
      auto primOpAdjusted = prim_op;
      primOpAdjusted.arity = std::max(prim_op.args.size(), prim_op.arity);
      addPrimOp(std::move(primOpAdjusted));
    }
  }

  for (auto& prim_op : eval_settings.extraPrimOps) {
    auto primOpAdjusted = prim_op;
    primOpAdjusted.arity = std::max(prim_op.args.size(), prim_op.arity);
    addPrimOp(std::move(primOpAdjusted));
  }

  /* Add a wrapper around the derivation primop that computes the
     `drv_path' and `outPath' attributes lazily.

     Null docs because it is documented separately.
     */
  auto vDerivation = allocValue();
  addConstant("derivation", vDerivation,
              {
                  .type = nFunction,
              });

  /* Now that we've added all primops, sort the `builtins' set,
     because attribute lookups expect it to be sorted. */
  const_cast<bindings_t*>(getBuiltins().attrs())->sort();

  staticBaseEnv->sort();

  /* Note: we have to initialize the 'derivation' constant *after*
     building baseEnv/staticBaseEnv because it uses 'builtins'. */
  evalFile(derivationInternal, *vDerivation);
}

} // namespace nix
