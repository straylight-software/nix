///@file nix-language/runtime/runtime.cpp
/// WASM runtime implementation for Nix values.

#include "nix-language/runtime/runtime.hh"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
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

auto to_double(nix_value v) -> double {
  if (is_int(v)) {
    return static_cast<double>(static_cast<std::int32_t>(get_payload(v)));
  }
  // float: payload is reinterpreted f32 bits
  auto bits = get_payload(v);
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return static_cast<double>(f);
}

auto make_int(std::int32_t i) -> nix_value {
  return make_value(value_tag::integer, static_cast<std::uint32_t>(i));
}

auto make_float(double d) -> nix_value {
  // store as float (lose precision for now)
  auto f = static_cast<float>(d);
  std::uint32_t bits;
  std::memcpy(&bits, &f, sizeof(bits));
  return make_value(value_tag::floating, bits);
}

// Forward declaration for deep equality in rt_eq
auto find_attr(runtime_context& ctx, std::uint32_t attrs_ptr, std::string_view key)
    -> std::optional<nix_value>;

} // namespace

auto rt_add([[maybe_unused]] runtime_context& ctx, nix_value a, nix_value b, std::uint32_t line,
            std::uint32_t col) -> nix_value {
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
  return make_float(to_double(a) + to_double(b));
}

auto rt_sub([[maybe_unused]] runtime_context& ctx, nix_value a, nix_value b, std::uint32_t line,
            std::uint32_t col) -> nix_value {
  expect_numeric(a, line, col);
  expect_numeric(b, line, col);

  if (is_int(a) && is_int(b)) {
    auto ia = static_cast<std::int32_t>(get_payload(a));
    auto ib = static_cast<std::int32_t>(get_payload(b));
    return make_int(ia - ib);
  }

  return make_float(to_double(a) - to_double(b));
}

auto rt_mul([[maybe_unused]] runtime_context& ctx, nix_value a, nix_value b, std::uint32_t line,
            std::uint32_t col) -> nix_value {
  expect_numeric(a, line, col);
  expect_numeric(b, line, col);

  if (is_int(a) && is_int(b)) {
    auto ia = static_cast<std::int32_t>(get_payload(a));
    auto ib = static_cast<std::int32_t>(get_payload(b));
    return make_int(ia * ib);
  }

  return make_float(to_double(a) * to_double(b));
}

auto rt_div([[maybe_unused]] runtime_context& ctx, nix_value a, nix_value b, std::uint32_t line,
            std::uint32_t col) -> nix_value {
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
  auto db = to_double(b);
  if (db == 0.0) {
    throw runtime_error("division by zero", line, col);
  }
  return make_float(to_double(a) / db);
}

auto rt_negate([[maybe_unused]] runtime_context& ctx, nix_value v) -> nix_value {
  if (is_int(v)) {
    auto i = static_cast<std::int32_t>(get_payload(v));
    return make_int(-i);
  }
  if (is_float(v)) {
    return make_float(-to_double(v));
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
    return to_double(a) < to_double(b) ? constants::bool_true : constants::bool_false;
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
      return to_double(a) == to_double(b) ? constants::bool_true : constants::bool_false;
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
      return to_double(a) == to_double(b) ? constants::bool_true : constants::bool_false;

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
    throw type_error("builtins.concatStringsSep: expected list, got '" +
                     std::string(type_name(list)) + "'");
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
    case b::throw_error:    // throw msg
    case b::abort_eval:     // abort msg
    case b::try_eval:       // tryEval expr
    case b::to_string:      // toString val
    case b::concat_strings: // concatStrings list
    case b::list_to_attrs:  // listToAttrs list
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
      return 2;

    // 3-arg primops
    case b::foldl:           // foldl' op init list
    case b::substring:       // substring start len str
    case b::replace_strings: // replaceStrings from to str
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

  // Attrset operations
  entries.emplace_back("attrNames", make_value(value_tag::primop, b::attr_names));
  entries.emplace_back("attrValues", make_value(value_tag::primop, b::attr_values));
  entries.emplace_back("hasAttr", make_value(value_tag::primop, b::has_attr));
  entries.emplace_back("getAttr", make_value(value_tag::primop, b::get_attr));
  entries.emplace_back("removeAttrs", make_value(value_tag::primop, b::remove_attrs));
  entries.emplace_back("listToAttrs", make_value(value_tag::primop, b::list_to_attrs));

  // String operations
  entries.emplace_back("stringLength", make_value(value_tag::primop, b::string_length));
  entries.emplace_back("substring", make_value(value_tag::primop, b::substring));
  entries.emplace_back("replaceStrings", make_value(value_tag::primop, b::replace_strings));
  entries.emplace_back("toString", make_value(value_tag::primop, b::to_string));
  entries.emplace_back("concatStrings", make_value(value_tag::primop, b::concat_strings));
  entries.emplace_back("typeOf", make_value(value_tag::primop, b::type_of));

  // Arithmetic (as functions)
  entries.emplace_back("add", make_value(value_tag::primop, b::builtin_add));
  entries.emplace_back("sub", make_value(value_tag::primop, b::builtin_sub));
  entries.emplace_back("mul", make_value(value_tag::primop, b::builtin_mul));
  entries.emplace_back("div", make_value(value_tag::primop, b::builtin_div));
  entries.emplace_back("lessThan", make_value(value_tag::primop, b::builtin_less_than));

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
