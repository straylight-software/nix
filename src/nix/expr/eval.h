#pragma once
///@file

#include "nix/expr/attr-set.h"
#include "nix/expr/counter.h"
#include "nix/expr/eval-error.h"
#include "nix/expr/eval-profiler.h"
#include "nix/expr/nixexpr.h"
#include "nix/expr/repl-exit-status.h"
#include "nix/expr/search-path.h"
#include "nix/expr/symbol-table.h"
#include "nix/expr/value.h"
#include "nix/util/configuration.h"
#include "nix/util/experimental-features.h"
#include "nix/util/pos-table.h"
#include "nix/util/position.h"
#include "nix/util/ref.h"
#include "nix/util/repair-flag.h"
#include "nix/util/source-accessor.h"
#include "nix/util/types.h"

// For `NIX_USE_BOEHMGC`, and if that's set, `GC_THREADS`
#include <functional>
#include <map>
#include <optional>

#include <boost/unordered/concurrent_flat_map_fwd.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

#include "nix/expr/config.h"

namespace nix {

/**
 * We put a limit on primop arity because it lets us use a fixed size array on
 * the stack. 8 is already an impractical number of arguments. use an attrset
 * argument for such overly complicated functions.
 */
constexpr size_t maxPrimOpArity = 8;

class store_t;

namespace fetchers {
struct settings_t;
struct InputCache;
struct input_t;
} // namespace fetchers
struct eval_settings_t;
class eval_state_t;
class store_path_t;
struct SingleDerivedPath;
struct memory_source_accessor_t;
struct mounted_source_accessor_t;
struct AsyncPathWriter;

namespace eval_cache {
class EvalCache;
}
struct Executor;

/**
 * Increments a count on construction and decrements on destruction.
 */
class CallDepth {
  size_t& count;

public:
  CallDepth(size_t& count) : count(count) { ++count; }

  ~CallDepth() { --count; }
};

/**
 * Function that implements a primop.
 */
using PrimOpFun = void(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v);

/**
 * Info about a primitive operation, and its implementation
 */
struct PrimOp {
  /**
   * Name of the primop. `__` prefix is treated specially.
   */
  std::string name;

  /**
   * Names of the parameters of a primop, for primops that take a
   * fixed number of arguments to be substituted for these parameters.
   */
  std::vector<std::string> args;

  /**
   * Aritiy of the primop.
   *
   * If `args` is not empty, this field will be computed from that
   * field instead, so it doesn't need to be manually set.
   */
  size_t arity = 0;

  /**
   * Optional free-form documentation about the primop.
   */
  std::optional<std::string> doc;

  /**
   * Add a trace item, while calling the `<name>` builtin.
   *
   * This is used to remove the redundant item for `builtins.addErrorContext`.
   */
  bool add_trace = true;

  /**
   * Implementation of the primop.
   */
  std::function<PrimOpFun> fun;

  /**
   * Optional experimental for this to be gated on.
   */
  std::optional<experimental_feature_t> experimental_feature;

  /**
   * If true, this primop is not exposed to the user.
   */
  bool internal = false;

  /**
   * Validity check to be performed by functions that introduce primops,
   * such as RegisterPrimOp() and value_t::mkPrimOp().
   */
  void check();
};

std::ostream& operator<<(std::ostream& output, const PrimOp& prim_op);

/**
 * Info about a constant
 */
struct Constant {
  /**
   * Optional type of the constant (known since it is a fixed value).
   *
   * @todo we should use an enum for this.
   */
  ValueType type = nThunk;

  /**
   * Optional free-form documentation about the constant.
   */
  const char* doc = nullptr;

  /**
   * Whether the constant is impure, and not available in pure mode.
   */
  bool impureOnly = false;
};

typedef std::map<std::string, value_t*, std::less<std::string>,
                 traceable_allocator<std::pair<const std::string, value_t*>>>
    ValMap;

using DocCommentMap = boost::unordered_flat_map<pos_idx_t, DocComment, std::hash<pos_idx_t>>;

struct Env {
  Env* up;
  value_t* values[0];
};

void print_env_bindings(const eval_state_t& es, const expr_t& expr, const Env& env);
void print_env_bindings(const symbol_table_t& st, const StaticEnv& se, const Env& env, int lvl = 0);

std::unique_ptr<ValMap> map_static_env_bindings(const symbol_table_t& st, const StaticEnv& se,
                                                const Env& env);

void copy_context(
    const value_t& v, NixStringContext& context,
    const experimental_feature_settings_t& xp_settings = experimental_feature_settings);

std::string print_value(eval_state_t& state, value_t& v);
std::ostream& operator<<(std::ostream& os, const ValueType t);

struct regex_cache_t;

ref<regex_cache_t> make_regex_cache();

struct DebugTrace {
  /* WARNING: Converting pos_idx_t -> pos_t should be done with extra care. This is
     due to the fact that operator[] of pos_table_t is incredibly expensive. */
  std::variant<pos_t, pos_idx_t> pos;
  const expr_t& expr;
  const Env& env;
  hint_fmt_t hint;
  bool isError;

