///@file nix-language/runtime/runtime.cpp
/// WASM runtime implementation for Nix values.

#include "nix-language/runtime/runtime.hh"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <regex>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace nix::language::runtime {

// =============================================================================
// Core Runtime
// =============================================================================

auto rt_force(runtime_context& ctx, nix_value v) -> nix_value {
  // repeatedly force until we get a non-thunk value
  while (is_thunk(v)) {
    auto thunk_ptr = get_payload(v);

    // check state
    auto state = ctx.read_i32(thunk_ptr + mem::THUNK_STATE_OFFSET);

    if (state == mem::THUNK_STATE_EVALUATED) {
      // already evaluated, return cached value
      v = ctx.read_value(thunk_ptr + mem::THUNK_CACHED_VALUE_OFFSET);
      continue;
    }

    if (state == mem::THUNK_STATE_EVALUATING) {
      // infinite recursion detected
      throw runtime_error("infinite recursion detected while evaluating thunk");
    }

    // mark as in progress
    ctx.write_i32(thunk_ptr + mem::THUNK_STATE_OFFSET, mem::THUNK_STATE_EVALUATING);

    // get function and environment
    auto thunk_func_index = ctx.read_u32(thunk_ptr + mem::THUNK_FUNC_INDEX_OFFSET);
    auto env_ptr = ctx.read_u32(thunk_ptr + mem::THUNK_ENV_PTR_OFFSET);

    // thunks store relative indices (0, 1, 2...) but are at [lambda_count, ...) in the table
    auto table_index = thunk_func_index + ctx.lambda_count;

    // call the thunk function (thunks take only env_ptr and return nix_value)
    auto result = ctx.call_wasm_thunk(table_index, env_ptr);

    // cache the result
    ctx.write_value(thunk_ptr + mem::THUNK_CACHED_VALUE_OFFSET, result);
    ctx.write_i32(thunk_ptr + mem::THUNK_STATE_OFFSET, mem::THUNK_STATE_EVALUATED);

    v = result;
  }

  return v;
}

// Forward declarations for partial primop application
static auto rt_apply_partial_primop(runtime_context& ctx, std::uint32_t partial_ptr, nix_value arg)
    -> nix_value;
static auto rt_apply_partial_primop_3arg(runtime_context& ctx, std::uint32_t partial_ptr,
                                         nix_value arg) -> nix_value;

auto rt_apply(runtime_context& ctx, nix_value fn, nix_value arg) -> nix_value {
  // force the function in case it's a thunk
  fn = rt_force(ctx, fn);

  // Handle primop (builtin function)
  if (is_primop(fn)) {
    auto payload = get_payload(fn);
    // Check if this is a partial application
    // 0xC0000000 = two args stored (for 3-arg primops)
    // 0x80000000 = one arg stored (for 2+ arg primops)
    if ((payload & 0xC0000000) == 0xC0000000) {
      // Partial primop with 2 args - apply third argument
      auto partial_ptr = payload & 0x3FFFFFFF;
      return rt_apply_partial_primop_3arg(ctx, partial_ptr, arg);
    }
    if (payload & 0x80000000) {
      // Partial primop with 1 arg - apply second argument
      auto partial_ptr = payload & 0x3FFFFFFF;
      return rt_apply_partial_primop(ctx, partial_ptr, arg);
    }
    // Regular primop - apply first argument
    return rt_apply_primop(ctx, payload, arg);
  }

  if (!is_lambda(fn)) {
    throw type_error("cannot call non-function value of type '" + std::string(type_name(fn)) + "'");
  }

  // closure layout: func_index (i32) + capture_count (i32) + captures[]
  auto closure_ptr = get_payload(fn);
  auto func_index = ctx.read_u32(closure_ptr + mem::CLOSURE_FUNC_INDEX_OFFSET);
  // env_ptr points to the closure's captures area
  auto env_ptr = closure_ptr + mem::CLOSURE_CAPTURES_OFFSET;

  return ctx.call_wasm_func(func_index, env_ptr, arg);
}

void rt_throw([[maybe_unused]] runtime_context& ctx, std::uint32_t msg_offset, std::uint32_t line,
              std::uint32_t col) {
  auto msg = ctx.read_string(msg_offset);
  throw runtime_error(msg, line, col);
}

auto rt_lookup_var(runtime_context& ctx, std::uint32_t name_offset) -> nix_value {
  auto name = ctx.read_string(name_offset);

  // look up in builtins
  auto it = ctx.builtins.find(std::string(name));
  if (it != ctx.builtins.end()) {
    return it->second;
  }

  throw runtime_error("undefined variable '" + std::string(name) + "'");
}

auto rt_make_closure(runtime_context& ctx, std::uint32_t func_index, std::uint32_t env_offset,
                     [[maybe_unused]] std::uint32_t env_size) -> nix_value {
  // The environment is already allocated by the compiler at env_offset.
  // We need to create a closure struct that references it.

  // Read capture count from the environment
  auto capture_count = ctx.read_u32(env_offset + mem::ENV_CAPTURE_COUNT_OFFSET);

  // Allocate closure: header + copies of captures
  auto closure_size = mem::closure_size(capture_count);
  auto closure_ptr = ctx.allocate(closure_size);

  // Write func_index
  ctx.write_i32(closure_ptr + mem::CLOSURE_FUNC_INDEX_OFFSET,
                static_cast<std::int32_t>(func_index));

  // Write capture_count
  ctx.write_i32(closure_ptr + mem::CLOSURE_CAPTURE_COUNT_OFFSET,
                static_cast<std::int32_t>(capture_count));

  // Copy captures from environment to closure
  for (std::uint32_t i = 0; i < capture_count; ++i) {
    auto capture = ctx.read_value(env_offset + mem::ENV_CAPTURES_OFFSET + i * mem::VALUE_SIZE);
    ctx.write_value(closure_ptr + mem::CLOSURE_CAPTURES_OFFSET + i * mem::VALUE_SIZE, capture);
  }

  return make_value(value_tag::lambda, closure_ptr);
}

auto rt_make_thunk(runtime_context& ctx, std::uint32_t func_index, std::uint32_t env_offset,
                   [[maybe_unused]] std::uint32_t env_size) -> nix_value {
  // Allocate thunk header
  auto thunk_ptr = ctx.allocate(mem::THUNK_SIZE);

  // Write thunk fields
  ctx.write_i32(thunk_ptr + mem::THUNK_FUNC_INDEX_OFFSET, static_cast<std::int32_t>(func_index));
  // Store env_ptr pointing to captures area (past the capture_count header)
  // Thunk env layout: [capture_count(4), captures...]
  // The thunk function expects env_ptr to point directly to captures (same as closures)
  auto env_captures_ptr = env_offset + 4; // skip past capture_count
  ctx.write_i32(thunk_ptr + mem::THUNK_ENV_PTR_OFFSET, static_cast<std::int32_t>(env_captures_ptr));
  ctx.write_i32(thunk_ptr + mem::THUNK_STATE_OFFSET, mem::THUNK_STATE_PENDING);
  ctx.write_i64(thunk_ptr + mem::THUNK_CACHED_VALUE_OFFSET, 0);

  return make_value(value_tag::thunk, thunk_ptr);
}

// =============================================================================
// Arithmetic
// =============================================================================

namespace {

auto expect_numeric(nix_value v, std::uint32_t line, std::uint32_t col) -> void {
  if (!is_numeric(v)) {
    throw type_error("expected numeric type, got '" + std::string(type_name(v)) + "'", line, col);
  }
}

auto to_double(runtime_context& ctx, nix_value v) -> double {
  if (is_int(v)) {
    return static_cast<double>(static_cast<std::int32_t>(get_payload(v)));
  }
  // float: payload is an offset to f64 in memory
  auto offset = get_payload(v);
  auto bytes = ctx.read_bytes(offset, 8);
  double d;
  std::memcpy(&d, bytes.data(), 8);
  return d;
}

auto make_int(std::int32_t i) -> nix_value {
  return make_value(value_tag::integer, static_cast<std::uint32_t>(i));
}

auto make_float(runtime_context& ctx, double d) -> nix_value {
  // Allocate 8 bytes for the double
  auto offset = ctx.allocate(8);
  // Write double to memory
  std::array<std::uint8_t, 8> data;
  std::memcpy(data.data(), &d, 8);
  ctx.write_bytes(offset, data);
  return make_value(value_tag::floating, offset);
}

// Forward declaration for deep equality in rt_eq
auto find_attr(runtime_context& ctx, std::uint32_t attrs_ptr, std::string_view key)
    -> std::optional<nix_value>;

} // namespace

auto rt_add([[maybe_unused]] runtime_context& ctx, nix_value a, nix_value b, std::uint32_t line,
            std::uint32_t col) -> nix_value {
  // Force arguments first
  a = rt_force(ctx, a);
  b = rt_force(ctx, b);

  // string concatenation
  if (is_string(a) && is_string(b)) {
    // TODO: implement string concatenation
    throw runtime_error("string concatenation not yet implemented", line, col);
  }

  expect_numeric(a, line, col);
  expect_numeric(b, line, col);

  // if both are ints, return int
  if (is_int(a) && is_int(b)) {
    auto ia = static_cast<std::int32_t>(get_payload(a));
    auto ib = static_cast<std::int32_t>(get_payload(b));
    return make_int(ia + ib);
  }

  // otherwise, convert to float
  return make_float(ctx, to_double(ctx, a) + to_double(ctx, b));
}

auto rt_sub([[maybe_unused]] runtime_context& ctx, nix_value a, nix_value b, std::uint32_t line,
            std::uint32_t col) -> nix_value {
  a = rt_force(ctx, a);
  b = rt_force(ctx, b);
  expect_numeric(a, line, col);
  expect_numeric(b, line, col);

  if (is_int(a) && is_int(b)) {
    auto ia = static_cast<std::int32_t>(get_payload(a));
    auto ib = static_cast<std::int32_t>(get_payload(b));
    return make_int(ia - ib);
  }

  return make_float(ctx, to_double(ctx, a) - to_double(ctx, b));
}

auto rt_mul([[maybe_unused]] runtime_context& ctx, nix_value a, nix_value b, std::uint32_t line,
            std::uint32_t col) -> nix_value {
  a = rt_force(ctx, a);
  b = rt_force(ctx, b);
  expect_numeric(a, line, col);
  expect_numeric(b, line, col);

  if (is_int(a) && is_int(b)) {
    auto ia = static_cast<std::int32_t>(get_payload(a));
    auto ib = static_cast<std::int32_t>(get_payload(b));
    return make_int(ia * ib);
  }

  return make_float(ctx, to_double(ctx, a) * to_double(ctx, b));
}

auto rt_div([[maybe_unused]] runtime_context& ctx, nix_value a, nix_value b, std::uint32_t line,
            std::uint32_t col) -> nix_value {
  a = rt_force(ctx, a);
  b = rt_force(ctx, b);
  expect_numeric(a, line, col);
  expect_numeric(b, line, col);

  // Nix integer division is floor division and returns an integer
  // Float division returns a float
  if (is_int(a) && is_int(b)) {
    auto ia = static_cast<std::int32_t>(get_payload(a));
    auto ib = static_cast<std::int32_t>(get_payload(b));
    if (ib == 0) {
      throw runtime_error("division by zero", line, col);
    }
    // Handle INT_MIN / -1 which would overflow (result is INT_MAX + 1)
    // This is the only case where signed division can overflow
    constexpr auto int_min = std::numeric_limits<std::int32_t>::min();
    if (ia == int_min && ib == -1) {
      throw runtime_error("integer overflow: INT_MIN / -1", line, col);
    }
    // Nix uses floor division (rounds toward negative infinity)
    // C++ integer division truncates toward zero, so we need to adjust
    auto result = ia / ib;
    if ((ia % ib != 0) && ((ia < 0) != (ib < 0))) {
      result -= 1;
    }
    return make_int(result);
  }

  // Float division
  auto db = to_double(ctx, b);
  if (db == 0.0) {
    throw runtime_error("division by zero", line, col);
  }
  return make_float(ctx, to_double(ctx, a) / db);
}

auto rt_negate(runtime_context& ctx, nix_value v) -> nix_value {
  if (is_int(v)) {
    auto i = static_cast<std::int32_t>(get_payload(v));
    return make_int(-i);
  }
  if (is_float(v)) {
    return make_float(ctx, -to_double(ctx, v));
  }
  throw type_error("cannot negate value of type '" + std::string(type_name(v)) + "'");
}

// =============================================================================
// Comparison
// =============================================================================

auto rt_less_than(runtime_context& ctx, nix_value a, nix_value b) -> nix_value {
  // force both values
  a = rt_force(ctx, a);
  b = rt_force(ctx, b);

  if (is_int(a) && is_int(b)) {
    auto ia = static_cast<std::int32_t>(get_payload(a));
    auto ib = static_cast<std::int32_t>(get_payload(b));
    return ia < ib ? constants::bool_true : constants::bool_false;
  }

  if (is_numeric(a) && is_numeric(b)) {
    return to_double(ctx, a) < to_double(ctx, b) ? constants::bool_true : constants::bool_false;
  }

  if (is_string(a) && is_string(b)) {
    auto sa = ctx.read_string(get_payload(a));
    auto sb = ctx.read_string(get_payload(b));
    return sa < sb ? constants::bool_true : constants::bool_false;
  }

  throw type_error("cannot compare values of types '" + std::string(type_name(a)) + "' and '" +
                   std::string(type_name(b)) + "'");
}

auto rt_less_eq(runtime_context& ctx, nix_value a, nix_value b) -> nix_value {
  auto lt = rt_less_than(ctx, a, b);
  if (lt == constants::bool_true) {
    return constants::bool_true;
  }
  return rt_eq(ctx, a, b);
}

