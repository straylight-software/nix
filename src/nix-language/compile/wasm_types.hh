#pragma once
///@file nix-language/compile/wasm_types.hh
/// WASM value representation for Nix values.
///
/// Nix values are represented as tagged unions in WASM:
///
///   struct nix_value {
///     i32 tag;      // discriminator
///     i32 payload;  // value or pointer to heap data
///   }
///
/// Tags:
///   0 = null
///   1 = bool (payload: 0 or 1)
///   2 = int (payload: i64 stored inline or pointer to bigint)
///   3 = float (payload: pointer to f64)
///   4 = string (payload: pointer to string data)
///   5 = path (payload: pointer to path data)
///   6 = list (payload: pointer to list header)
///   7 = attrset (payload: pointer to attrset data)
///   8 = lambda (payload: funcref index)
///   9 = thunk (payload: funcref index, lazy evaluation)
///  10 = primop (payload: builtin function index)

#include <cstdint>

namespace nix::language::compile {

/// value type tags for WASM representation
enum class value_tag : std::uint8_t {
  null_value = 0,
  boolean = 1,
  integer = 2,
  floating = 3,
  string = 4,
  path = 5,
  list = 6,
  attribute_set = 7,
  lambda = 8,
  thunk = 9,
  primop = 10,
};

/// WASM primitive types we use
enum class wasm_type : std::uint8_t {
  i32,
  i64,
  f32,
  f64,
  funcref,
  externref,
};

/// memory layout constants
namespace memory {

// nix_value struct layout (8 bytes total)
constexpr std::uint32_t value_size = 8;
constexpr std::uint32_t value_tag_offset = 0;
constexpr std::uint32_t value_payload_offset = 4;

// string layout: length (i32) + data (bytes)
constexpr std::uint32_t string_length_offset = 0;
constexpr std::uint32_t string_data_offset = 4;

// list layout: length (i32) + elements (nix_value*)
constexpr std::uint32_t list_length_offset = 0;
constexpr std::uint32_t list_elements_offset = 4;

// attrset layout: count (i32) + sorted_entries (name_hash, nix_value*)
constexpr std::uint32_t attrset_count_offset = 0;
constexpr std::uint32_t attrset_entries_offset = 4;
constexpr std::uint32_t attrset_entry_size = 12; // hash(4) + value_ptr(4) + name_ptr(4)

// thunk layout: evaluated (i32) + result/funcref (i32)
constexpr std::uint32_t thunk_evaluated_offset = 0;
constexpr std::uint32_t thunk_payload_offset = 4;

// lambda/closure layout:
// - func_index (i32): index into function table
// - capture_count (i32): number of captured variables
// - captures[N] (nix_value each): captured values (8 bytes each)
// total size: 8 + N * 8 bytes
constexpr std::uint32_t closure_func_index_offset = 0;
constexpr std::uint32_t closure_capture_count_offset = 4;
constexpr std::uint32_t closure_captures_offset = 8;
constexpr std::uint32_t closure_capture_size = 8; // each capture is a nix_value (i64)

// legacy names for compatibility
constexpr std::uint32_t closure_funcref_offset = 0;
constexpr std::uint32_t closure_env_offset = 4;

} // namespace memory

/// builtin function indices (imported from host)
namespace builtins {

// pure builtins - can be inlined as WASM
constexpr std::uint32_t add = 0;
constexpr std::uint32_t sub = 1;
constexpr std::uint32_t mul = 2;
constexpr std::uint32_t div = 3;
constexpr std::uint32_t less_than = 4;
constexpr std::uint32_t length = 5;
constexpr std::uint32_t head = 6;
constexpr std::uint32_t tail = 7;
constexpr std::uint32_t concat = 8;
constexpr std::uint32_t elem = 9;
constexpr std::uint32_t type_of = 10;
constexpr std::uint32_t is_null = 11;
constexpr std::uint32_t is_bool = 12;
constexpr std::uint32_t is_int = 13;
constexpr std::uint32_t is_float = 14;
constexpr std::uint32_t is_string = 15;
constexpr std::uint32_t is_path = 16;
constexpr std::uint32_t is_list = 17;
constexpr std::uint32_t is_attrs = 18;
constexpr std::uint32_t is_function = 19;

// impure builtins - must be host imports
constexpr std::uint32_t import_path = 100;
constexpr std::uint32_t read_file = 101;
constexpr std::uint32_t path_exists = 102;
constexpr std::uint32_t fetch_url = 103;
constexpr std::uint32_t to_file = 104;
constexpr std::uint32_t derivation = 105;
constexpr std::uint32_t store_path = 106;

// string builtins
constexpr std::uint32_t string_length = 200;
constexpr std::uint32_t substring = 201;
constexpr std::uint32_t hash_string = 202;
constexpr std::uint32_t match = 203;
constexpr std::uint32_t split = 204;
constexpr std::uint32_t replace_strings = 205;
constexpr std::uint32_t to_lower = 206;
constexpr std::uint32_t to_upper = 207;
constexpr std::uint32_t to_string = 208;
constexpr std::uint32_t concat_strings = 209;

// attrset builtins
constexpr std::uint32_t attr_names = 300;
constexpr std::uint32_t attr_values = 301;
constexpr std::uint32_t get_attr = 302;
constexpr std::uint32_t has_attr = 303;
constexpr std::uint32_t intersect_attrs = 304;
constexpr std::uint32_t remove_attrs = 305;
constexpr std::uint32_t list_to_attrs = 306;

// list builtins
constexpr std::uint32_t map = 400;
constexpr std::uint32_t filter = 401;
constexpr std::uint32_t foldl = 402;
constexpr std::uint32_t sort = 403;
constexpr std::uint32_t gen_list = 404;
constexpr std::uint32_t concat_lists = 405;
constexpr std::uint32_t all = 406;
constexpr std::uint32_t any = 407;
constexpr std::uint32_t concat_map = 408;

// error handling builtins
constexpr std::uint32_t throw_error = 500;
constexpr std::uint32_t abort_eval = 501;
constexpr std::uint32_t try_eval = 502;
constexpr std::uint32_t trace = 503;
constexpr std::uint32_t seq = 504;
constexpr std::uint32_t deep_seq = 505;

// arithmetic builtins (as functions)
constexpr std::uint32_t builtin_add = 600;
constexpr std::uint32_t builtin_sub = 601;
constexpr std::uint32_t builtin_mul = 602;
constexpr std::uint32_t builtin_div = 603;
constexpr std::uint32_t builtin_less_than = 604;

} // namespace builtins

/// helper functions for creating packed nix_value constants
namespace packed {

/// create a packed nix_value: low 32 bits = tag, high 32 bits = payload
constexpr auto make_value(value_tag tag, std::uint32_t payload) -> std::int64_t {
  return (static_cast<std::int64_t>(payload) << 32) | static_cast<std::int64_t>(tag);
}

/// packed null value
constexpr std::int64_t null_value = make_value(value_tag::null_value, 0);

/// packed boolean true
constexpr std::int64_t boolean_true = make_value(value_tag::boolean, 1);

/// packed boolean false
constexpr std::int64_t boolean_false = make_value(value_tag::boolean, 0);

/// packed empty list (count=0)
constexpr std::int64_t empty_list = make_value(value_tag::list, 0);

/// packed empty attribute set (count=0)
constexpr std::int64_t empty_attribute_set = make_value(value_tag::attribute_set, 0);

} // namespace packed

} // namespace nix::language::compile