  pos_t getPos(const pos_table_t& table) const {
    return std::visit(overloaded{
                          [&](pos_idx_t idx) {
                            // Prefer direct pos, but if noPos then try the expr.
                            if (!idx) {
                              idx = expr.getPos();
                            }
                            return table[idx];
                          },
                          [&](pos_t pos) { return pos; },
                      },
                      pos);
  }
};

struct StaticEvalSymbols {
  symbol_t with, out_path, drv_path, type, meta, name, value, system, overrides, outputs,
      output_name, ignore_nulls, file, line, column, functor, toString, right, wrong,
      structured_attrs, json, allowedReferences, allowedRequisites, disallowedReferences,
      disallowedRequisites, max_size, maxClosureSize, builder, args, content_addressed, impure,
      outputHash, outputHashAlgo, outputHashMode, recurseForDerivations, description, self, epsilon,
      start_set, operator_, key, path, prefix, outputSpecified;

  expr_t::AstSymbols exprSymbols;

  static constexpr auto preallocate() {
    StaticSymbolTable alloc;

    StaticEvalSymbols staticSymbols = {.with = alloc.create("<with>"),
                                       .out_path = alloc.create("outPath"),
                                       .drv_path = alloc.create("drvPath"),
                                       .type = alloc.create("type"),
                                       .meta = alloc.create("meta"),
                                       .name = alloc.create("name"),
                                       .value = alloc.create("value"),
                                       .system = alloc.create("system"),
                                       .overrides = alloc.create("__overrides"),
                                       .outputs = alloc.create("outputs"),
                                       .output_name = alloc.create("outputName"),
                                       .ignore_nulls = alloc.create("__ignoreNulls"),
                                       .file = alloc.create("file"),
                                       .line = alloc.create("line"),
                                       .column = alloc.create("column"),
                                       .functor = alloc.create("__functor"),
                                       .toString = alloc.create("__toString"),
                                       .right = alloc.create("right"),
                                       .wrong = alloc.create("wrong"),
                                       .structured_attrs = alloc.create("__structuredAttrs"),
                                       .json = alloc.create("__json"),
                                       .allowedReferences = alloc.create("allowedReferences"),
                                       .allowedRequisites = alloc.create("allowedRequisites"),
                                       .disallowedReferences = alloc.create("disallowedReferences"),
                                       .disallowedRequisites = alloc.create("disallowedRequisites"),
                                       .max_size = alloc.create("maxSize"),
                                       .maxClosureSize = alloc.create("maxClosureSize"),
                                       .builder = alloc.create("builder"),
                                       .args = alloc.create("args"),
                                       .content_addressed = alloc.create("__contentAddressed"),
                                       .impure = alloc.create("__impure"),
                                       .outputHash = alloc.create("outputHash"),
                                       .outputHashAlgo = alloc.create("outputHashAlgo"),
                                       .outputHashMode = alloc.create("outputHashMode"),
                                       .recurseForDerivations =
                                           alloc.create("recurseForDerivations"),
                                       .description = alloc.create("description"),
                                       .self = alloc.create("self"),
                                       .epsilon = alloc.create(""),
                                       .start_set = alloc.create("startSet"),
                                       .operator_ = alloc.create("operator"),
                                       .key = alloc.create("key"),
                                       .path = alloc.create("path"),
                                       .prefix = alloc.create("prefix"),
                                       .outputSpecified = alloc.create("outputSpecified"),
                                       .exprSymbols = {
                                           .sub = alloc.create("__sub"),
                                           .lessThan = alloc.create("__lessThan"),
                                           .mul = alloc.create("__mul"),
                                           .div = alloc.create("__div"),
                                           .or_ = alloc.create("or"),
                                           .findFile = alloc.create("__findFile"),
                                           .nixPath = alloc.create("__nixPath"),
                                           .body = alloc.create("body"),
                                       }};

    return std::pair{staticSymbols, alloc};
  }

