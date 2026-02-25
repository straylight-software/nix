#pragma once
///@file straylight/nix/compiler/runtime/runtime.h
/// WASM runtime implementation for Nix values.
///
/// This module provides the host functions imported by compiled Nix WASM modules.
/// It manages memory, implements builtins, and handles thunk evaluation.
///
/// See MEMORY.md for detailed memory architecture documentation.

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "straylight/nix/compiler/compile/wasm_types.h"
#include "straylight/nix/compiler/runtime/memory_layout.h"
#include "straylight/nix/compiler/runtime/result.h"
#include "straylight/nix/compiler/runtime/wasm_memory.h"

namespace straylight::nix::compiler::runtime {

// Forward declarations
class io_backend_interface;

using compile::value_tag;
namespace mem = memory_layout;

// =============================================================================
// Error Types
// =============================================================================

/// runtime error with source position
class runtime_error : public std::runtime_error {
public:
  std::uint32_t line;
  std::uint32_t column;

  runtime_error(std::string_view message, std::uint32_t line, std::uint32_t column)
      : std::runtime_error(format_error(message, line, column)), line(line), column(column) {}

  explicit runtime_error(std::string_view message)
      : std::runtime_error(std::string(message)), line(0), column(0) {}

private:
  static auto format_error(std::string_view message, std::uint32_t line, std::uint32_t column)
      -> std::string {
    if (line == 0 && column == 0) {
      return std::string(message);
    }
    return std::string(message) + " at line " + std::to_string(line) + ", column " +
           std::to_string(column);
  }
};

/// type error (e.g., adding string to int)
class type_error : public runtime_error {
  using runtime_error::runtime_error;
};

/// attribute not found error
class attr_error : public runtime_error {
  using runtime_error::runtime_error;
};

/// assertion failure
class assertion_error : public runtime_error {
  using runtime_error::runtime_error;
};

/// out of memory error
class oom_error : public runtime_error {
  using runtime_error::runtime_error;
};

// =============================================================================
// Value Representation
// =============================================================================

/// packed nix_value: i64 with tag in low 32 bits, payload in high 32 bits
using nix_value = std::int64_t;

/// extract tag from packed value
[[nodiscard]] constexpr auto get_tag(nix_value v) noexcept -> value_tag {
  return static_cast<value_tag>(v & 0xFFFFFFFF);
}

/// extract payload from packed value
[[nodiscard]] constexpr auto get_payload(nix_value v) noexcept -> std::uint32_t {
  return static_cast<std::uint32_t>(static_cast<std::uint64_t>(v) >> 32);
}

/// create packed value from tag and payload
[[nodiscard]] constexpr auto make_value(value_tag tag, std::uint32_t payload) noexcept
    -> nix_value {
  return (static_cast<std::int64_t>(payload) << 32) | static_cast<std::int64_t>(tag);
}

/// well-known constants
namespace constants {
constexpr nix_value null_value = make_value(value_tag::null_value, 0);
constexpr nix_value bool_true = make_value(value_tag::boolean, 1);
constexpr nix_value bool_false = make_value(value_tag::boolean, 0);
} // namespace constants

/// Result type for runtime operations returning nix_value
using rt_result = rt_result_t<nix_value>;

/// check if value is null
[[nodiscard]] constexpr auto is_null(nix_value v) noexcept -> bool {
  return get_tag(v) == value_tag::null_value;
}

/// check if value is a boolean
[[nodiscard]] constexpr auto is_bool(nix_value v) noexcept -> bool {
  return get_tag(v) == value_tag::boolean;
}

/// check if value is an integer
[[nodiscard]] constexpr auto is_int(nix_value v) noexcept -> bool {
  return get_tag(v) == value_tag::integer;
}

/// check if value is a float
[[nodiscard]] constexpr auto is_float(nix_value v) noexcept -> bool {
  return get_tag(v) == value_tag::floating;
}

/// check if value is numeric (int or float)
[[nodiscard]] constexpr auto is_numeric(nix_value v) noexcept -> bool {
  return is_int(v) || is_float(v);
}

/// check if value is a string
[[nodiscard]] constexpr auto is_string(nix_value v) noexcept -> bool {
  return get_tag(v) == value_tag::string;
}

/// check if value is a path
[[nodiscard]] constexpr auto is_path(nix_value v) noexcept -> bool {
  return get_tag(v) == value_tag::path;
}

/// check if value is a list
[[nodiscard]] constexpr auto is_list(nix_value v) noexcept -> bool {
  return get_tag(v) == value_tag::list;
}

/// check if value is an attribute set
[[nodiscard]] constexpr auto is_attrset(nix_value v) noexcept -> bool {
  return get_tag(v) == value_tag::attribute_set;
}

/// check if value is a lambda/closure
[[nodiscard]] constexpr auto is_lambda(nix_value v) noexcept -> bool {
  return get_tag(v) == value_tag::lambda;
}

/// check if value is a thunk
[[nodiscard]] constexpr auto is_thunk(nix_value v) noexcept -> bool {
  return get_tag(v) == value_tag::thunk;
}

/// check if value is a primop
[[nodiscard]] constexpr auto is_primop(nix_value v) noexcept -> bool {
  return get_tag(v) == value_tag::primop;
}

/// check if value is callable (lambda, thunk that evaluates to lambda, or primop)
[[nodiscard]] constexpr auto is_callable(nix_value v) noexcept -> bool {
  return is_lambda(v) || is_thunk(v) || is_primop(v);
}

/// get type name for error messages
[[nodiscard]] inline auto type_name(nix_value v) -> std::string_view {
  switch (get_tag(v)) {
    case value_tag::null_value:
      return "null";
    case value_tag::boolean:
      return "bool";
    case value_tag::integer:
      return "int";
    case value_tag::floating:
      return "float";
    case value_tag::string:
      return "string";
    case value_tag::path:
      return "path";
    case value_tag::list:
      return "list";
    case value_tag::attribute_set:
      return "set";
    case value_tag::lambda:
      return "lambda";
    case value_tag::thunk:
      return "thunk";
    case value_tag::primop:
      return "primop";
    default:
      return "unknown";
  }
}

// =============================================================================
// Heap Allocator
// =============================================================================

/// simple bump allocator for runtime heap
class heap_allocator {
public:
  explicit heap_allocator(std::uint32_t base = mem::HEAP_BASE,
                          std::uint32_t limit = mem::DEFAULT_MEMORY_SIZE)
      : next_free_(base), heap_base_(base), heap_limit_(limit) {}