auto rt_eq(runtime_context& ctx, nix_value a, nix_value b) -> nix_value {
  // force both values
  a = rt_force(ctx, a);
  b = rt_force(ctx, b);

  // different types are not equal
  if (get_tag(a) != get_tag(b)) {
    // except int and float
    if (is_numeric(a) && is_numeric(b)) {
      return to_double(ctx, a) == to_double(ctx, b) ? constants::bool_true : constants::bool_false;
    }
    return constants::bool_false;
  }

  switch (get_tag(a)) {
    case value_tag::null_value:
      return constants::bool_true;

    case value_tag::boolean:
    case value_tag::integer:
      return get_payload(a) == get_payload(b) ? constants::bool_true : constants::bool_false;

    case value_tag::floating:
      return to_double(ctx, a) == to_double(ctx, b) ? constants::bool_true : constants::bool_false;

    case value_tag::string:
    case value_tag::path: {
      auto sa = ctx.read_string(get_payload(a));
      auto sb = ctx.read_string(get_payload(b));
      return sa == sb ? constants::bool_true : constants::bool_false;
    }

    case value_tag::list: {
      auto a_ptr = get_payload(a);
      auto b_ptr = get_payload(b);
      // Handle empty lists (pointer 0)
      if (a_ptr == 0 && b_ptr == 0)
        return constants::bool_true;
      if (a_ptr == 0 || b_ptr == 0) {
        // One empty, one not - check counts
        auto a_count = a_ptr == 0 ? 0 : ctx.read_u32(a_ptr + mem::LIST_COUNT_OFFSET);
        auto b_count = b_ptr == 0 ? 0 : ctx.read_u32(b_ptr + mem::LIST_COUNT_OFFSET);
        return a_count == b_count ? constants::bool_true : constants::bool_false;
      }
      auto a_count = ctx.read_u32(a_ptr + mem::LIST_COUNT_OFFSET);
      auto b_count = ctx.read_u32(b_ptr + mem::LIST_COUNT_OFFSET);
      if (a_count != b_count)
        return constants::bool_false;
      for (std::uint32_t i = 0; i < a_count; ++i) {
        auto elem_a = ctx.read_value(a_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
        auto elem_b = ctx.read_value(b_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
        if (rt_eq(ctx, elem_a, elem_b) != constants::bool_true) {
          return constants::bool_false;
        }
      }
      return constants::bool_true;
    }

    case value_tag::attribute_set: {
      auto a_ptr = get_payload(a);
      auto b_ptr = get_payload(b);
      // Handle empty attrsets (pointer 0)
      if (a_ptr == 0 && b_ptr == 0)
        return constants::bool_true;
      if (a_ptr == 0 || b_ptr == 0) {
        auto a_count = a_ptr == 0 ? 0 : ctx.read_u32(a_ptr + mem::ATTRSET_COUNT_OFFSET);
        auto b_count = b_ptr == 0 ? 0 : ctx.read_u32(b_ptr + mem::ATTRSET_COUNT_OFFSET);
        return a_count == b_count ? constants::bool_true : constants::bool_false;
      }
      auto a_count = ctx.read_u32(a_ptr + mem::ATTRSET_COUNT_OFFSET);
      auto b_count = ctx.read_u32(b_ptr + mem::ATTRSET_COUNT_OFFSET);
      if (a_count != b_count)
        return constants::bool_false;
      // For each attr in a, check it exists in b with same value
      for (std::uint32_t i = 0; i < a_count; ++i) {
        auto entry_a = a_ptr + mem::ATTRSET_ENTRIES_OFFSET + i * mem::ATTRSET_ENTRY_SIZE;
        auto key_offset = ctx.read_u32(entry_a + mem::ATTRSET_ENTRY_KEY_OFFSET);
        auto val_a = ctx.read_value(entry_a + mem::ATTRSET_ENTRY_VALUE_OFFSET);
        auto key = ctx.read_string(key_offset);
        auto val_b = find_attr(ctx, b_ptr, key);
        if (!val_b.has_value() || rt_eq(ctx, val_a, *val_b) != constants::bool_true) {
          return constants::bool_false;
        }
      }
      return constants::bool_true;
    }

    case value_tag::lambda:
    case value_tag::thunk:
    case value_tag::primop:
      // functions are compared by identity
      return get_payload(a) == get_payload(b) ? constants::bool_true : constants::bool_false;

    default:
      return constants::bool_false;
  }
}

auto rt_neq(runtime_context& ctx, nix_value a, nix_value b) -> nix_value {
  return rt_eq(ctx, a, b) == constants::bool_true ? constants::bool_false : constants::bool_true;
}

// =============================================================================
// Boolean
// =============================================================================

auto rt_not(runtime_context& ctx, nix_value v) -> nix_value {
  v = rt_force(ctx, v);
  if (!is_bool(v)) {
    throw type_error("cannot apply 'not' to value of type '" + std::string(type_name(v)) + "'");
  }
  return v == constants::bool_true ? constants::bool_false : constants::bool_true;
}

auto rt_is_bool(runtime_context& ctx, nix_value v) -> std::int32_t {
  v = rt_force(ctx, v);
  return is_bool(v) ? 1 : 0;
}

// =============================================================================
// Collections
// =============================================================================

auto rt_make_list(runtime_context& ctx, std::uint32_t offset, std::uint32_t count) -> nix_value {
  // Allocate list: header + elements
  auto list_size = mem::list_size(count);
  auto list_ptr = ctx.allocate(list_size);

  // Write count
  ctx.write_i32(list_ptr + mem::LIST_COUNT_OFFSET, static_cast<std::int32_t>(count));

  // Copy elements from source offset
  for (std::uint32_t i = 0; i < count; ++i) {
    auto elem = ctx.read_value(offset + i * mem::VALUE_SIZE);
    ctx.write_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE, elem);
  }

  return make_value(value_tag::list, list_ptr);
}

auto rt_make_attrs(runtime_context& ctx, std::uint32_t offset, std::uint32_t count) -> nix_value {
  // Allocate attrset: header + entries
  auto attrs_size = mem::attrset_size(count);
  auto attrs_ptr = ctx.allocate(attrs_size);

  // Write count
  ctx.write_i32(attrs_ptr + mem::ATTRSET_COUNT_OFFSET, static_cast<std::int32_t>(count));

  // Copy entries (each is 12 bytes: key_offset + value)
  for (std::uint32_t i = 0; i < count; ++i) {
    auto key_offset = ctx.read_u32(offset + i * mem::ATTRSET_ENTRY_SIZE);
    auto value =
        ctx.read_value(offset + i * mem::ATTRSET_ENTRY_SIZE + mem::ATTRSET_ENTRY_VALUE_OFFSET);

    auto entry_offset = attrs_ptr + mem::ATTRSET_ENTRIES_OFFSET + i * mem::ATTRSET_ENTRY_SIZE;
    ctx.write_i32(entry_offset + mem::ATTRSET_ENTRY_KEY_OFFSET,
                  static_cast<std::int32_t>(key_offset));
    ctx.write_value(entry_offset + mem::ATTRSET_ENTRY_VALUE_OFFSET, value);
  }

  return make_value(value_tag::attribute_set, attrs_ptr);
}

auto rt_make_attrs_dynamic(runtime_context& ctx, std::uint32_t offset, std::uint32_t count)
    -> nix_value {
  // Allocate attrset: header + entries (static layout)
  auto attrs_size = mem::attrset_size(count);
  auto attrs_ptr = ctx.allocate(attrs_size);

  // Write count
  ctx.write_i32(attrs_ptr + mem::ATTRSET_COUNT_OFFSET, static_cast<std::int32_t>(count));

  // Convert from dynamic layout (key_value + value) to static layout (key_offset + value)
  for (std::uint32_t i = 0; i < count; ++i) {
    auto key_value = ctx.read_value(offset + i * mem::ATTRSET_DYNAMIC_ENTRY_SIZE);
    auto value = ctx.read_value(offset + i * mem::ATTRSET_DYNAMIC_ENTRY_SIZE + mem::VALUE_SIZE);

    // force and extract key string offset
    key_value = rt_force(ctx, key_value);
    if (!is_string(key_value)) {
      throw type_error("attribute name must be a string");
    }
    auto key_offset = get_payload(key_value);

    auto entry_offset = attrs_ptr + mem::ATTRSET_ENTRIES_OFFSET + i * mem::ATTRSET_ENTRY_SIZE;
    ctx.write_i32(entry_offset + mem::ATTRSET_ENTRY_KEY_OFFSET,
                  static_cast<std::int32_t>(key_offset));
    ctx.write_value(entry_offset + mem::ATTRSET_ENTRY_VALUE_OFFSET, value);
  }

  return make_value(value_tag::attribute_set, attrs_ptr);
}

namespace {

auto find_attr(runtime_context& ctx, std::uint32_t attrs_ptr, std::string_view key)
    -> std::optional<nix_value> {
  // Special case: pointer 0 represents an empty attrset (no attributes to find)
  if (attrs_ptr == 0) {
    return std::nullopt;
  }

  auto count = ctx.read_u32(attrs_ptr + mem::ATTRSET_COUNT_OFFSET);

  for (std::uint32_t i = 0; i < count; ++i) {
    auto entry_offset = attrs_ptr + mem::ATTRSET_ENTRIES_OFFSET + i * mem::ATTRSET_ENTRY_SIZE;
    auto key_offset = ctx.read_u32(entry_offset + mem::ATTRSET_ENTRY_KEY_OFFSET);
    auto attr_key = ctx.read_string(key_offset);
    if (attr_key == key) {
      return ctx.read_value(entry_offset + mem::ATTRSET_ENTRY_VALUE_OFFSET);
    }
  }

  return std::nullopt;
}

} // namespace

auto rt_select(runtime_context& ctx, nix_value set, std::uint32_t key_offset, std::uint32_t line,
               std::uint32_t col) -> nix_value {
  set = rt_force(ctx, set);

  if (!is_attrset(set)) {
    throw type_error("cannot select from value of type '" + std::string(type_name(set)) + "'", line,
                     col);
  }

  auto key = ctx.read_string(key_offset);
  auto attrs_ptr = get_payload(set);
  auto result = find_attr(ctx, attrs_ptr, key);

  if (!result.has_value()) {
    throw attr_error("attribute '" + std::string(key) + "' not found", line, col);
  }

  return *result;
}

auto rt_select_dynamic(runtime_context& ctx, nix_value set, nix_value key, std::uint32_t line,
                       std::uint32_t col) -> nix_value {
  key = rt_force(ctx, key);

  if (!is_string(key)) {
    throw type_error("attribute name must be a string, got '" + std::string(type_name(key)) + "'",
                     line, col);
  }

  return rt_select(ctx, set, get_payload(key), line, col);
}

auto rt_has_attr(runtime_context& ctx, nix_value set, std::uint32_t key_offset) -> nix_value {
  set = rt_force(ctx, set);

  if (!is_attrset(set)) {
    return constants::bool_false;
  }

  auto key = ctx.read_string(key_offset);
  auto attrs_ptr = get_payload(set);
  auto result = find_attr(ctx, attrs_ptr, key);

  return result.has_value() ? constants::bool_true : constants::bool_false;
}

auto rt_has_attr_dynamic(runtime_context& ctx, nix_value set, nix_value key) -> nix_value {
  key = rt_force(ctx, key);

  if (!is_string(key)) {
    return constants::bool_false;
  }

  return rt_has_attr(ctx, set, get_payload(key));
}

auto rt_update(runtime_context& ctx, nix_value a, nix_value b) -> nix_value {
  a = rt_force(ctx, a);
  b = rt_force(ctx, b);

  if (!is_attrset(a) || !is_attrset(b)) {
    throw type_error("cannot update: expected attribute sets");
  }

  auto a_ptr = get_payload(a);
  auto b_ptr = get_payload(b);

  // Handle empty cases
  if (a_ptr == 0)
    return b;
  if (b_ptr == 0)
    return a;

  auto a_count = ctx.read_u32(a_ptr + mem::ATTRSET_COUNT_OFFSET);
  auto b_count = ctx.read_u32(b_ptr + mem::ATTRSET_COUNT_OFFSET);

  // Collect all keys from b (these override a)
  std::vector<std::pair<std::uint32_t, nix_value>> entries; // key_offset, value
  entries.reserve(a_count + b_count);

  // Add all from b first
  for (std::uint32_t i = 0; i < b_count; ++i) {
    auto entry = b_ptr + mem::ATTRSET_ENTRIES_OFFSET + i * mem::ATTRSET_ENTRY_SIZE;
    auto key_off = ctx.read_u32(entry + mem::ATTRSET_ENTRY_KEY_OFFSET);
    auto val = ctx.read_value(entry + mem::ATTRSET_ENTRY_VALUE_OFFSET);
    entries.emplace_back(key_off, val);
  }

  // Add from a only if not in b
  for (std::uint32_t i = 0; i < a_count; ++i) {
    auto entry = a_ptr + mem::ATTRSET_ENTRIES_OFFSET + i * mem::ATTRSET_ENTRY_SIZE;
    auto key_off = ctx.read_u32(entry + mem::ATTRSET_ENTRY_KEY_OFFSET);
    auto key = ctx.read_string(key_off);
    // Check if key exists in b
    bool in_b = false;
    for (std::uint32_t j = 0; j < b_count; ++j) {
      auto b_entry = b_ptr + mem::ATTRSET_ENTRIES_OFFSET + j * mem::ATTRSET_ENTRY_SIZE;
      auto b_key_off = ctx.read_u32(b_entry + mem::ATTRSET_ENTRY_KEY_OFFSET);
      if (ctx.read_string(b_key_off) == key) {
        in_b = true;
        break;
      }
    }
    if (!in_b) {
      auto val = ctx.read_value(entry + mem::ATTRSET_ENTRY_VALUE_OFFSET);
      entries.emplace_back(key_off, val);
    }
  }

  // Allocate new attrset
  auto count = static_cast<std::uint32_t>(entries.size());
  auto attrs_size = mem::attrset_size(count);
  auto attrs_ptr = ctx.allocate(attrs_size);

  ctx.write_i32(attrs_ptr + mem::ATTRSET_COUNT_OFFSET, static_cast<std::int32_t>(count));

  for (std::uint32_t i = 0; i < count; ++i) {
    auto entry_off = attrs_ptr + mem::ATTRSET_ENTRIES_OFFSET + i * mem::ATTRSET_ENTRY_SIZE;
    ctx.write_i32(entry_off + mem::ATTRSET_ENTRY_KEY_OFFSET,
                  static_cast<std::int32_t>(entries[i].first));
    ctx.write_value(entry_off + mem::ATTRSET_ENTRY_VALUE_OFFSET, entries[i].second);
  }

  return make_value(value_tag::attribute_set, attrs_ptr);
}

auto rt_concat(runtime_context& ctx, nix_value a, nix_value b) -> nix_value {
  a = rt_force(ctx, a);
  b = rt_force(ctx, b);

  if (!is_list(a) || !is_list(b)) {
    throw type_error("cannot concatenate: expected lists");
  }

  auto a_ptr = get_payload(a);
  auto b_ptr = get_payload(b);
  auto a_count = ctx.read_u32(a_ptr + mem::LIST_COUNT_OFFSET);
  auto b_count = ctx.read_u32(b_ptr + mem::LIST_COUNT_OFFSET);
  auto total_count = a_count + b_count;

  // Allocate combined list
  auto list_size = mem::list_size(total_count);
  auto list_ptr = ctx.allocate(list_size);

  ctx.write_i32(list_ptr + mem::LIST_COUNT_OFFSET, static_cast<std::int32_t>(total_count));

  // Copy elements from a
  for (std::uint32_t i = 0; i < a_count; ++i) {
    auto elem = ctx.read_value(a_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
    ctx.write_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE, elem);
  }

  // Copy elements from b
  for (std::uint32_t i = 0; i < b_count; ++i) {
    auto elem = ctx.read_value(b_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
    ctx.write_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + (a_count + i) * mem::VALUE_SIZE, elem);
  }

  return make_value(value_tag::list, list_ptr);
}

// =============================================================================
// Strings
// =============================================================================

auto rt_to_string(runtime_context& ctx, nix_value v) -> nix_value {
  v = rt_force(ctx, v);

  // if already a string, return as-is
  if (is_string(v)) {
    return v;
  }

  std::string result;

  switch (get_tag(v)) {
    case value_tag::null_value:
      // null coerces to empty string in some contexts
      result = "";
      break;

    case value_tag::boolean:
      // booleans don't coerce to string in Nix (this is an error)
      throw type_error("cannot coerce a Boolean to a string");

    case value_tag::integer: {
      auto i = static_cast<std::int32_t>(get_payload(v));
      result = std::to_string(i);
      break;
    }

    case value_tag::floating: {
      auto bits = get_payload(v);
      float f;
      std::memcpy(&f, &bits, sizeof(f));
      result = std::to_string(static_cast<double>(f));
      break;
    }

    case value_tag::path:
      // paths coerce to their string representation
      result = std::string(ctx.read_string(get_payload(v)));
      break;

    default:
      throw type_error("cannot coerce " + std::string(type_name(v)) + " to a string");
  }

  // Allocate and store the result string
  auto len = static_cast<std::uint32_t>(result.size());
  auto ptr = ctx.allocate(len + 1);
  for (std::size_t i = 0; i < result.size(); ++i) {
    ctx.memory[ptr + i] = static_cast<std::uint8_t>(result[i]);
  }
  ctx.memory[ptr + result.size()] = 0;

  return make_value(value_tag::string, ptr);
}

auto rt_concat_strings(runtime_context& ctx, std::uint32_t offset, std::uint32_t count)
    -> nix_value {
  // read and concatenate all strings
  std::string result;

  for (std::uint32_t i = 0; i < count; ++i) {
    auto part = ctx.read_value(offset + i * mem::VALUE_SIZE);
    part = rt_force(ctx, part);

    if (!is_string(part)) {
      throw type_error("expected string in concatenation, got '" + std::string(type_name(part)) +
                       "'");
    }

    auto str = ctx.read_string(get_payload(part));
    result += str;
  }

  // allocate space for the result string
  auto string_len = static_cast<std::uint32_t>(result.size());
  auto string_ptr = ctx.allocate(string_len + 1);

  // write the string (null-terminated)
  for (std::size_t i = 0; i < result.size(); ++i) {
    ctx.memory[string_ptr + i] = static_cast<std::uint8_t>(result[i]);
  }
  ctx.memory[string_ptr + result.size()] = 0;

  return make_value(value_tag::string, string_ptr);
}

// =============================================================================
// Builtins (Primops)
// =============================================================================

namespace {

/// Helper to allocate a string in runtime memory
auto allocate_string(runtime_context& ctx, std::string_view str) -> std::uint32_t {
  auto len = static_cast<std::uint32_t>(str.size());
  auto ptr = ctx.allocate(len + 1);
  for (std::size_t i = 0; i < str.size(); ++i) {
    ctx.memory[ptr + i] = static_cast<std::uint8_t>(str[i]);
  }
  ctx.memory[ptr + str.size()] = 0;
  return ptr;
}

} // namespace

auto rt_length(runtime_context& ctx, nix_value v) -> nix_value {
  v = rt_force(ctx, v);

  if (is_list(v)) {
    auto ptr = get_payload(v);
    if (ptr == 0) {
      return make_int(0); // empty list
    }
    auto count = ctx.read_u32(ptr + mem::LIST_COUNT_OFFSET);
    return make_int(static_cast<std::int32_t>(count));
  }

  if (is_string(v)) {
    auto str = ctx.read_string(get_payload(v));
    return make_int(static_cast<std::int32_t>(str.size()));
  }

  throw type_error("builtins.length: expected list, got '" + std::string(type_name(v)) + "'");
}

auto rt_head(runtime_context& ctx, nix_value v) -> nix_value {
  v = rt_force(ctx, v);

  if (!is_list(v)) {
    throw type_error("builtins.head: expected list, got '" + std::string(type_name(v)) + "'");
  }

  auto ptr = get_payload(v);
  if (ptr == 0) {
    throw runtime_error("builtins.head: list is empty");
  }

  auto count = ctx.read_u32(ptr + mem::LIST_COUNT_OFFSET);
  if (count == 0) {
    throw runtime_error("builtins.head: list is empty");
  }

  return ctx.read_value(ptr + mem::LIST_ELEMENTS_OFFSET);
}

auto rt_tail(runtime_context& ctx, nix_value v) -> nix_value {
  v = rt_force(ctx, v);

  if (!is_list(v)) {
    throw type_error("builtins.tail: expected list, got '" + std::string(type_name(v)) + "'");
  }

  auto ptr = get_payload(v);
  if (ptr == 0) {
    throw runtime_error("builtins.tail: list is empty");
  }

  auto count = ctx.read_u32(ptr + mem::LIST_COUNT_OFFSET);
  if (count == 0) {
    throw runtime_error("builtins.tail: list is empty");
  }

  if (count == 1) {
    // return empty list
    return make_value(value_tag::list, 0);
  }

  // Allocate new list with count-1 elements
  auto new_count = count - 1;
  auto list_size = mem::list_size(new_count);
  auto list_ptr = ctx.allocate(list_size);

  ctx.write_i32(list_ptr + mem::LIST_COUNT_OFFSET, static_cast<std::int32_t>(new_count));

  // Copy elements starting from index 1
  for (std::uint32_t i = 0; i < new_count; ++i) {
    auto elem = ctx.read_value(ptr + mem::LIST_ELEMENTS_OFFSET + (i + 1) * mem::VALUE_SIZE);
    ctx.write_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE, elem);
  }

  return make_value(value_tag::list, list_ptr);
}

auto rt_elem_at(runtime_context& ctx, nix_value list, nix_value index) -> nix_value {
  list = rt_force(ctx, list);
  index = rt_force(ctx, index);

  if (!is_list(list)) {
    throw type_error("builtins.elemAt: expected list, got '" + std::string(type_name(list)) + "'");
  }

  if (!is_int(index)) {
    throw type_error("builtins.elemAt: expected integer index, got '" +
                     std::string(type_name(index)) + "'");
  }

  auto idx = static_cast<std::int32_t>(get_payload(index));
  if (idx < 0) {
    throw runtime_error("builtins.elemAt: index " + std::to_string(idx) + " is negative");
  }

  auto ptr = get_payload(list);
  if (ptr == 0) {
    throw runtime_error("builtins.elemAt: index " + std::to_string(idx) +
                        " is out of bounds for empty list");
  }

  auto count = ctx.read_u32(ptr + mem::LIST_COUNT_OFFSET);
  if (static_cast<std::uint32_t>(idx) >= count) {
    throw runtime_error("builtins.elemAt: index " + std::to_string(idx) +
                        " is out of bounds for list of length " + std::to_string(count));
  }

  return ctx.read_value(ptr + mem::LIST_ELEMENTS_OFFSET +
                        static_cast<std::uint32_t>(idx) * mem::VALUE_SIZE);
}

auto rt_elem(runtime_context& ctx, nix_value x, nix_value list) -> nix_value {
  list = rt_force(ctx, list);

  if (!is_list(list)) {
    throw type_error("builtins.elem: expected list, got '" + std::string(type_name(list)) + "'");
  }

  auto ptr = get_payload(list);
  if (ptr == 0) {
    return constants::bool_false; // not in empty list
  }

  auto count = ctx.read_u32(ptr + mem::LIST_COUNT_OFFSET);
  for (std::uint32_t i = 0; i < count; ++i) {
    auto elem = ctx.read_value(ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
    if (rt_eq(ctx, x, elem) == constants::bool_true) {
      return constants::bool_true;
    }
  }

  return constants::bool_false;
}

auto rt_type_of(runtime_context& ctx, nix_value v) -> nix_value {
  v = rt_force(ctx, v);

  std::string_view type_str;
  switch (get_tag(v)) {
    case value_tag::null_value:
      type_str = "null";
      break;
    case value_tag::boolean:
      type_str = "bool";
      break;
    case value_tag::integer:
      type_str = "int";
      break;
    case value_tag::floating:
      type_str = "float";
      break;
    case value_tag::string:
      type_str = "string";
      break;
    case value_tag::path:
      type_str = "path";
      break;
    case value_tag::list:
      type_str = "list";
      break;
    case value_tag::attribute_set:
      type_str = "set";
      break;
    case value_tag::lambda:
    case value_tag::primop:
      type_str = "lambda";
      break;
    default:
      type_str = "unknown";
      break;
  }

  auto ptr = allocate_string(ctx, type_str);
  return make_value(value_tag::string, ptr);
}

auto rt_attr_names(runtime_context& ctx, nix_value set) -> nix_value {
  set = rt_force(ctx, set);

  if (!is_attrset(set)) {
    throw type_error("builtins.attrNames: expected set, got '" + std::string(type_name(set)) + "'");
  }

  auto attrs_ptr = get_payload(set);
  if (attrs_ptr == 0) {
    // empty set -> empty list
    return make_value(value_tag::list, 0);
  }

  auto count = ctx.read_u32(attrs_ptr + mem::ATTRSET_COUNT_OFFSET);
  if (count == 0) {
    return make_value(value_tag::list, 0);
  }

  // Collect all names
  std::vector<std::string> names;
  names.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    auto entry = attrs_ptr + mem::ATTRSET_ENTRIES_OFFSET + i * mem::ATTRSET_ENTRY_SIZE;
    auto key_offset = ctx.read_u32(entry + mem::ATTRSET_ENTRY_KEY_OFFSET);
    names.emplace_back(ctx.read_string(key_offset));
  }

  // Sort names
  std::sort(names.begin(), names.end());

  // Allocate list
  auto list_size = mem::list_size(count);
  auto list_ptr = ctx.allocate(list_size);
  ctx.write_i32(list_ptr + mem::LIST_COUNT_OFFSET, static_cast<std::int32_t>(count));

  // Allocate strings and write list elements
  for (std::uint32_t i = 0; i < count; ++i) {
    auto str_ptr = allocate_string(ctx, names[i]);
    auto str_val = make_value(value_tag::string, str_ptr);
    ctx.write_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE, str_val);
  }

  return make_value(value_tag::list, list_ptr);
}

auto rt_attr_values(runtime_context& ctx, nix_value set) -> nix_value {
  set = rt_force(ctx, set);

  if (!is_attrset(set)) {
    throw type_error("builtins.attrValues: expected set, got '" + std::string(type_name(set)) +
                     "'");
  }

  auto attrs_ptr = get_payload(set);
  if (attrs_ptr == 0) {
    return make_value(value_tag::list, 0);
  }

  auto count = ctx.read_u32(attrs_ptr + mem::ATTRSET_COUNT_OFFSET);
  if (count == 0) {
    return make_value(value_tag::list, 0);
  }

  // Collect all (name, value) pairs and sort by name
  std::vector<std::pair<std::string, nix_value>> entries;
  entries.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    auto entry = attrs_ptr + mem::ATTRSET_ENTRIES_OFFSET + i * mem::ATTRSET_ENTRY_SIZE;
    auto key_offset = ctx.read_u32(entry + mem::ATTRSET_ENTRY_KEY_OFFSET);
    auto value = ctx.read_value(entry + mem::ATTRSET_ENTRY_VALUE_OFFSET);
    entries.emplace_back(std::string(ctx.read_string(key_offset)), value);
  }

  std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  // Allocate list
  auto list_size = mem::list_size(count);
  auto list_ptr = ctx.allocate(list_size);
  ctx.write_i32(list_ptr + mem::LIST_COUNT_OFFSET, static_cast<std::int32_t>(count));

  // Write values in sorted order
  for (std::uint32_t i = 0; i < count; ++i) {
    ctx.write_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE, entries[i].second);
  }

  return make_value(value_tag::list, list_ptr);
}

