/**
 * @file nix.h
 * @brief C API for Nix expression evaluation and store operations.
 *
 * This header exposes the core Nix primitives via a stable C ABI suitable
 * for consumption from Haskell (GHC FFI), Rust (bindgen), and other languages.
 *
 * Design principles:
 * - Opaque handle types with explicit lifecycle (create/free)
 * - Error codes returned as int, detailed messages via nix_error_message()
 * - Strings passed as const char* (NUL-terminated), returned via buffer+length
 * - Collections passed as pointer+count pairs
 * - Thread-local error state for detailed diagnostics
 * - No exceptions across FFI boundary
 *
 * Memory ownership:
 * - Caller owns all handles and must call corresponding *_free() functions
 * - String outputs use caller-provided buffers or internal allocation with free
 * - Callbacks use function pointer + void* user_data pattern
 */

#ifndef NIX_FFI_H
#define NIX_FFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Error Handling
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Error codes returned by most functions. */
typedef enum NixError {
  NIX_OK = 0,
  NIX_ERR_INVALID_ARG = -1,
  NIX_ERR_NULL_POINTER = -2,
  NIX_ERR_PARSE = -3,
  NIX_ERR_EVAL = -4,
  NIX_ERR_TYPE = -5,
  NIX_ERR_STORE = -6,
  NIX_ERR_BUILD = -7,
  NIX_ERR_PATH = -8,
  NIX_ERR_IO = -9,
  NIX_ERR_INTERRUPTED = -10,
  NIX_ERR_OVERFLOW = -11,
  NIX_ERR_NOT_FOUND = -12,
  NIX_ERR_INTERNAL = -99,
} NixError;

/** Get human-readable error name for an error code. */
const char* nix_error_name(NixError error);

/** Get detailed error message from last failed operation (thread-local). */
const char* nix_error_message(void);

/** Clear the thread-local error state. */
void nix_error_clear(void);

/* ═══════════════════════════════════════════════════════════════════════════
 * Opaque Handle Types
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Store handle - connection to a Nix store. */
typedef struct NixStore NixStore;

/** Evaluator state - expression parser and evaluator context. */
typedef struct NixEvalState NixEvalState;

/** Expression AST node. */
typedef struct NixExpr NixExpr;

/** Evaluated value (may be a thunk). */
typedef struct NixValue NixValue;

/** Attribute set bindings. */
typedef struct NixBindings NixBindings;

/** Store path (hash + name). */
typedef struct NixStorePath NixStorePath;

/** Derivation. */
typedef struct NixDerivation NixDerivation;

/** Realised path (for content-addressed derivations). */
typedef struct NixRealisation NixRealisation;

/** Symbol (interned string). */
typedef struct NixSymbol NixSymbol;

/** Position in source (file + line + column). */
typedef struct NixPos NixPos;

/* ═══════════════════════════════════════════════════════════════════════════
 * Value Types
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Value type discriminator. */
typedef enum NixValueType {
  NIX_TYPE_THUNK = 0,     /**< Unevaluated thunk */
  NIX_TYPE_INT = 1,       /**< 64-bit signed integer */
  NIX_TYPE_FLOAT = 2,     /**< 64-bit floating point */
  NIX_TYPE_BOOL = 3,      /**< Boolean */
  NIX_TYPE_STRING = 4,    /**< String (with context) */
  NIX_TYPE_PATH = 5,      /**< Path */
  NIX_TYPE_NULL = 6,      /**< Null */
  NIX_TYPE_ATTRS = 7,     /**< Attribute set */
  NIX_TYPE_LIST = 8,      /**< List */
  NIX_TYPE_FUNCTION = 9,  /**< Lambda or primop */
  NIX_TYPE_EXTERNAL = 10, /**< External value (plugin) */
  NIX_TYPE_FAILED = 11,   /**< Failed thunk (cached error) */
} NixValueType;

/** Get string name of value type. */
const char* nix_value_type_name(NixValueType type);

/* ═══════════════════════════════════════════════════════════════════════════
 * String Buffer for Output
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * String result buffer.
 * For functions that return strings, caller provides this struct.
 * On success, `data` points to the string and `len` is its length.
 * If `owned` is true, caller must call nix_string_free().
 */