  /// allocate bytes, returns offset in memory
  [[nodiscard]] auto allocate(std::uint32_t size) -> std::uint32_t {
    size = mem::align_up(size);
    if (next_free_ + size > heap_limit_) {
      throw oom_error("runtime heap exhausted");
    }
    auto ptr = next_free_;
    next_free_ += size;
    return ptr;
  }

  /// reset allocator (for arena-style GC)
  void reset() { next_free_ = heap_base_; }

  /// get current allocation high-water mark
  [[nodiscard]] auto bytes_allocated() const noexcept -> std::uint32_t {
    return next_free_ - heap_base_;
  }

  /// get remaining space
  [[nodiscard]] auto bytes_remaining() const noexcept -> std::uint32_t {
    return heap_limit_ - next_free_;
  }

private:
  std::uint32_t next_free_;
  std::uint32_t heap_base_;
  std::uint32_t heap_limit_;
};

// =============================================================================
// Memory Context
// =============================================================================

/// function type for WASM lambda calls (func_index, env_ptr, arg) -> result
using wasm_func_t =
    std::function<rt_result(std::uint32_t func_index, std::uint32_t env_ptr, nix_value arg)>;

/// function type for WASM thunk calls (func_index, env_ptr) -> result (no arg parameter)
using wasm_thunk_func_t = std::function<rt_result(std::uint32_t func_index, std::uint32_t env_ptr)>;

/// the runtime context holding memory and function table
/// All memory access goes through wasm_memory* - no separate buffer, no syncing.
struct runtime_context {
  /// Direct access to WASM linear memory. Set by wasm_executor before use.
  /// All read/write operations go through this - single source of truth.
  wasm_memory* mem = nullptr;

  /// I/O backend for impure operations (import, readFile, etc.)
  /// May be nullptr if running in pure evaluation mode.
  io_backend_interface* io = nullptr;

  /// function to call lambda functions in WASM module (env_ptr, arg) -> result
  wasm_func_t call_wasm_func;