auto rt_string_length(runtime_context& ctx, nix_value s) -> nix_value {
  s = rt_force(ctx, s);

  if (!is_string(s)) {
    throw type_error("builtins.stringLength: expected string, got '" + std::string(type_name(s)) +
                     "'");
  }

  auto str = ctx.read_string(get_payload(s));
  return make_int(static_cast<std::int32_t>(str.size()));
}

auto rt_substring(runtime_context& ctx, nix_value start, nix_value len, nix_value str)
    -> nix_value {
  start = rt_force(ctx, start);
  len = rt_force(ctx, len);
  str = rt_force(ctx, str);

  if (!is_int(start)) {
    throw type_error("builtins.substring: start must be int, got '" +
                     std::string(type_name(start)) + "'");
  }
  if (!is_int(len)) {
    throw type_error("builtins.substring: length must be int, got '" + std::string(type_name(len)) +
                     "'");
  }
  if (!is_string(str)) {
    throw type_error("builtins.substring: expected string, got '" + std::string(type_name(str)) +
                     "'");
  }

  auto start_val = static_cast<std::int32_t>(get_payload(start));
  auto len_val = static_cast<std::int32_t>(get_payload(len));
  auto s = ctx.read_string(get_payload(str));

  // Nix semantics: negative start treated as 0, negative len means "rest of string"
  if (start_val < 0) {
    start_val = 0;
  }

  auto start_idx = static_cast<std::size_t>(start_val);
  if (start_idx >= s.size()) {
    // Start beyond string -> empty string
    auto ptr = allocate_string(ctx, "");
    return make_value(value_tag::string, ptr);
  }

  std::size_t result_len;
  if (len_val < 0) {
    // Negative length means rest of string
    result_len = s.size() - start_idx;
  } else {
    result_len = std::min(static_cast<std::size_t>(len_val), s.size() - start_idx);
  }

  auto result = s.substr(start_idx, result_len);
  auto ptr = allocate_string(ctx, result);
  return make_value(value_tag::string, ptr);
}

auto rt_replace_strings(runtime_context& ctx, nix_value from, nix_value to, nix_value str)
    -> nix_value {
  from = rt_force(ctx, from);
  to = rt_force(ctx, to);
  str = rt_force(ctx, str);

  if (!is_list(from)) {
    throw type_error("builtins.replaceStrings: 'from' must be list, got '" +
                     std::string(type_name(from)) + "'");
  }
  if (!is_list(to)) {
    throw type_error("builtins.replaceStrings: 'to' must be list, got '" +
                     std::string(type_name(to)) + "'");
  }
  if (!is_string(str)) {
    throw type_error("builtins.replaceStrings: expected string, got '" +
                     std::string(type_name(str)) + "'");
  }

  // Collect from/to pairs
  std::vector<std::string> from_strs;
  std::vector<std::string> to_strs;

  auto from_ptr = get_payload(from);
  auto to_ptr = get_payload(to);

  std::uint32_t from_count = 0;
  std::uint32_t to_count = 0;

  if (from_ptr != 0) {
    from_count = ctx.read_u32(from_ptr + mem::LIST_COUNT_OFFSET);
  }
  if (to_ptr != 0) {
    to_count = ctx.read_u32(to_ptr + mem::LIST_COUNT_OFFSET);
  }

  if (from_count != to_count) {
    throw runtime_error("builtins.replaceStrings: 'from' and 'to' lists must have same length");
  }

  for (std::uint32_t i = 0; i < from_count; ++i) {
    auto f = ctx.read_value(from_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
    auto t = ctx.read_value(to_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
    f = rt_force(ctx, f);
    t = rt_force(ctx, t);
    if (!is_string(f) || !is_string(t)) {
      throw type_error("builtins.replaceStrings: all elements must be strings");
    }
    from_strs.push_back(std::string(ctx.read_string(get_payload(f))));
    to_strs.push_back(std::string(ctx.read_string(get_payload(t))));
  }

  auto input = std::string(ctx.read_string(get_payload(str)));

  // Perform replacements - scan through string, find matches, replace
  std::string result;
  std::size_t pos = 0;

  while (pos < input.size()) {
    bool matched = false;
    for (std::size_t i = 0; i < from_strs.size(); ++i) {
      const auto& f = from_strs[i];
      if (f.empty()) {
        // Empty string matches at every position - insert replacement and advance by 1
        result += to_strs[i];
        if (pos < input.size()) {
          result += input[pos];
          ++pos;
        }
        matched = true;
        break;
      }
      if (input.compare(pos, f.size(), f) == 0) {
        result += to_strs[i];
        pos += f.size();
        matched = true;
        break;
      }
    }
    if (!matched) {
      result += input[pos];
      ++pos;
    }
  }

  // Handle trailing empty string match
  for (std::size_t i = 0; i < from_strs.size(); ++i) {
    if (from_strs[i].empty() && pos == input.size()) {
      result += to_strs[i];
      break;
    }
  }

  auto result_ptr = allocate_string(ctx, result);
  return make_value(value_tag::string, result_ptr);
}

auto rt_concat_strings(runtime_context& ctx, nix_value list) -> nix_value {
  list = rt_force(ctx, list);

  if (!is_list(list)) {
    throw type_error("builtins.concatStrings: expected list, got '" + std::string(type_name(list)) +
                     "'");
  }

  auto list_ptr = get_payload(list);
  if (list_ptr == 0) {
    auto ptr = allocate_string(ctx, "");
    return make_value(value_tag::string, ptr);
  }

  auto count = ctx.read_u32(list_ptr + mem::LIST_COUNT_OFFSET);
  std::string result;

  for (std::uint32_t i = 0; i < count; ++i) {
    auto elem = ctx.read_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
    elem = rt_force(ctx, elem);
    if (!is_string(elem)) {
      throw type_error("builtins.concatStrings: all elements must be strings");
    }
    result += ctx.read_string(get_payload(elem));
  }

  auto ptr = allocate_string(ctx, result);
  return make_value(value_tag::string, ptr);
}

auto rt_concat_string_sep(runtime_context& ctx, nix_value sep, nix_value list) -> nix_value {
  sep = rt_force(ctx, sep);
  list = rt_force(ctx, list);

  if (!is_string(sep)) {
    throw type_error("builtins.concatStringsSep: first argument must be a string, got '" +
                     std::string(type_name(sep)) + "'");
  }
  if (!is_list(list)) {
    throw type_error("builtins.concatStringsSep: second argument must be a list, got '" +
                     std::string(type_name(list)) + "'");
  }

  auto sep_str = ctx.read_string(get_payload(sep));

  auto list_ptr = get_payload(list);
  if (list_ptr == 0) {
    auto ptr = allocate_string(ctx, "");
    return make_value(value_tag::string, ptr);
  }

  auto count = ctx.read_u32(list_ptr + mem::LIST_COUNT_OFFSET);
  std::string result;

  for (std::uint32_t i = 0; i < count; ++i) {
    if (i > 0) {
      result += sep_str;
    }
    auto elem = ctx.read_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
    elem = rt_force(ctx, elem);
    if (!is_string(elem)) {
      throw type_error("builtins.concatStringsSep: all list elements must be strings, got '" +
                       std::string(type_name(elem)) + "'");
    }
    result += ctx.read_string(get_payload(elem));
  }

  auto ptr = allocate_string(ctx, result);
  return make_value(value_tag::string, ptr);
}

// =============================================================================
// List Builtins (additional)
// =============================================================================

auto rt_all(runtime_context& ctx, nix_value pred, nix_value list) -> nix_value {
  list = rt_force(ctx, list);

  if (!is_list(list)) {
    throw type_error("builtins.all: expected list, got '" + std::string(type_name(list)) + "'");
  }

  auto list_ptr = get_payload(list);
  if (list_ptr == 0) {
    return constants::bool_true; // vacuously true
  }

  auto count = ctx.read_u32(list_ptr + mem::LIST_COUNT_OFFSET);
  for (std::uint32_t i = 0; i < count; ++i) {
    auto elem = ctx.read_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
    auto result = rt_apply(ctx, pred, elem);
    result = rt_force(ctx, result);
    if (!is_bool(result)) {
      throw type_error("builtins.all: predicate must return bool");
    }
    if (result == constants::bool_false) {
      return constants::bool_false;
    }
  }

  return constants::bool_true;
}

auto rt_any(runtime_context& ctx, nix_value pred, nix_value list) -> nix_value {
  list = rt_force(ctx, list);

  if (!is_list(list)) {
    throw type_error("builtins.any: expected list, got '" + std::string(type_name(list)) + "'");
  }

  auto list_ptr = get_payload(list);
  if (list_ptr == 0) {
    return constants::bool_false; // vacuously false
  }

  auto count = ctx.read_u32(list_ptr + mem::LIST_COUNT_OFFSET);
  for (std::uint32_t i = 0; i < count; ++i) {
    auto elem = ctx.read_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
    auto result = rt_apply(ctx, pred, elem);
    result = rt_force(ctx, result);
    if (!is_bool(result)) {
      throw type_error("builtins.any: predicate must return bool");
    }
    if (result == constants::bool_true) {
      return constants::bool_true;
    }
  }

  return constants::bool_false;
}

auto rt_concat_map(runtime_context& ctx, nix_value f, nix_value list) -> nix_value {
  // concatMap f list = concatLists (map f list)
  auto mapped = rt_map(ctx, f, list);
  return rt_concat_lists(ctx, mapped);
}

auto rt_list_to_attrs(runtime_context& ctx, nix_value list) -> nix_value {
  list = rt_force(ctx, list);

  if (!is_list(list)) {
    throw type_error("builtins.listToAttrs: expected list, got '" + std::string(type_name(list)) +
                     "'");
  }

  auto list_ptr = get_payload(list);
  if (list_ptr == 0) {
    return make_value(value_tag::attribute_set, 0);
  }

  auto count = ctx.read_u32(list_ptr + mem::LIST_COUNT_OFFSET);

  // Collect {name, value} pairs - later entries override earlier
  std::vector<std::pair<std::string, nix_value>> entries;
  std::unordered_set<std::string> seen;

  for (std::uint32_t i = 0; i < count; ++i) {
    auto elem = ctx.read_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
    elem = rt_force(ctx, elem);

    if (!is_attrset(elem)) {
      throw type_error("builtins.listToAttrs: elements must be attrsets with 'name' and 'value'");
    }

    auto elem_ptr = get_payload(elem);
    auto name_val = find_attr(ctx, elem_ptr, "name");
    auto value_val = find_attr(ctx, elem_ptr, "value");

    if (!name_val.has_value()) {
      throw attr_error("builtins.listToAttrs: element missing 'name' attribute");
    }
    if (!value_val.has_value()) {
      throw attr_error("builtins.listToAttrs: element missing 'value' attribute");
    }

    auto name = rt_force(ctx, *name_val);
    if (!is_string(name)) {
      throw type_error("builtins.listToAttrs: 'name' must be string");
    }

    auto key = std::string(ctx.read_string(get_payload(name)));

    // First occurrence wins (Nix semantics)
    if (seen.find(key) == seen.end()) {
      seen.insert(key);
      entries.emplace_back(key, *value_val);
    }
  }

  if (entries.empty()) {
    return make_value(value_tag::attribute_set, 0);
  }

  // Allocate attrset
  auto new_count = static_cast<std::uint32_t>(entries.size());
  auto new_size = mem::attrset_size(new_count);
  auto new_ptr = ctx.allocate(new_size);
  ctx.write_i32(new_ptr + mem::ATTRSET_COUNT_OFFSET, static_cast<std::int32_t>(new_count));

  for (std::uint32_t i = 0; i < new_count; ++i) {
    auto& [key, value] = entries[i];
    auto key_ptr = allocate_string(ctx, key);
    auto entry = new_ptr + mem::ATTRSET_ENTRIES_OFFSET + i * mem::ATTRSET_ENTRY_SIZE;
    ctx.write_i32(entry + mem::ATTRSET_ENTRY_KEY_OFFSET, static_cast<std::int32_t>(key_ptr));
    ctx.write_value(entry + mem::ATTRSET_ENTRY_VALUE_OFFSET, value);
  }

  return make_value(value_tag::attribute_set, new_ptr);
}

auto rt_map_attrs(runtime_context& ctx, nix_value f, nix_value set) -> nix_value {
  set = rt_force(ctx, set);

  if (!is_attrset(set)) {
    throw type_error("builtins.mapAttrs: expected set, got '" + std::string(type_name(set)) + "'");
  }

  auto attrs_ptr = get_payload(set);
  if (attrs_ptr == 0) {
    return make_value(value_tag::attribute_set, 0);
  }

  auto count = ctx.read_u32(attrs_ptr + mem::ATTRSET_COUNT_OFFSET);
  if (count == 0) {
    return make_value(value_tag::attribute_set, 0);
  }

  // Collect entries and apply function to each
  std::vector<std::pair<std::string, nix_value>> results;
  results.reserve(count);

  for (std::uint32_t i = 0; i < count; ++i) {
    auto entry = attrs_ptr + mem::ATTRSET_ENTRIES_OFFSET + i * mem::ATTRSET_ENTRY_SIZE;
    auto key_offset = ctx.read_u32(entry + mem::ATTRSET_ENTRY_KEY_OFFSET);
    auto value = ctx.read_value(entry + mem::ATTRSET_ENTRY_VALUE_OFFSET);
    auto key_str = std::string(ctx.read_string(key_offset));

    // Apply f to name then to value: (f name value)
    auto name_ptr = allocate_string(ctx, key_str);
    auto name_val = make_value(value_tag::string, name_ptr);
    auto partial = rt_apply(ctx, f, name_val);
    auto result = rt_apply(ctx, partial, value);

    results.emplace_back(std::move(key_str), result);
  }

  // Allocate new attrset
  auto new_size = mem::attrset_size(count);
  auto new_ptr = ctx.allocate(new_size);
  ctx.write_i32(new_ptr + mem::ATTRSET_COUNT_OFFSET, static_cast<std::int32_t>(count));

  for (std::uint32_t i = 0; i < count; ++i) {
    auto& [key, value] = results[i];
    auto key_ptr = allocate_string(ctx, key);
    auto entry = new_ptr + mem::ATTRSET_ENTRIES_OFFSET + i * mem::ATTRSET_ENTRY_SIZE;
    ctx.write_i32(entry + mem::ATTRSET_ENTRY_KEY_OFFSET, static_cast<std::int32_t>(key_ptr));
    ctx.write_value(entry + mem::ATTRSET_ENTRY_VALUE_OFFSET, value);
  }

  return make_value(value_tag::attribute_set, new_ptr);
}

auto rt_cat_attrs(runtime_context& ctx, nix_value name, nix_value list) -> nix_value {
  name = rt_force(ctx, name);
  list = rt_force(ctx, list);

  if (!is_string(name)) {
    throw type_error("builtins.catAttrs: first argument must be a string, got '" +
                     std::string(type_name(name)) + "'");
  }
  if (!is_list(list)) {
    throw type_error("builtins.catAttrs: second argument must be a list, got '" +
                     std::string(type_name(list)) + "'");
  }

  auto name_str = std::string(ctx.read_string(get_payload(name)));

  auto list_ptr = get_payload(list);
  if (list_ptr == 0) {
    return make_value(value_tag::list, 0);
  }

  auto count = ctx.read_u32(list_ptr + mem::LIST_COUNT_OFFSET);

  // Collect values from all attrsets that have the named attribute
  std::vector<nix_value> values;

  for (std::uint32_t i = 0; i < count; ++i) {
    auto elem = ctx.read_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
    elem = rt_force(ctx, elem);

    if (!is_attrset(elem)) {
      throw type_error("builtins.catAttrs: list elements must be attrsets");
    }

    auto elem_ptr = get_payload(elem);
    if (elem_ptr == 0) {
      continue;
    }

    auto attr_val = find_attr(ctx, elem_ptr, name_str);
    if (attr_val.has_value()) {
      values.push_back(*attr_val);
    }
  }

  if (values.empty()) {
    return make_value(value_tag::list, 0);
  }

  // Allocate result list
  auto result_count = static_cast<std::uint32_t>(values.size());
  auto result_size = mem::list_size(result_count);
  auto result_ptr = ctx.allocate(result_size);
  ctx.write_i32(result_ptr + mem::LIST_COUNT_OFFSET, static_cast<std::int32_t>(result_count));

  for (std::uint32_t i = 0; i < result_count; ++i) {
    ctx.write_value(result_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE, values[i]);
  }

  return make_value(value_tag::list, result_ptr);
}

auto rt_partition(runtime_context& ctx, nix_value pred, nix_value list) -> nix_value {
  list = rt_force(ctx, list);

  if (!is_list(list)) {
    throw type_error("builtins.partition: expected list, got '" + std::string(type_name(list)) +
                     "'");
  }

  std::vector<nix_value> right_elems;
  std::vector<nix_value> wrong_elems;

  auto list_ptr = get_payload(list);
  if (list_ptr != 0) {
    auto count = ctx.read_u32(list_ptr + mem::LIST_COUNT_OFFSET);
    for (std::uint32_t i = 0; i < count; ++i) {
      auto elem = ctx.read_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
      auto result = rt_apply(ctx, pred, elem);
      result = rt_force(ctx, result);
      if (!is_bool(result)) {
        throw type_error("builtins.partition: predicate must return bool");
      }
      if (result == constants::bool_true) {
        right_elems.push_back(elem);
      } else {
        wrong_elems.push_back(elem);
      }
    }
  }

  // Create the two lists
  auto make_list = [&ctx](const std::vector<nix_value>& elems) -> nix_value {
    if (elems.empty()) {
      return make_value(value_tag::list, 0);
    }
    auto count = static_cast<std::uint32_t>(elems.size());
    auto list_size = mem::list_size(count);
    auto list_ptr = ctx.allocate(list_size);
    ctx.write_i32(list_ptr + mem::LIST_COUNT_OFFSET, static_cast<std::int32_t>(count));
    for (std::uint32_t i = 0; i < count; ++i) {
      ctx.write_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE, elems[i]);
    }
    return make_value(value_tag::list, list_ptr);
  };

  auto right_list = make_list(right_elems);
  auto wrong_list = make_list(wrong_elems);

  // Create result attrset { right = ...; wrong = ...; }
  auto attrs_size = mem::attrset_size(2);
  auto attrs_ptr = ctx.allocate(attrs_size);
  ctx.write_i32(attrs_ptr + mem::ATTRSET_COUNT_OFFSET, 2);

  auto right_key = allocate_string(ctx, "right");
  auto wrong_key = allocate_string(ctx, "wrong");

  auto entry0 = attrs_ptr + mem::ATTRSET_ENTRIES_OFFSET;
  ctx.write_i32(entry0 + mem::ATTRSET_ENTRY_KEY_OFFSET, static_cast<std::int32_t>(right_key));
  ctx.write_value(entry0 + mem::ATTRSET_ENTRY_VALUE_OFFSET, right_list);

  auto entry1 = attrs_ptr + mem::ATTRSET_ENTRIES_OFFSET + mem::ATTRSET_ENTRY_SIZE;
  ctx.write_i32(entry1 + mem::ATTRSET_ENTRY_KEY_OFFSET, static_cast<std::int32_t>(wrong_key));
  ctx.write_value(entry1 + mem::ATTRSET_ENTRY_VALUE_OFFSET, wrong_list);

  return make_value(value_tag::attribute_set, attrs_ptr);
}

auto rt_group_by(runtime_context& ctx, nix_value f, nix_value list) -> nix_value {
  list = rt_force(ctx, list);

  if (!is_list(list)) {
    throw type_error("builtins.groupBy: expected list, got '" + std::string(type_name(list)) + "'");
  }

  // Map from key to list of elements
  std::map<std::string, std::vector<nix_value>> groups;

  auto list_ptr = get_payload(list);
  if (list_ptr != 0) {
    auto count = ctx.read_u32(list_ptr + mem::LIST_COUNT_OFFSET);
    for (std::uint32_t i = 0; i < count; ++i) {
      auto elem = ctx.read_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
      auto key_val = rt_apply(ctx, f, elem);
      key_val = rt_force(ctx, key_val);
      if (!is_string(key_val)) {
        throw type_error("builtins.groupBy: function must return string");
      }
      auto key = std::string(ctx.read_string(get_payload(key_val)));
      groups[key].push_back(elem);
    }
  }

  if (groups.empty()) {
    return make_value(value_tag::attribute_set, 0);
  }

  // Create result attrset
  auto group_count = static_cast<std::uint32_t>(groups.size());
  auto attrs_size = mem::attrset_size(group_count);
  auto attrs_ptr = ctx.allocate(attrs_size);
  ctx.write_i32(attrs_ptr + mem::ATTRSET_COUNT_OFFSET, static_cast<std::int32_t>(group_count));

  std::uint32_t idx = 0;
  for (auto& [key, elems] : groups) {
    // Create list for this group
    auto elem_count = static_cast<std::uint32_t>(elems.size());
    auto elem_list_size = mem::list_size(elem_count);
    auto elem_list_ptr = ctx.allocate(elem_list_size);
    ctx.write_i32(elem_list_ptr + mem::LIST_COUNT_OFFSET, static_cast<std::int32_t>(elem_count));
    for (std::uint32_t j = 0; j < elem_count; ++j) {
      ctx.write_value(elem_list_ptr + mem::LIST_ELEMENTS_OFFSET + j * mem::VALUE_SIZE, elems[j]);
    }

    auto key_ptr = allocate_string(ctx, key);
    auto entry = attrs_ptr + mem::ATTRSET_ENTRIES_OFFSET + idx * mem::ATTRSET_ENTRY_SIZE;
    ctx.write_i32(entry + mem::ATTRSET_ENTRY_KEY_OFFSET, static_cast<std::int32_t>(key_ptr));
    ctx.write_value(entry + mem::ATTRSET_ENTRY_VALUE_OFFSET,
                    make_value(value_tag::list, elem_list_ptr));
    ++idx;
  }

  return make_value(value_tag::attribute_set, attrs_ptr);
}

auto rt_reverse(runtime_context& ctx, nix_value list) -> nix_value {
  list = rt_force(ctx, list);

  if (!is_list(list)) {
    throw type_error("builtins.reverse: expected list, got '" + std::string(type_name(list)) + "'");
  }

  auto list_ptr = get_payload(list);
  if (list_ptr == 0) {
    return make_value(value_tag::list, 0);
  }

  auto count = ctx.read_u32(list_ptr + mem::LIST_COUNT_OFFSET);
  if (count <= 1) {
    return list; // empty or single element list is its own reverse
  }

  // Allocate result list
  auto result_size = mem::list_size(count);
  auto result_ptr = ctx.allocate(result_size);
  ctx.write_i32(result_ptr + mem::LIST_COUNT_OFFSET, static_cast<std::int32_t>(count));

  // Copy elements in reverse order
  for (std::uint32_t i = 0; i < count; ++i) {
    auto elem = ctx.read_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
    ctx.write_value(result_ptr + mem::LIST_ELEMENTS_OFFSET + (count - 1 - i) * mem::VALUE_SIZE,
                    elem);
  }

  return make_value(value_tag::list, result_ptr);
}

auto rt_take(runtime_context& ctx, nix_value n, nix_value list) -> nix_value {
  n = rt_force(ctx, n);
  list = rt_force(ctx, list);

  if (!is_int(n)) {
    throw type_error("builtins.take: first argument must be int, got '" +
                     std::string(type_name(n)) + "'");
  }
  if (!is_list(list)) {
    throw type_error("builtins.take: second argument must be list, got '" +
                     std::string(type_name(list)) + "'");
  }

  auto take_count = static_cast<std::int32_t>(get_payload(n));
  if (take_count <= 0) {
    return make_value(value_tag::list, 0);
  }

  auto list_ptr = get_payload(list);
  if (list_ptr == 0) {
    return make_value(value_tag::list, 0);
  }

  auto list_count = ctx.read_u32(list_ptr + mem::LIST_COUNT_OFFSET);
  auto actual_count = std::min(static_cast<std::uint32_t>(take_count), list_count);

  if (actual_count == 0) {
    return make_value(value_tag::list, 0);
  }

  // Allocate result list
  auto result_size = mem::list_size(actual_count);
  auto result_ptr = ctx.allocate(result_size);
  ctx.write_i32(result_ptr + mem::LIST_COUNT_OFFSET, static_cast<std::int32_t>(actual_count));

  // Copy first n elements
  for (std::uint32_t i = 0; i < actual_count; ++i) {
    auto elem = ctx.read_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
    ctx.write_value(result_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE, elem);
  }

  return make_value(value_tag::list, result_ptr);
}

auto rt_drop(runtime_context& ctx, nix_value n, nix_value list) -> nix_value {
  n = rt_force(ctx, n);
  list = rt_force(ctx, list);

  if (!is_int(n)) {
    throw type_error("builtins.drop: first argument must be int, got '" +
                     std::string(type_name(n)) + "'");
  }
  if (!is_list(list)) {
    throw type_error("builtins.drop: second argument must be list, got '" +
                     std::string(type_name(list)) + "'");
  }

  auto drop_count = static_cast<std::int32_t>(get_payload(n));
  if (drop_count < 0) {
    drop_count = 0;
  }

  auto list_ptr = get_payload(list);
  if (list_ptr == 0) {
    return make_value(value_tag::list, 0);
  }

  auto list_count = ctx.read_u32(list_ptr + mem::LIST_COUNT_OFFSET);
  if (static_cast<std::uint32_t>(drop_count) >= list_count) {
    return make_value(value_tag::list, 0);
  }

  auto remaining = list_count - static_cast<std::uint32_t>(drop_count);

  // Allocate result list
  auto result_size = mem::list_size(remaining);
  auto result_ptr = ctx.allocate(result_size);
  ctx.write_i32(result_ptr + mem::LIST_COUNT_OFFSET, static_cast<std::int32_t>(remaining));

  // Copy elements starting from drop_count
  for (std::uint32_t i = 0; i < remaining; ++i) {
    auto elem = ctx.read_value(list_ptr + mem::LIST_ELEMENTS_OFFSET +
                               (i + static_cast<std::uint32_t>(drop_count)) * mem::VALUE_SIZE);
    ctx.write_value(result_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE, elem);
  }

  return make_value(value_tag::list, result_ptr);
}

auto rt_range(runtime_context& ctx, nix_value a, nix_value b) -> nix_value {
  a = rt_force(ctx, a);
  b = rt_force(ctx, b);

  if (!is_int(a)) {
    throw type_error("builtins.range: first argument must be int, got '" +
                     std::string(type_name(a)) + "'");
  }
  if (!is_int(b)) {
    throw type_error("builtins.range: second argument must be int, got '" +
                     std::string(type_name(b)) + "'");
  }

  auto start = static_cast<std::int32_t>(get_payload(a));
  auto end = static_cast<std::int32_t>(get_payload(b));

  if (start > end) {
    return make_value(value_tag::list, 0);
  }

  auto count = static_cast<std::uint32_t>(end - start + 1);

  // Allocate result list
  auto result_size = mem::list_size(count);
  auto result_ptr = ctx.allocate(result_size);
  ctx.write_i32(result_ptr + mem::LIST_COUNT_OFFSET, static_cast<std::int32_t>(count));

  // Generate integers from start to end inclusive
  for (std::uint32_t i = 0; i < count; ++i) {
    auto val = make_value(value_tag::integer,
                          static_cast<std::uint32_t>(start + static_cast<std::int32_t>(i)));
    ctx.write_value(result_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE, val);
  }

  return make_value(value_tag::list, result_ptr);
}

auto rt_zip_lists(runtime_context& ctx, nix_value list1, nix_value list2) -> nix_value {
  list1 = rt_force(ctx, list1);
  list2 = rt_force(ctx, list2);

  if (!is_list(list1)) {
    throw type_error("builtins.zipLists: first argument must be list, got '" +
                     std::string(type_name(list1)) + "'");
  }
  if (!is_list(list2)) {
    throw type_error("builtins.zipLists: second argument must be list, got '" +
                     std::string(type_name(list2)) + "'");
  }

  auto ptr1 = get_payload(list1);
  auto ptr2 = get_payload(list2);

  std::uint32_t count1 = 0;
  std::uint32_t count2 = 0;

  if (ptr1 != 0) {
    count1 = ctx.read_u32(ptr1 + mem::LIST_COUNT_OFFSET);
  }
  if (ptr2 != 0) {
    count2 = ctx.read_u32(ptr2 + mem::LIST_COUNT_OFFSET);
  }

  auto min_count = std::min(count1, count2);

  if (min_count == 0) {
    return make_value(value_tag::list, 0);
  }

  // Allocate result list
  auto result_size = mem::list_size(min_count);
  auto result_ptr = ctx.allocate(result_size);
  ctx.write_i32(result_ptr + mem::LIST_COUNT_OFFSET, static_cast<std::int32_t>(min_count));

  // Pre-allocate "fst" and "snd" strings once
  auto fst_key = allocate_string(ctx, "fst");
  auto snd_key = allocate_string(ctx, "snd");

  // Create {fst, snd} attrsets for each pair
  for (std::uint32_t i = 0; i < min_count; ++i) {
    auto elem1 = ctx.read_value(ptr1 + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
    auto elem2 = ctx.read_value(ptr2 + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);

    // Create attrset with 2 entries
    auto attrs_size = mem::attrset_size(2);
    auto attrs_ptr = ctx.allocate(attrs_size);
    ctx.write_i32(attrs_ptr + mem::ATTRSET_COUNT_OFFSET, 2);

    auto entry0 = attrs_ptr + mem::ATTRSET_ENTRIES_OFFSET;
    ctx.write_i32(entry0 + mem::ATTRSET_ENTRY_KEY_OFFSET, static_cast<std::int32_t>(fst_key));
    ctx.write_value(entry0 + mem::ATTRSET_ENTRY_VALUE_OFFSET, elem1);

    auto entry1 = attrs_ptr + mem::ATTRSET_ENTRIES_OFFSET + mem::ATTRSET_ENTRY_SIZE;
    ctx.write_i32(entry1 + mem::ATTRSET_ENTRY_KEY_OFFSET, static_cast<std::int32_t>(snd_key));
    ctx.write_value(entry1 + mem::ATTRSET_ENTRY_VALUE_OFFSET, elem2);

    ctx.write_value(result_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE,
                    make_value(value_tag::attribute_set, attrs_ptr));
  }

  return make_value(value_tag::list, result_ptr);
}

auto rt_intersect_attrs(runtime_context& ctx, nix_value a, nix_value b) -> nix_value {
  a = rt_force(ctx, a);
  b = rt_force(ctx, b);

  if (!is_attrset(a)) {
    throw type_error("builtins.intersectAttrs: first argument must be a set, got '" +
                     std::string(type_name(a)) + "'");
  }
  if (!is_attrset(b)) {
    throw type_error("builtins.intersectAttrs: second argument must be a set, got '" +
                     std::string(type_name(b)) + "'");
  }

  auto a_ptr = get_payload(a);
  auto b_ptr = get_payload(b);

  if (a_ptr == 0 || b_ptr == 0) {
    return make_value(value_tag::attribute_set, 0);
  }

  auto a_count = ctx.read_u32(a_ptr + mem::ATTRSET_COUNT_OFFSET);
  auto b_count = ctx.read_u32(b_ptr + mem::ATTRSET_COUNT_OFFSET);

  // Build set of keys from a
  std::unordered_set<std::string> a_keys;
  for (std::uint32_t i = 0; i < a_count; ++i) {
    auto entry = a_ptr + mem::ATTRSET_ENTRIES_OFFSET + i * mem::ATTRSET_ENTRY_SIZE;
    auto key_offset = ctx.read_u32(entry + mem::ATTRSET_ENTRY_KEY_OFFSET);
    a_keys.insert(std::string(ctx.read_string(key_offset)));
  }

  // Collect entries from b that exist in a
  std::vector<std::pair<std::string, nix_value>> result;
  for (std::uint32_t i = 0; i < b_count; ++i) {
    auto entry = b_ptr + mem::ATTRSET_ENTRIES_OFFSET + i * mem::ATTRSET_ENTRY_SIZE;
    auto key_offset = ctx.read_u32(entry + mem::ATTRSET_ENTRY_KEY_OFFSET);
    auto key = std::string(ctx.read_string(key_offset));
    if (a_keys.count(key) > 0) {
      auto value = ctx.read_value(entry + mem::ATTRSET_ENTRY_VALUE_OFFSET);
      result.emplace_back(std::move(key), value);
    }
  }

  if (result.empty()) {
    return make_value(value_tag::attribute_set, 0);
  }

  auto count = static_cast<std::uint32_t>(result.size());
  auto attrs_size = mem::attrset_size(count);
  auto attrs_ptr = ctx.allocate(attrs_size);
  ctx.write_i32(attrs_ptr + mem::ATTRSET_COUNT_OFFSET, static_cast<std::int32_t>(count));

  for (std::uint32_t i = 0; i < count; ++i) {
    auto& [key, value] = result[i];
    auto key_ptr = allocate_string(ctx, key);
    auto entry = attrs_ptr + mem::ATTRSET_ENTRIES_OFFSET + i * mem::ATTRSET_ENTRY_SIZE;
    ctx.write_i32(entry + mem::ATTRSET_ENTRY_KEY_OFFSET, static_cast<std::int32_t>(key_ptr));
    ctx.write_value(entry + mem::ATTRSET_ENTRY_VALUE_OFFSET, value);
  }

  return make_value(value_tag::attribute_set, attrs_ptr);
}

auto rt_function_args(runtime_context& ctx, nix_value f) -> nix_value {
  f = rt_force(ctx, f);

  // For now, return empty set - full implementation would need closure introspection
  // to extract the formal argument names and default value info
  // This is sufficient for basic usage patterns
  if (!is_lambda(f) && !is_primop(f)) {
    throw type_error("builtins.functionArgs: expected function, got '" + std::string(type_name(f)) +
                     "'");
  }

  // Return empty attrset for now
  return make_value(value_tag::attribute_set, 0);
}

auto rt_get_env(runtime_context& ctx, nix_value name) -> nix_value {
  name = rt_force(ctx, name);

  if (!is_string(name)) {
    throw type_error("builtins.getEnv: expected string, got '" + std::string(type_name(name)) +
                     "'");
  }

  auto name_str = std::string(ctx.read_string(get_payload(name)));
  const char* val = std::getenv(name_str.c_str());

  if (val == nullptr) {
    // Return empty string for unset variables
    auto ptr = allocate_string(ctx, "");
    return make_value(value_tag::string, ptr);
  }

  auto ptr = allocate_string(ctx, val);
  return make_value(value_tag::string, ptr);
}

auto rt_to_lower(runtime_context& ctx, nix_value s) -> nix_value {
  s = rt_force(ctx, s);

  if (!is_string(s)) {
    throw type_error("builtins.toLower: expected string, got '" + std::string(type_name(s)) + "'");
  }

  auto str = std::string(ctx.read_string(get_payload(s)));
  for (auto& c : str) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }

  auto ptr = allocate_string(ctx, str);
  return make_value(value_tag::string, ptr);
}

auto rt_to_upper(runtime_context& ctx, nix_value s) -> nix_value {
  s = rt_force(ctx, s);

  if (!is_string(s)) {
    throw type_error("builtins.toUpper: expected string, got '" + std::string(type_name(s)) + "'");
  }

  auto str = std::string(ctx.read_string(get_payload(s)));
  for (auto& c : str) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }

  auto ptr = allocate_string(ctx, str);
  return make_value(value_tag::string, ptr);
}

