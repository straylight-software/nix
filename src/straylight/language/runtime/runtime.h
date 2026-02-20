#pragma once
///@file straylight/language/runtime/runtime.h
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

#include "straylight/language/compile/wasm_types.h"
#include "straylight/language/runtime/memory_layout.h"

namespace straylight::language::runtime {

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
    std::function<nix_value(std::uint32_t func_index, std::uint32_t env_ptr, nix_value arg)>;

/// function type for WASM thunk calls (func_index, env_ptr) -> result (no arg parameter)
using wasm_thunk_func_t = std::function<nix_value(std::uint32_t func_index, std::uint32_t env_ptr)>;

/// the runtime context holding memory and function table
class runtime_context {
public:
  /// linear memory (exported from WASM module)
  std::vector<std::uint8_t> memory;

  /// heap allocator for runtime allocations
  heap_allocator heap;

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

  explicit runtime_context(std::size_t memory_size = mem::DEFAULT_MEMORY_SIZE)
      : memory(memory_size, 0), heap(mem::HEAP_BASE, static_cast<std::uint32_t>(memory_size)) {}

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

  /// read bytes from memory
  [[nodiscard]] auto read_bytes(std::uint32_t offset, std::size_t length) const
      -> std::span<const std::uint8_t> {
    if (offset + length > memory.size()) {
      throw runtime_error("memory access out of bounds");
    }
    return {memory.data() + offset, length};
  }

  /// write bytes to memory
  void write_bytes(std::uint32_t offset, std::span<const std::uint8_t> data) {
    if (offset + data.size() > memory.size()) {
      throw runtime_error("memory write out of bounds");
    }
    std::copy(data.begin(), data.end(), memory.begin() + offset);
  }

  /// read i32 from memory (little-endian)
  [[nodiscard]] auto read_i32(std::uint32_t offset) const -> std::int32_t {
    auto bytes = read_bytes(offset, 4);
    return static_cast<std::int32_t>(bytes[0]) | (static_cast<std::int32_t>(bytes[1]) << 8) |
           (static_cast<std::int32_t>(bytes[2]) << 16) |
           (static_cast<std::int32_t>(bytes[3]) << 24);
  }

  /// read u32 from memory (little-endian)
  [[nodiscard]] auto read_u32(std::uint32_t offset) const -> std::uint32_t {
    return static_cast<std::uint32_t>(read_i32(offset));
  }

  /// read i64 from memory (little-endian)
  [[nodiscard]] auto read_i64(std::uint32_t offset) const -> std::int64_t {
    auto low = static_cast<std::uint64_t>(read_u32(offset));
    auto high = static_cast<std::uint64_t>(read_u32(offset + 4));
    return static_cast<std::int64_t>(low | (high << 32));
  }

  /// write i32 to memory (little-endian)
  void write_i32(std::uint32_t offset, std::int32_t value) {
    std::uint8_t bytes[4] = {
        static_cast<std::uint8_t>(value),
        static_cast<std::uint8_t>(value >> 8),
        static_cast<std::uint8_t>(value >> 16),
        static_cast<std::uint8_t>(value >> 24),
    };
    write_bytes(offset, bytes);
  }

  /// write i64 to memory (little-endian)
  void write_i64(std::uint32_t offset, std::int64_t value) {
    write_i32(offset, static_cast<std::int32_t>(value));
    write_i32(offset + 4, static_cast<std::int32_t>(static_cast<std::uint64_t>(value) >> 32));
  }

  /// read null-terminated string from memory
  [[nodiscard]] auto read_string(std::uint32_t offset) const -> std::string_view {
    std::size_t length = 0;
    while (offset + length < memory.size() && memory[offset + length] != 0) {
      ++length;
    }
    return {reinterpret_cast<const char*>(memory.data() + offset), length};
  }

  /// read nix_value from memory
  [[nodiscard]] auto read_value(std::uint32_t offset) const -> nix_value {
    return read_i64(offset);
  }

  /// write nix_value to memory
  void write_value(std::uint32_t offset, nix_value value) { write_i64(offset, value); }

  /// allocate from heap (convenience wrapper)
  [[nodiscard]] auto allocate(std::uint32_t size) -> std::uint32_t { return heap.allocate(size); }