  static consteval StaticEvalSymbols create() { return preallocate().first; }

  static constexpr StaticSymbolTable staticSymbolTable() { return preallocate().second; }
};

class EvalMemory {
#if NIX_USE_BOEHMGC
  /**
   * Allocation cache for GC'd value_t objects.
   */
  std::shared_ptr<void*> valueAllocCache;

  /**
   * Allocation cache for size-1 Env objects.
   */
  std::shared_ptr<void*> env1AllocCache;
#endif

public:
  struct Statistics {
    Counter nrEnvs;
    Counter nrValuesInEnvs;
    Counter nrValues;
    Counter nrAttrsets;
    Counter nrAttrsInAttrsets;
    Counter nrListElems;
  };

  EvalMemory();

  EvalMemory(const EvalMemory&) = delete;
  EvalMemory(EvalMemory&&) = delete;
  EvalMemory& operator=(const EvalMemory&) = delete;
  EvalMemory& operator=(EvalMemory&&) = delete;

  inline void* allocBytes(size_t n);
  inline value_t* allocValue();
  inline Env& allocEnv(size_t size);

  bindings_t* allocBindings(size_t capacity);

  BindingsBuilder buildBindings(symbol_table_t& symbols, size_t capacity) {
    return BindingsBuilder(*this, symbols, allocBindings(capacity), capacity);
  }

  ListBuilder buildList(size_t size) {
    stats.nrListElems += size;
    return ListBuilder(*this, size);
  }

  const Statistics& get_stats() const& { return stats; }

  /**
   * Storage for the AST nodes
   */
  Exprs exprs;

private:
  Statistics stats;
};

class eval_state_t : public std::enable_shared_from_this<eval_state_t> {
public:
  static constexpr StaticEvalSymbols s = StaticEvalSymbols::create();

  const fetchers::settings_t& fetch_settings;
  const eval_settings_t& settings;

  symbol_table_t symbols;
  pos_table_t positions;

  EvalMemory mem;

  /**
   * If set, force copying files to the Nix store even if they
   * already exist there.
   */
  RepairFlag repair;

  /**
   * The accessor corresponding to `store`.
   */
  const ref<mounted_source_accessor_t> storeFS;

  /**
   * The accessor for the root filesystem.
   */
  const ref<source_accessor_t> root_fs;

  /**
   * The in-memory filesystem for <nix/...> paths.
   */
  const ref<memory_source_accessor_t> corepkgsFS;

  /**
   * In-memory filesystem for internal, non-user-callable Nix
   * expressions like `derivation.nix`.
   */
  const ref<memory_source_accessor_t> internal_fs;

  const source_path_t derivationInternal;

  /**
   * store_t used to materialise .drv files.
   */
  const ref<store_t> store;

  /**
   * store_t used to build stuff.
   */
  const ref<store_t> buildStore;

  RootValue vImportedDrvToDerivation = nullptr;

  const ref<fetchers::InputCache> inputCache;

  /**
   * Debugger
   */
  ReplExitStatus (*debugRepl)(ref<eval_state_t> es, const ValMap& extraEnv);
  bool debugStop;
  bool inDebugger = false;
  int trylevel;
  std::list<DebugTrace> debugTraces;
  boost::unordered_flat_map<const expr_t*, const std::shared_ptr<const StaticEnv>> exprEnvs;

  ref<AsyncPathWriter> async_path_writer;

  const std::shared_ptr<const StaticEnv> getStaticEnv(const expr_t& expr) const {
    auto i = exprEnvs.find(&expr);
    if (i != exprEnvs.end()) {
      return i->second;
    } else {
      return std::shared_ptr<const StaticEnv>();
    };
  }

  /** Whether a debug repl can be started. If `false`, `runDebugRepl(error)` will return without
   * starting a repl. */
  bool canDebug();

  /** use front of `debugTraces`; see `runDebugRepl(error,env,expr)` */
  void runDebugRepl(const Error* error);

  /**
   * Run a debug repl with the given error, environment and expression.
   * @param error The error to debug, may be nullptr.
   * @param env The environment to debug, matching the expression.
   * @param expr The expression to debug, matching the environment.
   */
  void runDebugRepl(const Error* error, const Env& env, const expr_t& expr);

  template <class T, typename... args_t>
  [[nodiscard, gnu::noinline]]
  EvalErrorBuilder<T>& error(const args_t&... args) {
    // `EvalErrorBuilder::debugThrow` performs the corresponding `delete`.
    return *new EvalErrorBuilder<T>(*this, args...);
  }

