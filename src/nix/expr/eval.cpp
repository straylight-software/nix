#include "nix/expr/eval.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <ranges>
#include <sstream>

#include <sys/time.h>
#include <unistd.h>

#include <boost/container/small_vector.hpp>
#include <boost/unordered/concurrent_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <nlohmann/json.hpp>

#include "nix/expr/eval-inline.h"
#include "nix/expr/eval-settings.h"
#include "nix/expr/function-trace.h"
#include "nix/expr/gc-small-vector.h"
#include "nix/expr/parallel-eval.h"
#include "nix/expr/primops.h"
#include "nix/expr/print-options.h"
#include "nix/expr/print.h"
#include "nix/expr/symbol-table.h"
#include "nix/expr/value.h"
#include "nix/fetchers/fetch-to-store.h"
#include "nix/fetchers/filtering-source-accessor.h"
#include "nix/fetchers/input-cache.h"
#include "nix/fetchers/tarball.h"
#include "nix/store/async-path-writer.h"
#include "nix/store/derivations.h"
#include "nix/store/downstream-placeholder.h"
#include "nix/store/filetransfer.h"
#include "nix/store/profiles.h"
#include "nix/store/store-api.h"
#include "nix/util/current-process.h"
#include "nix/util/environment-variables.h"
#include "nix/util/exit.h"
#include "nix/util/memory-source-accessor.h"
#include "nix/util/mounted-source-accessor.h"
#include "nix/util/types.h"
#include "nix/util/url.h"
#include "nix/util/util.h"
#include "parser-tab.h"

#ifndef _WIN32 // TODO use portable implementation
#  include <sys/resource.h>
#endif

#include "nix/util/strings-inline.h"

using json = nlohmann::json;

namespace nix {

/**
 * Just for doc strings. Not for regular string values.
 */
static char* alloc_string(size_t size) {
  char* t;
  t = (char*)GC_MALLOC_ATOMIC(size);
  if (!t)
    throw std::bad_alloc();
  return t;
}

// When there's no need to write to the string, we can optimize away empty
// string allocations.
// This function handles makeImmutableString(std::string_view()) by returning
// the empty string.
/**
 * Just for doc strings. Not for regular string values.
 */
static const char* make_immutable_string(std::string_view s) {
  const size_t size = s.size();
  if (size == 0)
    return "";
  auto t = alloc_string(size + 1);
  memcpy(t, s.data(), size);
  t[size] = '\0';
  return t;
}

StringData& StringData::alloc(EvalMemory& mem, size_t size) {
  void* t = mem.allocBytes(sizeof(StringData) + size + 1);
  if (!t)
    throw std::bad_alloc();
  auto res = new (t) StringData(size);
  return *res;
}

const StringData& StringData::make(EvalMemory& mem, std::string_view s) {
  if (s.empty())
    return ""_sds;
  auto& res = alloc(mem, s.size());
  std::memcpy(&res.data_, s.data(), s.size());
  res.data_[s.size()] = '\0';
  return res;
}

RootValue alloc_root_value(Value* v) {
  return std::allocate_shared<Value*>(traceable_allocator<Value*>(), v);
}

// Pretty print types for assertion errors
std::ostream& operator<<(std::ostream& os, const ValueType t) {
  os << show_type(t);
  return os;
}

std::string print_value(EvalState& state, Value& v) {
  std::ostringstream out;
  v.print(state, out);
  return out.str();
}

Value* Value::toPtr(SymbolStr str) noexcept {
  return const_cast<Value*>(str.valuePtr());
}

void Value::print(EvalState& state, std::ostream& str, PrintOptions options) {
  print_value(state, str, *this, options);
}

std::string_view show_type(ValueType type, bool withArticle) {
#define WA(a, w) withArticle ? a " " w : w
  switch (type) {
    case nInt:
      return WA("an", "integer");
    case nBool:
      return WA("a", "Boolean");
    case nString:
      return WA("a", "string");
    case nPath:
      return WA("a", "path");
    case nNull:
      return "null";
    case nAttrs:
      return WA("a", "set");
    case nList:
      return WA("a", "list");
    case nFunction:
      return WA("a", "function");
    case nExternal:
      return WA("an", "external value");
    case nFloat:
      return WA("a", "float");
    case nThunk:
      return WA("a", "thunk");
    case nFailed:
      return WA("a", "failure");
  }
  unreachable();
}

std::string show_type(const Value& v) {
// Allow selecting a subset of enum values
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
  switch (v.getInternalType()) {
    case t_string:
      return v.context() ? "a string with context" : "a string";
    case tPrimOp:
      return fmt("the built-in function '%s'", std::string(v.prim_op()->name));
    case tPrimOpApp:
      return fmt("the partially applied built-in function '%s'", v.primOpAppPrimOp()->name);
    case tExternal:
      return v.external()->show_type();
    case tThunk:
      return v.isBlackhole() ? "a black hole" : "a thunk";
    case tApp:
      return "a function application";
    default:
      return std::string(show_type(v.type()));
  }
#pragma GCC diagnostic pop
}

pos_idx_t Value::determinePos(const pos_idx_t pos) const {
// Allow selecting a subset of enum values
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
  switch (getInternalType()) {
    case tAttrs:
      return attrs()->pos;
    case tLambda:
      return lambda().fun->pos;
#if 0
    // FIXME: disabled because reading from an app is racy.
    case tApp:
        return app().left->determinePos(pos);
#endif
    default:
      return pos;
  }
#pragma GCC diagnostic pop
}

template <>
bool ValueStorage<sizeof(void*)>::isTrivial() const {
  auto p1_ = p1; // must acquire before reading p0, since thunks can change
  auto p0_ = p0.load(std::memory_order_acquire);

  auto pd = static_cast<PrimaryDiscriminator>(p0_ & discriminatorMask);

  if (pd == pdThunk || pd == pdPending || pd == pdAwaited) {
    bool isApp = p1_ & discriminatorMask;
    if (isApp)
      return false;
    auto expr = untagPointer<Expr*>(p1_);
    return (dynamic_cast<ExprAttrs*>(expr) && ((ExprAttrs*)expr)->dynamicAttrs->empty()) ||
           dynamic_cast<ExprLambda*>(expr) || dynamic_cast<ExprList*>(expr);
  }

  else
    return true;
}

static Symbol get_name(const AttrName& name, EvalState& state, Env& env) {
  if (name.symbol) {
    return name.symbol;
  } else {
    Value name_value;
    name.expr->eval(state, env, name_value);
    state.forceStringNoCtx(name_value, name.expr->getPos(), "while evaluating an attribute name");
    return state.symbols.create(name_value.string_view());
  }
}

static constexpr size_t BASE_ENV_SIZE = 128;

EvalMemory::EvalMemory()
#if NIX_USE_BOEHMGC
    : valueAllocCache(std::allocate_shared<void*>(traceable_allocator<void*>(), nullptr)),
      env1AllocCache(std::allocate_shared<void*>(traceable_allocator<void*>(), nullptr))
#endif
{
  assert_gc_initialized();
}

EvalState::EvalState(const LookupPath& lookupPathFromArguments, ref<Store> store,
                     const fetchers::settings_t& fetch_settings, const EvalSettings& settings,
                     std::shared_ptr<Store> buildStore)
    : fetch_settings{fetch_settings},
      settings{settings},
      symbols(StaticEvalSymbols::staticSymbolTable()),
      repair(NoRepair),
      storeFS(make_mounted_source_accessor({
          {canon_path_t::root, make_empty_source_accessor()},
          /* In the pure eval case, we can simply require
             valid paths. However, in the *impure* eval
             case this gets in the way of the union
             mechanism, because an invalid access in the
             upper layer will *not* be caught by the union
             source accessor, but instead abort the entire
             lookup.

             This happens when the store dir in the
             ambient file system has a path (e.g. because
             another Nix store there), but the relocated
             store does not.

             TODO make the various source accessors doing
             access control all throw the same type of
             exception, and make union source accessor
             catch it, so we don't need to do this hack.
           */
          {canon_path_t(store->store_dir), store->getFSAccessor(settings.pureEval)},
      })),
      root_fs([&] {
        /* In pure eval mode, we provide a filesystem that only
           contains the Nix store.

           Otherwise, use a union accessor to make the augmented store
           available at its logical location while still having the
           underlying directory available. This is necessary for
           instance if we're evaluating a file from the physical
           /nix/store while using a chroot store, and also for lazy
           mounted fetch_tree. */
        auto accessor = settings.pureEval
                            ? storeFS.cast<SourceAccessor>()
                            : make_union_source_accessor({get_fs_source_accessor(), storeFS});

        /* Apply access control if needed. */
        if (settings.restrictEval || settings.pureEval)
          accessor = AllowListSourceAccessor::create(
              accessor, {}, {}, [&settings](const canon_path_t& path) -> RestrictedPathError {
                auto modeInformation = settings.pureEval
                                           ? "in pure evaluation mode (use '--impure' to override)"
                                           : "in restricted mode";
                throw RestrictedPathError("access to absolute path '%1%' is forbidden %2%", path,
                                          modeInformation);
              });

        return accessor;
      }()),
      corepkgsFS(make_ref<memory_source_accessor_t>()),
      internal_fs(make_ref<memory_source_accessor_t>()),
      derivationInternal{internal_fs->add_file(canon_path_t("derivation-internal.nix"),
#include "primops/derivation.nix.gen.h"
                                             )},
      store(store),
      buildStore(buildStore ? buildStore : store),
      inputCache(fetchers::InputCache::create()),
      debugRepl(nullptr),
      debugStop(false),
      trylevel(0),
      async_path_writer(AsyncPathWriter::make(store)),
      srcToStore(make_ref<decltype(srcToStore)::element_type>()),
      importResolutionCache(make_ref<decltype(importResolutionCache)::element_type>()),
      fileEvalCache(make_ref<decltype(fileEvalCache)::element_type>()),
      regexCache(make_regex_cache())
#if NIX_USE_BOEHMGC
      ,
      baseEnvP(
          std::allocate_shared<Env*>(traceable_allocator<Env*>(), &mem.allocEnv(BASE_ENV_SIZE))),
      baseEnv(**baseEnvP)
#else
      ,
      baseEnv(mem.allocEnv(BASE_ENV_SIZE))
#endif
      ,
      staticBaseEnv{std::make_shared<StaticEnv>(nullptr, nullptr)},
      executor{make_ref<Executor>(settings)} {
  corepkgsFS->set_path_display("<nix", ">");
  internal_fs->set_path_display("«nix-internal»", "");

  countCalls = get_env("NIX_COUNT_CALLS").value_or("0") != "0";

  static_assert(sizeof(Env) <= 16, "environment must be <= 16 bytes");
  static_assert(sizeof(Counter) == 64, "counters must be 64 bytes");

  /* Construct the Nix expression search path. */
  assert(lookup_path.elements.empty());
  if (!settings.pureEval) {
    for (auto& i : lookupPathFromArguments.elements) {
      lookup_path.elements.emplace_back(LookupPath::Elem{i});
    }
    /* $NIX_PATH overriding regular settings is implemented as a hack in `init_gc()` */
    for (auto& i : settings.nixPath.get()) {
      lookup_path.elements.emplace_back(LookupPath::Elem::parse(i));
    }
    if (!settings.restrictEval) {
      for (auto& i : EvalSettings::getDefaultNixPath()) {
        lookup_path.elements.emplace_back(LookupPath::Elem::parse(i));
      }
    }
  }

  /* Allow access to all paths in the search path. */
  if (root_fs.dynamic_pointer_cast<AllowListSourceAccessor>())
    for (auto& i : lookup_path.elements)
      resolveLookupPathPath(i.path, true);

  corepkgsFS->add_file(canon_path_t("fetchurl.nix"),
#include "fetchurl.nix.gen.h"
  );

  createBaseEnv(settings);

  /* Register function call tracer. */
  if (settings.traceFunctionCalls)
    profiler.addProfiler(make_ref<FunctionCallTrace>());

  switch (settings.evalProfilerMode) {
    case EvalProfilerMode::flamegraph:
      profiler.addProfiler(make_sample_stack_profiler(*this, settings.evalProfileFile.get(),
                                                   settings.evalProfilerFrequency));
      break;
    case EvalProfilerMode::disabled:
      break;
  }
}

EvalState::~EvalState() {}

void EvalState::allowPathLegacy(const Path& path) {
  if (auto rootFS2 = root_fs.dynamic_pointer_cast<AllowListSourceAccessor>())
    rootFS2->allowPrefix(canon_path_t(path));
}

void EvalState::allowPath(const StorePath& store_path) {
  if (auto rootFS2 = root_fs.dynamic_pointer_cast<AllowListSourceAccessor>())
    rootFS2->allowPrefix(canon_path_t(store->printStorePath(store_path)));
}

void EvalState::allowClosure(const StorePath& store_path) {
  if (!root_fs.dynamic_pointer_cast<AllowListSourceAccessor>())
    return;

  StorePathSet closure;
  store->computeFSClosure(store_path, closure);
  for (auto& p : closure)
    allowPath(p);
}

void EvalState::allowAndSetStorePathString(const StorePath& store_path, Value& v) {
  allowPath(store_path);

  mkStorePathString(store_path, v);
}

inline static bool is_just_scheme_prefix(std::string_view prefix) {
  return !prefix.empty() && prefix[prefix.size() - 1] == ':' &&
         is_valid_scheme_name(prefix.substr(0, prefix.size() - 1));
}

bool is_allowed_uri(std::string_view uri, const strings_t& allowed_uris) {
  /* 'uri' should be equal to a prefix, or in a subdirectory of a
     prefix. Thus, the prefix https://github.co does not permit
     access to https://github.com. */
  for (auto& prefix : allowed_uris) {
    if (uri == prefix
        // Allow access to subdirectories of the prefix.
        || (uri.size() > prefix.size() && prefix.size() > 0 && has_prefix(uri, prefix) &&
            (
                // Allow access to subdirectories of the prefix.
                prefix[prefix.size() - 1] == '/' ||
                uri[prefix.size()] == '/'

                // Allow access to whole schemes
                || is_just_scheme_prefix(prefix))))
      return true;
  }

  return false;
}

void EvalState::checkURI(const std::string& uri) {
  if (!settings.restrictEval)
    return;

  if (is_allowed_uri(uri, settings.allowed_uris.get()))
    return;

  /* If the URI is a path, then check it against allowed_paths as
     well. */
  if (is_absolute(uri)) {
    if (auto rootFS2 = root_fs.dynamic_pointer_cast<AllowListSourceAccessor>())
      rootFS2->checkAccess(canon_path_t(uri));
    return;
  }

  if (has_prefix(uri, "file://")) {
    if (auto rootFS2 = root_fs.dynamic_pointer_cast<AllowListSourceAccessor>())
      rootFS2->checkAccess(canon_path_t(uri.substr(7)));
    return;
  }

  throw RestrictedPathError("access to URI '%s' is forbidden in restricted mode", uri);
}

Value* EvalState::addConstant(const std::string& name, Value& v, Constant info) {
  Value* v2 = allocValue();
  // Do a raw copy since `operator =` barfs on thunks.
  memcpy((char*)v2, (char*)&v, sizeof(Value));
  addConstant(name, v2, info);
  return v2;
}

void EvalState::addConstant(const std::string& name, Value* v, Constant info) {
  auto name2 = name.substr(0, 2) == "__" ? name.substr(2) : name;

  constantInfos.push_back({name2, info});

  if (!(settings.pureEval && info.impureOnly)) {
    /* Check the type, if possible.

       We might know the type of a thunk in advance, so be allowed
       to just write it down in that case. */
    if (v->isFinished()) {
      if (auto gotType = v->type(); gotType != nThunk)
        assert(info.type == gotType);
    }

    /* Install value the base environment. */
    staticBaseEnv->vars.emplace_back(symbols.create(name), baseEnvDispl);
    baseEnv.values[baseEnvDispl++] = v;
    const_cast<Bindings*>(getBuiltins().attrs())->push_back(Attr(symbols.create(name2), v));
  }
}

void PrimOp::check() {
  if (arity > maxPrimOpArity) {
    throw Error("primop arity must not exceed %1%", maxPrimOpArity);
  }
}

std::ostream& operator<<(std::ostream& output, const PrimOp& prim_op) {
  output << "primop " << prim_op.name;
  return output;
}

const PrimOp* Value::primOpAppPrimOp() const {
  Value* left = primOpApp().left;
  while (left && !left->isPrimOp()) {
    left = left->primOpApp().left;
  }

  if (!left)
    return nullptr;

  assert(left->isPrimOp());
  return left->prim_op();
}

void Value::mkPrimOp(PrimOp* p) {
  p->check();
  setStorage(p);
}

Value* EvalState::addPrimOp(PrimOp&& prim_op) {
  /* Hack to make constants lazy: turn them into a application of
     the primop to a dummy value. */
  if (prim_op.arity == 0) {
    prim_op.arity = 1;
    auto vPrimOp = allocValue();
    vPrimOp->mkPrimOp(new PrimOp(std::move(prim_op)));
    Value v;
    v.mkApp(vPrimOp, vPrimOp);
    auto& primOp1 = *vPrimOp->prim_op();
    return addConstant(primOp1.name, v,
                       {
                           .type = nThunk, // FIXME
                           .doc = primOp1.doc ? primOp1.doc->c_str() : nullptr,
                       });
  }

  auto envName = symbols.create(prim_op.name);
  if (has_prefix(prim_op.name, "__"))
    prim_op.name = prim_op.name.substr(2);

  Value* v = allocValue();
  v->mkPrimOp(new PrimOp(prim_op));

  if (prim_op.internal)
    internalPrimOps.emplace(prim_op.name, v);
  else {
    staticBaseEnv->vars.emplace_back(envName, baseEnvDispl);
    baseEnv.values[baseEnvDispl++] = v;
    const_cast<Bindings*>(getBuiltins().attrs())->push_back(Attr(symbols.create(prim_op.name), v));
  }

  return v;
}

Value& EvalState::getBuiltins() {
  return *baseEnv.values[0];
}

Value& EvalState::getBuiltin(const std::string& name) {
  auto it = getBuiltins().attrs()->get(symbols.create(name));
  if (it)
    return *it->value;
  else
    error<EvalError>("builtin '%1%' not found", name).debugThrow();
}

std::optional<EvalState::Doc> EvalState::getDoc(Value& v) {
  if (v.isPrimOp()) {
    auto v2 = &v;
    auto& prim_op = *v2->prim_op();
    if (prim_op.doc)
      return Doc{
          .pos = {},
          .name = prim_op.name,
          .arity = prim_op.arity,
          .args = prim_op.args,
          .doc = prim_op.doc->c_str(),
      };
  }
  if (v.isLambda()) {
    auto exprLambda = v.lambda().fun;

    std::ostringstream s;
    std::string name;
    auto pos = positions[exprLambda->getPos()];
    std::string docStr;

    if (exprLambda->name) {
      name = symbols[exprLambda->name];
    }

    if (exprLambda->doc_comment) {
      docStr = exprLambda->doc_comment.getInnerText(positions);
    }

    if (name.empty()) {
      s << "Function ";
    } else {
      s << "Function `" << name << "`";
      if (pos)
        s << "\\\n  … ";
      else
        s << "\\\n";
    }
    if (pos) {
      s << "defined at " << pos;
    }
    if (!docStr.empty()) {
      s << "\n\n";
    }

    s << docStr;

    return Doc{
        .pos = pos,
        .name = name,
        .arity =
            0, // FIXME: figure out how deep by syntax only? It's not semantically useful though...
        .args = {},
        /* N.B. Can't use StringData here, because that would lead to an interior pointer.
           NOTE: memory leak when compiled without GC. */
        .doc = make_immutable_string(s.view()),
    };
  }
  if (isFunctor(v)) {
    try {
      Value& functor = *v.attrs()->get(s.functor)->value;
      Value* vp[] = {&v};
      Value partiallyApplied;
      // The first parameter is not user-provided, and may be
      // handled by code that is opaque to the user, like lib.const = x: y: y;
      // So preferably we show docs that are relevant to the
      // "partially applied" function returned by e.g. `const`.
      // We apply the first argument:
      callFunction(functor, vp, partiallyApplied, no_pos);
      auto _level = addCallDepth(no_pos);
      return getDoc(partiallyApplied);
    } catch (Error& e) {
      e.add_trace(nullptr, "while partially calling '%1%' to retrieve documentation", "__functor");
      throw;
    }
  }
  return {};
}

// just for the current level of StaticEnv, not the whole chain.
void print_static_env_bindings(const SymbolTable& st, const StaticEnv& se) {
  std::cout << ANSI_MAGENTA;
  for (auto& i : se.vars)
    std::cout << st[i.first] << " ";
  std::cout << ANSI_NORMAL;
  std::cout << std::endl;
}

// just for the current level of Env, not the whole chain.
void print_with_bindings(const SymbolTable& st, const Env& env) {
  if (env.values[0]->isFinished()) {
    std::cout << "with: ";
    std::cout << ANSI_MAGENTA;
    auto j = env.values[0]->attrs()->begin();
    while (j != env.values[0]->attrs()->end()) {
      std::cout << st[j->name] << " ";
      ++j;
    }
    std::cout << ANSI_NORMAL;
    std::cout << std::endl;
  }
}

void print_env_bindings(const SymbolTable& st, const StaticEnv& se, const Env& env, int lvl) {
  std::cout << "Env level " << lvl << std::endl;

  if (se.up && env.up) {
    std::cout << "static: ";
    print_static_env_bindings(st, se);
    if (se.isWith)
      print_with_bindings(st, env);
    std::cout << std::endl;
    print_env_bindings(st, *se.up, *env.up, ++lvl);
  } else {
    std::cout << ANSI_MAGENTA;
    // for the top level, don't print the double underscore ones;
    // they are in builtins.
    for (auto& i : se.vars)
      if (!has_prefix(st[i.first], "__"))
        std::cout << st[i.first] << " ";
    std::cout << ANSI_NORMAL;
    std::cout << std::endl;
    if (se.isWith)
      print_with_bindings(st, env); // probably nothing there for the top level.
    std::cout << std::endl;
  }
}

void print_env_bindings(const EvalState& es, const Expr& expr, const Env& env) {
  // just print the names for now
  auto se = es.getStaticEnv(expr);
  if (se)
    print_env_bindings(es.symbols, *se, env, 0);
}

void map_static_env_bindings(const SymbolTable& st, const StaticEnv& se, const Env& env, ValMap& vm) {
  // add bindings for the next level up first, so that the bindings for this level
  // override the higher levels.
  // The top level bindings (builtins) are skipped since they are added for us by initEnv()
  if (env.up && se.up) {
    map_static_env_bindings(st, *se.up, *env.up, vm);

    if (se.isWith && env.values[0]->isFinished()) {
      // add 'with' bindings.
      for (auto& j : *env.values[0]->attrs())
        vm.insert_or_assign(std::string(st[j.name]), j.value);
    } else {
      // iterate through staticenv bindings and add them.
      for (auto& i : se.vars)
        vm.insert_or_assign(std::string(st[i.first]), env.values[i.second]);
    }
  }
}

std::unique_ptr<ValMap> map_static_env_bindings(const SymbolTable& st, const StaticEnv& se,
                                             const Env& env) {
  auto vm = std::make_unique<ValMap>();
  map_static_env_bindings(st, se, env, *vm);
  return vm;
}

/**
 * Sets `inDebugger` to true on construction and false on destruction.
 */
class debugger_guard_t {
  bool& inDebugger;

public:
  debugger_guard_t(bool& inDebugger) : inDebugger(inDebugger) { inDebugger = true; }