  /// allocate and write a string, returns offset
  [[nodiscard]] auto alloc_string(std::string_view str) -> std::uint32_t {
    auto len = static_cast<std::uint32_t>(str.size());
    auto ptr = allocate(len + 1);
    for (std::size_t idx = 0; idx < str.size(); ++idx) {
      memory[ptr + idx] = static_cast<std::uint8_t>(str[idx]);
    }
    memory[ptr + str.size()] = 0;
    return ptr;
  }
};

// =============================================================================
// Runtime Functions (imported by WASM)
// =============================================================================

/// force a value (evaluate thunks)
[[nodiscard]] auto rt_force(runtime_context& ctx, nix_value v) -> nix_value;

/// apply a function to an argument
[[nodiscard]] auto rt_apply(runtime_context& ctx, nix_value fn, nix_value arg) -> nix_value;

/// throw an error with position
[[noreturn]] void rt_throw(runtime_context& ctx, std::uint32_t msg_offset, std::uint32_t line,
                           std::uint32_t col);

/// lookup a variable by name
[[nodiscard]] auto rt_lookup_var(runtime_context& ctx, std::uint32_t name_offset) -> nix_value;

/// create a closure
[[nodiscard]] auto rt_make_closure(runtime_context& ctx, std::uint32_t func_index,
                                   std::uint32_t env_offset, std::uint32_t env_size) -> nix_value;

/// create a thunk
[[nodiscard]] auto rt_make_thunk(runtime_context& ctx, std::uint32_t func_index,
                                 std::uint32_t env_offset, std::uint32_t env_size) -> nix_value;

// --- Arithmetic ---

[[nodiscard]] auto rt_add(runtime_context& ctx, nix_value a, nix_value b, std::uint32_t line,
                          std::uint32_t col) -> nix_value;

[[nodiscard]] auto rt_sub(runtime_context& ctx, nix_value a, nix_value b, std::uint32_t line,
                          std::uint32_t col) -> nix_value;

[[nodiscard]] auto rt_mul(runtime_context& ctx, nix_value a, nix_value b, std::uint32_t line,
                          std::uint32_t col) -> nix_value;

[[nodiscard]] auto rt_div(runtime_context& ctx, nix_value a, nix_value b, std::uint32_t line,
                          std::uint32_t col) -> nix_value;

[[nodiscard]] auto rt_negate(runtime_context& ctx, nix_value v) -> nix_value;

// --- Comparison ---

[[nodiscard]] auto rt_less_than(runtime_context& ctx, nix_value a, nix_value b) -> nix_value;
[[nodiscard]] auto rt_less_eq(runtime_context& ctx, nix_value a, nix_value b) -> nix_value;
[[nodiscard]] auto rt_eq(runtime_context& ctx, nix_value a, nix_value b) -> nix_value;
[[nodiscard]] auto rt_neq(runtime_context& ctx, nix_value a, nix_value b) -> nix_value;

// --- Boolean ---

[[nodiscard]] auto rt_not(runtime_context& ctx, nix_value v) -> nix_value;
[[nodiscard]] auto rt_is_bool(runtime_context& ctx, nix_value v) -> std::int32_t;

// --- Collections ---

[[nodiscard]] auto rt_make_list(runtime_context& ctx, std::uint32_t offset, std::uint32_t count)
    -> nix_value;

[[nodiscard]] auto rt_make_attrs(runtime_context& ctx, std::uint32_t offset, std::uint32_t count)
    -> nix_value;

[[nodiscard]] auto rt_make_attrs_dynamic(runtime_context& ctx, std::uint32_t offset,
                                         std::uint32_t count) -> nix_value;

[[nodiscard]] auto rt_select(runtime_context& ctx, nix_value set, std::uint32_t key_offset,
                             std::uint32_t line, std::uint32_t col) -> nix_value;

[[nodiscard]] auto rt_select_dynamic(runtime_context& ctx, nix_value set, nix_value key,
                                     std::uint32_t line, std::uint32_t col) -> nix_value;

[[nodiscard]] auto rt_has_attr(runtime_context& ctx, nix_value set, std::uint32_t key_offset)
    -> nix_value;

[[nodiscard]] auto rt_has_attr_dynamic(runtime_context& ctx, nix_value set, nix_value key)
    -> nix_value;

[[nodiscard]] auto rt_update(runtime_context& ctx, nix_value a, nix_value b) -> nix_value;

[[nodiscard]] auto rt_concat(runtime_context& ctx, nix_value a, nix_value b) -> nix_value;

// --- Strings ---

[[nodiscard]] auto rt_to_string(runtime_context& ctx, nix_value v) -> nix_value;

[[nodiscard]] auto rt_concat_strings(runtime_context& ctx, std::uint32_t offset,
                                     std::uint32_t count) -> nix_value;

// --- Builtins (primops) ---

/// Get the length of a list or string
[[nodiscard]] auto rt_length(runtime_context& ctx, nix_value v) -> nix_value;

/// Get the first element of a list
[[nodiscard]] auto rt_head(runtime_context& ctx, nix_value v) -> nix_value;

/// Get all but the first element of a list
[[nodiscard]] auto rt_tail(runtime_context& ctx, nix_value v) -> nix_value;

/// Get element at index from a list
[[nodiscard]] auto rt_elem_at(runtime_context& ctx, nix_value list, nix_value index) -> nix_value;

/// Check if element is in list
[[nodiscard]] auto rt_elem(runtime_context& ctx, nix_value x, nix_value list) -> nix_value;

/// Get the type of a value as a string
[[nodiscard]] auto rt_type_of(runtime_context& ctx, nix_value v) -> nix_value;

/// Get attribute names from an attrset (returns sorted list)
[[nodiscard]] auto rt_attr_names(runtime_context& ctx, nix_value set) -> nix_value;

/// Get attribute values from an attrset (sorted by key name)
[[nodiscard]] auto rt_attr_values(runtime_context& ctx, nix_value set) -> nix_value;

/// Get string length
[[nodiscard]] auto rt_string_length(runtime_context& ctx, nix_value s) -> nix_value;

/// Apply a primop to arguments (handles currying)
[[nodiscard]] auto rt_apply_primop(runtime_context& ctx, std::uint32_t primop_index, nix_value arg)
    -> nix_value;

/// Initialize the builtins attrset in the runtime context
void rt_init_builtins(runtime_context& ctx);

// --- Type predicates ---

[[nodiscard]] auto rt_is_null(runtime_context& ctx, nix_value v) -> nix_value;
[[nodiscard]] auto rt_is_int(runtime_context& ctx, nix_value v) -> nix_value;
[[nodiscard]] auto rt_is_float(runtime_context& ctx, nix_value v) -> nix_value;
[[nodiscard]] auto rt_is_string(runtime_context& ctx, nix_value v) -> nix_value;
[[nodiscard]] auto rt_is_path(runtime_context& ctx, nix_value v) -> nix_value;
[[nodiscard]] auto rt_is_list(runtime_context& ctx, nix_value v) -> nix_value;
[[nodiscard]] auto rt_is_attrs(runtime_context& ctx, nix_value v) -> nix_value;
[[nodiscard]] auto rt_is_function(runtime_context& ctx, nix_value v) -> nix_value;

// --- Higher-order functions ---

/// Map a function over a list: map f [x1 x2 ...] = [f x1  f x2  ...]
[[nodiscard]] auto rt_map(runtime_context& ctx, nix_value f, nix_value list) -> nix_value;

/// Filter a list by a predicate: filter p [x1 x2 ...] = elements where p xi is true
[[nodiscard]] auto rt_filter(runtime_context& ctx, nix_value pred, nix_value list) -> nix_value;

/// Fold left: foldl' op init [x1 x2 ...] = op (op (op init x1) x2) ...
[[nodiscard]] auto rt_foldl(runtime_context& ctx, nix_value op, nix_value init, nix_value list)
    -> nix_value;

/// Generate a list: genList f n = [f 0  f 1  ... f (n-1)]
[[nodiscard]] auto rt_gen_list(runtime_context& ctx, nix_value f, nix_value n) -> nix_value;

/// Concatenate a list of lists: concatLists [[a b] [c d]] = [a b c d]
[[nodiscard]] auto rt_concat_lists(runtime_context& ctx, nix_value lists) -> nix_value;

/// Sort a list using comparator: sort comparator list
[[nodiscard]] auto rt_sort(runtime_context& ctx, nix_value comparator, nix_value list) -> nix_value;

// --- String Builtins ---

/// Extract substring: substring start len str
[[nodiscard]] auto rt_substring(runtime_context& ctx, nix_value start, nix_value len, nix_value str)
    -> nix_value;

/// Replace strings: replaceStrings from to str
[[nodiscard]] auto rt_replace_strings(runtime_context& ctx, nix_value from, nix_value to,
                                      nix_value str) -> nix_value;

/// Convert to string: toString val
[[nodiscard]] auto rt_to_string(runtime_context& ctx, nix_value v) -> nix_value;

/// Concatenate strings: concatStrings list
[[nodiscard]] auto rt_concat_strings(runtime_context& ctx, nix_value list) -> nix_value;

/// Concatenate strings with separator: concatStringsSep sep list
[[nodiscard]] auto rt_concat_string_sep(runtime_context& ctx, nix_value sep, nix_value list)
    -> nix_value;

// --- List Builtins (additional) ---

/// Check if all elements satisfy predicate: all pred list
[[nodiscard]] auto rt_all(runtime_context& ctx, nix_value pred, nix_value list) -> nix_value;

/// Check if any element satisfies predicate: any pred list
[[nodiscard]] auto rt_any(runtime_context& ctx, nix_value pred, nix_value list) -> nix_value;

/// Map then concat: concatMap f list
[[nodiscard]] auto rt_concat_map(runtime_context& ctx, nix_value f, nix_value list) -> nix_value;

/// Partition list by predicate: partition pred list
[[nodiscard]] auto rt_partition(runtime_context& ctx, nix_value pred, nix_value list) -> nix_value;

/// Group list elements by key function: groupBy f list
[[nodiscard]] auto rt_group_by(runtime_context& ctx, nix_value f, nix_value list) -> nix_value;

/// Reverse a list: reverse list
[[nodiscard]] auto rt_reverse(runtime_context& ctx, nix_value list) -> nix_value;

/// Take first n elements: take n list
[[nodiscard]] auto rt_take(runtime_context& ctx, nix_value n, nix_value list) -> nix_value;

/// Drop first n elements: drop n list
[[nodiscard]] auto rt_drop(runtime_context& ctx, nix_value n, nix_value list) -> nix_value;

/// Generate list from a to b (inclusive): range a b
[[nodiscard]] auto rt_range(runtime_context& ctx, nix_value a, nix_value b) -> nix_value;

/// Zip two lists into list of {fst, snd} attrsets: zipLists list1 list2
[[nodiscard]] auto rt_zip_lists(runtime_context& ctx, nix_value list1, nix_value list2)
    -> nix_value;

/// Convert list of {name, value} to attrset: listToAttrs list
[[nodiscard]] auto rt_list_to_attrs(runtime_context& ctx, nix_value list) -> nix_value;

/// Map function over attrset values: mapAttrs f set
[[nodiscard]] auto rt_map_attrs(runtime_context& ctx, nix_value f, nix_value set) -> nix_value;

/// Extract attribute from list of attrsets: catAttrs name list
[[nodiscard]] auto rt_cat_attrs(runtime_context& ctx, nix_value name, nix_value list) -> nix_value;

/// Intersect attrsets: intersectAttrs a b (returns attrs from b that exist in a)
[[nodiscard]] auto rt_intersect_attrs(runtime_context& ctx, nix_value a, nix_value b) -> nix_value;

/// Get function arguments: functionArgs f
[[nodiscard]] auto rt_function_args(runtime_context& ctx, nix_value f) -> nix_value;

/// Get environment variable: getEnv name
[[nodiscard]] auto rt_get_env(runtime_context& ctx, nix_value name) -> nix_value;

/// Convert to lowercase: toLower str
[[nodiscard]] auto rt_to_lower(runtime_context& ctx, nix_value s) -> nix_value;

/// Convert to uppercase: toUpper str
[[nodiscard]] auto rt_to_upper(runtime_context& ctx, nix_value s) -> nix_value;

/// Compare version strings: compareVersions a b
[[nodiscard]] auto rt_compare_versions(runtime_context& ctx, nix_value a, nix_value b) -> nix_value;

/// Split version string: splitVersion v
[[nodiscard]] auto rt_split_version(runtime_context& ctx, nix_value v) -> nix_value;

/// Parse derivation name: parseDrvName name
[[nodiscard]] auto rt_parse_drv_name(runtime_context& ctx, nix_value s) -> nix_value;

/// Get base name of path: baseNameOf path
[[nodiscard]] auto rt_base_name_of(runtime_context& ctx, nix_value s) -> nix_value;

/// Get directory of path: dirOf path
[[nodiscard]] auto rt_dir_of(runtime_context& ctx, nix_value s) -> nix_value;

/// Check if string has prefix: hasPrefix prefix str
[[nodiscard]] auto rt_has_prefix(runtime_context& ctx, nix_value prefix, nix_value str)
    -> nix_value;

/// Check if string has suffix: hasSuffix suffix str
[[nodiscard]] auto rt_has_suffix(runtime_context& ctx, nix_value suffix, nix_value str)
    -> nix_value;

/// Remove prefix from string: removePrefix prefix str
[[nodiscard]] auto rt_remove_prefix(runtime_context& ctx, nix_value prefix, nix_value str)
    -> nix_value;

/// Remove suffix from string: removeSuffix suffix str
[[nodiscard]] auto rt_remove_suffix(runtime_context& ctx, nix_value suffix, nix_value str)
    -> nix_value;

// --- JSON Builtins ---

/// Convert Nix value to JSON string: toJSON val
[[nodiscard]] auto rt_to_json(runtime_context& ctx, nix_value v) -> nix_value;

/// Parse JSON string to Nix value: fromJSON str
[[nodiscard]] auto rt_from_json(runtime_context& ctx, nix_value s) -> nix_value;

// --- Arithmetic Builtins (as functions) ---

/// Add: add a b
[[nodiscard]] auto rt_builtin_add(runtime_context& ctx, nix_value a, nix_value b) -> nix_value;

/// Subtract: sub a b
[[nodiscard]] auto rt_builtin_sub(runtime_context& ctx, nix_value a, nix_value b) -> nix_value;

/// Multiply: mul a b
[[nodiscard]] auto rt_builtin_mul(runtime_context& ctx, nix_value a, nix_value b) -> nix_value;

/// Divide: div a b
[[nodiscard]] auto rt_builtin_div(runtime_context& ctx, nix_value a, nix_value b) -> nix_value;

/// Less than comparison: lessThan a b
[[nodiscard]] auto rt_builtin_less_than(runtime_context& ctx, nix_value a, nix_value b)
    -> nix_value;

/// Floor: floor x
[[nodiscard]] auto rt_floor(runtime_context& ctx, nix_value v) -> nix_value;

/// Ceil: ceil x
[[nodiscard]] auto rt_ceil(runtime_context& ctx, nix_value v) -> nix_value;

/// Bitwise and: bitAnd a b
[[nodiscard]] auto rt_bit_and(runtime_context& ctx, nix_value a, nix_value b) -> nix_value;

/// Bitwise or: bitOr a b
[[nodiscard]] auto rt_bit_or(runtime_context& ctx, nix_value a, nix_value b) -> nix_value;

/// Bitwise xor: bitXor a b
[[nodiscard]] auto rt_bit_xor(runtime_context& ctx, nix_value a, nix_value b) -> nix_value;

// --- Attrset Builtins ---

/// Check if attrset has attribute: hasAttr name set
[[nodiscard]] auto rt_builtin_has_attr(runtime_context& ctx, nix_value name, nix_value set)
    -> nix_value;

/// Get attribute from attrset: getAttr name set
[[nodiscard]] auto rt_builtin_get_attr(runtime_context& ctx, nix_value name, nix_value set)
    -> nix_value;

/// Remove attributes from attrset: removeAttrs set names
[[nodiscard]] auto rt_remove_attrs(runtime_context& ctx, nix_value set, nix_value names)
    -> nix_value;

// --- Error Handling ---

/// Throw an error with message: throw "message"
/// Returns a sentinel value if inside tryEval (ctx.try_eval_depth > 0)
auto rt_throw_error(runtime_context& ctx, nix_value msg) -> nix_value;

/// Abort evaluation with message: abort "message"
[[noreturn]] auto rt_abort(runtime_context& ctx, nix_value msg) -> nix_value;

/// Try to evaluate, return { success, value }: tryEval expr
[[nodiscard]] auto rt_try_eval(runtime_context& ctx, nix_value expr) -> nix_value;

/// Print trace message and return second arg: trace msg val
[[nodiscard]] auto rt_trace(runtime_context& ctx, nix_value msg, nix_value val) -> nix_value;

/// Force first arg, return second: seq a b
[[nodiscard]] auto rt_seq(runtime_context& ctx, nix_value a, nix_value b) -> nix_value;

/// Deeply force first arg, return second: deepSeq a b
[[nodiscard]] auto rt_deep_seq(runtime_context& ctx, nix_value a, nix_value b) -> nix_value;

// --- Advanced Builtins ---

/// Compute transitive closure: genericClosure { startSet, operator }
[[nodiscard]] auto rt_generic_closure(runtime_context& ctx, nix_value attrs) -> nix_value;

/// Find first element matching predicate: findFirst pred default list
[[nodiscard]] auto rt_find_first(runtime_context& ctx, nix_value pred, nix_value def,
                                 nix_value list) -> nix_value;

/// Hash a string: hashString type str
[[nodiscard]] auto rt_hash_string(runtime_context& ctx, nix_value type, nix_value str) -> nix_value;

/// Match regex: match regex str (returns list or null)
[[nodiscard]] auto rt_match(runtime_context& ctx, nix_value regex, nix_value str) -> nix_value;

/// Split by regex: split regex str (returns list)
[[nodiscard]] auto rt_split(runtime_context& ctx, nix_value regex, nix_value str) -> nix_value;

} // namespace straylight::language::runtime