  /**
   * A cache for evaluation caches, so as to reuse the same root value if possible
   */
  std::map<const Hash, ref<eval_cache::EvalCache>> evalCaches;

private:
  /* cache_t for calls to add_to_store(); maps source paths to the store
     paths. */
  const ref<boost::concurrent_flat_map<source_path_t, store_path_t>> srcToStore;

  /**
   * A cache that maps paths to "resolved" paths for importing Nix
   * expressions, i.e. `/foo` to `/foo/default.nix`.
   */
  const ref<boost::concurrent_flat_map<source_path_t, source_path_t>> importResolutionCache;

  /**
   * A cache from resolved paths to values.
   */
  const ref<boost::concurrent_flat_map<
      source_path_t, value_t*, std::hash<source_path_t>, std::equal_to<source_path_t>,
      traceable_allocator<std::pair<const source_path_t, value_t*>>>>
      fileEvalCache;

  /**
   * Associate source positions of certain AST nodes with their preceding doc comment, if they have
   * one. Grouped by file.
   */
  shared_sync_t<boost::unordered_flat_map<source_path_t, ref<DocCommentMap>>> positionToDocComment;

  LookupPath lookup_path;

  // FIXME: make thread-safe.
  boost::unordered_flat_map<std::string, std::optional<source_path_t>, string_view_hash_t,
                            std::equal_to<>>
      lookupPathResolved;

  /**
   * cache_t used by prim_match().
   */
  const ref<regex_cache_t> regexCache;

public:
  /**
   * @param lookup_path     Only used during construction.
   * @param store          The store to use for instantiation
   * @param fetch_settings  Must outlive the lifetime of this eval_state_t!
   * @param settings       Must outlive the lifetime of this eval_state_t!
   * @param buildStore     The store to use for builds ("import from derivation", C API
   * `nix_string_realise`)
   */
  eval_state_t(const LookupPath& lookup_path, ref<store_t> store,
               const fetchers::settings_t& fetch_settings, const eval_settings_t& settings,
               std::shared_ptr<store_t> buildStore = nullptr);
  ~eval_state_t();

  /**
   * A wrapper around EvalMemory::allocValue() to avoid code churn when it
   * was introduced.
   */
  inline value_t* allocValue() { return mem.allocValue(); }

  LookupPath getLookupPath() { return lookup_path; }

  /**
   * Return a `source_path_t` that refers to `path` in the root
   * filesystem.
   */
  source_path_t root_path(canon_path_t path);

  /**
   * Variant which accepts relative paths too.
   */
  source_path_t root_path(path_view_t path);

  /**
   * Return a `source_path_t` that refers to `path` in the store.
   *
   * For now, this has to also be within the root filesystem for
   * backwards compat, but for Windows and maybe also pure eval, we'll
   * probably want to do something different.
   */
  source_path_t store_path(const store_path_t& path);

  /**
   * Allow access to a path.
   *
   * Only for restrict eval: pure eval just whitelist store paths,
   * never arbitrary paths.
   */
  void allowPathLegacy(const Path& path);

  /**
   * Allow access to a store path. Note that this gets remapped to
   * the real store path if `store` is a chroot store.
   */
  void allowPath(const store_path_t& store_path);

  /**
   * Allow access to the closure of a store path.
   */
  void allowClosure(const store_path_t& store_path);

  /**
   * Allow access to a store path and return it as a string.
   */
  void allowAndSetStorePathString(const store_path_t& store_path, value_t& v);

  void checkURI(const std::string& uri);

  /**
   * Mount an input on the Nix store.
   */
  store_path_t mountInput(fetchers::input_t& input, const fetchers::input_t& original_input,
                          ref<source_accessor_t> accessor, bool require_lockable,
                          bool forceNarHash = false);

  /**
   * Parse a Nix expression from the specified file.
   */
  expr_t* parseExprFromFile(const source_path_t& path);
  expr_t* parseExprFromFile(const source_path_t& path,
                            const std::shared_ptr<StaticEnv>& static_env);

  /**
   * Parse a Nix expression from the specified string.
   */
  expr_t* parseExprFromString(std::string s, const source_path_t& base_path,
                              const std::shared_ptr<StaticEnv>& static_env);
  expr_t* parseExprFromString(std::string s, const source_path_t& base_path);

  expr_t* parseStdin();

  /**
   * Evaluate an expression read from the given file to normal
   * form. Optionally enforce that the top-level expression is
   * trivial (i.e. doesn't require arbitrary computation).
   */
  void evalFile(const source_path_t& path, value_t& v, bool must_be_trivial = false);