  ~debugger_guard_t() { inDebugger = false; }
};

bool EvalState::canDebug() {
  return debugRepl && !debugTraces.empty();
}

void EvalState::runDebugRepl(const Error* error) {
  if (!canDebug())
    return;

  assert(!debugTraces.empty());
  const DebugTrace& last = debugTraces.front();
  const Env& env = last.env;
  const Expr& expr = last.expr;

  runDebugRepl(error, env, expr);
}

void EvalState::runDebugRepl(const Error* error, const Env& env, const Expr& expr) {
  // Make sure we have a debugger to run and we're not already in a debugger.
  if (!debugRepl || inDebugger)
    return;

  auto dts = [&]() -> std::unique_ptr<DebugTraceStacker> {
    if (error && expr.getPos()) {
      auto trace = DebugTrace{.pos = [&]() -> std::variant<pos_t, pos_idx_t> {
                                if (error->info().pos) {
                                  if (auto* pos = error->info().pos.get())
                                    return *pos;
                                  return no_pos;
                                }
                                return expr.getPos();
                              }(),
                              .expr = expr,
                              .env = env,
                              .hint = error->info().msg,
                              .isError = true};

      return std::make_unique<DebugTraceStacker>(*this, std::move(trace));
    }
    return nullptr;
  }();

  if (error) {
    printError("%s\n", error->what());

    if (trylevel > 0 && error->info().level != lvl_info)
      printError("This exception occurred in a 'tryEval' call. Use " ANSI_GREEN
                 "--ignore-try" ANSI_NORMAL " to skip these.\n");
  }

  auto se = getStaticEnv(expr);
  if (se) {
    auto vm = map_static_env_bindings(symbols, *se.get(), env);
    debugger_guard_t _guard(inDebugger);
    auto exitStatus = (debugRepl)(ref<EvalState>(shared_from_this()), *vm);
    switch (exitStatus) {
      case ReplExitStatus::QuitAll:
        if (error)
          throw *error;
        throw exit_t(0);
      case ReplExitStatus::Continue:
        break;
      default:
        unreachable();
    }
  }
}

template <typename... Args>
void EvalState::addErrorTrace(Error& e, const Args&... formatArgs) const {
  e.add_trace(nullptr, hint_fmt_t(formatArgs...));
}

template <typename... Args>
void EvalState::addErrorTrace(Error& e, const pos_idx_t pos, const Args&... formatArgs) const {
  e.add_trace(positions[pos], hint_fmt_t(formatArgs...));
}

template <typename... Args>
static std::unique_ptr<DebugTraceStacker>
make_debug_trace_stacker(EvalState& state, Expr& expr, Env& env, std::variant<pos_t, pos_idx_t> pos,
                      const Args&... formatArgs) {
  return std::make_unique<DebugTraceStacker>(state, DebugTrace{.pos = std::move(pos),
                                                               .expr = expr,
                                                               .env = env,
                                                               .hint = hint_fmt_t(formatArgs...),
                                                               .isError = false});
}

DebugTraceStacker::DebugTraceStacker(EvalState& eval_state, DebugTrace t)
    : eval_state(eval_state), trace(std::move(t)) {
  eval_state.debugTraces.push_front(trace);
  if (eval_state.debugStop && eval_state.debugRepl)
    eval_state.runDebugRepl(nullptr, trace.env, trace.expr);
}

void Value::mk_string(std::string_view s, EvalMemory& mem) {
  mkStringNoCopy(StringData::make(mem, s));
}

Value::StringWithContext::Context*
Value::StringWithContext::Context::fromBuilder(const NixStringContext& context, EvalMemory& mem) {
  if (context.empty())
    return nullptr;

  auto ctx = new (mem.allocBytes(sizeof(Context) + context.size() * sizeof(value_type)))
      Context(context.size());
  std::ranges::transform(context, ctx->elems, [&](const NixStringContextElem& elt) {
    return &StringData::make(mem, elt.to_string());
  });
  return ctx;
}

void Value::mk_string(std::string_view s, const NixStringContext& context, EvalMemory& mem) {
  mkStringNoCopy(StringData::make(mem, s),
                 Value::StringWithContext::Context::fromBuilder(context, mem));
}

void Value::mkStringMove(const StringData& s, const NixStringContext& context, EvalMemory& mem) {
  mkStringNoCopy(s, Value::StringWithContext::Context::fromBuilder(context, mem));
}

void Value::mkPath(const source_path_t& path, EvalMemory& mem) {
  mkPath(&*path.accessor, StringData::make(mem, path.path.abs()));
}

inline Value* EvalState::lookupVar(Env* env, const ExprVar& var, bool noEval) {
  for (auto l = var.level; l; --l, env = env->up)
    ;

  if (!var.fromWith)
    return env->values[var.displ];

  // This early exit defeats the `maybeThunk` optimization for variables from `with`,
  // The added complexity of handling this appears to be similarly in cost, or
  // the cases where applicable were insignificant in the first place.
  if (noEval)
    return nullptr;

  auto* fromWith = var.fromWith;
  while (1) {
    forceAttrs(*env->values[0], fromWith->pos,
               "while evaluating the first subexpression of a with expression");
    if (auto j = env->values[0]->attrs()->get(var.name)) {
      if (countCalls)
        attrSelects[j->pos]++;
      return j->value;
    }
    if (!fromWith->parentWith)
      error<UndefinedVarError>("undefined variable '%1%'", symbols[var.name])
          .at_pos(var.pos)
          .withFrame(*env, var)
          .debugThrow();
    for (size_t l = fromWith->prevWith; l; --l, env = env->up)
      ;
    fromWith = fromWith->parentWith;
  }
}

ListBuilder::ListBuilder(EvalMemory& mem, size_t size)
    : size(size), elems(size <= 2 ? inlineElems : (Value**)mem.allocBytes(size * sizeof(Value*))) {}

Value* EvalState::getBool(bool b) {
  return b ? &Value::vTrue : &Value::vFalse;
}

static Counter nr_thunks;

static inline void mk_thunk(Value& v, Env& env, Expr* expr) {
  v.mk_thunk(&env, expr);
  nr_thunks++;
}

void EvalState::mkThunk_(Value& v, Expr* expr) {
  mk_thunk(v, baseEnv, expr);
}

void EvalState::mkPos(Value& v, pos_idx_t p) {
  auto origin = positions.origin_of(p);
  if (auto path = std::get_if<source_path_t>(&origin)) {
    auto attrs = buildBindings(3);
    if (path->accessor == root_fs && store->isInStore(path->path.abs()))
      // FIXME: only do this for virtual store paths?
      attrs.alloc(s.file).mk_string(
          path->path.abs(),
          {NixStringContextElem::Path{.store_path = store->toStorePath(path->path.abs()).first}},
          mem);
    else
      attrs.alloc(s.file).mk_string(path->path.abs(), mem);
    make_position_thunks(*this, p, attrs.alloc(s.line), attrs.alloc(s.column));
    v.mkAttrs(attrs);
  } else
    v.mkNull();
}

void EvalState::mkStorePathString(const StorePath& p, Value& v) {
  v.mk_string(store->printStorePath(p),
             NixStringContext{
                 NixStringContextElem::opaque_t{.path = p},
             },
             mem);
}

std::string EvalState::mkOutputStringRaw(const SingleDerivedPath::Built& b,
                                         std::optional<StorePath> optStaticOutputPath,
                                         const experimental_feature_settings_t& xp_settings) {
  /* In practice, this is testing for the case of CA derivations, or
     dynamic derivations. */
  return optStaticOutputPath
             ? store->printStorePath(std::move(*optStaticOutputPath))
             /* Downstream we would substitute this for an actual path once
                we build the floating CA derivation */
             : DownstreamPlaceholder::fromSingleDerivedPathBuilt(b, xp_settings).render();
}

void EvalState::mk_output_string(Value& value, const SingleDerivedPath::Built& b,
                               std::optional<StorePath> optStaticOutputPath,
                               const experimental_feature_settings_t& xp_settings) {
  value.mk_string(mkOutputStringRaw(b, optStaticOutputPath, xp_settings), NixStringContext{b}, mem);
}

std::string EvalState::mkSingleDerivedPathStringRaw(const SingleDerivedPath& p) {
  return std::visit(
      overloaded{[&](const SingleDerivedPath::opaque_t& o) { return store->printStorePath(o.path); },
                 [&](const SingleDerivedPath::Built& b) {
                   auto optStaticOutputPath = std::visit(
                       overloaded{
                           [&](const SingleDerivedPath::opaque_t& o) {
                             waitForPath(o.path);
                             auto drv = store->read_derivation(o.path);
                             auto i = drv.outputs.find(b.output);
                             if (i == drv.outputs.end())
                               throw Error("derivation '%s' does not have output '%s'",
                                           b.drv_path->to_string(*store), b.output);
                             return i->second.path(*store, drv.name, b.output);
                           },
                           [&](const SingleDerivedPath::Built& o) -> std::optional<StorePath> {
                             return std::nullopt;
                           },
                       },
                       b.drv_path->raw());
                   return mkOutputStringRaw(b, optStaticOutputPath);
                 }},
      p.raw());
}

void EvalState::mkSingleDerivedPathString(const SingleDerivedPath& p, Value& v) {
  v.mk_string(mkSingleDerivedPathStringRaw(p),
             NixStringContext{
                 std::visit([](auto&& v) -> NixStringContextElem { return v; }, p),
             },
             mem);
}

Value* Expr::maybeThunk(EvalState& state, Env& env) {
  Value* v = state.allocValue();
  mk_thunk(*v, env, this);
  return v;
}

Value* ExprVar::maybeThunk(EvalState& state, Env& env) {
  Value* v = state.lookupVar(&env, *this, true);
  /* The value might not be initialised in the environment yet.
     In that case, ignore it. */
  if (v) {
    state.nrAvoided++;
    return v;
  }
  return Expr::maybeThunk(state, env);
}

Value* ExprString::maybeThunk(EvalState& state, Env& env) {
  state.nrAvoided++;
  return &v;
}

Value* ExprInt::maybeThunk(EvalState& state, Env& env) {
  state.nrAvoided++;
  return &v;
}

Value* ExprFloat::maybeThunk(EvalState& state, Env& env) {
  state.nrAvoided++;
  return &v;
}

Value* ExprPath::maybeThunk(EvalState& state, Env& env) {
  state.nrAvoided++;
  return &v;
}

/**
 * A helper `Expr` class to lets us parse and evaluate Nix expressions
 * from a thunk, ensuring that every file is parsed/evaluated only
 * once (via the thunk stored in `EvalState::fileEvalCache`).
 */
struct expr_parse_file_t : Expr {
  source_path_t& path;
  bool must_be_trivial;

