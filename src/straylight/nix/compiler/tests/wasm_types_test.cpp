// straylight // nix-language // tests
//
// Unit tests for WASM value types and memory layout

#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "straylight/nix/compiler/compile/wasm_types.h"

using namespace straylight::nix::compiler::compile;

// =============================================================================
// value_tag enum tests
// =============================================================================

TEST_CASE("value_tag enum values are distinct", "[wasm][types]") {
  REQUIRE(static_cast<std::uint8_t>(value_tag::null_value) == 0);
  REQUIRE(static_cast<std::uint8_t>(value_tag::boolean) == 1);
  REQUIRE(static_cast<std::uint8_t>(value_tag::integer) == 2);
  REQUIRE(static_cast<std::uint8_t>(value_tag::floating) == 3);
  REQUIRE(static_cast<std::uint8_t>(value_tag::string) == 4);
  REQUIRE(static_cast<std::uint8_t>(value_tag::path) == 5);
  REQUIRE(static_cast<std::uint8_t>(value_tag::list) == 6);
  REQUIRE(static_cast<std::uint8_t>(value_tag::attribute_set) == 7);
  REQUIRE(static_cast<std::uint8_t>(value_tag::lambda) == 8);
  REQUIRE(static_cast<std::uint8_t>(value_tag::thunk) == 9);
  REQUIRE(static_cast<std::uint8_t>(value_tag::primop) == 10);
}

TEST_CASE("value_tag fits in single byte", "[wasm][types]") {
  // all tags should fit in std::uint8_t
  REQUIRE(sizeof(value_tag) == 1);
}

// =============================================================================
// wasm_type enum tests
// =============================================================================

TEST_CASE("wasm_type enum values", "[wasm][types]") {
  // just verify the enum exists and has expected values
  auto i32 = wasm_type::i32;
  auto i64 = wasm_type::i64;
  auto f32 = wasm_type::f32;
  auto f64 = wasm_type::f64;
  auto funcref = wasm_type::funcref;
  auto externref = wasm_type::externref;

  REQUIRE(i32 != i64);
  REQUIRE(f32 != f64);
  REQUIRE(funcref != externref);
}

// =============================================================================
// memory layout constant tests
// =============================================================================

TEST_CASE("nix_value memory layout", "[wasm][memory]") {
  // nix_value is 8 bytes: tag (4) + payload (4)
  REQUIRE(memory::value_size == 8);
  REQUIRE(memory::value_tag_offset == 0);
  REQUIRE(memory::value_payload_offset == 4);

  // offsets must be within value size
  REQUIRE(memory::value_tag_offset < memory::value_size);
  REQUIRE(memory::value_payload_offset < memory::value_size);
}

TEST_CASE("string memory layout", "[wasm][memory]") {
  // string: length (4) + data
  REQUIRE(memory::string_length_offset == 0);
  REQUIRE(memory::string_data_offset == 4);
}

TEST_CASE("list memory layout", "[wasm][memory]") {
  // list: length (4) + elements
  REQUIRE(memory::list_length_offset == 0);
  REQUIRE(memory::list_elements_offset == 4);
}

TEST_CASE("attrset memory layout", "[wasm][memory]") {
  // attrset: count (4) + entries
  REQUIRE(memory::attrset_count_offset == 0);
  REQUIRE(memory::attrset_entries_offset == 4);
  // each entry: hash (4) + value_ptr (4) + name_ptr (4) = 12
  REQUIRE(memory::attrset_entry_size == 12);
}

TEST_CASE("thunk memory layout", "[wasm][memory]") {
  // thunk: evaluated (4) + payload (4)
  REQUIRE(memory::thunk_evaluated_offset == 0);
  REQUIRE(memory::thunk_payload_offset == 4);
}

TEST_CASE("closure memory layout", "[wasm][memory]") {
  // closure: funcref (4) + env_ptr (4)
  REQUIRE(memory::closure_funcref_offset == 0);
  REQUIRE(memory::closure_env_offset == 4);
}

// =============================================================================
// builtin function index tests
// =============================================================================

