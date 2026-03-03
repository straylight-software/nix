// straylight // nix-language // tests
//
// Unit tests for WASM value types and memory layout

#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "straylight/nix/compiler/compile/wasm_types.h"

namespace compile = straylight::nix::compiler::compile;

// =============================================================================
// value_tag enum tests
// =============================================================================

TEST_CASE("value_tag enum values are distinct", "[wasm][types]") {
  REQUIRE(static_cast<std::uint8_t>(compile::value_tag::null_value) == 0);
  REQUIRE(static_cast<std::uint8_t>(compile::value_tag::boolean) == 1);
  REQUIRE(static_cast<std::uint8_t>(compile::value_tag::integer) == 2);
  REQUIRE(static_cast<std::uint8_t>(compile::value_tag::floating) == 3);
  REQUIRE(static_cast<std::uint8_t>(compile::value_tag::string) == 4);
  REQUIRE(static_cast<std::uint8_t>(compile::value_tag::path) == 5);
  REQUIRE(static_cast<std::uint8_t>(compile::value_tag::list) == 6);
  REQUIRE(static_cast<std::uint8_t>(compile::value_tag::attribute_set) == 7);
  REQUIRE(static_cast<std::uint8_t>(compile::value_tag::lambda) == 8);
  REQUIRE(static_cast<std::uint8_t>(compile::value_tag::thunk) == 9);
  REQUIRE(static_cast<std::uint8_t>(compile::value_tag::primop) == 10);
}

TEST_CASE("value_tag fits in single byte", "[wasm][types]") {
  // all tags should fit in std::uint8_t
  REQUIRE(sizeof(compile::value_tag) == 1);
}

// =============================================================================
// wasm_type enum tests
// =============================================================================

TEST_CASE("wasm_type enum values", "[wasm][types]") {
  // just verify the enum exists and has expected values
  auto i32 = compile::wasm_type::i32;
  auto i64 = compile::wasm_type::i64;
  auto f32 = compile::wasm_type::f32;
  auto f64 = compile::wasm_type::f64;
  auto funcref = compile::wasm_type::funcref;
  auto externref = compile::wasm_type::externref;

  REQUIRE(i32 != i64);
  REQUIRE(f32 != f64);
  REQUIRE(funcref != externref);
}

// =============================================================================
// memory layout constant tests
// =============================================================================

TEST_CASE("nix_value memory layout", "[wasm][memory]") {
  // nix_value is 8 bytes: tag (4) + payload (4)
  REQUIRE(compile::memory::value_size == 8);
  REQUIRE(compile::memory::value_tag_offset == 0);
  REQUIRE(compile::memory::value_payload_offset == 4);

  // offsets must be within value size
  REQUIRE(compile::memory::value_tag_offset < compile::memory::value_size);
  REQUIRE(compile::memory::value_payload_offset < compile::memory::value_size);
}

TEST_CASE("string memory layout", "[wasm][memory]") {
  // string: length (4) + data
  REQUIRE(compile::memory::string_length_offset == 0);
  REQUIRE(compile::memory::string_data_offset == 4);
}

TEST_CASE("list memory layout", "[wasm][memory]") {
  // list: length (4) + elements
  REQUIRE(compile::memory::list_length_offset == 0);
  REQUIRE(compile::memory::list_elements_offset == 4);
}

TEST_CASE("attrset memory layout", "[wasm][memory]") {
  // attrset: count (4) + entries
  REQUIRE(compile::memory::attrset_count_offset == 0);
  REQUIRE(compile::memory::attrset_entries_offset == 4);
  // each entry: hash (4) + value_ptr (4) + name_ptr (4) = 12
  REQUIRE(compile::memory::attrset_entry_size == 12);
}

TEST_CASE("thunk memory layout", "[wasm][memory]") {
  // thunk: evaluated (4) + payload (4)
  REQUIRE(compile::memory::thunk_evaluated_offset == 0);
  REQUIRE(compile::memory::thunk_payload_offset == 4);
}

TEST_CASE("closure memory layout", "[wasm][memory]") {
  // closure: funcref (4) + env_ptr (4)
  REQUIRE(compile::memory::closure_funcref_offset == 0);
  REQUIRE(compile::memory::closure_env_offset == 4);
}

// =============================================================================
// builtin function index tests
// =============================================================================