  expr_parse_file_t(source_path_t& path, bool must_be_trivial) : path(path), must_be_trivial(must_be_trivial) {}

  void eval(EvalState& state, Env& env, Value& v) override {
    printTalkative("evaluating file '%s'", path);

    auto e = state.parseExprFromFile(path);

    try {
      auto dts = state.debugRepl
                     ? make_debug_trace_stacker(state, *e, state.baseEnv, e->getPos(),
                                             "while evaluating the file '%s':", path.to_string())
                     : nullptr;

      // Enforce that 'flake.nix' is a direct attrset, not a
      // computation.
      if (must_be_trivial && !(dynamic_cast<ExprAttrs*>(e)))
        state.error<EvalError>("file '%s' must be an attribute set", path).debugThrow();

      state.eval(e, v);
    } catch (Error& e) {
      state.addErrorTrace(e, "while evaluating the file '%s':", path.to_string());
      throw;
    }
  }
};

void EvalState::evalFile(const source_path_t& path, Value& v, bool must_be_trivial) {
  auto resolvedPath = get_concurrent(*importResolutionCache, path);

  if (!resolvedPath) {
    resolvedPath = resolve_expr_path(path);
    importResolutionCache->emplace(path, *resolvedPath);
  }

  if (auto v2 = get_concurrent(*fileEvalCache, *resolvedPath)) {
    forceValue(**v2, no_pos);
    v = **v2;
    return;
  }

  Value* vExpr;
  expr_parse_file_t expr{*resolvedPath, must_be_trivial};

  fileEvalCache->try_emplace_and_cvisit(
      *resolvedPath, nullptr,
      [&](auto& i) {
        vExpr = allocValue();
        vExpr->mk_thunk(&baseEnv, &expr);
        i.second = vExpr;
      },
      [&](auto& i) { vExpr = i.second; });

  forceValue(*vExpr, no_pos);

  v = *vExpr;
}

void EvalState::resetFileCache() {
  importResolutionCache->clear();
  fileEvalCache->clear();
  inputCache->clear();
  positions.clear();
}

void EvalState::eval(Expr* e, Value& v) {
  e->eval(*this, baseEnv, v);
}

inline bool EvalState::evalBool(Env& env, Expr* e, const pos_idx_t pos, std::string_view error_ctx) {
  try {
    Value v;
    e->eval(*this, env, v);
    if (v.type() != nBool)
      error<TypeError>("expected a Boolean but found %1%: %2%", show_type(v),
                       ValuePrinter(*this, v, errorPrintOptions))
          .at_pos(pos)
          .withFrame(env, *e)
          .debugThrow();
    return v.boolean();
  } catch (Error& e) {
    e.add_trace(positions[pos], error_ctx);
    throw;
  }
}

inline void EvalState::evalAttrs(Env& env, Expr* e, Value& v, const pos_idx_t pos,
                                 std::string_view error_ctx) {
  try {
    e->eval(*this, env, v);
    if (v.type() != nAttrs)
      error<TypeError>("expected a set but found %1%: %2%", show_type(v),
                       ValuePrinter(*this, v, errorPrintOptions))
          .withFrame(env, *e)
          .debugThrow();
  } catch (Error& e) {
    e.add_trace(positions[pos], error_ctx);
    throw;
  }
}

void Expr::eval(EvalState& state, Env& env, Value& v) {
  unreachable();
}

void ExprInt::eval(EvalState& state, Env& env, Value& v) {
  v = this->v;
}

void ExprFloat::eval(EvalState& state, Env& env, Value& v) {
  v = this->v;
}

void ExprString::eval(EvalState& state, Env& env, Value& v) {
  v = this->v;
}

void ExprPath::eval(EvalState& state, Env& env, Value& v) {
  v = this->v;
}

Env* ExprAttrs::buildInheritFromEnv(EvalState& state, Env& up) {
  Env& inheritEnv = state.mem.allocEnv(inheritFromExprs->size());
  inheritEnv.up = &up;

  Displacement displ = 0;
  for (auto from : *inheritFromExprs)
    inheritEnv.values[displ++] = from->maybeThunk(state, up);

  return &inheritEnv;
}

void ExprAttrs::eval(EvalState& state, Env& env, Value& v) {
  auto bindings = state.buildBindings(attrs->size() + dynamicAttrs->size());
  auto dynamicEnv = &env;
  bool sort = false;

  if (recursive) {
    /* Create a new environment that contains the attributes in
       this `rec'. */
    Env& env2(state.mem.allocEnv(attrs->size()));
    env2.up = &env;
    dynamicEnv = &env2;
    Env* inheritEnv = inheritFromExprs ? buildInheritFromEnv(state, env2) : nullptr;

    AttrDefs::iterator overrides = attrs->find(state.s.overrides);
    bool hasOverrides = overrides != attrs->end();

    /* The recursive attributes are evaluated in the new
       environment, while the inherited attributes are evaluated
       in the original environment. */
    Displacement displ = 0;
    for (auto& i : *attrs) {
      Value* vAttr;
      if (hasOverrides && i.second.kind != AttrDef::Kind::Inherited) {
        vAttr = state.allocValue();
        mk_thunk(*vAttr, *i.second.chooseByKind(&env2, &env, inheritEnv), i.second.e);
      } else
        vAttr = i.second.e->maybeThunk(state, *i.second.chooseByKind(&env2, &env, inheritEnv));
      env2.values[displ++] = vAttr;
      bindings.insert(i.first, vAttr, i.second.pos);
    }

    /* If the rec contains an attribute called `__overrides', then
       evaluate it, and add the attributes in that set to the rec.
       This allows overriding of recursive attributes, which is
       otherwise not possible.  (You can use the // operator to
       replace an attribute, but other attributes in the rec will
       still reference the original value, because that value has
       been substituted into the bodies of the other attributes.
       Hence we need __overrides.) */
    if (hasOverrides) {
      Value* v_overrides = (*bindings.bindings)[overrides->second.displ].value;
      state.forceAttrs(
          *v_overrides, [&]() { return v_overrides->determinePos(no_pos); },
          "while evaluating the `__overrides` attribute");
      bindings.grow(state.buildBindings(bindings.capacity() + v_overrides->attrs()->size()));
      for (auto& i : *v_overrides->attrs()) {
        AttrDefs::iterator j = attrs->find(i.name);
        if (j != attrs->end()) {
          (*bindings.bindings)[j->second.displ] = i;
          env2.values[j->second.displ] = i.value;
        } else
          bindings.push_back(i);
      }
      sort = true;
    }
  }

  else {
    Env* inheritEnv = inheritFromExprs ? buildInheritFromEnv(state, env) : nullptr;
    for (auto& i : *attrs)
      bindings.insert(i.first,
                      i.second.e->maybeThunk(state, *i.second.chooseByKind(&env, &env, inheritEnv)),
                      i.second.pos);
  }

  /* Dynamic attrs apply *after* rec and __overrides. */
  for (auto& i : *dynamicAttrs) {
    Value nameVal;
    i.nameExpr->eval(state, *dynamicEnv, nameVal);
    state.forceValue(nameVal, i.pos);
    if (nameVal.type() == nNull)
      continue;
    state.forceStringNoCtx(nameVal, i.pos, "while evaluating the name of a dynamic attribute");
    auto nameSym = state.symbols.create(nameVal.string_view());
    if (sort)
      // FIXME: inefficient
      bindings.bindings->sort();
    if (auto j = bindings.bindings->get(nameSym))
      state
          .error<EvalError>("dynamic attribute '%1%' already defined at %2%",
                            state.symbols[nameSym], state.positions[j->pos])
          .at_pos(i.pos)
          .withFrame(env, *this)
          .debugThrow();

    i.valueExpr->setName(nameSym);
    /* Keep sorted order so find can catch duplicates */
    bindings.insert(nameSym, i.valueExpr->maybeThunk(state, *dynamicEnv), i.pos);
    sort = true;
  }

  bindings.bindings->pos = pos;

  v.mkAttrs(sort ? bindings.finish() : bindings.alreadySorted());
}

void ExprLet::eval(EvalState& state, Env& env, Value& v) {
  /* Create a new environment that contains the attributes in this
     `let'. */
  Env& env2(state.mem.allocEnv(attrs->attrs->size()));
  env2.up = &env;

  Env* inheritEnv = attrs->inheritFromExprs ? attrs->buildInheritFromEnv(state, env2) : nullptr;

  /* The recursive attributes are evaluated in the new environment,
     while the inherited attributes are evaluated in the original
     environment. */
  Displacement displ = 0;
  for (auto& i : *attrs->attrs) {
    env2.values[displ++] =
        i.second.e->maybeThunk(state, *i.second.chooseByKind(&env2, &env, inheritEnv));
  }

  auto dts = state.debugRepl ? make_debug_trace_stacker(state, *this, env2, getPos(),
                                                     "while evaluating a '%1%' expression", "let")
                             : nullptr;

  body->eval(state, env2, v);
}

void ExprList::eval(EvalState& state, Env& env, Value& v) {
  auto list = state.buildList(elems.size());
  for (const auto& [n, v2] : enumerate(list))
    v2 = elems[n]->maybeThunk(state, env);
  v.mkList(list);
}

Value* ExprList::maybeThunk(EvalState& state, Env& env) {
  if (elems.empty()) {
    return &Value::vEmptyList;
  }
  return Expr::maybeThunk(state, env);
}

void ExprVar::eval(EvalState& state, Env& env, Value& v) {
  Value* v2 = state.lookupVar(&env, *this, false);
  state.forceValue(*v2, pos);
  v = *v2;
}

static std::string show_attr_selection_path(EvalState& state, Env& env,
                                         std::span<const AttrName> attr_path) {
  std::ostringstream out;
  bool first = true;
  for (auto& i : attr_path) {
    if (!first)
      out << '.';
    else
      first = false;
    try {
      out << state.symbols[get_name(i, state, env)];
    } catch (Error& e) {
      assert(!i.symbol);
      out << "\"${";
      i.expr->show(state.symbols, out);
      out << "}\"";
    }
  }
  return out.str();
}

void ExprSelect::eval(EvalState& state, Env& env, Value& v) {
  Value vTmp;
  pos_idx_t pos2;
  Value* v_attrs = &vTmp;

  e->eval(state, env, vTmp);

  try {
    auto dts = state.debugRepl
                   ? make_debug_trace_stacker(state, *this, env, getPos(),
                                           "while evaluating the attribute '%1%'",
                                           show_attr_selection_path(state, env, getAttrPath()))
                   : nullptr;

    for (auto& i : getAttrPath()) {
      state.nrLookups++;
      const Attr* j;
      auto name = get_name(i, state, env);
      if (def) {
        state.forceValue(*v_attrs, pos);
        if (v_attrs->type() != nAttrs || !(j = v_attrs->attrs()->get(name))) {
          def->eval(state, env, v);
          return;
        }
      } else {
        state.forceAttrs(*v_attrs, pos, "while selecting an attribute");
        if (!(j = v_attrs->attrs()->get(name))) {
          string_set_t allAttrNames;
          for (auto& attr : *v_attrs->attrs())
            allAttrNames.insert(std::string(state.symbols[attr.name]));
          auto suggestions = suggestions_t::best_matches(allAttrNames, state.symbols[name]);
          state.error<EvalError>("attribute '%1%' missing", state.symbols[name])
              .at_pos(pos)
              .withSuggestions(suggestions)
              .withFrame(env, *this)
              .debugThrow();
        }
      }
      v_attrs = j->value;
      pos2 = j->pos;
      if (state.countCalls)
        state.attrSelects[pos2]++;
    }

    state.forceValue(*v_attrs, pos2 ? pos2 : this->pos);

  } catch (Error& e) {
    if (pos2) {
      auto pos2r = state.positions[pos2];
      auto origin = std::get_if<source_path_t>(&pos2r.origin);
      if (!(origin && *origin == state.derivationInternal))
        state.addErrorTrace(e, pos2, "while evaluating the attribute '%1%'",
                            show_attr_selection_path(state, env, getAttrPath()));
    }
    throw;
  }

  v = *v_attrs;
}

Symbol ExprSelect::evalExceptFinalSelect(EvalState& state, Env& env, Value& attrs) {
  Value vTmp;
  Symbol name = get_name(attrPathStart[nAttrPath - 1], state, env);

  if (nAttrPath == 1) {
    e->eval(state, env, vTmp);
  } else {
    ExprSelect init(*this);
    init.nAttrPath--;
    init.eval(state, env, vTmp);
  }
  attrs = vTmp;
  return name;
}

void ExprOpHasAttr::eval(EvalState& state, Env& env, Value& v) {
  Value vTmp;
  Value* v_attrs = &vTmp;

  e->eval(state, env, vTmp);

  for (auto& i : attr_path) {
    state.forceValue(*v_attrs, getPos());
    const Attr* j;
    auto name = get_name(i, state, env);
    if (v_attrs->type() == nAttrs && (j = v_attrs->attrs()->get(name))) {
      v_attrs = j->value;
    } else {
      v.mkBool(false);
      return;
    }
  }

  v.mkBool(true);
}

void ExprLambda::eval(EvalState& state, Env& env, Value& v) {
  v.mkLambda(&env, this);
}

thread_local size_t EvalState::callDepth = 0;

void EvalState::callFunction(Value& fun, std::span<Value*> args, Value& v_res, const pos_idx_t pos) {
  auto _level = addCallDepth(pos);

  auto neededHooks = profiler.getNeededHooks();
  if (neededHooks.test(EvalProfiler::preFunctionCall)) [[unlikely]]
    profiler.pre_function_call_hook(*this, fun, args, pos);

  finally_t traceExit_{[&]() {
    if (profiler.getNeededHooks().test(EvalProfiler::postFunctionCall)) [[unlikely]]
      profiler.post_function_call_hook(*this, fun, args, pos);
  }};

  forceValue(fun, pos);

  Value v_cur = fun;

  auto makeAppChain = [&]() {
    for (auto arg : args) {
      auto fun2 = allocValue();
      *fun2 = v_cur;
      v_cur.reset();
      v_cur.mkPrimOpApp(fun2, arg);
    }
    v_res = v_cur;
  };

  const Attr* functor;

  while (args.size() > 0) {
    if (v_cur.isLambda()) {
      ExprLambda& lambda(*v_cur.lambda().fun);

      auto size =
          (!lambda.arg ? 0 : 1) + (lambda.getFormals() ? lambda.getFormals()->formals.size() : 0);
      Env& env2(mem.allocEnv(size));
      env2.up = v_cur.lambda().env;

      Displacement displ = 0;

      if (auto formals = lambda.getFormals()) {
        try {
          forceAttrs(*args[0], lambda.pos,
                     "while evaluating the value passed for the lambda argument");
        } catch (Error& e) {
          if (pos)
            e.add_trace(positions[pos], "from call site");
          throw;
        }

        if (lambda.arg)
          env2.values[displ++] = args[0];

        /* For each formal argument, get the actual argument.  If
           there is no matching actual argument but the formal
           argument has a default, use the default. */
        size_t attrsUsed = 0;
        for (auto& i : formals->formals) {
          auto j = args[0]->attrs()->get(i.name);
          if (!j) {
            if (!i.def) {
              error<TypeError>(
                  "function '%1%' called without required argument '%2%'",
                  (lambda.name ? std::string(symbols[lambda.name]) : "anonymous lambda"),
                  symbols[i.name])
                  .at_pos(lambda.pos)
                  .withTrace(pos, "from call site")
                  .withFrame(*v_cur.lambda().env, lambda)
                  .debugThrow();
            }
            env2.values[displ++] = i.def->maybeThunk(*this, env2);
          } else {
            attrsUsed++;
            env2.values[displ++] = j->value;
          }
        }

        /* Check that each actual argument is listed as a formal
           argument (unless the attribute match specifies a `...'). */
        if (!formals->ellipsis && attrsUsed != args[0]->attrs()->size()) {
          /* Nope, so show the first unexpected argument to the
             user. */
          for (auto& i : *args[0]->attrs())
            if (!formals->has(i.name)) {
              string_set_t formalNames;
              for (auto& formal : formals->formals)
                formalNames.insert(std::string(symbols[formal.name]));
              auto suggestions = suggestions_t::best_matches(formalNames, symbols[i.name]);
              error<TypeError>(
                  "function '%1%' called with unexpected argument '%2%'",
                  (lambda.name ? std::string(symbols[lambda.name]) : "anonymous lambda"),
                  symbols[i.name])
                  .at_pos(lambda.pos)
                  .withTrace(pos, "from call site")
                  .withSuggestions(suggestions)
                  .withFrame(*v_cur.lambda().env, lambda)
                  .debugThrow();
            }
          unreachable();
        }
      } else {
        env2.values[displ++] = args[0];
      }

      nrFunctionCalls++;
      if (countCalls)
        incrFunctionCall(&lambda);

      /* Evaluate the body. */
      try {
        auto dts =
            debugRepl
                ? make_debug_trace_stacker(*this, *lambda.body, env2, lambda.pos, "while calling %s",
                                        lambda.name ? concat_strings("'", symbols[lambda.name], "'")
                                                    : "anonymous lambda")
                : nullptr;

        v_cur.reset();
        lambda.body->eval(*this, env2, v_cur);
      } catch (Error& e) {
        if (logger_settings.show_trace.get()) {
          addErrorTrace(e, lambda.pos, "while calling %s",
                        lambda.name ? concat_strings("'", symbols[lambda.name], "'")
                                    : "anonymous lambda");
          if (pos)
            addErrorTrace(e, pos, "from call site");
        }
        throw;
      }

      args = args.subspan(1);
    }

    else if (v_cur.isPrimOp()) {
      size_t argsLeft = v_cur.prim_op()->arity;

      if (args.size() < argsLeft) {
        /* We don't have enough arguments, so create a tPrimOpApp chain. */
        makeAppChain();
        return;
      } else {
        /* We have all the arguments, so call the primop. */
        auto* fn = v_cur.prim_op();

        nrPrimOpCalls++;
        if (countCalls)
          primOpCalls[fn->name]++;

        try {
          auto pos = v_cur.determinePos(no_pos);
          v_cur.reset();
          fn->fun(*this, pos, args.data(), v_cur);
        } catch (Error& e) {
          if (fn->add_trace)
            addErrorTrace(e, pos, "while calling the '%1%' builtin", fn->name);
          throw;
        }

        args = args.subspan(argsLeft);
      }
    }

    else if (v_cur.isPrimOpApp()) {
      /* Figure out the number of arguments still needed. */
      size_t argsDone = 0;
      Value* prim_op = &v_cur;
      while (prim_op->isPrimOpApp()) {
        argsDone++;
        prim_op = prim_op->primOpApp().left;
      }
      assert(prim_op->isPrimOp());
      auto arity = prim_op->prim_op()->arity;
      auto argsLeft = arity - argsDone;
      assert(argsLeft);

      if (args.size() < argsLeft) {
        /* We still don't have enough arguments, so extend the tPrimOpApp chain. */
        makeAppChain();
        return;
      } else {
        /* We have all the arguments, so call the primop with
           the previous and new arguments. */

        Value* vArgs[maxPrimOpArity];
        auto n = argsDone;
        for (Value* arg = &v_cur; arg->isPrimOpApp(); arg = arg->primOpApp().left)
          vArgs[--n] = arg->primOpApp().right;

        for (size_t i = 0; i < argsLeft; ++i)
          vArgs[argsDone + i] = args[i];

        auto fn = prim_op->prim_op();
        nrPrimOpCalls++;
        if (countCalls)
          primOpCalls[fn->name]++;

        try {
          // TODO:
          // 1. Unify this and above code. Heavily redundant.
          // 2. Create a fake env (arg1, arg2, etc.) and a fake expr (arg1: arg2: etc: builtins.name
          // arg1 arg2 etc)
          //    so the debugger allows to inspect the wrong parameters passed to the builtin.
          auto pos = v_cur.determinePos(no_pos);
          v_cur.reset();
          fn->fun(*this, pos, vArgs, v_cur);
        } catch (Error& e) {
          if (fn->add_trace)
            addErrorTrace(e, pos, "while calling the '%1%' builtin", fn->name);
          throw;
        }

        args = args.subspan(argsLeft);
      }
    }

    else if (v_cur.type() == nAttrs && (functor = v_cur.attrs()->get(s.functor))) {
      /* 'vCur' may be allocated on the stack of the calling
         function, but for functors we may keep a reference, so
         heap-allocate a copy and use that instead. */
      Value* args2[] = {allocValue(), args[0]};
      *args2[0] = v_cur;
      v_cur.reset();
      try {
        callFunction(*functor->value, args2, v_cur, functor->pos);
      } catch (Error& e) {
        e.add_trace(positions[pos],
                   "while calling a functor (an attribute set with a '__functor' attribute)");
        throw;
      }
      args = args.subspan(1);
    }

    else
      error<TypeError>("attempt to call something which is not a function but %1%: %2%",
                       show_type(v_cur), ValuePrinter(*this, v_cur, errorPrintOptions))
          .at_pos(pos)
          .debugThrow();
  }

  v_res = v_cur;
}

void ExprCall::eval(EvalState& state, Env& env, Value& v) {
  auto dts = state.debugRepl
                 ? make_debug_trace_stacker(state, *this, env, getPos(), "while calling a function")
                 : nullptr;

  Value vFun;
  fun->eval(state, env, vFun);

  // Empirical arity of Nixpkgs lambdas by regex e.g. ([a-zA-Z]+:(\s|(/\*.*\/)|(#.*\n))*){5}
  // 2: over 4000
  // 3: about 300
  // 4: about 60
  // 5: under 10
  // This excluded attrset lambdas (`{...}:`). Contributions of mixed lambdas appears insignificant
  // at ~150 total.
  SmallValueVector<4> vArgs(args->size());
  for (size_t i = 0; i < args->size(); ++i)
    vArgs[i] = (*args)[i]->maybeThunk(state, env);

  state.callFunction(vFun, vArgs, v, pos);
}

// Lifted out of callFunction() because it creates a temporary that
// prevents tail-call optimisation.
void EvalState::incrFunctionCall(ExprLambda* fun) {
  functionCalls[fun]++;
}

void EvalState::autoCallFunction(const Bindings& args, Value& fun, Value& res) {
  auto pos = fun.determinePos(no_pos);

  forceValue(fun, pos);

  if (fun.type() == nAttrs) {
    auto found = fun.attrs()->get(s.functor);
    if (found) {
      Value* v = allocValue();
      callFunction(*found->value, fun, *v, pos);
      forceValue(*v, pos);
      return autoCallFunction(args, *v, res);
    }
  }

  if (!fun.isLambda() || !fun.lambda().fun->getFormals()) {
    res = fun;
    return;
  }
  auto formals = fun.lambda().fun->getFormals();

  auto attrs = buildBindings(std::max(static_cast<uint32_t>(formals->formals.size()), args.size()));

  if (formals->ellipsis) {
    // If the formals have an ellipsis (eg the function accepts extra args) pass
    // all available automatic arguments (which includes arguments specified on
    // the command line via --arg/--argstr)
    for (auto& v : args)
      attrs.insert(v);
  } else {
    // Otherwise, only pass the arguments that the function accepts
    for (auto& i : formals->formals) {
      auto j = args.get(i.name);
      if (j) {
        attrs.insert(*j);
      } else if (!i.def) {
        error<MissingArgumentError>(
            R"(cannot evaluate a function that has an argument without a value ('%1%')
Nix attempted to evaluate a function as a top level expression; in
this case it must have its arguments supplied either by default
values, or passed explicitly with '--arg' or '--argstr'. See
https://nix.dev/manual/nix/stable/language/syntax.html#functions.)",
            symbols[i.name])
            .at_pos(i.pos)
            .withFrame(*fun.lambda().env, *fun.lambda().fun)
            .debugThrow();
      }
    }
  }

  callFunction(fun, allocValue()->mkAttrs(attrs), res, pos);
}

void ExprWith::eval(EvalState& state, Env& env, Value& v) {
  Env& env2(state.mem.allocEnv(1));
  env2.up = &env;
  env2.values[0] = attrs->maybeThunk(state, env);

  body->eval(state, env2, v);
}

void ExprIf::eval(EvalState& state, Env& env, Value& v) {
  // We cheat in the parser, and pass the position of the condition as the position of the if
  // itself.
  (state.evalBool(env, cond, pos, "while evaluating a branch condition") ? then : else_)
      ->eval(state, env, v);
}

void ExprAssert::eval(EvalState& state, Env& env, Value& v) {
  if (!state.evalBool(env, cond, pos, "in the condition of the assert statement")) {
    std::ostringstream out;
    cond->show(state.symbols, out);
    auto exprStr = out.view();

    if (auto eq = dynamic_cast<ExprOpEq*>(cond)) {
      try {
        Value v1;
        eq->e1->eval(state, env, v1);
        Value v2;
        eq->e2->eval(state, env, v2);
        state.assertEqValues(v1, v2, eq->pos, "in an equality assertion");
      } catch (AssertionError& e) {
        e.add_trace(state.positions[pos], "while evaluating the condition of the assertion '%s'",
                   exprStr);
        throw;
      }
    }

    state.error<AssertionError>("assertion '%1%' failed", exprStr)
        .at_pos(pos)
        .withFrame(env, *this)
        .debugThrow();
  }
  body->eval(state, env, v);
}

void ExprOpNot::eval(EvalState& state, Env& env, Value& v) {
  v.mkBool(
      !state.evalBool(env, e, getPos(), "in the argument of the not operator")); // XXX: FIXME: !
}

void ExprOpEq::eval(EvalState& state, Env& env, Value& v) {
  Value v1;
  e1->eval(state, env, v1);
  Value v2;
  e2->eval(state, env, v2);
  v.mkBool(state.eqValues(v1, v2, pos, "while testing two values for equality"));
}

void ExprOpNEq::eval(EvalState& state, Env& env, Value& v) {
  Value v1;
  e1->eval(state, env, v1);
  Value v2;
  e2->eval(state, env, v2);
  v.mkBool(!state.eqValues(v1, v2, pos, "while testing two values for inequality"));
}

void ExprOpAnd::eval(EvalState& state, Env& env, Value& v) {
  v.mkBool(state.evalBool(env, e1, pos, "in the left operand of the AND (&&) operator") &&
           state.evalBool(env, e2, pos, "in the right operand of the AND (&&) operator"));
}

void ExprOpOr::eval(EvalState& state, Env& env, Value& v) {
  v.mkBool(state.evalBool(env, e1, pos, "in the left operand of the OR (||) operator") ||
           state.evalBool(env, e2, pos, "in the right operand of the OR (||) operator"));
}

void ExprOpImpl::eval(EvalState& state, Env& env, Value& v) {
  v.mkBool(!state.evalBool(env, e1, pos, "in the left operand of the IMPL (->) operator") ||
           state.evalBool(env, e2, pos, "in the right operand of the IMPL (->) operator"));
}

void ExprOpUpdate::eval(EvalState& state, Env& env, Value& v) {
  Value v1, v2;
  state.evalAttrs(env, e1, v1, pos, "in the left operand of the update (//) operator");
  state.evalAttrs(env, e2, v2, pos, "in the right operand of the update (//) operator");

  state.nrOpUpdates++;

  const Bindings& bindings1 = *v1.attrs();
  if (bindings1.empty()) {
    v = v2;
    return;
  }

  const Bindings& bindings2 = *v2.attrs();
  if (bindings2.empty()) {
    v = v1;
    return;
  }

  /* Simple heuristic for determining whether attrs2 should be "layered" on top of
     attrs1 instead of copying to a new Bindings. */
  bool shouldLayer = [&]() -> bool {
    if (bindings1.isLayerListFull())
      return false;

    if (bindings2.size() > state.settings.bindingsUpdateLayerRhsSizeThreshold)
      return false;

    return true;
  }();

  if (shouldLayer) {
    auto attrs = state.buildBindings(bindings2.size());
    attrs.layerOnTopOf(bindings1);

    std::ranges::copy(bindings2, std::back_inserter(attrs));
    v.mkAttrs(attrs.alreadySorted());

    state.nrOpUpdateValuesCopied += bindings2.size();
    return;
  }

  auto attrs = state.buildBindings(bindings1.size() + bindings2.size());

  /* Merge the sets, preferring values from the second set.  Make
     sure to keep the resulting vector in sorted order. */
  auto i = bindings1.begin();
  auto j = bindings2.begin();

  while (i != bindings1.end() && j != bindings2.end()) {
    if (i->name == j->name) {
      attrs.insert(*j);
      ++i;
      ++j;
    } else if (i->name < j->name) {
      attrs.insert(*i);
      ++i;
    } else {
      attrs.insert(*j);
      ++j;
    }
  }

  while (i != bindings1.end()) {
    attrs.insert(*i);
    ++i;
  }

  while (j != bindings2.end()) {
    attrs.insert(*j);
    ++j;
  }

  v.mkAttrs(attrs.alreadySorted());

  state.nrOpUpdateValuesCopied += v.attrs()->size();
}

void ExprOpConcatLists::eval(EvalState& state, Env& env, Value& v) {
  Value v1;
  e1->eval(state, env, v1);
  Value v2;
  e2->eval(state, env, v2);
  Value* lists[2] = {&v1, &v2};
  state.concatLists(v, 2, lists, pos, "while evaluating one of the elements to concatenate");
}

void EvalState::concatLists(Value& v, size_t nr_lists, Value* const* lists, const pos_idx_t pos,
                            std::string_view error_ctx) {
  nrListConcats++;

  Value* nonEmpty = 0;
  size_t len = 0;
  for (size_t n = 0; n < nr_lists; ++n) {
    forceList(*lists[n], pos, error_ctx);
    auto l = lists[n]->list_size();
    len += l;
    if (l)
      nonEmpty = lists[n];
  }

  if (nonEmpty && len == nonEmpty->list_size()) {
    v = *nonEmpty;
    return;
  }

  auto list = buildList(len);
  auto out = list.elems;
  for (size_t n = 0, pos = 0; n < nr_lists; ++n) {
    auto list_view = lists[n]->list_view();
    auto l = list_view.size();
    if (l)
      memcpy(out + pos, list_view.data(), l * sizeof(Value*));
    pos += l;
  }
  v.mkList(list);
}

void ExprConcatStrings::eval(EvalState& state, Env& env, Value& v) {
  NixStringContext context;
  std::vector<backed_string_view_t> strings;
  size_t sSize = 0;
  NixInt n{0};
  NixFloat nf = 0;

  bool first = !forceString;
  ValueType firstType = nString;

  // List of returned strings. References to these Values must NOT be persisted.
  SmallTemporaryValueVector<conservativeStackReservation> values(es.size());
  Value* vTmpP = values.data();

  for (auto& [i_pos, i] : es) {
    Value& vTmp = *vTmpP++;
    i->eval(state, env, vTmp);

    /* If the first element is a path, then the result will also
       be a path, we don't copy anything (yet - that's done later,
       since paths are copied when they are used in a derivation),
       and none of the strings are allowed to have contexts. */
    if (first) {
      firstType = vTmp.type();
    }

    if (firstType == nInt) {
      if (vTmp.type() == nInt) {
        auto newN = n + vTmp.integer();
        if (auto checked = newN.valueChecked(); checked.has_value()) {
          n = NixInt(*checked);
        } else {
          state.error<EvalError>("integer overflow in adding %1% + %2%", n, vTmp.integer())
              .at_pos(i_pos)
              .debugThrow();
        }
      } else if (vTmp.type() == nFloat) {
        // Upgrade the type from int to float;
        firstType = nFloat;
        nf = n.value;
        nf += vTmp.fpoint();
      } else
        state.error<EvalError>("cannot add %1% to an integer", show_type(vTmp))
            .at_pos(i_pos)
            .withFrame(env, *this)
            .debugThrow();
    } else if (firstType == nFloat) {
      if (vTmp.type() == nInt) {
        nf += vTmp.integer().value;
      } else if (vTmp.type() == nFloat) {
        nf += vTmp.fpoint();
      } else
        state.error<EvalError>("cannot add %1% to a float", show_type(vTmp))
            .at_pos(i_pos)
            .withFrame(env, *this)
            .debugThrow();
    } else {
      if (strings.empty())
        strings.reserve(es.size());
      /* skip canonization of first path, which would only be not
      canonized in the first place if it's coming from a ./${foo} type
      path */
      auto part = state.coerceToString(i_pos, vTmp, context, "while evaluating a path segment",
                                       false, firstType == nString, !first);
      sSize += part->size();
      strings.emplace_back(std::move(part));
    }

    first = false;
  }

  if (firstType == nInt) {
    v.mkInt(n);
  } else if (firstType == nFloat) {
    v.mkFloat(nf);
  } else if (firstType == nPath) {
    if (has_context(context))
      state.error<EvalError>("a string that refers to a store path cannot be appended to a path")
          .at_pos(pos)
          .withFrame(env, *this)
          .debugThrow();
    std::string resultStr;
    resultStr.reserve(sSize);
    for (const auto& part : strings) {
      resultStr += *part;
    }
    v.mkPath(state.root_path(canon_path_t(resultStr)), state.mem);
  } else {
    auto& resultStr = StringData::alloc(state.mem, sSize);
    auto* tmp = resultStr.data();
    for (const auto& part : strings) {
      std::memcpy(tmp, part->data(), part->size());
      tmp += part->size();
    }
    *tmp = '\0';
    v.mkStringMove(resultStr, context, state.mem);
  }
}

void ExprPos::eval(EvalState& state, Env& env, Value& v) {
  state.mkPos(v, pos);
}

// always force this to be separate, otherwise forceValue may inline it and take
// a massive perf hit
[[gnu::noinline]]
void EvalState::tryFixupBlackHolePos(Value& v, pos_idx_t pos) {
  if (!v.isBlackhole())
    return;
  auto e = std::current_exception();
  try {
    std::rethrow_exception(e);
  } catch (InfiniteRecursionError& e) {
    e.at_pos(positions[pos]);
  } catch (...) {
  }
}

void EvalState::forceValueDeep(Value& v) {
  std::set<const Value*> seen;

  [&, &state(*this)](this const auto& recurse, Value& v) {
    auto _level = state.addCallDepth(v.determinePos(no_pos));

    if (!seen.insert(&v).second)
      return;

    state.forceValue(v, v.determinePos(no_pos));

    if (v.type() == nAttrs) {
      for (auto& i : *v.attrs())
        try {
          // If the value is a thunk, we're evaling. Otherwise no trace necessary.
          // FIXME: race, thunk might be updated by another thread
          auto dts = state.debugRepl && i.value->isThunk()
                         ? make_debug_trace_stacker(
                               state, *i.value->thunk().expr, *i.value->thunk().env, i.pos,
                               "while evaluating the attribute '%1%'", state.symbols[i.name])
                         : nullptr;

          recurse(*i.value);
        } catch (Error& e) {
          state.addErrorTrace(e, i.pos, "while evaluating the attribute '%1%'",
                              state.symbols[i.name]);
          throw;
        }
    }

    else if (v.isList()) {
      size_t index = 0;
      for (auto v2 : v.list_view())
        try {
          recurse(*v2);
          index++;
        } catch (Error& e) {
          state.addErrorTrace(e, "while evaluating list element at index %1%", index);
          throw;
        }
    }
  }(v);
}

NixInt EvalState::forceInt(Value& v, const pos_idx_t pos, std::string_view error_ctx) {
  try {
    forceValue(v, pos);
    if (v.type() != nInt)
      error<TypeError>("expected an integer but found %1%: %2%", show_type(v),
                       ValuePrinter(*this, v, errorPrintOptions))
          .at_pos(pos)
          .debugThrow();
    return v.integer();
  } catch (Error& e) {
    e.add_trace(positions[pos], error_ctx);
    throw;
  }

  return v.integer();
}

NixFloat EvalState::forceFloat(Value& v, const pos_idx_t pos, std::string_view error_ctx) {
  try {
    forceValue(v, pos);
    if (v.type() == nInt)
      return v.integer().value;
    else if (v.type() != nFloat)
      error<TypeError>("expected a float but found %1%: %2%", show_type(v),
                       ValuePrinter(*this, v, errorPrintOptions))
          .at_pos(pos)
          .debugThrow();
    return v.fpoint();
  } catch (Error& e) {
    e.add_trace(positions[pos], error_ctx);
    throw;
  }
}

bool EvalState::forceBool(Value& v, const pos_idx_t pos, std::string_view error_ctx) {
  try {
    forceValue(v, pos);
    if (v.type() != nBool)
      error<TypeError>("expected a Boolean but found %1%: %2%", show_type(v),
                       ValuePrinter(*this, v, errorPrintOptions))
          .at_pos(pos)
          .debugThrow();
    return v.boolean();
  } catch (Error& e) {
    e.add_trace(positions[pos], error_ctx);
    throw;
  }

  return v.boolean();
}

const Attr* EvalState::get_attr(Symbol attrSym, const Bindings* attrSet, std::string_view error_ctx) {
  auto value = attrSet->get(attrSym);
  if (!value) {
    error<TypeError>("attribute '%s' missing", symbols[attrSym])
        .withTrace(no_pos, error_ctx)
        .debugThrow();
  }
  return value;
}

bool EvalState::isFunctor(const Value& fun) const {
  return fun.type() == nAttrs && fun.attrs()->get(s.functor);
}

void EvalState::forceFunction(Value& v, const pos_idx_t pos, std::string_view error_ctx) {
  try {
    forceValue(v, pos);
    if (v.type() != nFunction && !isFunctor(v))
      error<TypeError>("expected a function but found %1%: %2%", show_type(v),
                       ValuePrinter(*this, v, errorPrintOptions))
          .at_pos(pos)
          .debugThrow();
  } catch (Error& e) {
    e.add_trace(positions[pos], error_ctx);
    throw;
  }
}

std::string_view EvalState::forceString(Value& v, const pos_idx_t pos, std::string_view error_ctx) {
  try {
    forceValue(v, pos);
    if (v.type() != nString)
      error<TypeError>("expected a string but found %1%: %2%", show_type(v),
                       ValuePrinter(*this, v, errorPrintOptions))
          .at_pos(pos)
          .debugThrow();
    return v.string_view();
  } catch (Error& e) {
    e.add_trace(positions[pos], error_ctx);
    throw;
  }
}

void copy_context(const Value& v, NixStringContext& context,
                 const experimental_feature_settings_t& xp_settings) {
  if (auto* ctx = v.context())
    for (auto* elem : *ctx)
      context.insert(NixStringContextElem::parse(elem->view(), xp_settings));
}

std::string_view EvalState::forceString(Value& v, NixStringContext& context, const pos_idx_t pos,
                                        std::string_view error_ctx,
                                        const experimental_feature_settings_t& xp_settings) {
  auto s = forceString(v, pos, error_ctx);
  copy_context(v, context, xp_settings);
  return s;
}

std::string_view EvalState::forceStringNoCtx(Value& v, const pos_idx_t pos,
                                             std::string_view error_ctx) {
  auto s = forceString(v, pos, error_ctx);
  if (v.context()) {
    NixStringContext context;
    copy_context(v, context);
    if (has_context(context))
      error<EvalError>("the string '%1%' is not allowed to refer to a store path (such as '%2%')",
                       v.string_view(), (*v.context()->begin())->view())
          .withTrace(pos, error_ctx)
          .debugThrow();
  }
  return s;
}

bool EvalState::is_derivation(Value& v) {
  if (v.type() != nAttrs)
    return false;
  auto i = v.attrs()->get(s.type);
  if (!i)
    return false;
  forceValue(*i->value, i->pos);
  if (i->value->type() != nString)
    return false;
  return i->value->string_view().compare("derivation") == 0;
}

std::optional<std::string> EvalState::tryAttrsToString(const pos_idx_t pos, Value& v,
                                                       NixStringContext& context, bool coerceMore,
                                                       bool copy_to_store) {
  auto i = v.attrs()->get(s.toString);
  if (i) {
    Value v1;
    callFunction(*i->value, v, v1, pos);
    return coerceToString(pos, v1, context,
                          "while evaluating the result of the `__toString` attribute", coerceMore,
                          copy_to_store)
        .to_owned();
  }

  return {};
}

backed_string_view_t EvalState::coerceToString(const pos_idx_t pos, Value& v, NixStringContext& context,
                                           std::string_view error_ctx, bool coerceMore,
                                           bool copy_to_store, bool canonicalizePath) {
  forceValue(v, pos);

  if (v.type() == nString) {
    copy_context(v, context);
    return v.string_view();
  }

  if (v.type() == nPath) {
    // FIXME: instead of copying the path to the store, we could
    // return a virtual store path that lazily copies the path to
    // the store in devirtualize().
    if (!canonicalizePath && !copy_to_store) {
      // FIXME: hack to preserve path literals that end in a
      // slash, as in /foo/${x}.
      return v.pathStrView();
    } else if (copy_to_store) {
      return store->printStorePath(copyPathToStore(context, v.path(), v.determinePos(pos)));
    } else {
      auto path = v.path();
      if (path.accessor == root_fs && store->isInStore(path.path.abs())) {
        context.insert(
            NixStringContextElem::Path{.store_path = store->toStorePath(path.path.abs()).first});
      }
      return std::string(path.path.abs());
    }
  }

  if (v.type() == nAttrs) {
    auto maybe_string = tryAttrsToString(pos, v, context, coerceMore, copy_to_store);
    if (maybe_string)
      return std::move(*maybe_string);
    auto i = v.attrs()->get(s.out_path);
    if (!i) {
      error<TypeError>("cannot coerce %1% to a string: %2%", show_type(v),
                       ValuePrinter(*this, v, errorPrintOptions))
          .withTrace(pos, error_ctx)
          .debugThrow();
    }
    return coerceToString(pos, *i->value, context, error_ctx, coerceMore, copy_to_store,
                          canonicalizePath);
  }

  if (v.type() == nExternal) {
    try {
      return v.external()->coerceToString(*this, pos, context, coerceMore, copy_to_store);
    } catch (Error& e) {
      e.add_trace(nullptr, error_ctx);
      throw;
    }
  }

  if (coerceMore) {
    /* Note that `false' is represented as an empty string for
       shell scripting convenience, just like `null'. */
    if (v.type() == nBool && v.boolean())
      return "1";
    if (v.type() == nBool && !v.boolean())
      return "";
    if (v.type() == nInt)
      return std::to_string(v.integer().value);
    if (v.type() == nFloat)
      return std::to_string(v.fpoint());
    if (v.type() == nNull)
      return "";

    if (v.isList()) {
      std::string result;
      auto list_view = v.list_view();
      for (auto [n, v2] : enumerate(list_view)) {
        try {
          result += *coerceToString(pos, *v2, context, "while evaluating one element of the list",
                                    coerceMore, copy_to_store, canonicalizePath);
        } catch (Error& e) {
          e.add_trace(positions[pos], error_ctx);
          throw;
        }
        if (n < v.list_size() - 1
            /* !!! not quite correct */
            && (!v2->isList() || v2->list_size() != 0))
          result += " ";
      }
      return result;
    }
  }

  error<TypeError>("cannot coerce %1% to a string: %2%", show_type(v),
                   ValuePrinter(*this, v, errorPrintOptions))
      .withTrace(pos, error_ctx)
      .debugThrow();
}

StorePath EvalState::copyPathToStore(NixStringContext& context, const source_path_t& path,
                                     pos_idx_t pos) {
  if (nix::is_derivation(path.path.abs()))
    error<EvalError>("file names are not allowed to end in '%1%'", drvExtension).debugThrow();

  auto dstPathCached = get_concurrent(*srcToStore, path);

  auto dst_path = dstPathCached ? *dstPathCached : [&]() {
    auto dst_path = fetch_to_store(
        fetch_settings, *store, path.resolve_symlinks(symlink_resolution_t::ancestors),
        settings.readOnlyMode ? FetchMode::DryRun : FetchMode::Copy, computeBaseName(path, pos),
        ContentAddressMethod::raw_t::nix_archive, nullptr, repair);
    allowPath(dst_path);
    srcToStore->try_emplace(path, dst_path);
    printMsg(lvl_chatty, "copied source '%1%' -> '%2%'", path, store->printStorePath(dst_path));
    return dst_path;
  }();

  context.insert(NixStringContextElem::opaque_t{.path = dst_path});
  return dst_path;
}

source_path_t EvalState::coerceToPath(const pos_idx_t pos, Value& v, NixStringContext& context,
                                   std::string_view error_ctx) {
  try {
    forceValue(v, pos);
  } catch (Error& e) {
    e.add_trace(positions[pos], error_ctx);
    throw;
  }

  /* Handle path values directly, without coercing to a string. */
  if (v.type() == nPath)
    return v.path();

  /* Similarly, handle __toString where the result may be a path
     value. */
  if (v.type() == nAttrs) {
    auto i = v.attrs()->get(s.toString);
    if (i) {
      Value v1;
      callFunction(*i->value, v, v1, pos);
      return coerceToPath(pos, v1, context, error_ctx);
    }
  }

  /* Any other value should be coercible to a string, interpreted
     relative to the root filesystem. */
  auto path = coerceToString(pos, v, context, error_ctx, false, false, true).to_owned();
  if (path == "" || path[0] != '/')
    error<EvalError>("string '%1%' doesn't represent an absolute path", path)
        .withTrace(pos, error_ctx)
        .debugThrow();
  return root_path(path);
}

StorePath EvalState::coerceToStorePath(const pos_idx_t pos, Value& v, NixStringContext& context,
                                       std::string_view error_ctx) {
  auto path = coerceToString(pos, v, context, error_ctx, false, false, true).to_owned();
  if (auto store_path = store->maybeParseStorePath(path))
    return *store_path;
  error<EvalError>(
      "cannot coerce '%s' to a store path because it is not a subpath of the Nix store", path)
      .withTrace(pos, error_ctx)
      .debugThrow();
}

std::pair<SingleDerivedPath, std::string_view>
EvalState::coerceToSingleDerivedPathUnchecked(const pos_idx_t pos, Value& v, std::string_view error_ctx,
                                              const experimental_feature_settings_t& xp_settings) {
  NixStringContext context;
  auto s = forceString(v, context, pos, error_ctx, xp_settings);
  auto csize = context.size();
  if (csize != 1)
    error<EvalError>(
        "string '%s' has %d entries in its context. It should only have exactly one entry", s,
        csize)
        .withTrace(pos, error_ctx)
        .debugThrow();
  auto derived_path = std::visit(
      overloaded{
          [&](NixStringContextElem::opaque_t&& o) -> SingleDerivedPath { return std::move(o); },
          [&](NixStringContextElem::DrvDeep&&) -> SingleDerivedPath {
            error<EvalError>("string '%s' has a context which refers to a complete source and "
                             "binary closure. This is not supported at this time",
                             s)
                .withTrace(pos, error_ctx)
                .debugThrow();
          },
          [&](NixStringContextElem::Built&& b) -> SingleDerivedPath { return std::move(b); },
          [&](NixStringContextElem::Path&& p) -> SingleDerivedPath {
            error<EvalError>("string '%s' has no context", s).withTrace(pos, error_ctx).debugThrow();
          },
      },
      ((NixStringContextElem&&)*context.begin()).raw);
  return {
      std::move(derived_path),
      std::move(s),
  };
}

SingleDerivedPath EvalState::coerceToSingleDerivedPath(const pos_idx_t pos, Value& v,
                                                       std::string_view error_ctx) {
  auto [derived_path, s_] = coerceToSingleDerivedPathUnchecked(pos, v, error_ctx);
  auto s = s_;
  auto sExpected = mkSingleDerivedPathStringRaw(derived_path);
  if (s != sExpected) {
    /* `std::visit` is used here just to provide a more precise
       error message. */
    std::visit(
        overloaded{[&](const SingleDerivedPath::opaque_t& o) {
                     error<EvalError>("path string '%s' has context with the different path '%s'",
                                      s, sExpected)
                         .withTrace(pos, error_ctx)
                         .debugThrow();
                   },
                   [&](const SingleDerivedPath::Built& b) {
                     error<EvalError>("string '%s' has context with the output '%s' from "
                                      "derivation '%s', but the string is not the right "
                                      "placeholder for this derivation output. It should be '%s'",
                                      s, b.output, b.drv_path->to_string(*store), sExpected)
                         .withTrace(pos, error_ctx)
                         .debugThrow();
                   }},
        derived_path.raw());
  }
  return derived_path;
}

// NOTE: This implementation must match eqValues!
// We accept this burden because informative error messages for
// `assert a == b; x` are critical for our users' testing UX.
void EvalState::assertEqValues(Value& v1, Value& v2, const pos_idx_t pos, std::string_view error_ctx) {
  // This implementation must match eqValues.
  forceValue(v1, pos);
  forceValue(v2, pos);

  if (&v1 == &v2)
    return;

  // Special case type-compatibility between float and int
  if ((v1.type() == nInt || v1.type() == nFloat) && (v2.type() == nInt || v2.type() == nFloat)) {
    if (eqValues(v1, v2, pos, error_ctx)) {
      return;
    } else {
      error<AssertionError>("%s with value '%s' is not equal to %s with value '%s'", show_type(v1),
                            ValuePrinter(*this, v1, errorPrintOptions), show_type(v2),
                            ValuePrinter(*this, v2, errorPrintOptions))
          .debugThrow();
    }
  }

  if (v1.type() != v2.type()) {
    error<AssertionError>("%s of value '%s' is not equal to %s of value '%s'", show_type(v1),
                          ValuePrinter(*this, v1, errorPrintOptions), show_type(v2),
                          ValuePrinter(*this, v2, errorPrintOptions))
        .debugThrow();
  }

  switch (v1.type()) {
    case nInt:
      if (v1.integer() != v2.integer()) {
        error<AssertionError>("integer '%d' is not equal to integer '%d'", v1.integer(),
                              v2.integer())
            .debugThrow();
      }
      return;

    case nBool:
      if (v1.boolean() != v2.boolean()) {
        error<AssertionError>("boolean '%s' is not equal to boolean '%s'",
                              ValuePrinter(*this, v1, errorPrintOptions),
                              ValuePrinter(*this, v2, errorPrintOptions))
            .debugThrow();
      }
      return;

    case nString:
      if (v1.string_view() != v2.string_view()) {
        error<AssertionError>("string '%s' is not equal to string '%s'",
                              ValuePrinter(*this, v1, errorPrintOptions),
                              ValuePrinter(*this, v2, errorPrintOptions))
            .debugThrow();
      }
      return;

    case nPath:
      if (v1.pathAccessor() != v2.pathAccessor()) {
        error<AssertionError>(
            "path '%s' is not equal to path '%s' because their accessors are different",
            ValuePrinter(*this, v1, errorPrintOptions), ValuePrinter(*this, v2, errorPrintOptions))
            .debugThrow();
      }
      if (v1.pathStrView() != v2.pathStrView()) {
        error<AssertionError>("path '%s' is not equal to path '%s'",
                              ValuePrinter(*this, v1, errorPrintOptions),
                              ValuePrinter(*this, v2, errorPrintOptions))
            .debugThrow();
      }
      return;

    case nNull:
      return;

    case nList:
      if (v1.list_size() != v2.list_size()) {
        error<AssertionError>("list of size '%d' is not equal to list of size '%d', left hand side "
                              "is '%s', right hand side is '%s'",
                              v1.list_size(), v2.list_size(),
                              ValuePrinter(*this, v1, errorPrintOptions),
                              ValuePrinter(*this, v2, errorPrintOptions))
            .debugThrow();
      }
      for (size_t n = 0; n < v1.list_size(); ++n) {
        try {
          assertEqValues(*v1.list_view()[n], *v2.list_view()[n], pos, error_ctx);
        } catch (Error& e) {
          e.add_trace(positions[pos], "while comparing list element %d", n);
          throw;
        }
      }
      return;

    case nAttrs: {
      if (is_derivation(v1) && is_derivation(v2)) {
        auto i = v1.attrs()->get(s.out_path);
        auto j = v2.attrs()->get(s.out_path);
        if (i && j) {
          try {
            assertEqValues(*i->value, *j->value, pos, error_ctx);
            return;
          } catch (Error& e) {
            e.add_trace(positions[pos], "while comparing a derivation by its '%s' attribute",
                       "outPath");
            throw;
          }
          assert(false);
        }
      }

      if (v1.attrs()->size() != v2.attrs()->size()) {
        error<AssertionError>(
            "attribute names of attribute set '%s' differs from attribute set '%s'",
            ValuePrinter(*this, v1, errorPrintOptions), ValuePrinter(*this, v2, errorPrintOptions))
            .debugThrow();
      }

      // Like normal comparison, we compare the attributes in non-deterministic Symbol index order.
      // This function is called when eqValues has found a difference, so to reliably
      // report about its result, we should follow in its literal footsteps and not
      // try anything fancy that could lead to an error.
      Bindings::const_iterator i, j;
      for (i = v1.attrs()->begin(), j = v2.attrs()->begin(); i != v1.attrs()->end(); ++i, ++j) {
        if (i->name != j->name) {
          // A difference in a sorted list means that one attribute is not contained in the other,
          // but we don't know which. Let's find out. Could use <, but this is more clear.
          if (!v2.attrs()->get(i->name)) {
            error<AssertionError>("attribute name '%s' is contained in '%s', but not in '%s'",
                                  symbols[i->name], ValuePrinter(*this, v1, errorPrintOptions),
                                  ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
          }
          if (!v1.attrs()->get(j->name)) {
            error<AssertionError>(
                "attribute name '%s' is missing in '%s', but is contained in '%s'",
                symbols[j->name], ValuePrinter(*this, v1, errorPrintOptions),
                ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
          }
          assert(false);
        }
        try {
          assertEqValues(*i->value, *j->value, pos, error_ctx);
        } catch (Error& e) {
          // The order of traces is reversed, so this presents as
          //  where left hand side is
          //    at <pos>
          //  where right hand side is
          //    at <pos>
          //  while comparing attribute '<name>'
          if (j->pos != no_pos)
            e.add_trace(positions[j->pos], "where right hand side is");
          if (i->pos != no_pos)
            e.add_trace(positions[i->pos], "where left hand side is");
          e.add_trace(positions[pos], "while comparing attribute '%s'", symbols[i->name]);
          throw;
        }
      }
      return;
    }

    case nFunction:
      error<AssertionError>(
          "distinct functions and immediate comparisons of identical functions compare as unequal")
          .debugThrow();

    case nExternal:
      if (!(*v1.external() == *v2.external())) {
        error<AssertionError>("external value '%s' is not equal to external value '%s'",
                              ValuePrinter(*this, v1, errorPrintOptions),
                              ValuePrinter(*this, v2, errorPrintOptions))
            .debugThrow();
      }
      return;

    case nFloat:
      // !!!
      if (!(v1.fpoint() == v2.fpoint())) {
        error<AssertionError>("float '%f' is not equal to float '%f'", v1.fpoint(), v2.fpoint())
            .debugThrow();
      }
      return;

    // Cannot be returned by forceValue().
    case nThunk:
    case nFailed:
      unreachable();

    default: // Note that we pass compiler flags that should make `default:` unreachable.
      // Also note that this probably ran after `eqValues`, which implements
      // the same logic more efficiently (without having to unwind stacks),
      // so maybe `assertEqValues` and `eqValues` are out of sync. Check it for solutions.
      error<EvalError>("assertEqValues: cannot compare %1% with %2%", show_type(v1), show_type(v2))
          .withTrace(pos, error_ctx)
          .panic();
  }
}

// This implementation must match assertEqValues
bool EvalState::eqValues(Value& v1, Value& v2, const pos_idx_t pos, std::string_view error_ctx) {
  forceValue(v1, pos);
  forceValue(v2, pos);

  /* !!! Hack to support some old broken code that relies on pointer
     equality tests between sets.  (Specifically, builderDefs calls
     uniqList on a list of sets.)  Will remove this eventually. */
  if (&v1 == &v2)
    return true;

  // Special case type-compatibility between float and int
  if (v1.type() == nInt && v2.type() == nFloat)
    return v1.integer().value == v2.fpoint();
  if (v1.type() == nFloat && v2.type() == nInt)
    return v1.fpoint() == v2.integer().value;

  // All other types are not compatible with each other.
  if (v1.type() != v2.type())
    return false;

  switch (v1.type()) {
    case nInt:
      return v1.integer() == v2.integer();

    case nBool:
      return v1.boolean() == v2.boolean();

    case nString:
      return v1.string_view() == v2.string_view();

    case nPath:
      return
          // FIXME: compare accessors by their fingerprint.
          v1.pathAccessor() == v2.pathAccessor() && v1.pathStrView() == v2.pathStrView();

    case nNull:
      return true;

    case nList:
      if (v1.list_size() != v2.list_size())
        return false;
      for (size_t n = 0; n < v1.list_size(); ++n)
        if (!eqValues(*v1.list_view()[n], *v2.list_view()[n], pos, error_ctx))
          return false;
      return true;

    case nAttrs: {
      /* If both sets denote a derivation (type = "derivation"),
         then compare their out_paths. */
      if (is_derivation(v1) && is_derivation(v2)) {
        auto i = v1.attrs()->get(s.out_path);
        auto j = v2.attrs()->get(s.out_path);
        if (i && j)
          return eqValues(*i->value, *j->value, pos, error_ctx);
      }

      if (v1.attrs()->size() != v2.attrs()->size())
        return false;

      /* Otherwise, compare the attributes one by one. */
      Bindings::const_iterator i, j;
      for (i = v1.attrs()->begin(), j = v2.attrs()->begin(); i != v1.attrs()->end(); ++i, ++j)
        if (i->name != j->name || !eqValues(*i->value, *j->value, pos, error_ctx))
          return false;

      return true;
    }

    /* Functions are incomparable. */
    case nFunction:
      return false;

    case nExternal:
      return *v1.external() == *v2.external();

    case nFloat:
      // !!!
      return v1.fpoint() == v2.fpoint();

    // Cannot be returned by forceValue().
    case nThunk:
    case nFailed:
      unreachable();

    default: // Note that we pass compiler flags that should make `default:` unreachable.
      error<EvalError>("eqValues: cannot compare %1% with %2%", show_type(v1), show_type(v2))
          .withTrace(pos, error_ctx)
          .panic();
  }
}

bool EvalState::fullGC() {
#if NIX_USE_BOEHMGC
  GC_gcollect();
  // Check that it ran. We might replace this with a version that uses more
  // of the boehm API to get this reliably, at a maintenance cost.
  // We use a 1K margin because technically this has a race condition, but we
  // probably won't encounter it in practice, because the CLI isn't concurrent
  // like that.
  return GC_get_bytes_since_gc() < 1024;
#else
  return false;
#endif
}

bool Counter::enabled = get_env("NIX_SHOW_STATS").value_or("0") != "0";

void EvalState::maybePrintStats() {
  if (Counter::enabled) {
    // Make the final heap size more deterministic.
#if NIX_USE_BOEHMGC
    if (!fullGC()) {
      warn("failed to perform a full GC before reporting stats");
    }
#endif
    printStatistics();
  }
}

void EvalState::printStatistics() {
  std::chrono::microseconds cpuTimeDuration = get_cpu_user_time();
  float cpuTime = std::chrono::duration_cast<std::chrono::duration<float>>(cpuTimeDuration).count();

  auto& memstats = mem.get_stats();

  uint64_t bEnvs = memstats.nrEnvs * sizeof(Env) + memstats.nrValuesInEnvs * sizeof(Value*);
  uint64_t bLists = memstats.nrListElems * sizeof(Value*);
  uint64_t bValues = memstats.nrValues * sizeof(Value);
  uint64_t bAttrsets =
      memstats.nrAttrsets * sizeof(Bindings) + memstats.nrAttrsInAttrsets * sizeof(Attr);

#if NIX_USE_BOEHMGC
  GC_word heapSize, totalBytes;
  GC_get_heap_usage_safe(&heapSize, 0, 0, 0, &totalBytes);
  double gcFullOnlyTime = ({
    auto ms = GC_get_full_gc_total_time();
    ms * 0.001;
  });
  auto gcCycles = getGCCycles();
#endif

  auto out_path = get_env("NIX_SHOW_STATS_PATH").value_or("-");
  std::fstream fs;
  if (out_path != "-")
    fs.open(out_path, std::fstream::out);
  json topObj = json::object();
  topObj["cpuTime"] = cpuTime;
  topObj["time"] = {
      {"cpu", cpuTime},
#if NIX_USE_BOEHMGC
      {GC_is_incremental_mode() ? "gcNonIncremental" : "gc", gcFullOnlyTime},
      {GC_is_incremental_mode() ? "gcNonIncrementalFraction" : "gcFraction",
       gcFullOnlyTime / cpuTime},
#endif
  };
  topObj["envs"] = {
      {"number", memstats.nrEnvs.load()},
      {"elements", memstats.nrValuesInEnvs.load()},
      {"bytes", bEnvs},
  };
  topObj["nrExprs"] = Expr::nrExprs.load();
  topObj["list"] = {
      {"elements", memstats.nrListElems.load()},
      {"bytes", bLists},
      {"concats", nrListConcats.load()},
  };
  topObj["values"] = {
      {"number", memstats.nrValues.load()},
      {"bytes", bValues},
  };
  topObj["symbols"] = {
      {"number", symbols.size()},
      {"bytes", symbols.totalSize()},
  };
  topObj["sets"] = {
      {"number", memstats.nrAttrsets.load()},
      {"bytes", bAttrsets},
      {"elements", memstats.nrAttrsInAttrsets.load()},
  };
  topObj["sizes"] = {
      {"Env", sizeof(Env)},
      {"Value", sizeof(Value)},
      {"Bindings", sizeof(Bindings)},
      {"Attr", sizeof(Attr)},
  };
  topObj["nrOpUpdates"] = nrOpUpdates.load();
  topObj["nrOpUpdateValuesCopied"] = nrOpUpdateValuesCopied.load();
  topObj["nrThunks"] = nr_thunks.load();
  topObj["nrThunksAwaited"] = nrThunksAwaited.load();
  topObj["nrThunksAwaitedSlow"] = nrThunksAwaitedSlow.load();
  topObj["nrSpuriousWakeups"] = nrSpuriousWakeups.load();
  topObj["maxWaiting"] = maxWaiting.load();
  topObj["waitingTime"] = microsecondsWaiting / (double)1000000;
  topObj["nrAvoided"] = nrAvoided.load();
  topObj["nrLookups"] = nrLookups.load();
  topObj["nrPrimOpCalls"] = nrPrimOpCalls.load();
  topObj["nrFunctionCalls"] = nrFunctionCalls.load();
#if NIX_USE_BOEHMGC
  topObj["gc"] = {
      {"heapSize", heapSize},
      {"totalBytes", totalBytes},
      {"cycles", gcCycles},
  };
#endif

  if (countCalls) {
    topObj["primops"] = primOpCalls;
    {
      auto& list = topObj["functions"];
      list = json::array();
      for (auto& [fun, count] : functionCalls) {
        json obj = json::object();
        if (fun->name)
          obj["name"] = (std::string_view)symbols[fun->name];
        else
          obj["name"] = nullptr;
        if (auto pos = positions[fun->pos]) {
          if (auto path = std::get_if<source_path_t>(&pos.origin))
            obj["file"] = path->to_string();
          obj["line"] = pos.line;
          obj["column"] = pos.column;
        }
        obj["count"] = count;
        list.push_back(obj);
      }
    }
    {
      auto list = topObj["attributes"];
      list = json::array();
      for (auto& i : attrSelects) {
        json obj = json::object();
        if (auto pos = positions[i.first]) {
          if (auto path = std::get_if<source_path_t>(&pos.origin))
            obj["file"] = path->to_string();
          obj["line"] = pos.line;
          obj["column"] = pos.column;
        }
        obj["count"] = i.second;
        list.push_back(obj);
      }
    }
  }

  if (get_env("NIX_SHOW_SYMBOLS").value_or("0") != "0") {
    auto list = json::array();
    symbols.dump([&](std::string_view s) { list.emplace_back(std::string(s)); });
    // XXX: overrides earlier assignment
    topObj["symbols"] = std::move(list);
  }
  if (out_path == "-") {
    std::cerr << topObj.dump(2) << std::endl;
  } else {
    fs << topObj.dump(2) << std::endl;
  }
}

source_path_t resolve_expr_path(source_path_t path, bool add_default_nix) {
  unsigned int follow_count = 0, max_follow = 1024;

  /* If `path' is a symlink, follow it.  This is so that relative
     path references work. */
  while (!path.path.is_root()) {
    // Basic cycle/depth limit to avoid infinite loops.
    if (++follow_count >= max_follow)
      throw Error("too many symbolic links encountered while traversing the path '%s'", path);
    auto p = path.parent().resolve_symlinks() / path.base_name();
    if (p.lstat().type != SourceAccessor::t_symlink)
      break;
    path = {path.accessor, canon_path_t(p.read_link(), path.path.parent().value_or(canon_path_t::root))};
  }

  /* If `path' refers to a directory, append `/default.nix'. */
  if (add_default_nix && path.resolve_symlinks().lstat().type == SourceAccessor::t_directory)
    return path / "default.nix";

  return path;
}

Expr* EvalState::parseExprFromFile(const source_path_t& path) {
  return parseExprFromFile(path, staticBaseEnv);
}

Expr* EvalState::parseExprFromFile(const source_path_t& path,
                                   const std::shared_ptr<StaticEnv>& static_env) {
  auto buffer = path.resolve_symlinks().read_file();
  // readFile hopefully have left some extra space for terminators
  buffer.append("\0\0", 2);
  return parse(buffer.data(), buffer.size(), pos_t::origin_t(path), path.parent(), static_env);
}

Expr* EvalState::parseExprFromString(std::string s_, const source_path_t& base_path,
                                     const std::shared_ptr<StaticEnv>& static_env) {
  // NOTE this method (and parseStdin) must take care to *fully copy* their input
  // into their respective Pos::Origin until the parser stops overwriting its input
  // data.
  auto s = make_ref<std::string>(s_);
  s_.append("\0\0", 2);
  return parse(s_.data(), s_.size(), pos_t::String{.source = s}, base_path, static_env);
}

Expr* EvalState::parseExprFromString(std::string s, const source_path_t& base_path) {
  return parseExprFromString(std::move(s), base_path, staticBaseEnv);
}

Expr* EvalState::parseStdin() {
  // NOTE this method (and parseExprFromString) must take care to *fully copy* their
  // input into their respective Pos::Origin until the parser stops overwriting its
  // input data.
  // Activity act(*logger, lvlTalkative, "parsing standard input");
  auto buffer = drain_fd(0);
  // drainFD should have left some extra space for terminators
  buffer.append("\0\0", 2);
  auto s = make_ref<std::string>(buffer);
  return parse(buffer.data(), buffer.size(), pos_t::Stdin{.source = s}, root_path("."), staticBaseEnv);
}

source_path_t EvalState::findFile(const std::string_view path) {
  return findFile(lookup_path, path);
}

source_path_t EvalState::findFile(const LookupPath& lookup_path, const std::string_view path,
                               const pos_idx_t pos) {
  for (auto& i : lookup_path.elements) {
    auto suffixOpt = i.prefix.suffixIfPotentialMatch(path);

    if (!suffixOpt)
      continue;
    auto suffix = *suffixOpt;

    auto rOpt = resolveLookupPathPath(i.path);
    if (!rOpt)
      continue;
    auto r = *rOpt;

    auto res = (r / canon_path_t(suffix)).resolve_symlinks();
    if (res.path_exists())
      return res;

    // Backward compatibility hack: throw an exception if access
    // to this path is not allowed.
    if (auto accessor = res.accessor.dynamic_pointer_cast<FilteringSourceAccessor>())
      accessor->checkAccess(res.path);
  }

  if (has_prefix(path, "nix/"))
    return {corepkgsFS, canon_path_t(path.substr(3))};

  error<ThrownError>(
      settings.pureEval
          ? "cannot look up '<%s>' in pure evaluation mode (use '--impure' to override)"
          : "file '%s' was not found in the Nix search path (add it using $NIX_PATH or -I)",
      path)
      .at_pos(pos)
      .debugThrow();
}

std::optional<source_path_t> EvalState::resolveLookupPathPath(const LookupPath::Path& value0,
                                                           bool initAccessControl) {
  auto& value = value0.s;
  auto i = lookupPathResolved.find(value);
  if (i != lookupPathResolved.end())
    return i->second;

  auto finish = [&](std::optional<source_path_t> res) {
    if (res)
      debug("resolved search path element '%s' to '%s'", value, *res);
    else
      debug("failed to resolve search path element '%s'", value);
    lookupPathResolved.emplace(value, res);
    return res;
  };

  if (EvalSettings::isPseudoUrl(value)) {
    try {
      auto accessor =
          fetchers::download_tarball(*store, fetch_settings, EvalSettings::resolvePseudoUrl(value));
      auto store_path = fetch_to_store(fetch_settings, *store, source_path_t(accessor), FetchMode::Copy);
      return finish(this->store_path(store_path));
    } catch (Error& e) {
      logWarning(
          {.msg = hint_fmt_t("Nix search path entry '%1%' cannot be downloaded, ignoring", value)});
    }
  }

  if (auto colPos = value.find(':'); colPos != value.npos) {
    auto scheme = value.substr(0, colPos);
    auto rest = value.substr(colPos + 1);
    if (auto* hook = get(settings.lookupPathHooks, scheme)) {
      auto res = (*hook)(*this, rest);
      if (res)
        return finish(std::move(*res));
    }
  }

  {
    auto path = root_path(value);

    /* Allow access to paths in the search path. */
    if (initAccessControl) {
      allowPathLegacy(path.path.abs());
      if (store->isInStore(path.path.abs())) {
        try {
          allowClosure(store->toStorePath(path.path.abs()).first);
        } catch (InvalidPath&) {
        }
      }
    }

    if (path.resolve_symlinks().path_exists())
      return finish(std::move(path));
    else {
      // Backward compatibility hack: throw an exception if access
      // to this path is not allowed.
      if (auto accessor = path.accessor.dynamic_pointer_cast<FilteringSourceAccessor>())
        accessor->checkAccess(path.path);

      logWarning({.msg = hint_fmt_t("Nix search path entry '%1%' does not exist, ignoring", value)});
    }
  }

  return finish(std::nullopt);
}

Expr* EvalState::parse(char* text, size_t length, pos_t::origin_t origin, const source_path_t& base_path,
                       const std::shared_ptr<StaticEnv>& static_env) {
  DocCommentMap tmpDocComments; // Only used when not origin is not a SourcePath
  auto* doc_comments = &tmpDocComments;

  if (auto source_path = std::get_if<source_path_t>(&origin)) {
    auto [it, _] = positionToDocComment.lock()->try_emplace(*source_path, make_ref<DocCommentMap>());
    doc_comments = &*it->second;
  }

  auto result = parse_expr_from_buf(text, length, origin, base_path, mem.exprs, symbols, settings,
                                 positions, *doc_comments, root_fs);

  result->bindVars(*this, static_env);

  return result;
}

DocComment EvalState::getDocCommentForPos(pos_idx_t pos) {
  auto pos2 = positions[pos];
  auto path = pos2.get_source_path();
  if (!path)
    return {};

  auto positionToDocComment_ = positionToDocComment.read_lock();

  auto table = positionToDocComment_->find(*path);
  if (table == positionToDocComment_->end())
    return {};

  auto it = table->second->find(pos);
  if (it == table->second->end())
    return {};
  return it->second;
}

std::string ExternalValueBase::coerceToString(EvalState& state, const pos_idx_t& pos,
                                              NixStringContext& context, bool copyMore,
                                              bool copy_to_store) const {
  state.error<TypeError>("cannot coerce %1% to a string: %2%", show_type(), *this)
      .at_pos(pos)
      .debugThrow();
}

bool ExternalValueBase::operator==(const ExternalValueBase& b) const noexcept {
  return false;
}

std::ostream& operator<<(std::ostream& str, const ExternalValueBase& v) {
  return v.print(str);
}

void force_no_null_byte(std::string_view s, std::function<pos_t()> pos) {
  if (s.find('\0') != s.npos) {
    using namespace std::string_view_literals;
    auto str = replace_strings(std::string(s), "\0"sv, "␀"sv);
    Error error(
        "input string '%s' cannot be represented as Nix string because it contains null bytes",
        str);
    if (pos) {
      error.at_pos(pos());
    }
    throw error;
  }
}

void EvalState::waitForPath(const StorePath& path) {
  async_path_writer->waitForPath(path);
}

void EvalState::waitForPath(const SingleDerivedPath& path) {
  std::visit(overloaded{
                 [&](const DerivedPathOpaque& p) { waitForPath(p.path); },
                 [&](const SingleDerivedPathBuilt& p) { waitForPath(*p.drv_path); },
             },
             path.raw());
}

void EvalState::waitForAllPaths() {
  async_path_writer->waitForAllPaths();
}

} // namespace nix