  void resetFileCache();

  /**
   * Look up a file in the search path.
   */
  source_path_t findFile(const std::string_view path);
  source_path_t findFile(const LookupPath& lookup_path, const std::string_view path,
                         const pos_idx_t pos = no_pos);

  /**
   * Try to resolve a search path value (not the optional key part).
   *
   * If the specified search path element is a URI, download it.
   *
   * If it is not found, return `std::nullopt`.
   */
  std::optional<source_path_t> resolveLookupPathPath(const LookupPath::Path& elem,
                                                     bool initAccessControl = false);

  /**
   * Evaluate an expression to normal form
   *
   * @param [out] v The resulting is stored here.
   */
  void eval(expr_t* e, value_t& v);

  /**
   * Evaluation the expression, then verify that it has the expected
   * type.
   */
  inline bool evalBool(Env& env, expr_t* e);
  inline bool evalBool(Env& env, expr_t* e, const pos_idx_t pos, std::string_view error_ctx);
  inline void evalAttrs(Env& env, expr_t* e, value_t& v, const pos_idx_t pos,
                        std::string_view error_ctx);

  /**
   * If `v` is a thunk, enter it and overwrite `v` with the result
   * of the evaluation of the thunk.  If `v` is a delayed function
   * application, call the function and overwrite `v` with the
   * result.  Otherwise, this is a no-op.
   */
  inline void forceValue(value_t& v, const pos_idx_t pos) { v.force(*this, pos); }

  void tryFixupBlackHolePos(value_t& v, pos_idx_t pos);

  /**
   * Force a value, then recursively force list elements and
   * attributes.
   */
  void forceValueDeep(value_t& v);

  /**
   * Force `v`, and then verify that it has the expected type.
   */
  NixInt forceInt(value_t& v, const pos_idx_t pos, std::string_view error_ctx);
  NixFloat forceFloat(value_t& v, const pos_idx_t pos, std::string_view error_ctx);
  bool forceBool(value_t& v, const pos_idx_t pos, std::string_view error_ctx);

  void forceAttrs(value_t& v, const pos_idx_t pos, std::string_view error_ctx);

  template <typename Callable>
  inline void forceAttrs(value_t& v, Callable getPos, std::string_view error_ctx);

  inline void forceList(value_t& v, const pos_idx_t pos, std::string_view error_ctx);
  /**
   * @param v either lambda or primop
   */
  void forceFunction(value_t& v, const pos_idx_t pos, std::string_view error_ctx);
  std::string_view forceString(value_t& v, const pos_idx_t pos, std::string_view error_ctx);
  std::string_view
  forceString(value_t& v, NixStringContext& context, const pos_idx_t pos,
              std::string_view error_ctx,
              const experimental_feature_settings_t& xp_settings = experimental_feature_settings);
  std::string_view forceStringNoCtx(value_t& v, const pos_idx_t pos, std::string_view error_ctx);

  /**
   * Get attribute from an attribute set and throw an error if it doesn't exist.
   */
  const attr_t* get_attr(symbol_t attrSym, const bindings_t* attrSet, std::string_view error_ctx);

  template <typename... args_t>
  [[gnu::noinline]]
  void addErrorTrace(Error& e, const args_t&... formatArgs) const;
  template <typename... args_t>
  [[gnu::noinline]]
  void addErrorTrace(Error& e, const pos_idx_t pos, const args_t&... formatArgs) const;

public:
  /**
   * @return true iff the value `v` denotes a derivation (i.e. a
   * set with attribute `type = "derivation"`).
   */
  bool is_derivation(value_t& v);

  std::optional<std::string> tryAttrsToString(const pos_idx_t pos, value_t& v,
                                              NixStringContext& context, bool coerceMore = false,
                                              bool copy_to_store = true);

  store_path_t devirtualize(const store_path_t& path, string_map_t* rewrites = nullptr);

  SingleDerivedPath devirtualize(const SingleDerivedPath& path, string_map_t* rewrites = nullptr);

  std::string devirtualize(std::string_view s, const NixStringContext& context);

  /**
   * String coercion.
   *
   * Converts strings, paths and derivations to a
   * string.  If `coerceMore` is set, also converts nulls, integers,
   * booleans and lists to a string.  If `copy_to_store` is set,
   * referenced paths are copied to the Nix store as a side effect.
   */
  backed_string_view_t coerceToString(const pos_idx_t pos, value_t& v, NixStringContext& context,
                                      std::string_view error_ctx, bool coerceMore = false,
                                      bool copy_to_store = true, bool canonicalizePath = true);