TEST_CASE("pure builtin indices are in low range", "[wasm][builtins]") {
  // pure builtins should be 0-99
  REQUIRE(compile::builtins::add < 100);
  REQUIRE(compile::builtins::sub < 100);
  REQUIRE(compile::builtins::mul < 100);
  REQUIRE(compile::builtins::div < 100);
  REQUIRE(compile::builtins::less_than < 100);
  REQUIRE(compile::builtins::length < 100);
  REQUIRE(compile::builtins::head < 100);
  REQUIRE(compile::builtins::tail < 100);
  REQUIRE(compile::builtins::concat < 100);
  REQUIRE(compile::builtins::elem < 100);
  REQUIRE(compile::builtins::type_of < 100);
  REQUIRE(compile::builtins::is_null < 100);
  REQUIRE(compile::builtins::is_bool < 100);
  REQUIRE(compile::builtins::is_int < 100);
  REQUIRE(compile::builtins::is_float < 100);
  REQUIRE(compile::builtins::is_string < 100);
  REQUIRE(compile::builtins::is_path < 100);
  REQUIRE(compile::builtins::is_list < 100);
  REQUIRE(compile::builtins::is_attrs < 100);
  REQUIRE(compile::builtins::is_function < 100);
}

TEST_CASE("impure builtin indices are in 100+ range", "[wasm][builtins]") {
  // impure builtins should be 100-199
  REQUIRE(compile::builtins::import_path >= 100);
  REQUIRE(compile::builtins::import_path < 200);
  REQUIRE(compile::builtins::read_file >= 100);
  REQUIRE(compile::builtins::read_file < 200);
  REQUIRE(compile::builtins::path_exists >= 100);
  REQUIRE(compile::builtins::path_exists < 200);
  REQUIRE(compile::builtins::fetch_url >= 100);
  REQUIRE(compile::builtins::fetch_url < 200);
  REQUIRE(compile::builtins::to_file >= 100);
  REQUIRE(compile::builtins::to_file < 200);
  REQUIRE(compile::builtins::derivation >= 100);
  REQUIRE(compile::builtins::derivation < 200);
  REQUIRE(compile::builtins::store_path >= 100);
  REQUIRE(compile::builtins::store_path < 200);
}

TEST_CASE("string builtin indices are in 200+ range", "[wasm][builtins]") {
  REQUIRE(compile::builtins::string_length >= 200);
  REQUIRE(compile::builtins::string_length < 300);
  REQUIRE(compile::builtins::substring >= 200);
  REQUIRE(compile::builtins::substring < 300);
  REQUIRE(compile::builtins::hash_string >= 200);
  REQUIRE(compile::builtins::hash_string < 300);
  REQUIRE(compile::builtins::match >= 200);
  REQUIRE(compile::builtins::match < 300);
  REQUIRE(compile::builtins::split >= 200);
  REQUIRE(compile::builtins::split < 300);
  REQUIRE(compile::builtins::replace_strings >= 200);
  REQUIRE(compile::builtins::replace_strings < 300);
  REQUIRE(compile::builtins::to_lower >= 200);
  REQUIRE(compile::builtins::to_lower < 300);
  REQUIRE(compile::builtins::to_upper >= 200);
  REQUIRE(compile::builtins::to_upper < 300);
}

TEST_CASE("attrset builtin indices are in 300+ range", "[wasm][builtins]") {
  REQUIRE(compile::builtins::attr_names >= 300);
  REQUIRE(compile::builtins::attr_names < 400);
  REQUIRE(compile::builtins::attr_values >= 300);
  REQUIRE(compile::builtins::attr_values < 400);
  REQUIRE(compile::builtins::get_attr >= 300);
  REQUIRE(compile::builtins::get_attr < 400);
  REQUIRE(compile::builtins::has_attr >= 300);
  REQUIRE(compile::builtins::has_attr < 400);
  REQUIRE(compile::builtins::intersect_attrs >= 300);
  REQUIRE(compile::builtins::intersect_attrs < 400);
  REQUIRE(compile::builtins::remove_attrs >= 300);
  REQUIRE(compile::builtins::remove_attrs < 400);
  REQUIRE(compile::builtins::list_to_attrs >= 300);
  REQUIRE(compile::builtins::list_to_attrs < 400);
}