TEST_CASE("pure builtin indices are in low range", "[wasm][builtins]") {
  // pure builtins should be 0-99
  REQUIRE(builtins::add < 100);
  REQUIRE(builtins::sub < 100);
  REQUIRE(builtins::mul < 100);
  REQUIRE(builtins::div < 100);
  REQUIRE(builtins::less_than < 100);
  REQUIRE(builtins::length < 100);
  REQUIRE(builtins::head < 100);
  REQUIRE(builtins::tail < 100);
  REQUIRE(builtins::concat < 100);
  REQUIRE(builtins::elem < 100);
  REQUIRE(builtins::type_of < 100);
  REQUIRE(builtins::is_null < 100);
  REQUIRE(builtins::is_bool < 100);
  REQUIRE(builtins::is_int < 100);
  REQUIRE(builtins::is_float < 100);
  REQUIRE(builtins::is_string < 100);
  REQUIRE(builtins::is_path < 100);
  REQUIRE(builtins::is_list < 100);
  REQUIRE(builtins::is_attrs < 100);
  REQUIRE(builtins::is_function < 100);
}

TEST_CASE("impure builtin indices are in 100+ range", "[wasm][builtins]") {
  // impure builtins should be 100-199
  REQUIRE(builtins::import_path >= 100);
  REQUIRE(builtins::import_path < 200);
  REQUIRE(builtins::read_file >= 100);
  REQUIRE(builtins::read_file < 200);
  REQUIRE(builtins::path_exists >= 100);
  REQUIRE(builtins::path_exists < 200);
  REQUIRE(builtins::fetch_url >= 100);
  REQUIRE(builtins::fetch_url < 200);
  REQUIRE(builtins::to_file >= 100);
  REQUIRE(builtins::to_file < 200);
  REQUIRE(builtins::derivation >= 100);
  REQUIRE(builtins::derivation < 200);
  REQUIRE(builtins::store_path >= 100);
  REQUIRE(builtins::store_path < 200);
}

TEST_CASE("string builtin indices are in 200+ range", "[wasm][builtins]") {
  REQUIRE(builtins::string_length >= 200);
  REQUIRE(builtins::string_length < 300);
  REQUIRE(builtins::substring >= 200);
  REQUIRE(builtins::substring < 300);
  REQUIRE(builtins::hash_string >= 200);
  REQUIRE(builtins::hash_string < 300);
  REQUIRE(builtins::match >= 200);
  REQUIRE(builtins::match < 300);
  REQUIRE(builtins::split >= 200);
  REQUIRE(builtins::split < 300);
  REQUIRE(builtins::replace_strings >= 200);
  REQUIRE(builtins::replace_strings < 300);
  REQUIRE(builtins::to_lower >= 200);
  REQUIRE(builtins::to_lower < 300);
  REQUIRE(builtins::to_upper >= 200);
  REQUIRE(builtins::to_upper < 300);
}

TEST_CASE("attrset builtin indices are in 300+ range", "[wasm][builtins]") {
  REQUIRE(builtins::attr_names >= 300);
  REQUIRE(builtins::attr_names < 400);
  REQUIRE(builtins::attr_values >= 300);
  REQUIRE(builtins::attr_values < 400);
  REQUIRE(builtins::get_attr >= 300);
  REQUIRE(builtins::get_attr < 400);
  REQUIRE(builtins::has_attr >= 300);
  REQUIRE(builtins::has_attr < 400);
  REQUIRE(builtins::intersect_attrs >= 300);
  REQUIRE(builtins::intersect_attrs < 400);
  REQUIRE(builtins::remove_attrs >= 300);
  REQUIRE(builtins::remove_attrs < 400);
  REQUIRE(builtins::list_to_attrs >= 300);
  REQUIRE(builtins::list_to_attrs < 400);
}