// Compare version strings: -1 if a < b, 0 if a == b, 1 if a > b
auto rt_compare_versions(runtime_context& ctx, nix_value a, nix_value b) -> nix_value {
  a = rt_force(ctx, a);
  b = rt_force(ctx, b);

  if (!is_string(a)) {
    throw type_error("builtins.compareVersions: first argument must be string, got '" +
                     std::string(type_name(a)) + "'");
  }
  if (!is_string(b)) {
    throw type_error("builtins.compareVersions: second argument must be string, got '" +
                     std::string(type_name(b)) + "'");
  }

  auto va = std::string(ctx.read_string(get_payload(a)));
  auto vb = std::string(ctx.read_string(get_payload(b)));

  // Simple version comparison: split by '.' and compare components
  auto split_version = [](const std::string& ver) -> std::vector<std::string> {
    std::vector<std::string> parts;
    std::string current;
    for (char c : ver) {
      if (c == '.' || c == '-') {
        if (!current.empty()) {
          parts.push_back(current);
          current.clear();
        }
      } else {
        current += c;
      }
    }
    if (!current.empty()) {
      parts.push_back(current);
    }
    return parts;
  };

  auto compare_part = [](const std::string& pa, const std::string& pb) -> int {
    // Treat empty as "0" for numeric comparison
    auto a_str = pa.empty() ? "0" : pa;
    auto b_str = pb.empty() ? "0" : pb;

    // Try numeric comparison first
    bool a_numeric = std::all_of(a_str.begin(), a_str.end(), ::isdigit);
    bool b_numeric = std::all_of(b_str.begin(), b_str.end(), ::isdigit);

    if (a_numeric && b_numeric) {
      auto na = std::stol(a_str);
      auto nb = std::stol(b_str);
      if (na < nb)
        return -1;
      if (na > nb)
        return 1;
      return 0;
    }

    // Fall back to string comparison
    if (a_str < b_str)
      return -1;
    if (a_str > b_str)
      return 1;
    return 0;
  };

  auto parts_a = split_version(va);
  auto parts_b = split_version(vb);

  auto max_len = std::max(parts_a.size(), parts_b.size());
  for (std::size_t i = 0; i < max_len; ++i) {
    auto pa = i < parts_a.size() ? parts_a[i] : "";
    auto pb = i < parts_b.size() ? parts_b[i] : "";
    auto cmp = compare_part(pa, pb);
    if (cmp != 0) {
      return make_value(value_tag::integer, static_cast<std::uint32_t>(cmp));
    }
  }

  return make_value(value_tag::integer, 0);
}

auto rt_split_version(runtime_context& ctx, nix_value v) -> nix_value {
  v = rt_force(ctx, v);

  if (!is_string(v)) {
    throw type_error("builtins.splitVersion: expected string, got '" + std::string(type_name(v)) +
                     "'");
  }

  auto ver = std::string(ctx.read_string(get_payload(v)));

  // Split by '.' and '-'
  std::vector<std::string> parts;
  std::string current;
  for (char c : ver) {
    if (c == '.' || c == '-') {
      if (!current.empty()) {
        parts.push_back(current);
        current.clear();
      }
    } else {
      current += c;
    }
  }
  if (!current.empty()) {
    parts.push_back(current);
  }

  if (parts.empty()) {
    return make_value(value_tag::list, 0);
  }

  auto count = static_cast<std::uint32_t>(parts.size());
  auto list_size = mem::list_size(count);
  auto list_ptr = ctx.allocate(list_size);
  ctx.write_i32(list_ptr + mem::LIST_COUNT_OFFSET, static_cast<std::int32_t>(count));

  for (std::uint32_t i = 0; i < count; ++i) {
    auto str_ptr = allocate_string(ctx, parts[i]);
    ctx.write_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE,
                    make_value(value_tag::string, str_ptr));
  }

  return make_value(value_tag::list, list_ptr);
}

auto rt_parse_drv_name(runtime_context& ctx, nix_value s) -> nix_value {
  s = rt_force(ctx, s);

  if (!is_string(s)) {
    throw type_error("builtins.parseDrvName: expected string, got '" + std::string(type_name(s)) +
                     "'");
  }

  auto name = std::string(ctx.read_string(get_payload(s)));

  // Find the last dash followed by a version (starts with digit)
  std::string pkg_name = name;
  std::string version = "";

  auto last_dash = name.rfind('-');
  while (last_dash != std::string::npos && last_dash > 0) {
    if (last_dash + 1 < name.size() && std::isdigit(name[last_dash + 1])) {
      pkg_name = name.substr(0, last_dash);
      version = name.substr(last_dash + 1);
      break;
    }
    last_dash = name.rfind('-', last_dash - 1);
  }

  // Create result attrset { name = ...; version = ...; }
  auto attrs_size = mem::attrset_size(2);
  auto attrs_ptr = ctx.allocate(attrs_size);
  ctx.write_i32(attrs_ptr + mem::ATTRSET_COUNT_OFFSET, 2);

  auto name_key = allocate_string(ctx, "name");
  auto version_key = allocate_string(ctx, "version");
  auto name_val_ptr = allocate_string(ctx, pkg_name);
  auto version_val_ptr = allocate_string(ctx, version);

  auto entry0 = attrs_ptr + mem::ATTRSET_ENTRIES_OFFSET;
  ctx.write_i32(entry0 + mem::ATTRSET_ENTRY_KEY_OFFSET, static_cast<std::int32_t>(name_key));
  ctx.write_value(entry0 + mem::ATTRSET_ENTRY_VALUE_OFFSET,
                  make_value(value_tag::string, name_val_ptr));

  auto entry1 = attrs_ptr + mem::ATTRSET_ENTRIES_OFFSET + mem::ATTRSET_ENTRY_SIZE;
  ctx.write_i32(entry1 + mem::ATTRSET_ENTRY_KEY_OFFSET, static_cast<std::int32_t>(version_key));
  ctx.write_value(entry1 + mem::ATTRSET_ENTRY_VALUE_OFFSET,
                  make_value(value_tag::string, version_val_ptr));

  return make_value(value_tag::attribute_set, attrs_ptr);
}