  /// function to call thunk functions in WASM module (env_ptr) -> result
  wasm_thunk_func_t call_wasm_thunk;

  /// lambda count from WASM module - used to compute thunk table indices
  /// thunks are stored at [lambda_count, lambda_count + thunk_count) in the function table
  std::uint32_t lambda_count = 0;

  /// Current module ID for cross-module lambda support.
  /// When creating closures/thunks, this ID is encoded in the func_index.
  /// Format: func_index = (module_id << 16) | local_func_index
  std::uint16_t current_module_id = 0;

  /// builtin variables (e.g., builtins.true, builtins.null)
  std::unordered_map<std::string, nix_value> builtins;

  /// error state (set instead of throwing to support wasmtime)
  bool has_error = false;
  std::string error_message;
  std::uint32_t error_line = 0;
  std::uint32_t error_column = 0;

  /// try_eval depth counter - when > 0, throw errors set error state instead of throwing
  /// this allows tryEval to catch errors without relying on C++ exceptions through WASM
  std::uint32_t try_eval_depth = 0;

  /// flag indicating an error was caught by tryEval
  bool try_eval_caught_error = false;

  runtime_context() = default;

  /// set error state (use instead of throwing when inside wasmtime callbacks)
  void set_error(std::string_view msg, std::uint32_t line = 0, std::uint32_t col = 0) {
    has_error = true;
    error_message = msg;
    error_line = line;
    error_column = col;
  }

  /// clear error state
  void clear_error() {
    has_error = false;
    error_message.clear();
    error_line = 0;
    error_column = 0;
  }

  // All memory operations delegate to wasm_memory
  // These are inline convenience wrappers

  [[nodiscard]] auto read_i32(std::uint32_t offset) const -> std::int32_t;
  [[nodiscard]] auto read_u32(std::uint32_t offset) const -> std::uint32_t;
  [[nodiscard]] auto read_i64(std::uint32_t offset) const -> std::int64_t;
  [[nodiscard]] auto read_string(std::uint32_t offset) const -> std::string_view;
  [[nodiscard]] auto read_value(std::uint32_t offset) const -> nix_value;
  [[nodiscard]] auto read_f64(std::uint32_t offset) const -> double;

  void write_i32(std::uint32_t offset, std::int32_t value);
  void write_i64(std::uint32_t offset, std::int64_t value);
  void write_value(std::uint32_t offset, nix_value value);
  void write_f64(std::uint32_t offset, double value);
  void write_byte(std::uint32_t offset, std::uint8_t value);