TEST_CASE("list builtin indices are in 400+ range", "[wasm][builtins]") {
  REQUIRE(builtins::map >= 400);
  REQUIRE(builtins::map < 500);
  REQUIRE(builtins::filter >= 400);
  REQUIRE(builtins::filter < 500);
  REQUIRE(builtins::foldl >= 400);
  REQUIRE(builtins::foldl < 500);
  REQUIRE(builtins::sort >= 400);
  REQUIRE(builtins::sort < 500);
  REQUIRE(builtins::gen_list >= 400);
  REQUIRE(builtins::gen_list < 500);
  REQUIRE(builtins::concat_lists >= 400);
  REQUIRE(builtins::concat_lists < 500);
}

TEST_CASE("builtin indices are unique", "[wasm][builtins]") {
  // collect all builtin indices
  std::vector<std::uint32_t> indices = {
      builtins::add,
      builtins::sub,
      builtins::mul,
      builtins::div,
      builtins::less_than,
      builtins::length,
      builtins::head,
      builtins::tail,
      builtins::concat,
      builtins::elem,
      builtins::type_of,
      builtins::is_null,
      builtins::is_bool,
      builtins::is_int,
      builtins::is_float,
      builtins::is_string,
      builtins::is_path,
      builtins::is_list,
      builtins::is_attrs,
      builtins::is_function,
      builtins::import_path,
      builtins::read_file,
      builtins::path_exists,
      builtins::fetch_url,
      builtins::to_file,
      builtins::derivation,
      builtins::store_path,
      builtins::string_length,
      builtins::substring,
      builtins::hash_string,
      builtins::match,
      builtins::split,
      builtins::replace_strings,
      builtins::to_lower,
      builtins::to_upper,
      builtins::attr_names,
      builtins::attr_values,
      builtins::get_attr,
      builtins::has_attr,
      builtins::intersect_attrs,
      builtins::remove_attrs,
      builtins::list_to_attrs,
      builtins::map,
      builtins::filter,
      builtins::foldl,
      builtins::sort,
      builtins::gen_list,
      builtins::concat_lists,
  };

  // check uniqueness
  std::sort(indices.begin(), indices.end());
  auto last = std::unique(indices.begin(), indices.end());
  REQUIRE(last == indices.end());
}

// =============================================================================
// value packing tests (for documentation/design verification)
// =============================================================================

TEST_CASE("i64 can hold tag and payload", "[wasm][packing]") {
  // we pack values as: (payload << 32) | tag
  // verify this fits in i64

  std::int64_t tag = static_cast<std::int64_t>(value_tag::integer);
  std::int64_t payload = 0x7FFFFFFF; // max positive i32

  std::int64_t packed = (payload << 32) | tag;

  // extract tag
  auto extracted_tag = static_cast<value_tag>(packed & 0xFFFFFFFF);
  // extract payload
  auto extracted_payload = packed >> 32;

  REQUIRE(extracted_tag == value_tag::integer);
  REQUIRE(extracted_payload == payload);
}

TEST_CASE("negative integers pack correctly", "[wasm][packing]") {
  std::int64_t tag = static_cast<std::int64_t>(value_tag::integer);
  std::int64_t value = -42;

  // for negative numbers, we need to handle sign extension carefully
  std::int64_t packed = (value << 32) | tag;

  // extract
  auto extracted_tag = static_cast<value_tag>(packed & 0xFFFFFFFF);
  auto extracted_value = packed >> 32; // arithmetic shift preserves sign

  REQUIRE(extracted_tag == value_tag::integer);
  REQUIRE(extracted_value == value);
}

TEST_CASE("boolean values pack correctly", "[wasm][packing]") {
  std::int64_t tag = static_cast<std::int64_t>(value_tag::boolean);

  std::int64_t packed_false = (0LL << 32) | tag;
  std::int64_t packed_true = (1LL << 32) | tag;

  REQUIRE((packed_false >> 32) == 0);
  REQUIRE((packed_true >> 32) == 1);
}

TEST_CASE("null value packing", "[wasm][packing]") {
  std::int64_t tag = static_cast<std::int64_t>(value_tag::null_value);
  std::int64_t packed = tag; // payload is 0 for null

  REQUIRE((packed & 0xFFFFFFFF) == 0); // tag is 0
  REQUIRE((packed >> 32) == 0);        // payload is 0
}