typedef struct NixString {
  const char* data; /**< NUL-terminated string data */
  size_t len;       /**< Length excluding NUL */
  bool owned;       /**< If true, caller must free with nix_string_free() */
} NixString;

/** Free a string returned by Nix functions. Only call if owned == true. */
void nix_string_free(NixString* str);

/* ═══════════════════════════════════════════════════════════════════════════
 * Logging and Callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Log levels. */
typedef enum NixLogLevel {
  NIX_LOG_ERROR = 0,
  NIX_LOG_WARN = 1,
  NIX_LOG_NOTICE = 2,
  NIX_LOG_INFO = 3,
  NIX_LOG_TALKATIVE = 4,
  NIX_LOG_CHATTY = 5,
  NIX_LOG_DEBUG = 6,
  NIX_LOG_VOMIT = 7,
} NixLogLevel;

/** Log callback function type. */
typedef void (*NixLogCallback)(NixLogLevel level, const char* msg, void* user_data);

/** Set global log callback. Pass NULL to disable. */
void nix_set_log_callback(NixLogCallback callback, void* user_data);

/** Set minimum log level (default: NIX_LOG_WARN). */
void nix_set_log_level(NixLogLevel level);

/* ═══════════════════════════════════════════════════════════════════════════
 * Initialization
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Initialize the Nix library. Must be called before any other functions.
 * Safe to call multiple times (idempotent).
 * @return NIX_OK on success.
 */
NixError nix_init(void);

/**
 * Get Nix version string.
 * @return Static string, do not free.
 */
const char* nix_version(void);

/* ═══════════════════════════════════════════════════════════════════════════
 * Store Operations
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Open a Nix store.
 * @param uri Store URI (e.g., "auto", "daemon", "/nix/store", "s3://bucket").
 *            Pass NULL or "" for default store.
 * @param[out] store_out Receives the store handle on success.
 * @return NIX_OK on success.
 */
NixError nix_store_open(const char* uri, NixStore** store_out);

/**
 * Close a store and free resources.
 */
void nix_store_free(NixStore* store);

/**
 * Get the store directory (e.g., "/nix/store").
 */
NixError nix_store_get_dir(const NixStore* store, NixString* out);

/**
 * Get the store URI.
 */
NixError nix_store_get_uri(const NixStore* store, NixString* out);

/**
 * Parse a store path string.
 * @param store The store (for validation).
 * @param path Path string (e.g., "/nix/store/abc...-foo").
 * @param[out] path_out Receives the store path handle.
 */
NixError nix_store_parse_path(const NixStore* store, const char* path, NixStorePath** path_out);

/**
 * Free a store path handle.
 */
void nix_store_path_free(NixStorePath* path);

/**
 * Get the full path string of a store path.
 */
NixError nix_store_path_to_string(const NixStore* store, const NixStorePath* path, NixString* out);

/**
 * Get the hash part of a store path (base32).
 */
NixError nix_store_path_hash(const NixStorePath* path, NixString* out);

/**
 * Get the name part of a store path.
 */
NixError nix_store_path_name(const NixStorePath* path, NixString* out);

/**
 * Check if a store path is valid (exists in store).
 */
NixError nix_store_is_valid_path(NixStore* store, const NixStorePath* path, bool* valid_out);

/**
 * Query path info (references, deriver, etc.).
 * Returns NIX_ERR_NOT_FOUND if path doesn't exist.
 *
 * @param store The store.
 * @param path The path to query.
 * @param[out] nar_size_out NAR size in bytes.
 * @param[out] deriver_out Deriver path (caller must free), or NULL if none.
 * @param[out] refs_out Array of reference paths (caller must free array and elements).
 * @param[out] refs_count_out Number of references.
 */
NixError nix_store_query_path_info(NixStore* store, const NixStorePath* path,
                                   uint64_t* nar_size_out, NixStorePath** deriver_out,
                                   NixStorePath*** refs_out, size_t* refs_count_out);