  store_path_t copyPathToStore(NixStringContext& context, const source_path_t& path, pos_idx_t pos);

  /**
   * Compute the base name for a `source_path_t`. For non-store paths,
   * this is just `source_path_t::base_name()`. But for store paths, for
   * backwards compatibility, it needs to be `<hash>-source`,
   * i.e. as if the path were copied to the Nix store. This results
   * in a "double-copied" store path like
   * `/nix/store/<hash1>-<hash2>-source`. We don't need to
   * materialize /nix/store/<hash2>-source though. Still, this
   * requires reading/hashing the path twice.
   */
  std::string computeBaseName(const source_path_t& path, pos_idx_t pos);

  /**
   * Path coercion.
   *
   * Converts strings, paths and derivations to a
   * path.  The result is guaranteed to be a canonicalised, absolute
   * path.  Nothing is copied to the store.
   */
  source_path_t coerceToPath(const pos_idx_t pos, value_t& v, NixStringContext& context,
                             std::string_view error_ctx);

  /**
   * Like coerceToPath, but the result must be a store path.
   */
  store_path_t coerceToStorePath(const pos_idx_t pos, value_t& v, NixStringContext& context,
                                 std::string_view error_ctx);

  /**
   * Part of `coerceToSingleDerivedPath()` without any store IO which is exposed for unit testing
   * only.
   */
  std::pair<SingleDerivedPath, std::string_view> coerceToSingleDerivedPathUnchecked(
      const pos_idx_t pos, value_t& v, std::string_view error_ctx,
      const experimental_feature_settings_t& xp_settings = experimental_feature_settings);

  /**
   * Coerce to `SingleDerivedPath`.
   *
   * Must be a string which is either a literal store path or a
   * "placeholder (see `DownstreamPlaceholder`).
   *
   * Even more importantly, the string context must be exactly one
   * element, which is either a `NixStringContextElem::opaque_t` or
   * `NixStringContextElem::Built`. (`NixStringContextEleme::DrvDeep`
   * is not permitted).
   *
   * The string is parsed based on the context --- the context is the
   * source of truth, and ultimately tells us what we want, and then
   * we ensure the string corresponds to it.
   */
  SingleDerivedPath coerceToSingleDerivedPath(const pos_idx_t pos, value_t& v,
                                              std::string_view error_ctx);

#if NIX_USE_BOEHMGC
  /** A GC root for the baseEnv reference. */
  const std::shared_ptr<Env*> baseEnvP;
#endif

public:
  /**
   * The base environment, containing the builtin functions and
   * values.
   */
  Env& baseEnv;

  /**
   * The same, but used during parsing to resolve variables.
   */
  const std::shared_ptr<StaticEnv> staticBaseEnv; // !!! should be private

  /**
   * Internal primops not exposed to the user.
   */
  boost::unordered_flat_map<std::string, value_t*, string_view_hash_t, std::equal_to<>,
                            traceable_allocator<std::pair<const std::string, value_t*>>>
      internalPrimOps;

  /**
   * Name and documentation about every constant.
   *
   * constants_t from primops are hard to crawl, and their docs will go
   * here too.
   */
  std::vector<std::pair<std::string, Constant>> constantInfos;

private:
  unsigned int baseEnvDispl = 0;

  void createBaseEnv(const eval_settings_t& settings);

  value_t* addConstant(const std::string& name, value_t& v, Constant info);

  void addConstant(const std::string& name, value_t* v, Constant info);

  value_t* addPrimOp(PrimOp&& prim_op);

public:
  /**
   * Retrieve a specific builtin, equivalent to evaluating `builtins.${name}`.
   * @param name The attribute name of the builtin to retrieve.
   * @throws EvalError if the builtin does not exist.
   */
  value_t& getBuiltin(const std::string& name);

  /**
   * Retrieve the `builtins` attrset, equivalent to evaluating the reference `builtins`.
   * always returns an attribute set value.
   */
  value_t& getBuiltins();

  struct Doc {
    pos_t pos;
    std::optional<std::string> name;
    size_t arity;
    std::vector<std::string> args;
    /**
     * Unlike the other `doc` fields in this file, this one should never be
     * `null`.
     */
    const char* doc;
  };

  /**
   * Retrieve the documentation for a value. This will evaluate the value if
   * it is a thunk, and it will partially apply __functor if applicable.
   *
   * @param v The value to get the documentation for.
   */
  std::optional<Doc> getDoc(value_t& v);

private:
  inline value_t* lookupVar(Env* env, const ExprVar& var, bool noEval);