TEST_CASE("list builtin indices are in 400+ range", "[wasm][builtins]") {
  REQUIRE(compile::builtins::map >= 400);
  REQUIRE(compile::builtins::map < 500);
  REQUIRE(compile::builtins::filter >= 400);
  REQUIRE(compile::builtins::filter < 500);
  REQUIRE(compile::builtins::foldl >= 400);
  REQUIRE(compile::builtins::foldl < 500);
  REQUIRE(compile::builtins::sort >= 400);
  REQUIRE(compile::builtins::sort < 500);
  REQUIRE(compile::builtins::gen_list >= 400);
  REQUIRE(compile::builtins::gen_list < 500);
  REQUIRE(compile::builtins::concat_lists >= 400);
  REQUIRE(compile::builtins::concat_lists < 500);
}

TEST_CASE("builtin indices are unique", "[wasm][builtins]") {
  // collect all builtin indices
  std::vector<std::uint32_t> indices = {
      compile::builtins::add,
      compile::builtins::sub,
      compile::builtins::mul,
      compile::builtins::div,
      compile::builtins::less_than,
      compile::builtins::length,
      compile::builtins::head,
      compile::builtins::tail,
      compile::builtins::concat,
      compile::builtins::elem,
      compile::builtins::type_of,
      compile::builtins::is_null,
      compile::builtins::is_bool,
      compile::builtins::is_int,
      compile::builtins::is_float,
      compile::builtins::is_string,
      compile::builtins::is_path,
      compile::builtins::is_list,
      compile::builtins::is_attrs,
      compile::builtins::is_function,
      compile::builtins::import_path,
      compile::builtins::read_file,
      compile::builtins::path_exists,
      compile::builtins::fetch_url,
      compile::builtins::to_file,
      compile::builtins::derivation,
      compile::builtins::store_path,
      compile::builtins::string_length,
      compile::builtins::substring,
      compile::builtins::hash_string,
      compile::builtins::match,
      compile::builtins::split,
      compile::builtins::replace_strings,
      compile::builtins::to_lower,
      compile::builtins::to_upper,
      compile::builtins::attr_names,
      compile::builtins::attr_values,
      compile::builtins::get_attr,
      compile::builtins::has_attr,
      compile::builtins::intersect_attrs,
      compile::builtins::remove_attrs,
      compile::builtins::list_to_attrs,
      compile::builtins::map,
      compile::builtins::filter,
      compile::builtins::foldl,
      compile::builtins::sort,
      compile::builtins::gen_list,
      compile::builtins::concat_lists,
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

  std::int64_t tag = static_cast<std::int64_t>(compile::value_tag::integer);
  std::int64_t payload = 0x7FFFFFFF; // max positive i32

  std::int64_t packed = (payload << 32) | tag;

  // extract tag
  auto extracted_tag = static_cast<compile::value_tag>(packed & 0xFFFFFFFF);
  // extract payload
  auto extracted_payload = packed >> 32;

  REQUIRE(extracted_tag == compile::value_tag::integer);
  REQUIRE(extracted_payload == payload);
}

TEST_CASE("negative integers pack correctly", "[wasm][packing]") {
  std::int64_t tag = static_cast<std::int64_t>(compile::value_tag::integer);
  std::int64_t value = -42;

  // for negative numbers, we need to handle sign extension carefully
  std::int64_t packed = (value << 32) | tag;

  // extract
  auto extracted_tag = static_cast<compile::value_tag>(packed & 0xFFFFFFFF);
  auto extracted_value = packed >> 32; // arithmetic shift preserves sign

  REQUIRE(extracted_tag == compile::value_tag::integer);
  REQUIRE(extracted_value == value);
}

TEST_CASE("boolean values pack correctly", "[wasm][packing]") {
  std::int64_t tag = static_cast<std::int64_t>(compile::value_tag::boolean);

  std::int64_t packed_false = (0LL << 32) | tag;
  std::int64_t packed_true = (1LL << 32) | tag;

  REQUIRE((packed_false >> 32) == 0);
  REQUIRE((packed_true >> 32) == 1);
}

TEST_CASE("null value packing", "[wasm][packing]") {
  std::int64_t tag = static_cast<std::int64_t>(compile::value_tag::null_value);
  std::int64_t packed = tag; // payload is 0 for null

  REQUIRE((packed & 0xFFFFFFFF) == 0); // tag is 0
  REQUIRE((packed >> 32) == 0);        // payload is 0
}