/**
 * Compute the closure of store paths.
 * @param store The store.
 * @param paths Array of starting paths.
 * @param paths_count Number of starting paths.
 * @param[out] closure_out Array of paths in closure.
 * @param[out] closure_count_out Number of paths in closure.
 */
NixError nix_store_compute_closure(NixStore* store, const NixStorePath* const* paths,
                                   size_t paths_count, NixStorePath*** closure_out,
                                   size_t* closure_count_out);

/**
 * Free an array of store paths returned by query functions.
 */
void nix_store_path_array_free(NixStorePath** paths, size_t count);

/**
 * Build store paths (derivations or already-built paths).
 * @param store The store.
 * @param paths Array of path strings (derivations or store paths).
 * @param paths_count Number of paths.
 */
NixError nix_store_build_paths(NixStore* store, const char* const* paths, size_t paths_count);

/**
 * Ensure a path exists (build or substitute as needed).
 */
NixError nix_store_ensure_path(NixStore* store, const NixStorePath* path);

/* ═══════════════════════════════════════════════════════════════════════════
 * Derivation Operations
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Read and parse a derivation from a .drv file.
 */
NixError nix_derivation_read(NixStore* store, const NixStorePath* drv_path,
                             NixDerivation** drv_out);

/**
 * Parse a derivation from ATerm string.
 */
NixError nix_derivation_parse(NixStore* store, const char* drv_content, const char* name,
                              NixDerivation** drv_out);

/**
 * Free a derivation.
 */
void nix_derivation_free(NixDerivation* drv);

/**
 * Get derivation name.
 */
NixError nix_derivation_name(const NixDerivation* drv, NixString* out);

/**
 * Get derivation builder.
 */
NixError nix_derivation_builder(const NixDerivation* drv, NixString* out);

/**
 * Get derivation platform/system.
 */
NixError nix_derivation_system(const NixDerivation* drv, NixString* out);

/**
 * Get derivation arguments.
 */
NixError nix_derivation_args(const NixDerivation* drv, NixString** args_out, size_t* count_out);

/**
 * Get derivation environment variables.
 * Returns parallel arrays of names and values.
 */
NixError nix_derivation_env(const NixDerivation* drv, NixString** names_out, NixString** values_out,
                            size_t* count_out);

/**
 * Get derivation output names.
 */
NixError nix_derivation_outputs(const NixDerivation* drv, NixString** names_out, size_t* count_out);

/**
 * Get output path for a specific output.
 * Returns NIX_ERR_NOT_FOUND if output doesn't exist.
 * Returns NULL path_out for floating CA outputs.
 */
NixError nix_derivation_output_path(NixStore* store, const NixDerivation* drv,
                                    const char* output_name, NixStorePath** path_out);

/**
 * Build a derivation.
 */
NixError nix_derivation_build(NixStore* store, const NixDerivation* drv,
                              NixStorePath*** outputs_out, size_t* outputs_count_out);

/* ═══════════════════════════════════════════════════════════════════════════
 * Evaluator State
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Evaluator configuration.
 */
typedef struct NixEvalConfig {
  /** Lookup path entries (like NIX_PATH). Array of "name=path" strings. */
  const char* const* lookup_path;
  size_t lookup_path_count;

  /** Allow impure operations (builtins.currentTime, etc.). */
  bool allow_impure;

  /** Enable tracing for debugging. */
  bool trace_enabled;
} NixEvalConfig;

/**
 * Create a new evaluator state.
 * @param store The store to use (must outlive the eval state).
 * @param config Configuration (may be NULL for defaults).
 * @param[out] state_out Receives the eval state handle.
 */
NixError nix_eval_state_new(NixStore* store, const NixEvalConfig* config, NixEvalState** state_out);

/**
 * Free an evaluator state.
 */
void nix_eval_state_free(NixEvalState* state);

/* NOTE: nix_eval_state_add_lookup_path was removed because lookup_path is
 * private in eval_state_t after construction. Lookup paths must be provided
 * via NixEvalConfig at eval state creation time. */

/* ═══════════════════════════════════════════════════════════════════════════
 * Expression Parsing
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Parse a Nix expression from a string.
 * @param state The evaluator state.
 * @param expr_str The expression source code.
 * @param base_path Base path for relative imports (or NULL for cwd).
 * @param[out] expr_out Receives the expression AST.
 */