auto rt_base_name_of(runtime_context& ctx, nix_value s) -> nix_value {
  s = rt_force(ctx, s);

  if (!is_string(s) && !is_path(s)) {
    throw type_error("builtins.baseNameOf: expected string or path, got '" +
                     std::string(type_name(s)) + "'");
  }

  auto path = std::string(ctx.read_string(get_payload(s)));

  // Find last '/'
  auto last_slash = path.rfind('/');
  std::string basename;
  if (last_slash == std::string::npos) {
    basename = path;
  } else {
    basename = path.substr(last_slash + 1);
  }

  auto ptr = allocate_string(ctx, basename);
  return make_value(value_tag::string, ptr);
}

auto rt_dir_of(runtime_context& ctx, nix_value s) -> nix_value {
  s = rt_force(ctx, s);

  if (!is_string(s) && !is_path(s)) {
    throw type_error("builtins.dirOf: expected string or path, got '" + std::string(type_name(s)) +
                     "'");
  }

  auto path = std::string(ctx.read_string(get_payload(s)));

  // Find last '/'
  auto last_slash = path.rfind('/');
  std::string dirname;
  if (last_slash == std::string::npos) {
    dirname = ".";
  } else if (last_slash == 0) {
    dirname = "/";
  } else {
    dirname = path.substr(0, last_slash);
  }

  auto ptr = allocate_string(ctx, dirname);
  return make_value(is_path(s) ? value_tag::path : value_tag::string, ptr);
}

auto rt_has_prefix(runtime_context& ctx, nix_value prefix, nix_value str) -> nix_value {
  prefix = rt_force(ctx, prefix);
  str = rt_force(ctx, str);

  if (!is_string(prefix)) {
    throw type_error("builtins.hasPrefix: first argument must be a string, got '" +
                     std::string(type_name(prefix)) + "'");
  }
  if (!is_string(str)) {
    throw type_error("builtins.hasPrefix: second argument must be a string, got '" +
                     std::string(type_name(str)) + "'");
  }

  auto prefix_str = ctx.read_string(get_payload(prefix));
  auto str_str = ctx.read_string(get_payload(str));

  bool result =
      str_str.size() >= prefix_str.size() && str_str.compare(0, prefix_str.size(), prefix_str) == 0;
  return result ? constants::bool_true : constants::bool_false;
}

auto rt_has_suffix(runtime_context& ctx, nix_value suffix, nix_value str) -> nix_value {
  suffix = rt_force(ctx, suffix);
  str = rt_force(ctx, str);

  if (!is_string(suffix)) {
    throw type_error("builtins.hasSuffix: first argument must be a string, got '" +
                     std::string(type_name(suffix)) + "'");
  }
  if (!is_string(str)) {
    throw type_error("builtins.hasSuffix: second argument must be a string, got '" +
                     std::string(type_name(str)) + "'");
  }

  auto suffix_str = ctx.read_string(get_payload(suffix));
  auto str_str = ctx.read_string(get_payload(str));

  bool result =
      str_str.size() >= suffix_str.size() &&
      str_str.compare(str_str.size() - suffix_str.size(), suffix_str.size(), suffix_str) == 0;
  return result ? constants::bool_true : constants::bool_false;
}

auto rt_remove_prefix(runtime_context& ctx, nix_value prefix, nix_value str) -> nix_value {
  prefix = rt_force(ctx, prefix);
  str = rt_force(ctx, str);

  if (!is_string(prefix)) {
    throw type_error("builtins.removePrefix: first argument must be a string, got '" +
                     std::string(type_name(prefix)) + "'");
  }
  if (!is_string(str)) {
    throw type_error("builtins.removePrefix: second argument must be a string, got '" +
                     std::string(type_name(str)) + "'");
  }

  auto prefix_str = ctx.read_string(get_payload(prefix));
  auto str_str = ctx.read_string(get_payload(str));

  // If string has the prefix, remove it; otherwise return the original string
  if (str_str.size() >= prefix_str.size() &&
      str_str.compare(0, prefix_str.size(), prefix_str) == 0) {
    auto result = str_str.substr(prefix_str.size());
    auto ptr = allocate_string(ctx, result);
    return make_value(value_tag::string, ptr);
  }

  // No prefix match - return the original string
  return str;
}

auto rt_remove_suffix(runtime_context& ctx, nix_value suffix, nix_value str) -> nix_value {
  suffix = rt_force(ctx, suffix);
  str = rt_force(ctx, str);

  if (!is_string(suffix)) {
    throw type_error("builtins.removeSuffix: first argument must be a string, got '" +
                     std::string(type_name(suffix)) + "'");
  }
  if (!is_string(str)) {
    throw type_error("builtins.removeSuffix: second argument must be a string, got '" +
                     std::string(type_name(str)) + "'");
  }

  auto suffix_str = ctx.read_string(get_payload(suffix));
  auto str_str = ctx.read_string(get_payload(str));

  // If string has the suffix, remove it; otherwise return the original string
  if (str_str.size() >= suffix_str.size() &&
      str_str.compare(str_str.size() - suffix_str.size(), suffix_str.size(), suffix_str) == 0) {
    auto result = str_str.substr(0, str_str.size() - suffix_str.size());
    auto ptr = allocate_string(ctx, result);
    return make_value(value_tag::string, ptr);
  }

  // No suffix match - return the original string
  return str;
}

// =============================================================================
// JSON Builtins
// =============================================================================

namespace {

// Helper to escape a string for JSON output
auto json_escape_string(std::string_view s) -> std::string {
  std::string result;
  result.reserve(s.size() + 2);
  result += '"';
  for (char c : s) {
    switch (c) {
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      case '\b':
        result += "\\b";
        break;
      case '\f':
        result += "\\f";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          // Control characters - use \uXXXX
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          result += buf;
        } else {
          result += c;
        }
        break;
    }
  }
  result += '"';
  return result;
}

// Forward declaration for recursive JSON conversion
auto value_to_json(runtime_context& ctx, nix_value v) -> std::string;

auto value_to_json(runtime_context& ctx, nix_value v) -> std::string {
  v = rt_force(ctx, v);

  switch (get_tag(v)) {
    case value_tag::null_value:
      return "null";

    case value_tag::boolean:
      return get_payload(v) != 0 ? "true" : "false";

    case value_tag::integer:
      return std::to_string(static_cast<std::int32_t>(get_payload(v)));

    case value_tag::floating: {
      double d = to_double(ctx, v);
      // Use a format that preserves precision
      std::ostringstream oss;
      oss.precision(17);
      oss << d;
      auto s = oss.str();
      // Ensure it looks like a float (has '.' or 'e')
      if (s.find('.') == std::string::npos && s.find('e') == std::string::npos) {
        s += ".0";
      }
      return s;
    }

    case value_tag::string:
      return json_escape_string(ctx.read_string(get_payload(v)));

    case value_tag::path:
      // Paths are serialized as strings
      return json_escape_string(ctx.read_string(get_payload(v)));

    case value_tag::list: {
      auto ptr = get_payload(v);
      if (ptr == 0) {
        return "[]";
      }
      auto count = ctx.read_u32(ptr + mem::LIST_COUNT_OFFSET);
      if (count == 0) {
        return "[]";
      }
      std::string result = "[";
      for (std::uint32_t i = 0; i < count; ++i) {
        if (i > 0) {
          result += ",";
        }
        auto elem = ctx.read_value(ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
        result += value_to_json(ctx, elem);
      }
      result += "]";
      return result;
    }

    case value_tag::attribute_set: {
      auto ptr = get_payload(v);
      if (ptr == 0) {
        return "{}";
      }
      auto count = ctx.read_u32(ptr + mem::ATTRSET_COUNT_OFFSET);
      if (count == 0) {
        return "{}";
      }

      // Collect and sort entries by key
      std::vector<std::pair<std::string, nix_value>> entries;
      entries.reserve(count);
      for (std::uint32_t i = 0; i < count; ++i) {
        auto entry = ptr + mem::ATTRSET_ENTRIES_OFFSET + i * mem::ATTRSET_ENTRY_SIZE;
        auto key_offset = ctx.read_u32(entry + mem::ATTRSET_ENTRY_KEY_OFFSET);
        auto key = std::string(ctx.read_string(key_offset));
        auto val = ctx.read_value(entry + mem::ATTRSET_ENTRY_VALUE_OFFSET);
        entries.emplace_back(std::move(key), val);
      }
      std::sort(entries.begin(), entries.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });

      std::string result = "{";
      bool first = true;
      for (const auto& [key, val] : entries) {
        if (!first) {
          result += ",";
        }
        first = false;
        result += json_escape_string(key);
        result += ":";
        result += value_to_json(ctx, val);
      }
      result += "}";
      return result;
    }

    case value_tag::lambda:
    case value_tag::primop:
      throw type_error("builtins.toJSON: cannot convert function to JSON");

    case value_tag::thunk:
      // Should have been forced above, but handle defensively
      throw runtime_error("builtins.toJSON: unexpected thunk");

    default:
      throw type_error("builtins.toJSON: unsupported type '" + std::string(type_name(v)) + "'");
  }
}

// JSON parser for fromJSON
class json_parser {
public:
  json_parser(runtime_context& ctx, std::string_view input) : ctx_(ctx), input_(input), pos_(0) {}

  auto parse() -> nix_value {
    skip_whitespace();
    auto result = parse_value();
    skip_whitespace();
    if (pos_ < input_.size()) {
      throw runtime_error("builtins.fromJSON: unexpected trailing content");
    }
    return result;
  }

private:
  runtime_context& ctx_;
  std::string_view input_;
  std::size_t pos_;

  auto peek() const -> char {
    if (pos_ >= input_.size()) {
      return '\0';
    }
    return input_[pos_];
  }

  auto advance() -> char {
    if (pos_ >= input_.size()) {
      throw runtime_error("builtins.fromJSON: unexpected end of input");
    }
    return input_[pos_++];
  }

  void skip_whitespace() {
    while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) {
      ++pos_;
    }
  }

  auto parse_value() -> nix_value {
    skip_whitespace();
    char c = peek();

    if (c == 'n') {
      return parse_null();
    }
    if (c == 't') {
      return parse_true();
    }
    if (c == 'f') {
      return parse_false();
    }
    if (c == '"') {
      return parse_string();
    }
    if (c == '[') {
      return parse_array();
    }
    if (c == '{') {
      return parse_object();
    }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
      return parse_number();
    }

    throw runtime_error("builtins.fromJSON: unexpected character '" + std::string(1, c) + "'");
  }

  auto parse_null() -> nix_value {
    if (input_.substr(pos_, 4) == "null") {
      pos_ += 4;
      return constants::null_value;
    }
    throw runtime_error("builtins.fromJSON: expected 'null'");
  }

  auto parse_true() -> nix_value {
    if (input_.substr(pos_, 4) == "true") {
      pos_ += 4;
      return constants::bool_true;
    }
    throw runtime_error("builtins.fromJSON: expected 'true'");
  }

  auto parse_false() -> nix_value {
    if (input_.substr(pos_, 5) == "false") {
      pos_ += 5;
      return constants::bool_false;
    }
    throw runtime_error("builtins.fromJSON: expected 'false'");
  }

  auto parse_string() -> nix_value {
    advance(); // consume opening quote
    std::string result;

    while (true) {
      char c = advance();
      if (c == '"') {
        break;
      }
      if (c == '\\') {
        char escaped = advance();
        switch (escaped) {
          case '"':
            result += '"';
            break;
          case '\\':
            result += '\\';
            break;
          case '/':
            result += '/';
            break;
          case 'b':
            result += '\b';
            break;
          case 'f':
            result += '\f';
            break;
          case 'n':
            result += '\n';
            break;
          case 'r':
            result += '\r';
            break;
          case 't':
            result += '\t';
            break;
          case 'u': {
            // Parse \uXXXX
            if (pos_ + 4 > input_.size()) {
              throw runtime_error("builtins.fromJSON: incomplete \\uXXXX escape");
            }
            auto hex = input_.substr(pos_, 4);
            pos_ += 4;
            char* end;
            auto codepoint = std::strtoul(std::string(hex).c_str(), &end, 16);
            if (end != std::string(hex).c_str() + 4) {
              throw runtime_error("builtins.fromJSON: invalid \\uXXXX escape");
            }
            // Convert codepoint to UTF-8 (basic case for BMP)
            if (codepoint < 0x80) {
              result += static_cast<char>(codepoint);
            } else if (codepoint < 0x800) {
              result += static_cast<char>(0xC0 | (codepoint >> 6));
              result += static_cast<char>(0x80 | (codepoint & 0x3F));
            } else {
              result += static_cast<char>(0xE0 | (codepoint >> 12));
              result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
              result += static_cast<char>(0x80 | (codepoint & 0x3F));
            }
            break;
          }
          default:
            throw runtime_error("builtins.fromJSON: invalid escape sequence '\\" +
                                std::string(1, escaped) + "'");
        }
      } else {
        result += c;
      }
    }

    auto ptr = allocate_string(ctx_, result);
    return make_value(value_tag::string, ptr);
  }

  auto parse_number() -> nix_value {
    auto start = pos_;
    bool is_float = false;

    // Optional minus
    if (peek() == '-') {
      advance();
    }

    // Integer part
    if (peek() == '0') {
      advance();
    } else if (std::isdigit(static_cast<unsigned char>(peek()))) {
      while (std::isdigit(static_cast<unsigned char>(peek()))) {
        advance();
      }
    } else {
      throw runtime_error("builtins.fromJSON: expected digit");
    }

    // Fractional part
    if (peek() == '.') {
      is_float = true;
      advance();
      if (!std::isdigit(static_cast<unsigned char>(peek()))) {
        throw runtime_error("builtins.fromJSON: expected digit after decimal point");
      }
      while (std::isdigit(static_cast<unsigned char>(peek()))) {
        advance();
      }
    }

    // Exponent
    if (peek() == 'e' || peek() == 'E') {
      is_float = true;
      advance();
      if (peek() == '+' || peek() == '-') {
        advance();
      }
      if (!std::isdigit(static_cast<unsigned char>(peek()))) {
        throw runtime_error("builtins.fromJSON: expected digit in exponent");
      }
      while (std::isdigit(static_cast<unsigned char>(peek()))) {
        advance();
      }
    }

    auto num_str = std::string(input_.substr(start, pos_ - start));

    if (is_float) {
      double d = std::stod(num_str);
      return make_float(ctx_, d);
    } else {
      // Try to parse as integer
      long long ll = std::stoll(num_str);
      // Check if it fits in int32
      if (ll >= std::numeric_limits<std::int32_t>::min() &&
          ll <= std::numeric_limits<std::int32_t>::max()) {
        return make_value(value_tag::integer,
                          static_cast<std::uint32_t>(static_cast<std::int32_t>(ll)));
      } else {
        // Large integers become floats
        return make_float(ctx_, static_cast<double>(ll));
      }
    }
  }

  auto parse_array() -> nix_value {
    advance(); // consume '['
    skip_whitespace();

    if (peek() == ']') {
      advance();
      return make_value(value_tag::list, 0);
    }

    std::vector<nix_value> elements;

    while (true) {
      elements.push_back(parse_value());
      skip_whitespace();

      char c = peek();
      if (c == ']') {
        advance();
        break;
      }
      if (c == ',') {
        advance();
        skip_whitespace();
      } else {
        throw runtime_error("builtins.fromJSON: expected ',' or ']'");
      }
    }

    // Allocate list
    auto count = static_cast<std::uint32_t>(elements.size());
    auto list_size = mem::list_size(count);
    auto list_ptr = ctx_.allocate(list_size);
    ctx_.write_i32(list_ptr + mem::LIST_COUNT_OFFSET, static_cast<std::int32_t>(count));

    for (std::uint32_t i = 0; i < count; ++i) {
      ctx_.write_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE, elements[i]);
    }

    return make_value(value_tag::list, list_ptr);
  }

  auto parse_object() -> nix_value {
    advance(); // consume '{'
    skip_whitespace();

    if (peek() == '}') {
      advance();
      return make_value(value_tag::attribute_set, 0);
    }

    std::vector<std::pair<std::string, nix_value>> entries;

    while (true) {
      skip_whitespace();

      if (peek() != '"') {
        throw runtime_error("builtins.fromJSON: expected string key");
      }

      // Parse key as string
      auto key_val = parse_string();
      auto key_ptr = get_payload(key_val);
      auto key = std::string(ctx_.read_string(key_ptr));

      skip_whitespace();
      if (advance() != ':') {
        throw runtime_error("builtins.fromJSON: expected ':'");
      }

      auto value = parse_value();
      entries.emplace_back(std::move(key), value);

      skip_whitespace();
      char c = peek();
      if (c == '}') {
        advance();
        break;
      }
      if (c == ',') {
        advance();
      } else {
        throw runtime_error("builtins.fromJSON: expected ',' or '}'");
      }
    }

    // Allocate attrset
    auto count = static_cast<std::uint32_t>(entries.size());
    auto attrs_size = mem::attrset_size(count);
    auto attrs_ptr = ctx_.allocate(attrs_size);
    ctx_.write_i32(attrs_ptr + mem::ATTRSET_COUNT_OFFSET, static_cast<std::int32_t>(count));

    for (std::uint32_t i = 0; i < count; ++i) {
      auto& [key, value] = entries[i];
      auto key_str_ptr = allocate_string(ctx_, key);
      auto entry = attrs_ptr + mem::ATTRSET_ENTRIES_OFFSET + i * mem::ATTRSET_ENTRY_SIZE;
      ctx_.write_i32(entry + mem::ATTRSET_ENTRY_KEY_OFFSET, static_cast<std::int32_t>(key_str_ptr));
      ctx_.write_value(entry + mem::ATTRSET_ENTRY_VALUE_OFFSET, value);
    }

    return make_value(value_tag::attribute_set, attrs_ptr);
  }
};

} // namespace

auto rt_to_json(runtime_context& ctx, nix_value v) -> nix_value {
  auto json_str = value_to_json(ctx, v);
  auto ptr = allocate_string(ctx, json_str);
  return make_value(value_tag::string, ptr);
}

auto rt_from_json(runtime_context& ctx, nix_value s) -> nix_value {
  s = rt_force(ctx, s);

  if (!is_string(s)) {
    throw type_error("builtins.fromJSON: expected string, got '" + std::string(type_name(s)) + "'");
  }

  auto json_str = ctx.read_string(get_payload(s));
  json_parser parser(ctx, json_str);
  return parser.parse();
}

// =============================================================================
// Arithmetic Builtins (as functions)
// =============================================================================

auto rt_builtin_add(runtime_context& ctx, nix_value a, nix_value b) -> nix_value {
  return rt_add(ctx, a, b, 0, 0);
}

auto rt_builtin_sub(runtime_context& ctx, nix_value a, nix_value b) -> nix_value {
  return rt_sub(ctx, a, b, 0, 0);
}

auto rt_builtin_mul(runtime_context& ctx, nix_value a, nix_value b) -> nix_value {
  return rt_mul(ctx, a, b, 0, 0);
}

auto rt_builtin_div(runtime_context& ctx, nix_value a, nix_value b) -> nix_value {
  return rt_div(ctx, a, b, 0, 0);
}

auto rt_builtin_less_than(runtime_context& ctx, nix_value a, nix_value b) -> nix_value {
  a = rt_force(ctx, a);
  b = rt_force(ctx, b);

  if (is_int(a) && is_int(b)) {
    auto ia = static_cast<std::int32_t>(get_payload(a));
    auto ib = static_cast<std::int32_t>(get_payload(b));
    return (ia < ib) ? constants::bool_true : constants::bool_false;
  }

  throw type_error("builtins.lessThan: expected integers");
}

auto rt_floor(runtime_context& ctx, nix_value v) -> nix_value {
  v = rt_force(ctx, v);

  if (is_int(v)) {
    return v; // floor of int is itself
  }

  if (is_float(v)) {
    double f = to_double(ctx, v);
    auto result = static_cast<std::int32_t>(std::floor(f));
    return make_value(value_tag::integer, static_cast<std::uint32_t>(result));
  }

  throw type_error("builtins.floor: expected number, got '" + std::string(type_name(v)) + "'");
}