  friend struct ExprVar;
  friend struct ExprAttrs;
  friend struct ExprLet;

  expr_t* parse(char* text, size_t length, pos_t::origin_t origin, const source_path_t& base_path,
                const std::shared_ptr<StaticEnv>& static_env);

  /**
   * Current Nix call stack depth, used with `max-call-depth`
   * setting to throw stack overflow hopefully before we run out of
   * system stack.
   */
  thread_local static size_t callDepth;

public:
  /**
   * Check that the call depth is within limits, and increment it, until the returned object is
   * destroyed.
   */
  inline CallDepth addCallDepth(const pos_idx_t pos);

  /**
   * Do a deep equality test between two values.  That is, list
   * elements and attributes are compared recursively.
   */
  bool eqValues(value_t& v1, value_t& v2, const pos_idx_t pos, std::string_view error_ctx);

  /**
   * Like `eqValues`, but throws an `AssertionError` if not equal.
   *
   * WARNING:
   * Callers should call `eqValues` first and report if `assertEqValues` behaves
   * incorrectly. (e.g. if it doesn't throw if eqValues returns false or vice versa)
   */
  void assertEqValues(value_t& v1, value_t& v2, const pos_idx_t pos, std::string_view error_ctx);

  bool isFunctor(const value_t& fun) const;

  void callFunction(value_t& fun, std::span<value_t*> args, value_t& v_res, const pos_idx_t pos);

  void callFunction(value_t& fun, value_t& arg, value_t& v_res, const pos_idx_t pos) {
    value_t* args[] = {&arg};
    callFunction(fun, args, v_res, pos);
  }

  /**
   * Automatically call a function for which each argument has a
   * default value or has a binding in the `args` map.
   */
  void autoCallFunction(const bindings_t& args, value_t& fun, value_t& res);

  BindingsBuilder buildBindings(size_t capacity) { return mem.buildBindings(symbols, capacity); }

  ListBuilder buildList(size_t size) { return mem.buildList(size); }

  /**
   * Return a boolean `value_t *` without allocating.
   */
  value_t* getBool(bool b);

  void mkThunk_(value_t& v, expr_t* expr);
  void mkPos(value_t& v, pos_idx_t pos);

  /**
   * Create a string representing a store path.
   *
   * The string is the printed store path with a context containing a
   * single `NixStringContextElem::opaque_t` element of that store path.
   */
  void mkStorePathString(const store_path_t& store_path, value_t& v);

  /**
   * Create a string representing a `SingleDerivedPath::Built`.
   *
   * The string is the printed store path with a context containing a
   * single `NixStringContextElem::Built` element of the drv path and
   * output name.
   *
   * @param value value_t we are settings
   *
   * @param b the drv whose output we are making a string for, and the
   * output
   *
   * @param optStaticOutputPath Optional output path for that string.
   * Must be passed if and only if output store object is
   * input-addressed or fixed output. Will be printed to form string
   * if passed, otherwise a placeholder will be used (see
   * `DownstreamPlaceholder`).
   *
   * @param xp_settings Stop-gap to avoid globals during unit tests.
   */
  void mk_output_string(
      value_t& value, const SingleDerivedPath::Built& b,
      std::optional<store_path_t> optStaticOutputPath,
      const experimental_feature_settings_t& xp_settings = experimental_feature_settings);

  /**
   * Create a string representing a `SingleDerivedPath`.
   *
   * A combination of `mkStorePathString` and `mk_output_string`.
   */
  void mkSingleDerivedPathString(const SingleDerivedPath& p, value_t& v);

  void concatLists(value_t& v, size_t nr_lists, value_t* const* lists, const pos_idx_t pos,
                   std::string_view error_ctx);

  /**
   * Print statistics, if enabled.
   *
   * Performs a full memory GC before printing the statistics, so that the
   * GC statistics are more accurate.
   */
  void maybePrintStats();

  /**
   * Print statistics, unconditionally, cheaply, without performing a GC first.
   */
  void printStatistics();

  /**
   * Perform a full memory garbage collection - not incremental.
   *
   * @return true if Nix was built with GC and a GC was performed, false if not.
   *              The return value is currently not thread safe - just the return value.
   */
  bool fullGC();

  /**
   * Realise the given context
   * @param[in] context the context to realise
   * @param[out] maybePaths if not nullptr, all built or referenced store paths will be added to
   * this set
   * @return a mapping from the placeholders used to construct the associated value to their final
   * store path.
   */
  [[nodiscard]] string_map_t realiseContext(const NixStringContext& context,
                                            store_path_set_t* maybePaths = nullptr,
                                            bool isIFD = true);