NixError nix_parse_expr_string(NixEvalState* state, const char* expr_str, const char* base_path,
                               NixExpr** expr_out);

/**
 * Parse a Nix expression from a file.
 */
NixError nix_parse_expr_file(NixEvalState* state, const char* file_path, NixExpr** expr_out);

/**
 * Free an expression AST.
 */
void nix_expr_free(NixExpr* expr);

/**
 * Get the source position of an expression.
 */
NixError nix_expr_pos(const NixExpr* expr, NixPos** pos_out);

/**
 * Free a position.
 */
void nix_pos_free(NixPos* pos);

/**
 * Get position details.
 */
NixError nix_pos_info(const NixPos* pos, NixString* file_out, uint32_t* line_out,
                      uint32_t* column_out);

/**
 * Pretty-print an expression.
 */
NixError nix_expr_show(const NixExpr* expr, NixString* out);

/* ═══════════════════════════════════════════════════════════════════════════
 * Expression Evaluation
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Evaluate an expression to a value.
 * The returned value may be a thunk; use nix_value_force() to fully evaluate.
 */
NixError nix_eval(NixEvalState* state, NixExpr* expr, NixValue** value_out);

/**
 * Evaluate a string expression directly.
 */
NixError nix_eval_string(NixEvalState* state, const char* expr_str, const char* base_path,
                         NixValue** value_out);

/**
 * Evaluate a file.
 */
NixError nix_eval_file(NixEvalState* state, const char* file_path, NixValue** value_out);

/**
 * Force a value (evaluate thunks recursively to given depth).
 * @param state The evaluator state.
 * @param value The value to force (may be modified).
 * @param depth How deep to recurse (0 = top-level only, SIZE_MAX = fully).
 */
NixError nix_value_force(NixEvalState* state, NixValue* value, size_t depth);

/**
 * Free a value.
 */
void nix_value_free(NixValue* value);

/* ═══════════════════════════════════════════════════════════════════════════
 * Value Inspection
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Get the type of a value.
 * Note: May return NIX_TYPE_THUNK if not yet forced.
 */
NixValueType nix_value_type(const NixValue* value);

/**
 * Check if value is a thunk (unevaluated).
 */
bool nix_value_is_thunk(const NixValue* value);

/**
 * Get integer value.
 */
NixError nix_value_get_int(const NixValue* value, int64_t* out);

/**
 * Get float value.
 */
NixError nix_value_get_float(const NixValue* value, double* out);

/**
 * Get boolean value.
 */
NixError nix_value_get_bool(const NixValue* value, bool* out);

/**
 * Get string value (without context).
 */
NixError nix_value_get_string(const NixValue* value, NixString* out);

/**
 * Get string context (store paths referenced by string).
 */
NixError nix_value_get_string_context(const NixValue* value, NixString** contexts_out,
                                      size_t* count_out);

/**
 * Get path value.
 */
NixError nix_value_get_path(const NixValue* value, NixString* out);

/**
 * Get list length.
 */
NixError nix_value_get_list_length(const NixValue* value, size_t* out);

/**
 * Get list element by index.
 */
NixError nix_value_get_list_elem(const NixValue* value, size_t index, NixValue** elem_out);

/**
 * Get attribute set size.
 */
NixError nix_value_get_attrs_size(const NixValue* value, size_t* out);

/**
 * Get attribute by name.
 * Returns NIX_ERR_NOT_FOUND if attribute doesn't exist.
 */
NixError nix_value_get_attr(NixEvalState* state, const NixValue* value, const char* name,
                            NixValue** attr_out);

/**
 * Check if attribute exists.
 */
NixError nix_value_has_attr(NixEvalState* state, const NixValue* value, const char* name,
                            bool* exists_out);

/**
 * Get all attribute names.
 */
NixError nix_value_get_attr_names(const NixValue* value, NixString** names_out, size_t* count_out);

/**
 * Iterate over attribute set.
 * Callback is called for each attribute.
 * Return false from callback to stop iteration.
 */