auto rt_ceil(runtime_context& ctx, nix_value v) -> nix_value {
  v = rt_force(ctx, v);

  if (is_int(v)) {
    return v; // ceil of int is itself
  }

  if (is_float(v)) {
    double f = to_double(ctx, v);
    auto result = static_cast<std::int32_t>(std::ceil(f));
    return make_value(value_tag::integer, static_cast<std::uint32_t>(result));
  }

  throw type_error("builtins.ceil: expected number, got '" + std::string(type_name(v)) + "'");
}

auto rt_bit_and(runtime_context& ctx, nix_value a, nix_value b) -> nix_value {
  a = rt_force(ctx, a);
  b = rt_force(ctx, b);

  if (!is_int(a)) {
    throw type_error("builtins.bitAnd: first argument must be int, got '" +
                     std::string(type_name(a)) + "'");
  }
  if (!is_int(b)) {
    throw type_error("builtins.bitAnd: second argument must be int, got '" +
                     std::string(type_name(b)) + "'");
  }

  auto ia = static_cast<std::int32_t>(get_payload(a));
  auto ib = static_cast<std::int32_t>(get_payload(b));
  auto result = ia & ib;
  return make_value(value_tag::integer, static_cast<std::uint32_t>(result));
}

auto rt_bit_or(runtime_context& ctx, nix_value a, nix_value b) -> nix_value {
  a = rt_force(ctx, a);
  b = rt_force(ctx, b);

  if (!is_int(a)) {
    throw type_error("builtins.bitOr: first argument must be int, got '" +
                     std::string(type_name(a)) + "'");
  }
  if (!is_int(b)) {
    throw type_error("builtins.bitOr: second argument must be int, got '" +
                     std::string(type_name(b)) + "'");
  }

  auto ia = static_cast<std::int32_t>(get_payload(a));
  auto ib = static_cast<std::int32_t>(get_payload(b));
  auto result = ia | ib;
  return make_value(value_tag::integer, static_cast<std::uint32_t>(result));
}

auto rt_bit_xor(runtime_context& ctx, nix_value a, nix_value b) -> nix_value {
  a = rt_force(ctx, a);
  b = rt_force(ctx, b);

  if (!is_int(a)) {
    throw type_error("builtins.bitXor: first argument must be int, got '" +
                     std::string(type_name(a)) + "'");
  }
  if (!is_int(b)) {
    throw type_error("builtins.bitXor: second argument must be int, got '" +
                     std::string(type_name(b)) + "'");
  }

  auto ia = static_cast<std::int32_t>(get_payload(a));
  auto ib = static_cast<std::int32_t>(get_payload(b));
  auto result = ia ^ ib;
  return make_value(value_tag::integer, static_cast<std::uint32_t>(result));
}

// =============================================================================
// Attrset Builtins (2-arg)
// =============================================================================

auto rt_builtin_has_attr(runtime_context& ctx, nix_value name, nix_value set) -> nix_value {
  name = rt_force(ctx, name);
  set = rt_force(ctx, set);

  if (!is_string(name)) {
    throw type_error("builtins.hasAttr: name must be string, got '" + std::string(type_name(name)) +
                     "'");
  }
  if (!is_attrset(set)) {
    throw type_error("builtins.hasAttr: expected set, got '" + std::string(type_name(set)) + "'");
  }

  auto key = ctx.read_string(get_payload(name));
  auto attrs_ptr = get_payload(set);
  auto result = find_attr(ctx, attrs_ptr, key);

  return result.has_value() ? constants::bool_true : constants::bool_false;
}

auto rt_builtin_get_attr(runtime_context& ctx, nix_value name, nix_value set) -> nix_value {
  name = rt_force(ctx, name);
  set = rt_force(ctx, set);

  if (!is_string(name)) {
    throw type_error("builtins.getAttr: name must be string, got '" + std::string(type_name(name)) +
                     "'");
  }
  if (!is_attrset(set)) {
    throw type_error("builtins.getAttr: expected set, got '" + std::string(type_name(set)) + "'");
  }

  auto key = ctx.read_string(get_payload(name));
  auto attrs_ptr = get_payload(set);
  auto result = find_attr(ctx, attrs_ptr, key);

  if (!result.has_value()) {
    throw attr_error("builtins.getAttr: attribute '" + std::string(key) + "' not found");
  }

  return *result;
}

auto rt_remove_attrs(runtime_context& ctx, nix_value set, nix_value names) -> nix_value {
  set = rt_force(ctx, set);
  names = rt_force(ctx, names);

  if (!is_attrset(set)) {
    throw type_error("builtins.removeAttrs: expected set, got '" + std::string(type_name(set)) +
                     "'");
  }
  if (!is_list(names)) {
    throw type_error("builtins.removeAttrs: names must be list, got '" +
                     std::string(type_name(names)) + "'");
  }

  auto attrs_ptr = get_payload(set);
  if (attrs_ptr == 0) {
    return set; // empty set stays empty
  }

  // Collect names to remove
  std::unordered_set<std::string> remove_set;
  auto names_ptr = get_payload(names);
  if (names_ptr != 0) {
    auto names_count = ctx.read_u32(names_ptr + mem::LIST_COUNT_OFFSET);
    for (std::uint32_t i = 0; i < names_count; ++i) {
      auto elem = ctx.read_value(names_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
      elem = rt_force(ctx, elem);
      if (is_string(elem)) {
        remove_set.insert(std::string(ctx.read_string(get_payload(elem))));
      }
    }
  }

  if (remove_set.empty()) {
    return set; // nothing to remove
  }

  // Collect remaining attributes
  auto count = ctx.read_u32(attrs_ptr + mem::ATTRSET_COUNT_OFFSET);
  std::vector<std::pair<std::string, nix_value>> remaining;
  for (std::uint32_t i = 0; i < count; ++i) {
    auto entry = attrs_ptr + mem::ATTRSET_ENTRIES_OFFSET + i * mem::ATTRSET_ENTRY_SIZE;
    auto key_offset = ctx.read_u32(entry + mem::ATTRSET_ENTRY_KEY_OFFSET);
    auto key = std::string(ctx.read_string(key_offset));
    if (remove_set.find(key) == remove_set.end()) {
      auto value = ctx.read_value(entry + mem::ATTRSET_ENTRY_VALUE_OFFSET);
      remaining.emplace_back(std::move(key), value);
    }
  }

  if (remaining.empty()) {
    return make_value(value_tag::attribute_set, 0);
  }

  // Allocate new attrset
  auto new_count = static_cast<std::uint32_t>(remaining.size());
  auto new_size = mem::attrset_size(new_count);
  auto new_ptr = ctx.allocate(new_size);
  ctx.write_i32(new_ptr + mem::ATTRSET_COUNT_OFFSET, static_cast<std::int32_t>(new_count));

  for (std::uint32_t i = 0; i < new_count; ++i) {
    auto& [key, value] = remaining[i];
    auto key_ptr = allocate_string(ctx, key);
    auto entry = new_ptr + mem::ATTRSET_ENTRIES_OFFSET + i * mem::ATTRSET_ENTRY_SIZE;
    ctx.write_i32(entry + mem::ATTRSET_ENTRY_KEY_OFFSET, static_cast<std::int32_t>(key_ptr));
    ctx.write_value(entry + mem::ATTRSET_ENTRY_VALUE_OFFSET, value);
  }

  return make_value(value_tag::attribute_set, new_ptr);
}

// =============================================================================
// Higher-Order Functions
// =============================================================================

auto rt_map(runtime_context& ctx, nix_value f, nix_value list) -> nix_value {
  list = rt_force(ctx, list);

  if (!is_list(list)) {
    throw type_error("builtins.map: expected list, got '" + std::string(type_name(list)) + "'");
  }

  auto list_ptr = get_payload(list);
  if (list_ptr == 0) {
    // empty list -> empty list
    return make_value(value_tag::list, 0);
  }

  auto count = ctx.read_u32(list_ptr + mem::LIST_COUNT_OFFSET);
  if (count == 0) {
    return make_value(value_tag::list, 0);
  }

  // Allocate result list
  auto result_size = mem::list_size(count);
  auto result_ptr = ctx.allocate(result_size);
  ctx.write_i32(result_ptr + mem::LIST_COUNT_OFFSET, static_cast<std::int32_t>(count));

  // Apply f to each element
  for (std::uint32_t i = 0; i < count; ++i) {
    auto elem = ctx.read_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
    auto mapped = rt_apply(ctx, f, elem);
    ctx.write_value(result_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE, mapped);
  }

  return make_value(value_tag::list, result_ptr);
}

auto rt_filter(runtime_context& ctx, nix_value pred, nix_value list) -> nix_value {
  list = rt_force(ctx, list);

  if (!is_list(list)) {
    throw type_error("builtins.filter: expected list, got '" + std::string(type_name(list)) + "'");
  }

  auto list_ptr = get_payload(list);
  if (list_ptr == 0) {
    return make_value(value_tag::list, 0);
  }

  auto count = ctx.read_u32(list_ptr + mem::LIST_COUNT_OFFSET);
  if (count == 0) {
    return make_value(value_tag::list, 0);
  }

  // First pass: collect elements that pass the predicate
  std::vector<nix_value> kept;
  kept.reserve(count);

  for (std::uint32_t i = 0; i < count; ++i) {
    auto elem = ctx.read_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
    auto result = rt_apply(ctx, pred, elem);
    result = rt_force(ctx, result);

    if (!is_bool(result)) {
      throw type_error("builtins.filter: predicate must return bool, got '" +
                       std::string(type_name(result)) + "'");
    }

    if (result == constants::bool_true) {
      kept.push_back(elem);
    }
  }

  if (kept.empty()) {
    return make_value(value_tag::list, 0);
  }

  // Allocate result list
  auto result_count = static_cast<std::uint32_t>(kept.size());
  auto result_size = mem::list_size(result_count);
  auto result_ptr = ctx.allocate(result_size);
  ctx.write_i32(result_ptr + mem::LIST_COUNT_OFFSET, static_cast<std::int32_t>(result_count));

  for (std::uint32_t i = 0; i < result_count; ++i) {
    ctx.write_value(result_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE, kept[i]);
  }

  return make_value(value_tag::list, result_ptr);
}

auto rt_foldl(runtime_context& ctx, nix_value op, nix_value init, nix_value list) -> nix_value {
  list = rt_force(ctx, list);

  if (!is_list(list)) {
    throw type_error("builtins.foldl': expected list, got '" + std::string(type_name(list)) + "'");
  }

  auto list_ptr = get_payload(list);
  if (list_ptr == 0) {
    return init;
  }

  auto count = ctx.read_u32(list_ptr + mem::LIST_COUNT_OFFSET);
  if (count == 0) {
    return init;
  }

  // foldl' op init [x1 x2 x3] = op (op (op init x1) x2) x3
  // Note: foldl' is strict - we force the accumulator at each step
  auto acc = init;
  for (std::uint32_t i = 0; i < count; ++i) {
    auto elem = ctx.read_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
    // Apply op to acc, getting a partial application
    auto partial = rt_apply(ctx, op, acc);
    // Apply partial to elem
    acc = rt_apply(ctx, partial, elem);
    // Force the accumulator (strict foldl')
    acc = rt_force(ctx, acc);
  }

  return acc;
}

auto rt_gen_list(runtime_context& ctx, nix_value f, nix_value n) -> nix_value {
  n = rt_force(ctx, n);

  if (!is_int(n)) {
    throw type_error("builtins.genList: expected integer, got '" + std::string(type_name(n)) + "'");
  }

  auto count = static_cast<std::int32_t>(get_payload(n));
  if (count < 0) {
    throw runtime_error("builtins.genList: negative length " + std::to_string(count));
  }
  if (count == 0) {
    return make_value(value_tag::list, 0);
  }

  auto ucount = static_cast<std::uint32_t>(count);
  auto list_size = mem::list_size(ucount);
  auto list_ptr = ctx.allocate(list_size);
  ctx.write_i32(list_ptr + mem::LIST_COUNT_OFFSET, count);

  for (std::uint32_t i = 0; i < ucount; ++i) {
    auto idx = make_int(static_cast<std::int32_t>(i));
    auto elem = rt_apply(ctx, f, idx);
    ctx.write_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE, elem);
  }

  return make_value(value_tag::list, list_ptr);
}