  [[nodiscard]] auto allocate(std::uint32_t size) -> std::uint32_t;
  [[nodiscard]] auto alloc_string(std::string_view str) -> std::uint32_t;
};

// =============================================================================
// Runtime Functions (imported by WASM)
// =============================================================================

/// force a value (evaluate thunks)
[[nodiscard]] auto rt_force(runtime_context& ctx, nix_value v) -> rt_result;

/// Reify a value by copying data segment strings to the heap.
/// This is needed when returning values from imported modules, since each
/// module's data segment initialization overwrites the previous one.
/// After reification, all string pointers (including attrset keys) point
/// to heap memory which survives module reloads.
[[nodiscard]] auto rt_reify_value(runtime_context& ctx, nix_value v) -> rt_result;

/// apply a function to an argument
[[nodiscard]] auto rt_apply(runtime_context& ctx, nix_value fn, nix_value arg) -> rt_result;

/// throw an error with position
[[noreturn]] void rt_throw(runtime_context& ctx, std::uint32_t msg_offset, std::uint32_t line,
                           std::uint32_t col);

/// lookup a variable by name
[[nodiscard]] auto rt_lookup_var(runtime_context& ctx, std::uint32_t name_offset) -> rt_result;

/// create a closure
[[nodiscard]] auto rt_make_closure(runtime_context& ctx, std::uint32_t func_index,
                                   std::uint32_t env_offset, std::uint32_t env_size) -> rt_result;

/// create a thunk
[[nodiscard]] auto rt_make_thunk(runtime_context& ctx, std::uint32_t func_index,
                                 std::uint32_t env_offset, std::uint32_t env_size) -> rt_result;

// --- Arithmetic ---

[[nodiscard]] auto rt_add(runtime_context& ctx, nix_value a, nix_value b, std::uint32_t line,
                          std::uint32_t col) -> rt_result;

[[nodiscard]] auto rt_sub(runtime_context& ctx, nix_value a, nix_value b, std::uint32_t line,
                          std::uint32_t col) -> rt_result;

[[nodiscard]] auto rt_mul(runtime_context& ctx, nix_value a, nix_value b, std::uint32_t line,
                          std::uint32_t col) -> rt_result;

[[nodiscard]] auto rt_div(runtime_context& ctx, nix_value a, nix_value b, std::uint32_t line,
                          std::uint32_t col) -> rt_result;

[[nodiscard]] auto rt_negate(runtime_context& ctx, nix_value v) -> rt_result;

// --- Comparison ---

[[nodiscard]] auto rt_less_than(runtime_context& ctx, nix_value a, nix_value b) -> rt_result;
[[nodiscard]] auto rt_less_eq(runtime_context& ctx, nix_value a, nix_value b) -> rt_result;
[[nodiscard]] auto rt_eq(runtime_context& ctx, nix_value a, nix_value b) -> rt_result;
[[nodiscard]] auto rt_neq(runtime_context& ctx, nix_value a, nix_value b) -> rt_result;

// --- Boolean ---

[[nodiscard]] auto rt_not(runtime_context& ctx, nix_value v) -> rt_result;
[[nodiscard]] auto rt_is_bool(runtime_context& ctx, nix_value v) -> rt_result;

/// Force and type-check a value to be boolean. Throws type_error if not.
/// Used for if conditions and assert conditions.
[[nodiscard]] auto rt_expect_bool(runtime_context& ctx, nix_value v, std::uint32_t line,
                                  std::uint32_t col) -> rt_result;

// --- Collections ---

[[nodiscard]] auto rt_make_list(runtime_context& ctx, std::uint32_t offset, std::uint32_t count)
    -> rt_result;

[[nodiscard]] auto rt_make_attrs(runtime_context& ctx, std::uint32_t offset, std::uint32_t count)
    -> rt_result;

[[nodiscard]] auto rt_make_attrs_dynamic(runtime_context& ctx, std::uint32_t offset,
                                         std::uint32_t count) -> rt_result;

[[nodiscard]] auto rt_select(runtime_context& ctx, nix_value set, std::uint32_t key_offset,
                             std::uint32_t line, std::uint32_t col) -> rt_result;

[[nodiscard]] auto rt_select_dynamic(runtime_context& ctx, nix_value set, nix_value key,
                                     std::uint32_t line, std::uint32_t col) -> rt_result;

[[nodiscard]] auto rt_has_attr(runtime_context& ctx, nix_value set, std::uint32_t key_offset)
    -> rt_result;

[[nodiscard]] auto rt_has_attr_dynamic(runtime_context& ctx, nix_value set, nix_value key)
    -> rt_result;

[[nodiscard]] auto rt_update(runtime_context& ctx, nix_value a, nix_value b) -> rt_result;

[[nodiscard]] auto rt_concat(runtime_context& ctx, nix_value a, nix_value b) -> rt_result;

// --- Strings ---

[[nodiscard]] auto rt_to_string(runtime_context& ctx, nix_value v) -> rt_result;

[[nodiscard]] auto rt_concat_strings(runtime_context& ctx, std::uint32_t offset,
                                     std::uint32_t count) -> rt_result;

// --- Builtins (primops) ---

/// Get the length of a list or string
[[nodiscard]] auto rt_length(runtime_context& ctx, nix_value v) -> rt_result;

/// Get the first element of a list
[[nodiscard]] auto rt_head(runtime_context& ctx, nix_value v) -> rt_result;

/// Get all but the first element of a list
[[nodiscard]] auto rt_tail(runtime_context& ctx, nix_value v) -> rt_result;

/// Get element at index from a list
[[nodiscard]] auto rt_elem_at(runtime_context& ctx, nix_value list, nix_value index) -> rt_result;

/// Check if element is in list
[[nodiscard]] auto rt_elem(runtime_context& ctx, nix_value x, nix_value list) -> rt_result;

/// Get the type of a value as a string
[[nodiscard]] auto rt_type_of(runtime_context& ctx, nix_value v) -> rt_result;

/// Get attribute names from an attrset (returns sorted list)
[[nodiscard]] auto rt_attr_names(runtime_context& ctx, nix_value set) -> rt_result;

/// Get attribute values from an attrset (sorted by key name)
[[nodiscard]] auto rt_attr_values(runtime_context& ctx, nix_value set) -> rt_result;

/// Get string length
[[nodiscard]] auto rt_string_length(runtime_context& ctx, nix_value s) -> rt_result;

/// Apply a primop to arguments (handles currying)
[[nodiscard]] auto rt_apply_primop(runtime_context& ctx, std::uint32_t primop_index, nix_value arg)
    -> rt_result;

/// Initialize the builtins attrset in the runtime context
void rt_init_builtins(runtime_context& ctx);

// --- Type predicates ---

[[nodiscard]] auto rt_is_null(runtime_context& ctx, nix_value v) -> rt_result;
[[nodiscard]] auto rt_is_int(runtime_context& ctx, nix_value v) -> rt_result;
[[nodiscard]] auto rt_is_float(runtime_context& ctx, nix_value v) -> rt_result;
[[nodiscard]] auto rt_is_string(runtime_context& ctx, nix_value v) -> rt_result;
[[nodiscard]] auto rt_is_path(runtime_context& ctx, nix_value v) -> rt_result;
[[nodiscard]] auto rt_is_list(runtime_context& ctx, nix_value v) -> rt_result;
[[nodiscard]] auto rt_is_attrs(runtime_context& ctx, nix_value v) -> rt_result;
[[nodiscard]] auto rt_is_function(runtime_context& ctx, nix_value v) -> rt_result;

// --- Higher-order functions ---

/// Map a function over a list: map f [x1 x2 ...] = [f x1  f x2  ...]
[[nodiscard]] auto rt_map(runtime_context& ctx, nix_value f, nix_value list) -> rt_result;

/// Filter a list by a predicate: filter p [x1 x2 ...] = elements where p xi is true
[[nodiscard]] auto rt_filter(runtime_context& ctx, nix_value pred, nix_value list) -> rt_result;

/// Fold left: foldl' op init [x1 x2 ...] = op (op (op init x1) x2) ...
[[nodiscard]] auto rt_foldl(runtime_context& ctx, nix_value op, nix_value init, nix_value list)
    -> rt_result;

/// Generate a list: genList f n = [f 0  f 1  ... f (n-1)]
[[nodiscard]] auto rt_gen_list(runtime_context& ctx, nix_value f, nix_value n) -> rt_result;

/// Concatenate a list of lists: concatLists [[a b] [c d]] = [a b c d]
[[nodiscard]] auto rt_concat_lists(runtime_context& ctx, nix_value lists) -> rt_result;

/// Sort a list using comparator: sort comparator list
[[nodiscard]] auto rt_sort(runtime_context& ctx, nix_value comparator, nix_value list) -> rt_result;

// --- String Builtins ---

/// Extract substring: substring start len str
[[nodiscard]] auto rt_substring(runtime_context& ctx, nix_value start, nix_value len, nix_value str)
    -> rt_result;

/// Replace strings: replaceStrings from to str
[[nodiscard]] auto rt_replace_strings(runtime_context& ctx, nix_value from, nix_value to,
                                      nix_value str) -> rt_result;

/// Convert to string: toString val
[[nodiscard]] auto rt_to_string(runtime_context& ctx, nix_value v) -> rt_result;

/// Concatenate strings: concatStrings list
[[nodiscard]] auto rt_concat_strings(runtime_context& ctx, nix_value list) -> rt_result;

/// Concatenate strings with separator: concatStringsSep sep list
[[nodiscard]] auto rt_concat_string_sep(runtime_context& ctx, nix_value sep, nix_value list)
    -> rt_result;

// --- List Builtins (additional) ---

/// Check if all elements satisfy predicate: all pred list
[[nodiscard]] auto rt_all(runtime_context& ctx, nix_value pred, nix_value list) -> rt_result;

/// Check if any element satisfies predicate: any pred list
[[nodiscard]] auto rt_any(runtime_context& ctx, nix_value pred, nix_value list) -> rt_result;

/// Map then concat: concatMap f list
[[nodiscard]] auto rt_concat_map(runtime_context& ctx, nix_value f, nix_value list) -> rt_result;

/// Partition list by predicate: partition pred list
[[nodiscard]] auto rt_partition(runtime_context& ctx, nix_value pred, nix_value list) -> rt_result;

/// Group list elements by key function: groupBy f list
[[nodiscard]] auto rt_group_by(runtime_context& ctx, nix_value f, nix_value list) -> rt_result;

/// Reverse a list: reverse list
[[nodiscard]] auto rt_reverse(runtime_context& ctx, nix_value list) -> rt_result;

/// Take first n elements: take n list
[[nodiscard]] auto rt_take(runtime_context& ctx, nix_value n, nix_value list) -> rt_result;

/// Drop first n elements: drop n list
[[nodiscard]] auto rt_drop(runtime_context& ctx, nix_value n, nix_value list) -> rt_result;

/// Generate list from a to b (inclusive): range a b
[[nodiscard]] auto rt_range(runtime_context& ctx, nix_value a, nix_value b) -> rt_result;

/// Zip two lists into list of {fst, snd} attrsets: zipLists list1 list2
[[nodiscard]] auto rt_zip_lists(runtime_context& ctx, nix_value list1, nix_value list2)
    -> rt_result;

/// Convert list of {name, value} to attrset: listToAttrs list
[[nodiscard]] auto rt_list_to_attrs(runtime_context& ctx, nix_value list) -> rt_result;

/// Map function over attrset values: mapAttrs f set
[[nodiscard]] auto rt_map_attrs(runtime_context& ctx, nix_value f, nix_value set) -> rt_result;

/// Extract attribute from list of attrsets: catAttrs name list
[[nodiscard]] auto rt_cat_attrs(runtime_context& ctx, nix_value name, nix_value list) -> rt_result;

/// Intersect attrsets: intersectAttrs a b (returns attrs from b that exist in a)
[[nodiscard]] auto rt_intersect_attrs(runtime_context& ctx, nix_value a, nix_value b) -> rt_result;

/// Get function arguments: functionArgs f
[[nodiscard]] auto rt_function_args(runtime_context& ctx, nix_value f) -> rt_result;

/// Get environment variable: getEnv name
[[nodiscard]] auto rt_get_env(runtime_context& ctx, nix_value name) -> rt_result;

/// Convert to lowercase: toLower str
[[nodiscard]] auto rt_to_lower(runtime_context& ctx, nix_value s) -> rt_result;

/// Convert to uppercase: toUpper str
[[nodiscard]] auto rt_to_upper(runtime_context& ctx, nix_value s) -> rt_result;

/// Compare version strings: compareVersions a b
[[nodiscard]] auto rt_compare_versions(runtime_context& ctx, nix_value a, nix_value b) -> rt_result;

/// Split version string: splitVersion v
[[nodiscard]] auto rt_split_version(runtime_context& ctx, nix_value v) -> rt_result;

/// Parse derivation name: parseDrvName name
[[nodiscard]] auto rt_parse_drv_name(runtime_context& ctx, nix_value s) -> rt_result;

/// Get base name of path: baseNameOf path
[[nodiscard]] auto rt_base_name_of(runtime_context& ctx, nix_value s) -> rt_result;

/// Get directory of path: dirOf path
[[nodiscard]] auto rt_dir_of(runtime_context& ctx, nix_value s) -> rt_result;

/// Check if string has prefix: hasPrefix prefix str
[[nodiscard]] auto rt_has_prefix(runtime_context& ctx, nix_value prefix, nix_value str)
    -> rt_result;

/// Check if string has suffix: hasSuffix suffix str
[[nodiscard]] auto rt_has_suffix(runtime_context& ctx, nix_value suffix, nix_value str)
    -> rt_result;

/// Remove prefix from string: removePrefix prefix str
[[nodiscard]] auto rt_remove_prefix(runtime_context& ctx, nix_value prefix, nix_value str)
    -> rt_result;

/// Remove suffix from string: removeSuffix suffix str
[[nodiscard]] auto rt_remove_suffix(runtime_context& ctx, nix_value suffix, nix_value str)
    -> rt_result;

// --- JSON Builtins ---

/// Convert Nix value to JSON string: toJSON val
[[nodiscard]] auto rt_to_json(runtime_context& ctx, nix_value v) -> rt_result;

/// Parse JSON string to Nix value: fromJSON str
[[nodiscard]] auto rt_from_json(runtime_context& ctx, nix_value s) -> rt_result;

// --- Arithmetic Builtins (as functions) ---

/// Add: add a b
[[nodiscard]] auto rt_builtin_add(runtime_context& ctx, nix_value a, nix_value b) -> rt_result;

/// Subtract: sub a b
[[nodiscard]] auto rt_builtin_sub(runtime_context& ctx, nix_value a, nix_value b) -> rt_result;

/// Multiply: mul a b
[[nodiscard]] auto rt_builtin_mul(runtime_context& ctx, nix_value a, nix_value b) -> rt_result;

/// Divide: div a b
[[nodiscard]] auto rt_builtin_div(runtime_context& ctx, nix_value a, nix_value b) -> rt_result;

/// Less than comparison: lessThan a b
[[nodiscard]] auto rt_builtin_less_than(runtime_context& ctx, nix_value a, nix_value b)
    -> rt_result;

/// Floor: floor x
[[nodiscard]] auto rt_floor(runtime_context& ctx, nix_value v) -> rt_result;

/// Ceil: ceil x
[[nodiscard]] auto rt_ceil(runtime_context& ctx, nix_value v) -> rt_result;

/// Bitwise and: bitAnd a b
[[nodiscard]] auto rt_bit_and(runtime_context& ctx, nix_value a, nix_value b) -> rt_result;

/// Bitwise or: bitOr a b
[[nodiscard]] auto rt_bit_or(runtime_context& ctx, nix_value a, nix_value b) -> rt_result;

/// Bitwise xor: bitXor a b
[[nodiscard]] auto rt_bit_xor(runtime_context& ctx, nix_value a, nix_value b) -> rt_result;

// --- Attrset Builtins ---

/// Check if attrset has attribute: hasAttr name set
[[nodiscard]] auto rt_builtin_has_attr(runtime_context& ctx, nix_value name, nix_value set)
    -> rt_result;

/// Get attribute from attrset: getAttr name set
[[nodiscard]] auto rt_builtin_get_attr(runtime_context& ctx, nix_value name, nix_value set)
    -> rt_result;

/// Remove attributes from attrset: removeAttrs set names
[[nodiscard]] auto rt_remove_attrs(runtime_context& ctx, nix_value set, nix_value names)
    -> rt_result;

// --- Error Handling ---

/// Throw an error with message: throw "message"
/// Returns rt_error_t with kind throw_error (catchable by tryEval)
[[nodiscard]] auto rt_throw_error(runtime_context& ctx, nix_value msg) -> rt_result;

/// Abort evaluation with message: abort "message"
[[nodiscard]] auto rt_abort(runtime_context& ctx, nix_value msg) -> rt_result;

/// Try to evaluate, return { success, value }: tryEval expr
[[nodiscard]] auto rt_try_eval(runtime_context& ctx, nix_value expr) -> rt_result;

/// Print trace message and return second arg: trace msg val
[[nodiscard]] auto rt_trace(runtime_context& ctx, nix_value msg, nix_value val) -> rt_result;

/// Force first arg, return second: seq a b
[[nodiscard]] auto rt_seq(runtime_context& ctx, nix_value a, nix_value b) -> rt_result;

/// Deeply force first arg, return second: deepSeq a b
[[nodiscard]] auto rt_deep_seq(runtime_context& ctx, nix_value a, nix_value b) -> rt_result;

// --- Advanced Builtins ---

/// Compute transitive closure: genericClosure { startSet, operator }
[[nodiscard]] auto rt_generic_closure(runtime_context& ctx, nix_value attrs) -> rt_result;

/// Find first element matching predicate: findFirst pred default list
[[nodiscard]] auto rt_find_first(runtime_context& ctx, nix_value pred, nix_value def,
                                 nix_value list) -> rt_result;

/// Hash a string: hashString type str
[[nodiscard]] auto rt_hash_string(runtime_context& ctx, nix_value type, nix_value str) -> rt_result;

/// Match regex: match regex str (returns list or null)
[[nodiscard]] auto rt_match(runtime_context& ctx, nix_value regex, nix_value str) -> rt_result;

/// Split by regex: split regex str (returns list)
[[nodiscard]] auto rt_split(runtime_context& ctx, nix_value regex, nix_value str) -> rt_result;

} // namespace straylight::nix::compiler::runtime