typedef bool (*NixAttrIterCallback)(const char* name, NixValue* value, void* user_data);

NixError nix_value_iter_attrs(const NixValue* value, NixAttrIterCallback callback, void* user_data);

/**
 * Coerce a value to a string.
 * @param state The evaluator state.
 * @param value The value to coerce.
 * @param copy_to_store If true, paths are copied to store.
 * @param[out] out The resulting string.
 */
NixError nix_value_coerce_to_string(NixEvalState* state, NixValue* value, bool copy_to_store,
                                    NixString* out);

/**
 * Coerce a value to a store path.
 */
NixError nix_value_coerce_to_path(NixEvalState* state, NixStore* store, NixValue* value,
                                  NixStorePath** path_out);

/**
 * Coerce a value to a derivation.
 */
NixError nix_value_coerce_to_derivation(NixEvalState* state, NixStore* store, NixValue* value,
                                        NixDerivation** drv_out);

/* ═══════════════════════════════════════════════════════════════════════════
 * Value Construction
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Create an integer value.
 */
NixError nix_value_new_int(int64_t n, NixValue** value_out);

/**
 * Create a float value.
 */
NixError nix_value_new_float(double n, NixValue** value_out);

/**
 * Create a boolean value.
 */
NixError nix_value_new_bool(bool b, NixValue** value_out);

/**
 * Create a string value.
 * @param state Eval state (required for memory allocation).
 * @param s The string content.
 * @param[out] value_out Receives the new value.
 */
NixError nix_value_new_string(NixEvalState* state, const char* s, NixValue** value_out);

/**
 * Create a string value with context.
 * Context strings are in the format used by Nix string context.
 * @param state Eval state (required for memory allocation).
 * @param s The string content.
 * @param context Array of context strings.
 * @param context_count Number of context strings.
 * @param[out] value_out Receives the new value.
 */
NixError nix_value_new_string_with_context(NixEvalState* state, const char* s,
                                           const char* const* context, size_t context_count,
                                           NixValue** value_out);

/**
 * Create a path value.
 * @param state Eval state (required for memory allocation).
 * @param path The filesystem path string.
 * @param[out] value_out Receives the new value.
 */
NixError nix_value_new_path(NixEvalState* state, const char* path, NixValue** value_out);

/**
 * Create a null value.
 */
NixError nix_value_new_null(NixValue** value_out);

/**
 * Create a list value.
 * @param state Eval state (required for memory allocation).
 * @param elems Array of values to include in the list.
 * @param count Number of elements.
 * @param[out] value_out Receives the new value.
 */
NixError nix_value_new_list(NixEvalState* state, NixValue* const* elems, size_t count,
                            NixValue** value_out);

/**
 * Create an attribute set value.
 * @param state Eval state (required for memory allocation).
 * @param names Array of attribute names.
 * @param values Array of attribute values (parallel to names).
 * @param count Number of attributes.
 * @param[out] value_out Receives the new value.
 */
NixError nix_value_new_attrs(NixEvalState* state, const char* const* names, NixValue* const* values,
                             size_t count, NixValue** value_out);


/* ═══════════════════════════════════════════════════════════════════════════
 * Function Application
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Apply a function to an argument.
 */
NixError nix_value_call(NixEvalState* state, NixValue* fn, NixValue* arg, NixValue** result_out);

/**
 * Apply a function to multiple arguments.
 */
NixError nix_value_call_many(NixEvalState* state, NixValue* fn, NixValue* const* args,
                             size_t args_count, NixValue** result_out);

/* ═══════════════════════════════════════════════════════════════════════════
 * Symbols
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Intern a string as a symbol.
 */
NixError nix_symbol_new(NixEvalState* state, const char* name, NixSymbol** sym_out);

/**
 * Get the string representation of a symbol.
 */
NixError nix_symbol_str(const NixSymbol* sym, NixString* out);

/**
 * Free a symbol.
 */
void nix_symbol_free(NixSymbol* sym);

/* ═══════════════════════════════════════════════════════════════════════════
 * Array Cleanup Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Free an array of NixString returned by query functions.
 */
void nix_string_array_free(NixString* strings, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* NIX_FFI_H */