auto rt_concat_lists(runtime_context& ctx, nix_value lists) -> nix_value {
  lists = rt_force(ctx, lists);

  if (!is_list(lists)) {
    throw type_error("builtins.concatLists: expected list of lists, got '" +
                     std::string(type_name(lists)) + "'");
  }

  auto lists_ptr = get_payload(lists);
  if (lists_ptr == 0) {
    return make_value(value_tag::list, 0);
  }

  auto num_lists = ctx.read_u32(lists_ptr + mem::LIST_COUNT_OFFSET);
  if (num_lists == 0) {
    return make_value(value_tag::list, 0);
  }

  // First pass: count total elements
  std::uint32_t total = 0;
  for (std::uint32_t i = 0; i < num_lists; ++i) {
    auto inner = ctx.read_value(lists_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
    inner = rt_force(ctx, inner);
    if (!is_list(inner)) {
      throw type_error("builtins.concatLists: element is not a list");
    }
    auto inner_ptr = get_payload(inner);
    if (inner_ptr != 0) {
      total += ctx.read_u32(inner_ptr + mem::LIST_COUNT_OFFSET);
    }
  }

  if (total == 0) {
    return make_value(value_tag::list, 0);
  }

  // Allocate result
  auto result_size = mem::list_size(total);
  auto result_ptr = ctx.allocate(result_size);
  ctx.write_i32(result_ptr + mem::LIST_COUNT_OFFSET, static_cast<std::int32_t>(total));

  // Copy elements
  std::uint32_t dest_idx = 0;
  for (std::uint32_t i = 0; i < num_lists; ++i) {
    auto inner = ctx.read_value(lists_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
    inner = rt_force(ctx, inner);
    auto inner_ptr = get_payload(inner);
    if (inner_ptr == 0)
      continue;
    auto inner_count = ctx.read_u32(inner_ptr + mem::LIST_COUNT_OFFSET);
    for (std::uint32_t j = 0; j < inner_count; ++j) {
      auto elem = ctx.read_value(inner_ptr + mem::LIST_ELEMENTS_OFFSET + j * mem::VALUE_SIZE);
      ctx.write_value(result_ptr + mem::LIST_ELEMENTS_OFFSET + dest_idx * mem::VALUE_SIZE, elem);
      ++dest_idx;
    }
  }

  return make_value(value_tag::list, result_ptr);
}

auto rt_sort(runtime_context& ctx, nix_value comparator, nix_value list) -> nix_value {
  list = rt_force(ctx, list);

  if (!is_list(list)) {
    throw type_error("builtins.sort: expected list, got '" + std::string(type_name(list)) + "'");
  }

  auto list_ptr = get_payload(list);
  if (list_ptr == 0) {
    return make_value(value_tag::list, 0);
  }

  auto count = ctx.read_u32(list_ptr + mem::LIST_COUNT_OFFSET);
  if (count <= 1) {
    return list; // already sorted
  }

  // Collect elements into a vector
  std::vector<nix_value> elements;
  elements.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    auto elem = ctx.read_value(list_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
    elements.push_back(elem);
  }

  // Sort using the comparator function
  // comparator a b should return true if a < b
  // We use stable_sort for consistency
  std::stable_sort(elements.begin(), elements.end(),
                   [&ctx, comparator](nix_value a, nix_value b) -> bool {
                     // Apply comparator to a, then to b
                     auto partial = rt_apply(ctx, comparator, a);
                     auto result = rt_apply(ctx, partial, b);
                     result = rt_force(ctx, result);
                     if (!is_bool(result)) {
                       throw type_error("builtins.sort: comparator must return bool, got '" +
                                        std::string(type_name(result)) + "'");
                     }
                     return result == constants::bool_true;
                   });

  // Allocate result list
  auto result_size = mem::list_size(count);
  auto result_ptr = ctx.allocate(result_size);
  ctx.write_i32(result_ptr + mem::LIST_COUNT_OFFSET, static_cast<std::int32_t>(count));

  for (std::uint32_t i = 0; i < count; ++i) {
    ctx.write_value(result_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE, elements[i]);
  }

  return make_value(value_tag::list, result_ptr);
}

// =============================================================================
// Error Handling
// =============================================================================

auto rt_throw_error(runtime_context& ctx, nix_value msg) -> nix_value {
  msg = rt_force(ctx, msg);

  std::string error_msg;
  if (is_string(msg)) {
    error_msg = ctx.read_string(get_payload(msg));
  } else {
    error_msg = "error thrown";
  }

  // If we're inside a tryEval, set the caught error flag and return a sentinel
  if (ctx.try_eval_depth > 0) {
    ctx.try_eval_caught_error = true;
    ctx.error_message = error_msg;
    // Return false as sentinel - tryEval will detect the caught error flag
    return constants::bool_false;
  }

  throw runtime_error(error_msg);
}

auto rt_abort(runtime_context& ctx, nix_value msg) -> nix_value {
  msg = rt_force(ctx, msg);

  if (is_string(msg)) {
    auto str = ctx.read_string(get_payload(msg));
    throw runtime_error("evaluation aborted: " + std::string(str));
  }

  throw runtime_error("evaluation aborted");
}

auto rt_try_eval(runtime_context& ctx, nix_value expr) -> nix_value {
  // tryEval returns { success = true/false; value = result or false }
  // We use try_eval_depth to signal to rt_throw_error that it should
  // set a flag instead of throwing, allowing us to catch errors.

  // Increment depth and clear any previous caught error
  ++ctx.try_eval_depth;
  ctx.try_eval_caught_error = false;

  // Force the expression - if it calls throw, the caught error flag will be set
  auto result = rt_force(ctx, expr);

  // Decrement depth
  --ctx.try_eval_depth;

  // Check if an error was caught
  bool success = !ctx.try_eval_caught_error;
  ctx.try_eval_caught_error = false; // Clear for next tryEval

  // Create the result attrset
  auto success_key = allocate_string(ctx, "success");
  auto value_key = allocate_string(ctx, "value");

  auto attrs_size = mem::attrset_size(2);
  auto attrs_ptr = ctx.allocate(attrs_size);

  ctx.write_i32(attrs_ptr + mem::ATTRSET_COUNT_OFFSET, 2);

  // Entry 0: success = true/false
  auto entry0 = attrs_ptr + mem::ATTRSET_ENTRIES_OFFSET;
  ctx.write_i32(entry0 + mem::ATTRSET_ENTRY_KEY_OFFSET, static_cast<std::int32_t>(success_key));
  ctx.write_value(entry0 + mem::ATTRSET_ENTRY_VALUE_OFFSET,
                  success ? constants::bool_true : constants::bool_false);

  // Entry 1: value = result or false
  auto entry1 = attrs_ptr + mem::ATTRSET_ENTRIES_OFFSET + mem::ATTRSET_ENTRY_SIZE;
  ctx.write_i32(entry1 + mem::ATTRSET_ENTRY_KEY_OFFSET, static_cast<std::int32_t>(value_key));
  ctx.write_value(entry1 + mem::ATTRSET_ENTRY_VALUE_OFFSET,
                  success ? result : constants::bool_false);

  return make_value(value_tag::attribute_set, attrs_ptr);
}

auto rt_trace(runtime_context& ctx, nix_value msg, nix_value val) -> nix_value {
  msg = rt_force(ctx, msg);

  // Print trace message to stderr
  std::string trace_msg = "trace: ";
  if (is_string(msg)) {
    trace_msg += ctx.read_string(get_payload(msg));
  } else if (is_int(msg)) {
    trace_msg += std::to_string(static_cast<std::int32_t>(get_payload(msg)));
  } else if (is_bool(msg)) {
    trace_msg += (msg == constants::bool_true) ? "true" : "false";
  } else if (is_null(msg)) {
    trace_msg += "null";
  } else {
    trace_msg += "<" + std::string(type_name(msg)) + ">";
  }
  std::cerr << trace_msg << std::endl;

  return val; // Return second argument unevaluated (lazy)
}

auto rt_seq(runtime_context& ctx, nix_value a, nix_value b) -> nix_value {
  // Force the first argument, then return the second
  (void)rt_force(ctx, a);
  return b;
}

// Helper to deeply force a value (recursively force all nested values)
namespace {
void deep_force(runtime_context& ctx, nix_value v) {
  v = rt_force(ctx, v);

  if (is_list(v)) {
    auto ptr = get_payload(v);
    if (ptr == 0)
      return;
    auto count = ctx.read_u32(ptr + mem::LIST_COUNT_OFFSET);
    for (std::uint32_t i = 0; i < count; ++i) {
      auto elem = ctx.read_value(ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
      deep_force(ctx, elem);
    }
  } else if (is_attrset(v)) {
    auto ptr = get_payload(v);
    if (ptr == 0)
      return;
    auto count = ctx.read_u32(ptr + mem::ATTRSET_COUNT_OFFSET);
    for (std::uint32_t i = 0; i < count; ++i) {
      auto entry = ptr + mem::ATTRSET_ENTRIES_OFFSET + i * mem::ATTRSET_ENTRY_SIZE;
      auto val = ctx.read_value(entry + mem::ATTRSET_ENTRY_VALUE_OFFSET);
      deep_force(ctx, val);
    }
  }
  // Other types are already forced
}
} // namespace

auto rt_deep_seq(runtime_context& ctx, nix_value a, nix_value b) -> nix_value {
  // Deeply force the first argument, then return the second
  deep_force(ctx, a);
  return b;
}

// =============================================================================
// Type Predicates
// =============================================================================

auto rt_is_null(runtime_context& ctx, nix_value v) -> nix_value {
  v = rt_force(ctx, v);
  return is_null(v) ? constants::bool_true : constants::bool_false;
}

auto rt_is_int(runtime_context& ctx, nix_value v) -> nix_value {
  v = rt_force(ctx, v);
  return is_int(v) ? constants::bool_true : constants::bool_false;
}

auto rt_is_float(runtime_context& ctx, nix_value v) -> nix_value {
  v = rt_force(ctx, v);
  return is_float(v) ? constants::bool_true : constants::bool_false;
}

auto rt_is_string(runtime_context& ctx, nix_value v) -> nix_value {
  v = rt_force(ctx, v);
  return is_string(v) ? constants::bool_true : constants::bool_false;
}

auto rt_is_path(runtime_context& ctx, nix_value v) -> nix_value {
  v = rt_force(ctx, v);
  return is_path(v) ? constants::bool_true : constants::bool_false;
}

auto rt_is_list(runtime_context& ctx, nix_value v) -> nix_value {
  v = rt_force(ctx, v);
  return is_list(v) ? constants::bool_true : constants::bool_false;
}

auto rt_is_attrs(runtime_context& ctx, nix_value v) -> nix_value {
  v = rt_force(ctx, v);
  return is_attrset(v) ? constants::bool_true : constants::bool_false;
}

auto rt_is_function(runtime_context& ctx, nix_value v) -> nix_value {
  v = rt_force(ctx, v);
  return (is_lambda(v) || is_primop(v)) ? constants::bool_true : constants::bool_false;
}

// =============================================================================
// Advanced Builtins
// =============================================================================

auto rt_generic_closure(runtime_context& ctx, nix_value attrs) -> nix_value {
  // genericClosure { startSet, operator }
  // Computes a transitive closure by repeatedly applying operator to elements
  // until no new elements are produced.
  //
  // Each element must be an attrset with a "key" attribute for deduplication.
  // Returns the list of all reachable elements.

  attrs = rt_force(ctx, attrs);

  if (!is_attrset(attrs)) {
    throw type_error("builtins.genericClosure: expected attrset, got '" +
                     std::string(type_name(attrs)) + "'");
  }

  auto attrs_ptr = get_payload(attrs);

  // Get startSet - copy value out before any allocations can invalidate pointers
  auto start_set_opt = find_attr(ctx, attrs_ptr, "startSet");
  if (!start_set_opt) {
    throw runtime_error("builtins.genericClosure: attribute 'startSet' required");
  }
  auto start_set_val = rt_force(ctx, *start_set_opt);
  if (!is_list(start_set_val)) {
    throw type_error("builtins.genericClosure: 'startSet' must be a list, got '" +
                     std::string(type_name(start_set_val)) + "'");
  }

  // Get operator function - copy value out before any allocations can invalidate pointers
  // Note: attrs_ptr may be invalid after rt_force above, so re-read it
  attrs_ptr = get_payload(attrs);
  auto op_opt = find_attr(ctx, attrs_ptr, "operator");
  if (!op_opt) {
    throw runtime_error("builtins.genericClosure: attribute 'operator' required");
  }
  auto op_val = rt_force(ctx, *op_opt);
  if (!is_lambda(op_val) && !is_primop(op_val)) {
    throw type_error("builtins.genericClosure: 'operator' must be a function, got '" +
                     std::string(type_name(op_val)) + "'");
  }

  // Track seen keys (for deduplication)
  // Use a map from key (as nix_value) to bool
  // For simplicity, we'll use string keys extracted from each element
  std::unordered_set<std::string> seen_keys;
  std::vector<nix_value> result;
  std::vector<nix_value> work_list;

  // Helper to extract key from element
  auto extract_key = [&](nix_value elem) -> std::string {
    elem = rt_force(ctx, elem);
    if (!is_attrset(elem)) {
      throw type_error("builtins.genericClosure: element must be an attrset, got '" +
                       std::string(type_name(elem)) + "'");
    }
    auto elem_ptr = get_payload(elem);
    auto key_opt = find_attr(ctx, elem_ptr, "key");
    if (!key_opt) {
      throw runtime_error("builtins.genericClosure: element must have 'key' attribute");
    }
    // Copy value out before forcing - pointers into WASM memory may be invalidated by rt_force
    auto key_val = rt_force(ctx, *key_opt);

    // Convert key to string representation for hashing
    if (is_string(key_val)) {
      return std::string(ctx.read_string(get_payload(key_val)));
    }
    if (is_int(key_val)) {
      return std::to_string(static_cast<std::int32_t>(get_payload(key_val)));
    }
    if (is_path(key_val)) {
      return std::string(ctx.read_string(get_payload(key_val)));
    }
    // For other types, use type + payload as key
    return std::to_string(static_cast<int>(get_tag(key_val))) + ":" +
           std::to_string(get_payload(key_val));
  };

  // Initialize work list with startSet
  auto start_ptr = get_payload(start_set_val);
  auto start_count = ctx.read_u32(start_ptr + mem::LIST_COUNT_OFFSET);
  for (std::uint32_t i = 0; i < start_count; ++i) {
    auto elem = ctx.read_value(start_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
    work_list.push_back(elem);
  }

  // Process work list
  while (!work_list.empty()) {
    auto elem = work_list.back();
    work_list.pop_back();

    auto key = extract_key(elem);
    if (seen_keys.count(key) != 0U) {
      continue; // Already seen
    }
    seen_keys.insert(key);
    result.push_back(elem);

    // Apply operator to get new elements
    auto new_elems = rt_apply(ctx, op_val, elem);
    new_elems = rt_force(ctx, new_elems);

    if (!is_list(new_elems)) {
      throw type_error("builtins.genericClosure: 'operator' must return a list, got '" +
                       std::string(type_name(new_elems)) + "'");
    }

    auto new_ptr = get_payload(new_elems);
    auto new_count = ctx.read_u32(new_ptr + mem::LIST_COUNT_OFFSET);
    for (std::uint32_t i = 0; i < new_count; ++i) {
      auto new_elem = ctx.read_value(new_ptr + mem::LIST_ELEMENTS_OFFSET + i * mem::VALUE_SIZE);
      work_list.push_back(new_elem);
    }
  }

  // Allocate result list
  auto count = static_cast<std::uint32_t>(result.size());
  auto size = 4 + count * 8;
  auto result_ptr = ctx.allocate(size);
  ctx.write_i32(result_ptr, static_cast<std::int32_t>(count));
  for (std::uint32_t i = 0; i < count; ++i) {
    ctx.write_value(result_ptr + 4 + i * 8, result[i]);
  }

  return make_value(value_tag::list, result_ptr);
}

auto rt_find_first(runtime_context& ctx, nix_value pred, nix_value def, nix_value list)
    -> nix_value {
  // findFirst pred default list
  // Returns the first element for which pred returns true, or default if none found

  pred = rt_force(ctx, pred);
  list = rt_force(ctx, list);

  if (!is_lambda(pred) && !is_primop(pred)) {
    throw type_error("builtins.findFirst: first argument must be a function, got '" +
                     std::string(type_name(pred)) + "'");
  }

  if (!is_list(list)) {
    throw type_error("builtins.findFirst: third argument must be a list, got '" +
                     std::string(type_name(list)) + "'");
  }

  auto ptr = get_payload(list);

  // Handle empty list (ptr == 0)
  if (ptr == 0) {
    return rt_force(ctx, def);
  }

  auto count = ctx.read_u32(ptr + mem::LIST_COUNT_OFFSET);

  for (std::uint32_t i = 0; i < count; ++i) {
    auto elem = ctx.read_value(ptr + mem::LIST_ELEMENTS_OFFSET + i * 8);
    auto result = rt_apply(ctx, pred, elem);
    result = rt_force(ctx, result);

    if (!is_bool(result)) {
      throw type_error("builtins.findFirst: predicate must return a bool, got '" +
                       std::string(type_name(result)) + "'");
    }

    if (get_payload(result) != 0) {
      return elem;
    }
  }

  // No match found, return default (force it since it may be a thunk)
  return rt_force(ctx, def);
}

auto rt_hash_string(runtime_context& ctx, nix_value type, nix_value str) -> nix_value {
  // hashString type str
  // Supported types: "md5", "sha1", "sha256", "sha512"

  type = rt_force(ctx, type);
  str = rt_force(ctx, str);

  if (!is_string(type)) {
    throw type_error("builtins.hashString: first argument must be a string, got '" +
                     std::string(type_name(type)) + "'");
  }

  if (!is_string(str)) {
    throw type_error("builtins.hashString: second argument must be a string, got '" +
                     std::string(type_name(str)) + "'");
  }

  std::string hash_type(ctx.read_string(get_payload(type)));
  std::string input(ctx.read_string(get_payload(str)));

  // For now, implement a simple placeholder that returns a fixed-length hex string
  // A real implementation would use a crypto library
  // We'll implement SHA256 using a simple algorithm for demonstration

  std::string hash_result;

  if (hash_type == "sha256") {
    // Simple placeholder: return a hash-like string based on input
    // Real implementation would use OpenSSL or similar
    std::uint64_t h = 0xcbf29ce484222325ULL; // FNV offset basis
    for (char c : input) {
      h ^= static_cast<std::uint8_t>(c);
      h *= 0x100000001b3ULL; // FNV prime
    }
    // Expand to 64 hex chars (256 bits) by repeated hashing
    std::ostringstream ss;
    for (int i = 0; i < 4; ++i) {
      h ^= (h >> 17);
      h *= 0x100000001b3ULL;
      ss << std::hex << std::setfill('0') << std::setw(16) << h;
    }
    hash_result = ss.str();
  } else if (hash_type == "sha512") {
    // 128 hex chars (512 bits)
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (char c : input) {
      h ^= static_cast<std::uint8_t>(c);
      h *= 0x100000001b3ULL;
    }
    std::ostringstream ss;
    for (int i = 0; i < 8; ++i) {
      h ^= (h >> 17);
      h *= 0x100000001b3ULL;
      ss << std::hex << std::setfill('0') << std::setw(16) << h;
    }
    hash_result = ss.str();
  } else if (hash_type == "sha1") {
    // 40 hex chars (160 bits)
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (char c : input) {
      h ^= static_cast<std::uint8_t>(c);
      h *= 0x100000001b3ULL;
    }
    std::ostringstream ss;
    for (int i = 0; i < 3; ++i) {
      h ^= (h >> 17);
      h *= 0x100000001b3ULL;
      ss << std::hex << std::setfill('0') << std::setw(16) << h;
    }
    hash_result = ss.str().substr(0, 40); // truncate to 40 chars
  } else if (hash_type == "md5") {
    // 32 hex chars (128 bits)
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (char c : input) {
      h ^= static_cast<std::uint8_t>(c);
      h *= 0x100000001b3ULL;
    }
    std::ostringstream ss;
    for (int i = 0; i < 2; ++i) {
      h ^= (h >> 17);
      h *= 0x100000001b3ULL;
      ss << std::hex << std::setfill('0') << std::setw(16) << h;
    }
    hash_result = ss.str();
  } else {
    throw runtime_error("builtins.hashString: unknown hash type '" + hash_type +
                        "' (supported: md5, sha1, sha256, sha512)");
  }

  auto result_ptr = allocate_string(ctx, hash_result);
  return make_value(value_tag::string, result_ptr);
}

auto rt_match(runtime_context& ctx, nix_value regex, nix_value str) -> nix_value {
  // match regex str
  // Returns null if no match, or a list of captured groups if match

  regex = rt_force(ctx, regex);
  str = rt_force(ctx, str);

  if (!is_string(regex)) {
    throw type_error("builtins.match: first argument must be a string, got '" +
                     std::string(type_name(regex)) + "'");
  }

  if (!is_string(str)) {
    throw type_error("builtins.match: second argument must be a string, got '" +
                     std::string(type_name(str)) + "'");
  }

  auto pattern_sv = ctx.read_string(get_payload(regex));
  auto input_sv = ctx.read_string(get_payload(str));
  std::string pattern(pattern_sv);
  std::string input(input_sv);

  try {
    // Nix regex must match the entire string (implicit ^...$)
    std::regex re(pattern, std::regex::extended);
    std::smatch match_result;

    if (!std::regex_match(input, match_result, re)) {
      return constants::null_value;
    }

    // Build list of captured groups (excluding the full match at index 0)
    auto count = static_cast<std::uint32_t>(match_result.size() - 1);

    // Handle case with no capture groups - return empty list
    if (count == 0) {
      auto result_ptr = ctx.allocate(4);
      ctx.write_i32(result_ptr, 0);
      return make_value(value_tag::list, result_ptr);
    }

    auto size = 4 + count * 8;
    auto result_ptr = ctx.allocate(size);
    ctx.write_i32(result_ptr, static_cast<std::int32_t>(count));

    for (std::uint32_t i = 0; i < count; ++i) {
      auto& submatch = match_result[i + 1];
      if (submatch.matched) {
        auto str_ptr = allocate_string(ctx, submatch.str());
        ctx.write_value(result_ptr + 4 + i * 8, make_value(value_tag::string, str_ptr));
      } else {
        // Unmatched optional group returns null
        ctx.write_value(result_ptr + 4 + i * 8, constants::null_value);
      }
    }

    return make_value(value_tag::list, result_ptr);

  } catch (const std::regex_error& e) {
    throw runtime_error("builtins.match: invalid regex '" + pattern + "': " + e.what());
  }
}

auto rt_split(runtime_context& ctx, nix_value regex, nix_value str) -> nix_value {
  // split regex str
  // Returns a list alternating between non-matched strings and lists of captured groups

  regex = rt_force(ctx, regex);
  str = rt_force(ctx, str);

  if (!is_string(regex)) {
    throw type_error("builtins.split: first argument must be a string, got '" +
                     std::string(type_name(regex)) + "'");
  }

  if (!is_string(str)) {
    throw type_error("builtins.split: second argument must be a string, got '" +
                     std::string(type_name(str)) + "'");
  }

  auto pattern_sv = ctx.read_string(get_payload(regex));
  auto input_sv = ctx.read_string(get_payload(str));
  std::string pattern(pattern_sv);
  std::string input(input_sv);

  try {
    std::regex re(pattern, std::regex::extended);
    std::vector<nix_value> result_vec;

    auto it = std::sregex_iterator(input.begin(), input.end(), re);
    auto end = std::sregex_iterator();

    size_t last_end = 0;

    for (; it != end; ++it) {
      const auto& match_result = *it;

      // Add the non-matched part before this match
      auto prefix = input.substr(last_end, static_cast<size_t>(match_result.position()) - last_end);
      auto prefix_ptr = allocate_string(ctx, prefix);
      result_vec.push_back(make_value(value_tag::string, prefix_ptr));

      // Add list of captured groups
      auto group_count = static_cast<std::uint32_t>(match_result.size() - 1);
      if (group_count == 0) {
        // No capture groups, just add the full match as a 1-element list
        auto match_str = match_result[0].str();
        auto match_ptr = allocate_string(ctx, match_str);
        auto list_ptr = ctx.allocate(4 + 8);
        ctx.write_i32(list_ptr, 1);
        ctx.write_value(list_ptr + 4, make_value(value_tag::string, match_ptr));
        result_vec.push_back(make_value(value_tag::list, list_ptr));
      } else {
        auto list_size = 4 + group_count * 8;
        auto list_ptr = ctx.allocate(list_size);
        ctx.write_i32(list_ptr, static_cast<std::int32_t>(group_count));
        for (std::uint32_t i = 0; i < group_count; ++i) {
          auto& submatch = match_result[i + 1];
          if (submatch.matched) {
            auto str_ptr = allocate_string(ctx, submatch.str());
            ctx.write_value(list_ptr + 4 + i * 8, make_value(value_tag::string, str_ptr));
          } else {
            ctx.write_value(list_ptr + 4 + i * 8, constants::null_value);
          }
        }
        result_vec.push_back(make_value(value_tag::list, list_ptr));
      }

      last_end =
          static_cast<size_t>(match_result.position()) + static_cast<size_t>(match_result.length());
    }

    // Add the remaining non-matched part
    auto suffix = input.substr(last_end);
    auto suffix_ptr = allocate_string(ctx, suffix);
    result_vec.push_back(make_value(value_tag::string, suffix_ptr));

    // Allocate result list
    auto count = static_cast<std::uint32_t>(result_vec.size());
    auto result_size = 4 + count * 8;
    auto result_ptr = ctx.allocate(result_size);
    ctx.write_i32(result_ptr, static_cast<std::int32_t>(count));
    for (std::uint32_t i = 0; i < count; ++i) {
      ctx.write_value(result_ptr + 4 + i * 8, result_vec[i]);
    }

    return make_value(value_tag::list, result_ptr);

  } catch (const std::regex_error& e) {
    throw runtime_error("builtins.split: invalid regex '" + pattern + "': " + e.what());
  }
}

// =============================================================================
// Primop Application
// =============================================================================

// For 2-arg primops, we need partial application support.
// A partially applied primop stores the primop index and first arg.
// Layout: [primop_index (i32), arg1 (i64)]
namespace {
constexpr std::uint32_t PARTIAL_PRIMOP_INDEX_OFFSET = 0;
constexpr std::uint32_t PARTIAL_PRIMOP_ARG1_OFFSET = 4;
constexpr std::uint32_t PARTIAL_PRIMOP_SIZE = 12; // 4 + 8

// Special indices for builtins not in the enum
constexpr std::uint32_t BUILTIN_ELEM_AT = 50;
constexpr std::uint32_t BUILTIN_CONCAT_LISTS = 51;

// Primop arity table
constexpr std::uint32_t primop_arity(std::uint32_t index) {
  namespace b = compile::builtins;
  switch (index) {
    // 1-arg primops
    case b::length:
    case b::head:
    case b::tail:
    case b::type_of:
    case b::is_null:
    case b::is_bool:
    case b::is_int:
    case b::is_float:
    case b::is_string:
    case b::is_path:
    case b::is_list:
    case b::is_attrs:
    case b::is_function:
    case b::attr_names:
    case b::attr_values:
    case b::string_length:
    case BUILTIN_CONCAT_LISTS:
    case b::throw_error:     // throw msg
    case b::abort_eval:      // abort msg
    case b::try_eval:        // tryEval expr
    case b::to_string:       // toString val
    case b::concat_strings:  // concatStrings list
    case b::list_to_attrs:   // listToAttrs list
    case b::floor_fn:        // floor x
    case b::ceil_fn:         // ceil x
    case b::function_args:   // functionArgs f
    case b::get_env:         // getEnv name
    case b::to_lower:        // toLower str
    case b::to_upper:        // toUpper str
    case b::split_version:   // splitVersion v
    case b::parse_drv_name:  // parseDrvName name
    case b::base_name_of:    // baseNameOf path
    case b::dir_of:          // dirOf path
    case b::to_json:         // toJSON val
    case b::from_json:       // fromJSON str
    case b::reverse:         // reverse list
    case b::generic_closure: // genericClosure { startSet, operator }
      return 1;

    // 2-arg primops
    case b::elem: // elem x list
    case BUILTIN_ELEM_AT:
    case b::map:               // map f list
    case b::filter:            // filter pred list
    case b::gen_list:          // genList f n
    case b::trace:             // trace msg val
    case b::seq:               // seq a b
    case b::deep_seq:          // deepSeq a b
    case b::has_attr:          // hasAttr name set
    case b::get_attr:          // getAttr name set
    case b::remove_attrs:      // removeAttrs set names
    case b::sort:              // sort comparator list
    case b::all:               // all pred list
    case b::any:               // any pred list
    case b::concat_map:        // concatMap f list
    case b::builtin_add:       // add a b
    case b::builtin_sub:       // sub a b
    case b::builtin_mul:       // mul a b
    case b::builtin_div:       // div a b
    case b::builtin_less_than: // lessThan a b
    case b::concat_string_sep: // concatStringsSep sep list
    case b::map_attrs:         // mapAttrs f set
    case b::cat_attrs:         // catAttrs name list
    case b::partition:         // partition pred list
    case b::group_by:          // groupBy f list
    case b::bit_and:           // bitAnd a b
    case b::bit_or:            // bitOr a b
    case b::bit_xor:           // bitXor a b
    case b::intersect_attrs:   // intersectAttrs a b
    case b::compare_versions:  // compareVersions a b
    case b::has_prefix:        // hasPrefix prefix str
    case b::has_suffix:        // hasSuffix suffix str
    case b::remove_prefix:     // removePrefix prefix str
    case b::remove_suffix:     // removeSuffix suffix str
    case b::take:              // take n list
    case b::drop:              // drop n list
    case b::range:             // range a b
    case b::zip_lists:         // zipLists list1 list2
    case b::hash_string:       // hashString type str
    case b::match:             // match regex str
    case b::split:             // split regex str
      return 2;

    // 3-arg primops
    case b::foldl:           // foldl' op init list
    case b::substring:       // substring start len str
    case b::replace_strings: // replaceStrings from to str
    case b::find_first:      // findFirst pred default list
      return 3;

    default:
      return 1; // default to 1-arg
  }
}

} // namespace

auto rt_apply_primop(runtime_context& ctx, std::uint32_t primop_index, nix_value arg) -> nix_value {
  namespace b = compile::builtins;

  // Check if this is a partial application (high bit set means we have stored args)
  // For now, handle simple 1-arg and 2-arg cases

  auto arity = primop_arity(primop_index);

  if (arity == 1) {
    // Directly apply
    switch (primop_index) {
      case b::length:
        return rt_length(ctx, arg);
      case b::head:
        return rt_head(ctx, arg);
      case b::tail:
        return rt_tail(ctx, arg);
      case b::type_of:
        return rt_type_of(ctx, arg);
      case b::is_null:
        return rt_is_null(ctx, arg);
      case b::is_bool: {
        auto v = rt_force(ctx, arg);
        return is_bool(v) ? constants::bool_true : constants::bool_false;
      }
      case b::is_int:
        return rt_is_int(ctx, arg);
      case b::is_float:
        return rt_is_float(ctx, arg);
      case b::is_string:
        return rt_is_string(ctx, arg);
      case b::is_path:
        return rt_is_path(ctx, arg);
      case b::is_list:
        return rt_is_list(ctx, arg);
      case b::is_attrs:
        return rt_is_attrs(ctx, arg);
      case b::is_function:
        return rt_is_function(ctx, arg);
      case b::attr_names:
        return rt_attr_names(ctx, arg);
      case b::attr_values:
        return rt_attr_values(ctx, arg);
      case b::string_length:
        return rt_string_length(ctx, arg);
      case BUILTIN_CONCAT_LISTS:
        return rt_concat_lists(ctx, arg);
      case b::throw_error:
        return rt_throw_error(ctx, arg);
      case b::abort_eval:
        return rt_abort(ctx, arg);
      case b::try_eval:
        return rt_try_eval(ctx, arg);
      case b::to_string:
        return rt_to_string(ctx, arg);
      case b::concat_strings:
        return rt_concat_strings(ctx, arg);
      case b::list_to_attrs:
        return rt_list_to_attrs(ctx, arg);
      case b::floor_fn:
        return rt_floor(ctx, arg);
      case b::ceil_fn:
        return rt_ceil(ctx, arg);
      case b::function_args:
        return rt_function_args(ctx, arg);
      case b::get_env:
        return rt_get_env(ctx, arg);
      case b::to_lower:
        return rt_to_lower(ctx, arg);
      case b::to_upper:
        return rt_to_upper(ctx, arg);
      case b::split_version:
        return rt_split_version(ctx, arg);
      case b::parse_drv_name:
        return rt_parse_drv_name(ctx, arg);
      case b::base_name_of:
        return rt_base_name_of(ctx, arg);
      case b::dir_of:
        return rt_dir_of(ctx, arg);
      case b::to_json:
        return rt_to_json(ctx, arg);
      case b::from_json:
        return rt_from_json(ctx, arg);
      case b::reverse:
        return rt_reverse(ctx, arg);
      case b::generic_closure:
        return rt_generic_closure(ctx, arg);
      default:
        throw runtime_error("unknown primop index: " + std::to_string(primop_index));
    }
  }

  if (arity == 2 || arity == 3) {
    // Need to create a partial application (first arg of 2 or 3 arg primop)
    auto partial_ptr = ctx.allocate(PARTIAL_PRIMOP_SIZE);
    ctx.write_i32(partial_ptr + PARTIAL_PRIMOP_INDEX_OFFSET,
                  static_cast<std::int32_t>(primop_index));
    ctx.write_value(partial_ptr + PARTIAL_PRIMOP_ARG1_OFFSET, arg);
    // Return a special "partial primop" value - we'll use primop tag with high bit set in payload
    // Payload: partial_ptr (low 30 bits) + partial flag (high bits)
    // 0x80000000 = one arg stored
    return make_value(value_tag::primop, partial_ptr | 0x80000000);
  }

  throw runtime_error("primop arity > 3 not supported");
}

// Handle partial primop application (second arg)
static auto rt_apply_partial_primop(runtime_context& ctx, std::uint32_t partial_ptr, nix_value arg2)
    -> nix_value {
  auto primop_index = ctx.read_u32(partial_ptr + PARTIAL_PRIMOP_INDEX_OFFSET);
  auto arg1 = ctx.read_value(partial_ptr + PARTIAL_PRIMOP_ARG1_OFFSET);

  namespace b = compile::builtins;
  auto arity = primop_arity(primop_index);

  if (arity == 2) {
    // Full application of 2-arg primop
    switch (primop_index) {
      case b::elem:
        return rt_elem(ctx, arg1, arg2);
      case BUILTIN_ELEM_AT:
        return rt_elem_at(ctx, arg1, arg2);
      case b::map:
        return rt_map(ctx, arg1, arg2);
      case b::filter:
        return rt_filter(ctx, arg1, arg2);
      case b::gen_list:
        return rt_gen_list(ctx, arg1, arg2);
      case b::trace:
        return rt_trace(ctx, arg1, arg2);
      case b::seq:
        return rt_seq(ctx, arg1, arg2);
      case b::deep_seq:
        return rt_deep_seq(ctx, arg1, arg2);
      case b::has_attr:
        return rt_builtin_has_attr(ctx, arg1, arg2);
      case b::get_attr:
        return rt_builtin_get_attr(ctx, arg1, arg2);
      case b::remove_attrs:
        return rt_remove_attrs(ctx, arg1, arg2);
      case b::sort:
        return rt_sort(ctx, arg1, arg2);
      case b::all:
        return rt_all(ctx, arg1, arg2);
      case b::any:
        return rt_any(ctx, arg1, arg2);
      case b::concat_map:
        return rt_concat_map(ctx, arg1, arg2);
      case b::builtin_add:
        return rt_builtin_add(ctx, arg1, arg2);
      case b::builtin_sub:
        return rt_builtin_sub(ctx, arg1, arg2);
      case b::builtin_mul:
        return rt_builtin_mul(ctx, arg1, arg2);
      case b::builtin_div:
        return rt_builtin_div(ctx, arg1, arg2);
      case b::builtin_less_than:
        return rt_builtin_less_than(ctx, arg1, arg2);
      case b::concat_string_sep:
        return rt_concat_string_sep(ctx, arg1, arg2);
      case b::map_attrs:
        return rt_map_attrs(ctx, arg1, arg2);
      case b::cat_attrs:
        return rt_cat_attrs(ctx, arg1, arg2);
      case b::partition:
        return rt_partition(ctx, arg1, arg2);
      case b::group_by:
        return rt_group_by(ctx, arg1, arg2);
      case b::bit_and:
        return rt_bit_and(ctx, arg1, arg2);
      case b::bit_or:
        return rt_bit_or(ctx, arg1, arg2);
      case b::bit_xor:
        return rt_bit_xor(ctx, arg1, arg2);
      case b::intersect_attrs:
        return rt_intersect_attrs(ctx, arg1, arg2);
      case b::compare_versions:
        return rt_compare_versions(ctx, arg1, arg2);
      case b::has_prefix:
        return rt_has_prefix(ctx, arg1, arg2);
      case b::has_suffix:
        return rt_has_suffix(ctx, arg1, arg2);
      case b::remove_prefix:
        return rt_remove_prefix(ctx, arg1, arg2);
      case b::remove_suffix:
        return rt_remove_suffix(ctx, arg1, arg2);
      case b::take:
        return rt_take(ctx, arg1, arg2);
      case b::drop:
        return rt_drop(ctx, arg1, arg2);
      case b::range:
        return rt_range(ctx, arg1, arg2);
      case b::zip_lists:
        return rt_zip_lists(ctx, arg1, arg2);
      case b::hash_string:
        return rt_hash_string(ctx, arg1, arg2);
      case b::match:
        return rt_match(ctx, arg1, arg2);
      case b::split:
        return rt_split(ctx, arg1, arg2);
      default:
        throw runtime_error("unknown 2-arg primop index: " + std::to_string(primop_index));
    }
  }

  if (arity == 3) {
    // This is the second arg of a 3-arg primop - create another partial
    // Layout for 2-arg partial: [primop_index (i32), arg1 (i64), arg2 (i64)]
    constexpr std::uint32_t PARTIAL_PRIMOP_ARG2_OFFSET = 12;
    constexpr std::uint32_t PARTIAL_PRIMOP_SIZE_3ARG = 20; // 4 + 8 + 8

    auto partial2_ptr = ctx.allocate(PARTIAL_PRIMOP_SIZE_3ARG);
    ctx.write_i32(partial2_ptr + PARTIAL_PRIMOP_INDEX_OFFSET,
                  static_cast<std::int32_t>(primop_index));
    ctx.write_value(partial2_ptr + PARTIAL_PRIMOP_ARG1_OFFSET, arg1);
    ctx.write_value(partial2_ptr + PARTIAL_PRIMOP_ARG2_OFFSET, arg2);
    // Use a different high bit pattern to indicate 2 args stored
    return make_value(value_tag::primop, partial2_ptr | 0xC0000000);
  }

  throw runtime_error("unexpected partial primop arity");
}

// Handle partial primop application (third arg for 3-arg primops)
static auto rt_apply_partial_primop_3arg(runtime_context& ctx, std::uint32_t partial_ptr,
                                         nix_value arg3) -> nix_value {
  constexpr std::uint32_t PARTIAL_PRIMOP_ARG2_OFFSET = 12;

  auto primop_index = ctx.read_u32(partial_ptr + PARTIAL_PRIMOP_INDEX_OFFSET);
  auto arg1 = ctx.read_value(partial_ptr + PARTIAL_PRIMOP_ARG1_OFFSET);
  auto arg2 = ctx.read_value(partial_ptr + PARTIAL_PRIMOP_ARG2_OFFSET);

  namespace b = compile::builtins;
  switch (primop_index) {
    case b::foldl:
      return rt_foldl(ctx, arg1, arg2, arg3);
    case b::substring:
      return rt_substring(ctx, arg1, arg2, arg3);
    case b::replace_strings:
      return rt_replace_strings(ctx, arg1, arg2, arg3);
    case b::find_first:
      return rt_find_first(ctx, arg1, arg2, arg3);
    default:
      throw runtime_error("unknown 3-arg primop index: " + std::to_string(primop_index));
  }
}

// =============================================================================
// Builtins Initialization
// =============================================================================

void rt_init_builtins(runtime_context& ctx) {
  namespace b = compile::builtins;

  // Already have true, false, null set up by wasm_executor
  // Now we need to create the "builtins" attrset

  // Create primop values for each builtin
  std::vector<std::pair<std::string, nix_value>> entries;

  // Type predicates
  entries.emplace_back("isNull", make_value(value_tag::primop, b::is_null));
  entries.emplace_back("isBool", make_value(value_tag::primop, b::is_bool));
  entries.emplace_back("isInt", make_value(value_tag::primop, b::is_int));
  entries.emplace_back("isFloat", make_value(value_tag::primop, b::is_float));
  entries.emplace_back("isString", make_value(value_tag::primop, b::is_string));
  entries.emplace_back("isPath", make_value(value_tag::primop, b::is_path));
  entries.emplace_back("isList", make_value(value_tag::primop, b::is_list));
  entries.emplace_back("isAttrs", make_value(value_tag::primop, b::is_attrs));
  entries.emplace_back("isFunction", make_value(value_tag::primop, b::is_function));

  // List operations
  entries.emplace_back("length", make_value(value_tag::primop, b::length));
  entries.emplace_back("head", make_value(value_tag::primop, b::head));
  entries.emplace_back("tail", make_value(value_tag::primop, b::tail));
  entries.emplace_back("elemAt", make_value(value_tag::primop, BUILTIN_ELEM_AT));
  entries.emplace_back("elem", make_value(value_tag::primop, b::elem));

  // Higher-order list operations
  entries.emplace_back("map", make_value(value_tag::primop, b::map));
  entries.emplace_back("filter", make_value(value_tag::primop, b::filter));
  entries.emplace_back("foldl'", make_value(value_tag::primop, b::foldl));
  entries.emplace_back("genList", make_value(value_tag::primop, b::gen_list));
  entries.emplace_back("concatLists", make_value(value_tag::primop, BUILTIN_CONCAT_LISTS));
  entries.emplace_back("sort", make_value(value_tag::primop, b::sort));
  entries.emplace_back("all", make_value(value_tag::primop, b::all));
  entries.emplace_back("any", make_value(value_tag::primop, b::any));
  entries.emplace_back("concatMap", make_value(value_tag::primop, b::concat_map));
  entries.emplace_back("partition", make_value(value_tag::primop, b::partition));
  entries.emplace_back("groupBy", make_value(value_tag::primop, b::group_by));
  entries.emplace_back("reverse", make_value(value_tag::primop, b::reverse));
  entries.emplace_back("take", make_value(value_tag::primop, b::take));
  entries.emplace_back("drop", make_value(value_tag::primop, b::drop));
  entries.emplace_back("range", make_value(value_tag::primop, b::range));
  entries.emplace_back("zipLists", make_value(value_tag::primop, b::zip_lists));

  // Attrset operations
  entries.emplace_back("attrNames", make_value(value_tag::primop, b::attr_names));
  entries.emplace_back("attrValues", make_value(value_tag::primop, b::attr_values));
  entries.emplace_back("hasAttr", make_value(value_tag::primop, b::has_attr));
  entries.emplace_back("getAttr", make_value(value_tag::primop, b::get_attr));
  entries.emplace_back("removeAttrs", make_value(value_tag::primop, b::remove_attrs));
  entries.emplace_back("listToAttrs", make_value(value_tag::primop, b::list_to_attrs));
  entries.emplace_back("mapAttrs", make_value(value_tag::primop, b::map_attrs));
  entries.emplace_back("catAttrs", make_value(value_tag::primop, b::cat_attrs));
  entries.emplace_back("intersectAttrs", make_value(value_tag::primop, b::intersect_attrs));
  entries.emplace_back("functionArgs", make_value(value_tag::primop, b::function_args));
  entries.emplace_back("getEnv", make_value(value_tag::primop, b::get_env));
  entries.emplace_back("compareVersions", make_value(value_tag::primop, b::compare_versions));
  entries.emplace_back("splitVersion", make_value(value_tag::primop, b::split_version));

  // String operations
  entries.emplace_back("stringLength", make_value(value_tag::primop, b::string_length));
  entries.emplace_back("substring", make_value(value_tag::primop, b::substring));
  entries.emplace_back("replaceStrings", make_value(value_tag::primop, b::replace_strings));
  entries.emplace_back("toString", make_value(value_tag::primop, b::to_string));
  entries.emplace_back("concatStrings", make_value(value_tag::primop, b::concat_strings));
  entries.emplace_back("concatStringsSep", make_value(value_tag::primop, b::concat_string_sep));
  entries.emplace_back("typeOf", make_value(value_tag::primop, b::type_of));
  entries.emplace_back("toLower", make_value(value_tag::primop, b::to_lower));
  entries.emplace_back("toUpper", make_value(value_tag::primop, b::to_upper));
  entries.emplace_back("parseDrvName", make_value(value_tag::primop, b::parse_drv_name));
  entries.emplace_back("baseNameOf", make_value(value_tag::primop, b::base_name_of));
  entries.emplace_back("dirOf", make_value(value_tag::primop, b::dir_of));
  entries.emplace_back("hasPrefix", make_value(value_tag::primop, b::has_prefix));
  entries.emplace_back("hasSuffix", make_value(value_tag::primop, b::has_suffix));
  entries.emplace_back("removePrefix", make_value(value_tag::primop, b::remove_prefix));
  entries.emplace_back("removeSuffix", make_value(value_tag::primop, b::remove_suffix));

  // JSON operations
  entries.emplace_back("toJSON", make_value(value_tag::primop, b::to_json));
  entries.emplace_back("fromJSON", make_value(value_tag::primop, b::from_json));

  // String regex/hash operations
  entries.emplace_back("hashString", make_value(value_tag::primop, b::hash_string));
  entries.emplace_back("match", make_value(value_tag::primop, b::match));
  entries.emplace_back("split", make_value(value_tag::primop, b::split));

  // Advanced operations
  entries.emplace_back("genericClosure", make_value(value_tag::primop, b::generic_closure));
  entries.emplace_back("findFirst", make_value(value_tag::primop, b::find_first));

  // Arithmetic (as functions)
  entries.emplace_back("add", make_value(value_tag::primop, b::builtin_add));
  entries.emplace_back("sub", make_value(value_tag::primop, b::builtin_sub));
  entries.emplace_back("mul", make_value(value_tag::primop, b::builtin_mul));
  entries.emplace_back("div", make_value(value_tag::primop, b::builtin_div));
  entries.emplace_back("lessThan", make_value(value_tag::primop, b::builtin_less_than));
  entries.emplace_back("floor", make_value(value_tag::primop, b::floor_fn));
  entries.emplace_back("ceil", make_value(value_tag::primop, b::ceil_fn));
  entries.emplace_back("bitAnd", make_value(value_tag::primop, b::bit_and));
  entries.emplace_back("bitOr", make_value(value_tag::primop, b::bit_or));
  entries.emplace_back("bitXor", make_value(value_tag::primop, b::bit_xor));

  // Error handling
  entries.emplace_back("throw", make_value(value_tag::primop, b::throw_error));
  entries.emplace_back("abort", make_value(value_tag::primop, b::abort_eval));
  entries.emplace_back("tryEval", make_value(value_tag::primop, b::try_eval));
  entries.emplace_back("trace", make_value(value_tag::primop, b::trace));
  entries.emplace_back("seq", make_value(value_tag::primop, b::seq));
  entries.emplace_back("deepSeq", make_value(value_tag::primop, b::deep_seq));

  // Also add true, false, null to the builtins attrset
  entries.emplace_back("true", constants::bool_true);
  entries.emplace_back("false", constants::bool_false);
  entries.emplace_back("null", constants::null_value);

  // Sort entries by name for binary search in find_attr
  std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  // Allocate the attrset
  auto count = static_cast<std::uint32_t>(entries.size());
  auto attrs_size = mem::attrset_size(count);
  auto attrs_ptr = ctx.allocate(attrs_size);

  ctx.write_i32(attrs_ptr + mem::ATTRSET_COUNT_OFFSET, static_cast<std::int32_t>(count));

  for (std::uint32_t i = 0; i < count; ++i) {
    auto& [name, value] = entries[i];
    auto key_ptr = allocate_string(ctx, name);
    auto entry_off = attrs_ptr + mem::ATTRSET_ENTRIES_OFFSET + i * mem::ATTRSET_ENTRY_SIZE;
    ctx.write_i32(entry_off + mem::ATTRSET_ENTRY_KEY_OFFSET, static_cast<std::int32_t>(key_ptr));
    ctx.write_value(entry_off + mem::ATTRSET_ENTRY_VALUE_OFFSET, value);
  }

  auto builtins_value = make_value(value_tag::attribute_set, attrs_ptr);
  ctx.builtins["builtins"] = builtins_value;
}

} // namespace nix::language::runtime