  /**
   * Realise the given string with context, and return the string with outputs instead of downstream
   * output placeholders.
   * @param[in] str the string to realise
   * @param[out] paths all referenced store paths will be added to this set
   * @return the realised string
   * @throw EvalError if the value is not a string, path or derivation (see `coerceToString`)
   */
  std::string realiseString(value_t& str, store_path_set_t* storePathsOutMaybe, bool isIFD = true,
                            const pos_idx_t pos = no_pos);

  /* Call the binary path filter predicate used builtins.path etc. */
  bool callPathFilter(value_t* filter_fun, const source_path_t& path, pos_idx_t pos);

  DocComment getDocCommentForPos(pos_idx_t pos);

  void waitForPath(const store_path_t& path);
  void waitForPath(const SingleDerivedPath& path);
  void waitForAllPaths();

private:
  /**
   * Like `mk_output_string` but just creates a raw string, not an
   * string value_t, which would also have a string context.
   */
  std::string mkOutputStringRaw(
      const SingleDerivedPath::Built& b, std::optional<store_path_t> optStaticOutputPath,
      const experimental_feature_settings_t& xp_settings = experimental_feature_settings);

  /**
   * Like `mkSingleDerivedPathStringRaw` but just creates a raw string
   * value_t, which would also have a string context.
   */
  std::string mkSingleDerivedPathStringRaw(const SingleDerivedPath& p);

  Counter nrLookups;
  Counter nrAvoided;
  Counter nrOpUpdates;
  Counter nrOpUpdateValuesCopied;
  Counter nrListConcats;
  Counter nrPrimOpCalls;
  Counter nrFunctionCalls;

public:
  Counter nrThunksAwaited;
  Counter nrThunksAwaitedSlow;
  Counter microsecondsWaiting;
  Counter currentlyWaiting;
  Counter maxWaiting;
  Counter nrSpuriousWakeups;

private:
  bool countCalls;

  // FIXME: make thread-safe.
  typedef boost::unordered_flat_map<std::string, size_t, string_view_hash_t, std::equal_to<>>
      PrimOpCalls;
  PrimOpCalls primOpCalls;

  typedef boost::unordered_flat_map<ExprLambda*, size_t> FunctionCalls;
  FunctionCalls functionCalls;

  /** Evaluation/call profiler. */
  MultiEvalProfiler profiler;

  void incrFunctionCall(ExprLambda* fun);

  // FIXME: make thread-safe.
  typedef boost::unordered_flat_map<pos_idx_t, size_t, std::hash<pos_idx_t>> AttrSelects;
  AttrSelects attrSelects;

  friend struct ExprOpUpdate;
  friend struct ExprOpConcatLists;
  friend struct ExprVar;
  friend struct ExprString;
  friend struct ExprInt;
  friend struct ExprFloat;
  friend struct ExprPath;
  friend struct ExprSelect;
  friend void prim_get_attr(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v);
  friend void prim_match(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v);
  friend void prim_split(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v);

  friend struct value_t;
  friend class ListBuilder;

public:
  /**
   * Worker threads manager.
   *
   * Note: keep this last to ensure that it's destroyed first, so we
   * don't have any background work items (e.g. from
   * `builtins.parallel`) referring to a partially destroyed
   * `eval_state_t`.
   */
  ref<Executor> executor;
};

struct DebugTraceStacker {
  DebugTraceStacker(eval_state_t& eval_state, DebugTrace t);

  ~DebugTraceStacker() { eval_state.debugTraces.pop_front(); }

  eval_state_t& eval_state;
  DebugTrace trace;
};

/**
 * @return A string representing the type of the value `v`.
 *
 * @param withArticle Whether to begin with an english article, e.g. "an
 * integer" vs "integer".
 */
std::string_view show_type(ValueType type, bool withArticle = true);
std::string show_type(const value_t& v);

/**
 * If `path` refers to a directory, then append "/default.nix".
 *
 * @param add_default_nix Whether to append "/default.nix" after resolving symlinks.
 */
source_path_t resolve_expr_path(source_path_t path, bool add_default_nix = true);

/**
 * Whether a URI is allowed, assuming restrictEval is enabled
 */
bool is_allowed_uri(std::string_view uri, const strings_t& allowed_paths);

} // namespace nix

#include "nix/expr/eval-inline.h"
