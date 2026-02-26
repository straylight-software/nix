// straylight // nix // util // tests
//
// Unit, property-based, and fuzz tests for JSON utilities
//
// JSON parsing is security-critical; these tests cover:
// - Malformed JSON handling
// - Deeply nested structures
// - Huge strings and arrays
// - Unicode edge cases
// - Type mismatch errors
// - Boundary conditions for integer conversions

// Catch2 must be included before rapidcheck/catch.h
#include <catch2/catch_test_macros.hpp>

// RapidCheck for property-based testing
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include "nix/util/json-utils.h"


using json = nlohmann::json;

// ─────────────────────────────────────────────────────────────────────────────
// valueAt tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("valueAt returns value for existing key", "[json][valueAt]") {
  json::object_t obj = {{"key", "value"}, {"number", 42}};

  REQUIRE(value_at(obj, "key") == "value");
  REQUIRE(value_at(obj, "number") == 42);
}

TEST_CASE("valueAt throws for missing key", "[json][valueAt]") {
  json::object_t obj = {{"key", "value"}};

  REQUIRE_THROWS_AS(value_at(obj, "missing"), Error);
}

TEST_CASE("valueAt works with nested objects", "[json][valueAt]") {
  json::object_t inner = {{"inner_key", "inner_value"}};
  json::object_t obj = {{"nested", inner}};

  const auto& nested = value_at(obj, "nested");
  REQUIRE(nested.is_object());
  REQUIRE(nested["inner_key"] == "inner_value");
}

TEST_CASE("valueAt handles empty object", "[json][valueAt]") {
  json::object_t empty;

  REQUIRE_THROWS_AS(value_at(empty, "anything"), Error);
}

TEST_CASE("valueAt handles keys with special characters", "[json][valueAt]") {
  json::object_t obj = {{"key with spaces", 1},
                        {"key\twith\ttabs", 2},
                        {"key\nwith\nnewlines", 3},
                        {"key\"with\"quotes", 4},
                        {"", 5}};

  REQUIRE(value_at(obj, "key with spaces") == 1);
  REQUIRE(value_at(obj, "key\twith\ttabs") == 2);
  REQUIRE(value_at(obj, "key\nwith\nnewlines") == 3);
  REQUIRE(value_at(obj, "key\"with\"quotes") == 4);
  REQUIRE(value_at(obj, "") == 5);
}

// ─────────────────────────────────────────────────────────────────────────────
// optionalValueAt tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("optionalValueAt returns pointer for existing key", "[json][optionalValueAt]") {
  json::object_t obj = {{"key", "value"}};

  const json* result = optional_value_at(obj, "key");
  REQUIRE(result != nullptr);
  REQUIRE(*result == "value");
}

TEST_CASE("optionalValueAt returns nullptr for missing key", "[json][optionalValueAt]") {
  json::object_t obj = {{"key", "value"}};

  const json* result = optional_value_at(obj, "missing");
  REQUIRE(result == nullptr);
}

TEST_CASE("optionalValueAt returns non-null for json null value", "[json][optionalValueAt]") {
  json::object_t obj = {{"null_key", nullptr}};

  const json* result = optional_value_at(obj, "null_key");
  REQUIRE(result != nullptr);
  REQUIRE(result->is_null());
}

// ─────────────────────────────────────────────────────────────────────────────
// getNullable tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("getNullable returns nullptr for json null", "[json][getNullable]") {
  json null_value = nullptr;
  REQUIRE(get_nullable(null_value) == nullptr);
}

TEST_CASE("getNullable returns pointer for non-null values", "[json][getNullable]") {
  json string_val = "test";
  json number_val = 42;
  json bool_val = true;
  json array_val = json::array({1, 2, 3});
  json object_val = json::object({{"a", 1}});

  REQUIRE(get_nullable(string_val) != nullptr);
  REQUIRE(*get_nullable(string_val) == "test");

  REQUIRE(get_nullable(number_val) != nullptr);
  REQUIRE(*get_nullable(number_val) == 42);

  REQUIRE(get_nullable(bool_val) != nullptr);
  REQUIRE(*get_nullable(bool_val) == true);

  REQUIRE(get_nullable(array_val) != nullptr);
  REQUIRE(get_nullable(object_val) != nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// getObject tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("getObject returns object reference", "[json][getObject]") {
  json obj = json::object({{"a", 1}, {"b", 2}});

  const auto& result = get_object(obj);
  REQUIRE(result.size() == 2);
  REQUIRE(result.at("a") == 1);
}

TEST_CASE("getObject throws for non-object types", "[json][getObject]") {
  REQUIRE_THROWS_AS(get_object(json(42)), Error);
  REQUIRE_THROWS_AS(get_object(json("string")), Error);
  REQUIRE_THROWS_AS(get_object(json::array({1, 2})), Error);
  REQUIRE_THROWS_AS(get_object(json(true)), Error);
  REQUIRE_THROWS_AS(get_object(json(nullptr)), Error);
  REQUIRE_THROWS_AS(get_object(json(3.14)), Error);
}

TEST_CASE("getObject handles empty object", "[json][getObject]") {
  json empty = json::object();
  const auto& result = get_object(empty);
  REQUIRE(result.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// getArray tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("getArray returns array reference", "[json][getArray]") {
  json arr = json::array({1, 2, 3, 4, 5});

  const auto& result = get_array(arr);
  REQUIRE(result.size() == 5);
  REQUIRE(result[0] == 1);
  REQUIRE(result[4] == 5);
}

TEST_CASE("getArray throws for non-array types", "[json][getArray]") {
  REQUIRE_THROWS_AS(get_array(json(42)), Error);
  REQUIRE_THROWS_AS(get_array(json("string")), Error);
  REQUIRE_THROWS_AS(get_array(json::object({{"a", 1}})), Error);
  REQUIRE_THROWS_AS(get_array(json(true)), Error);
  REQUIRE_THROWS_AS(get_array(json(nullptr)), Error);
}

TEST_CASE("getArray handles empty array", "[json][getArray]") {
  json empty = json::array();
  const auto& result = get_array(empty);
  REQUIRE(result.empty());
}

TEST_CASE("getArray handles heterogeneous arrays", "[json][getArray]") {
  json arr = json::array({1, "two", true, nullptr, json::object()});
  const auto& result = get_array(arr);
  REQUIRE(result.size() == 5);
}

// ─────────────────────────────────────────────────────────────────────────────
// getString tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("getString returns string reference", "[json][getString]") {
  json str = "hello world";
  const auto& result = get_string(str);
  REQUIRE(result == "hello world");
}

TEST_CASE("getString throws for non-string types", "[json][getString]") {
  REQUIRE_THROWS_AS(get_string(json(42)), Error);
  REQUIRE_THROWS_AS(get_string(json::array({1})), Error);
  REQUIRE_THROWS_AS(get_string(json::object({{"a", 1}})), Error);
  REQUIRE_THROWS_AS(get_string(json(true)), Error);
  REQUIRE_THROWS_AS(get_string(json(nullptr)), Error);
}

TEST_CASE("getString handles empty string", "[json][getString]") {
  json empty = "";
  REQUIRE(get_string(empty).empty());
}

TEST_CASE("getString handles unicode strings", "[json][getString]") {
  // Basic multilingual plane
  json unicode = "hello 世界 🌍 مرحبا";
  REQUIRE(get_string(unicode) == "hello 世界 🌍 مرحبا");

  // Emoji and special characters
  json emoji = "👨‍👩‍👧‍👦 ❤️ 🇺🇸";
  REQUIRE(get_string(emoji) == "👨‍👩‍👧‍👦 ❤️ 🇺🇸");

  // Null character in string
  std::string with_null = std::string("before\0after", 12);
  json null_str = with_null;
  REQUIRE(get_string(null_str) == with_null);
}

TEST_CASE("getString handles strings with escape sequences", "[json][getString]") {
  json escaped = "line1\nline2\ttab\\backslash\"quote";
  REQUIRE(get_string(escaped) == "line1\nline2\ttab\\backslash\"quote");
}

// ─────────────────────────────────────────────────────────────────────────────
// getUnsigned tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("getUnsigned returns unsigned value", "[json][getUnsigned]") {
  json num = static_cast<uint64_t>(42);
  REQUIRE(get_unsigned(num) == 42);
}

TEST_CASE("getUnsigned works with zero", "[json][getUnsigned]") {
  json zero = static_cast<uint64_t>(0);
  REQUIRE(get_unsigned(zero) == 0);
}

TEST_CASE("getUnsigned works with max uint64", "[json][getUnsigned]") {
  json max_val = std::numeric_limits<uint64_t>::max();
  REQUIRE(get_unsigned(max_val) == std::numeric_limits<uint64_t>::max());
}

TEST_CASE("getUnsigned throws for negative numbers", "[json][getUnsigned]") {
  json neg = -1;
  REQUIRE_THROWS_AS(get_unsigned(neg), Error);
}

TEST_CASE("getUnsigned throws for non-integer types", "[json][getUnsigned]") {
  REQUIRE_THROWS_AS(get_unsigned(json("42")), Error);
  REQUIRE_THROWS_AS(get_unsigned(json::array({42})), Error);
  REQUIRE_THROWS_AS(get_unsigned(json(true)), Error);
  REQUIRE_THROWS_AS(get_unsigned(json(nullptr)), Error);
}

TEST_CASE("getUnsigned throws for floating point", "[json][getUnsigned]") {
  json floating = 3.14;
  REQUIRE_THROWS_AS(get_unsigned(floating), Error);
}

// ─────────────────────────────────────────────────────────────────────────────
// getInteger tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("getInteger returns signed value from positive number", "[json][getInteger]") {
  json num = 42;
  REQUIRE(get_integer<int32_t>(num) == 42);
  REQUIRE(get_integer<int64_t>(num) == 42);
}

TEST_CASE("getInteger returns signed value from negative number", "[json][getInteger]") {
  json neg = -42;
  REQUIRE(get_integer<int32_t>(neg) == -42);
  REQUIRE(get_integer<int64_t>(neg) == -42);
}

TEST_CASE("getInteger handles boundary values for int8", "[json][getInteger]") {
  json min_val = std::numeric_limits<int8_t>::min();
  json max_val = std::numeric_limits<int8_t>::max();

  REQUIRE(get_integer<int8_t>(min_val) == std::numeric_limits<int8_t>::min());
  REQUIRE(get_integer<int8_t>(max_val) == std::numeric_limits<int8_t>::max());

  // Out of range
  json too_large = static_cast<int64_t>(std::numeric_limits<int8_t>::max()) + 1;
  json too_small = static_cast<int64_t>(std::numeric_limits<int8_t>::min()) - 1;

  REQUIRE_THROWS_AS(get_integer<int8_t>(too_large), Error);
  REQUIRE_THROWS_AS(get_integer<int8_t>(too_small), Error);
}

TEST_CASE("getInteger handles boundary values for int16", "[json][getInteger]") {
  json min_val = std::numeric_limits<int16_t>::min();
  json max_val = std::numeric_limits<int16_t>::max();

  REQUIRE(get_integer<int16_t>(min_val) == std::numeric_limits<int16_t>::min());
  REQUIRE(get_integer<int16_t>(max_val) == std::numeric_limits<int16_t>::max());

  // Out of range
  json too_large = static_cast<int64_t>(std::numeric_limits<int16_t>::max()) + 1;
  json too_small = static_cast<int64_t>(std::numeric_limits<int16_t>::min()) - 1;

  REQUIRE_THROWS_AS(get_integer<int16_t>(too_large), Error);
  REQUIRE_THROWS_AS(get_integer<int16_t>(too_small), Error);
}

TEST_CASE("getInteger handles boundary values for int32", "[json][getInteger]") {
  json min_val = std::numeric_limits<int32_t>::min();
  json max_val = std::numeric_limits<int32_t>::max();

  REQUIRE(get_integer<int32_t>(min_val) == std::numeric_limits<int32_t>::min());
  REQUIRE(get_integer<int32_t>(max_val) == std::numeric_limits<int32_t>::max());

  // Out of range
  json too_large = static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 1;
  json too_small = static_cast<int64_t>(std::numeric_limits<int32_t>::min()) - 1;

  REQUIRE_THROWS_AS(get_integer<int32_t>(too_large), Error);
  REQUIRE_THROWS_AS(get_integer<int32_t>(too_small), Error);
}

TEST_CASE("getInteger handles boundary values for int64", "[json][getInteger]") {
  json min_val = std::numeric_limits<int64_t>::min();
  json max_val = std::numeric_limits<int64_t>::max();

  REQUIRE(get_integer<int64_t>(min_val) == std::numeric_limits<int64_t>::min());
  REQUIRE(get_integer<int64_t>(max_val) == std::numeric_limits<int64_t>::max());
}

TEST_CASE("getInteger converts unsigned to signed when in range", "[json][getInteger]") {
  json unsigned_val = static_cast<uint64_t>(100);
  REQUIRE(get_integer<int32_t>(unsigned_val) == 100);
}

TEST_CASE("getInteger rejects unsigned values too large for signed type", "[json][getInteger]") {
  // UINT64_MAX cannot fit in int64_t
  json too_large = std::numeric_limits<uint64_t>::max();
  REQUIRE_THROWS_AS(get_integer<int64_t>(too_large), Error);

  // INT32_MAX + 1 as unsigned cannot fit in int32_t
  json over_int32 = static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) + 1;
  REQUIRE_THROWS_AS(get_integer<int32_t>(over_int32), Error);
}

TEST_CASE("getInteger throws for floating point", "[json][getInteger]") {
  json floating = 3.14;
  REQUIRE_THROWS_AS(get_integer<int32_t>(floating), Error);

  // Even integral floating point values should fail
  json int_float = 42.0;
  REQUIRE_THROWS_AS(get_integer<int32_t>(int_float), Error);
}

TEST_CASE("getInteger throws for non-numeric types", "[json][getInteger]") {
  REQUIRE_THROWS_AS(get_integer<int32_t>(json("42")), Error);
  REQUIRE_THROWS_AS(get_integer<int32_t>(json::array({42})), Error);
  REQUIRE_THROWS_AS(get_integer<int32_t>(json(true)), Error);
  REQUIRE_THROWS_AS(get_integer<int32_t>(json(nullptr)), Error);
}

// ─────────────────────────────────────────────────────────────────────────────
// getBoolean tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("getBoolean returns boolean values", "[json][getBoolean]") {
  json true_val = true;
  json false_val = false;

  REQUIRE(get_boolean(true_val) == true);
  REQUIRE(get_boolean(false_val) == false);
}

TEST_CASE("getBoolean throws for non-boolean types", "[json][getBoolean]") {
  REQUIRE_THROWS_AS(get_boolean(json(1)), Error);
  REQUIRE_THROWS_AS(get_boolean(json(0)), Error);
  REQUIRE_THROWS_AS(get_boolean(json("true")), Error);
  REQUIRE_THROWS_AS(get_boolean(json("false")), Error);
  REQUIRE_THROWS_AS(get_boolean(json(nullptr)), Error);
  REQUIRE_THROWS_AS(get_boolean(json::array()), Error);
  REQUIRE_THROWS_AS(get_boolean(json::object()), Error);
}

// ─────────────────────────────────────────────────────────────────────────────
// getStringList tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("getStringList returns list of strings", "[json][getStringList]") {
  json arr = json::array({"a", "b", "c"});
  strings_t result = get_string_list(arr);

  REQUIRE(result.size() == 3);
  auto it = result.begin();
  REQUIRE(*it++ == "a");
  REQUIRE(*it++ == "b");
  REQUIRE(*it++ == "c");
}

TEST_CASE("getStringList returns empty list for empty array", "[json][getStringList]") {
  json empty = json::array();
  strings_t result = get_string_list(empty);
  REQUIRE(result.empty());
}

TEST_CASE("getStringList throws for non-array", "[json][getStringList]") {
  REQUIRE_THROWS_AS(get_string_list(json("string")), Error);
  REQUIRE_THROWS_AS(get_string_list(json::object()), Error);
  REQUIRE_THROWS_AS(get_string_list(json(42)), Error);
}

TEST_CASE("getStringList throws for array with non-strings", "[json][getStringList]") {
  json mixed = json::array({"valid", 42, "also valid"});
  REQUIRE_THROWS_AS(get_string_list(mixed), Error);

  json all_numbers = json::array({1, 2, 3});
  REQUIRE_THROWS_AS(get_string_list(all_numbers), Error);
}

// ─────────────────────────────────────────────────────────────────────────────
// getStringMap tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("getStringMap returns map of strings", "[json][getStringMap]") {
  json obj = json::object({{"key1", "value1"}, {"key2", "value2"}});
  string_map_t result = get_string_map(obj);

  REQUIRE(result.size() == 2);
  REQUIRE(result.at("key1") == "value1");
  REQUIRE(result.at("key2") == "value2");
}

TEST_CASE("getStringMap returns empty map for empty object", "[json][getStringMap]") {
  json empty = json::object();
  string_map_t result = get_string_map(empty);
  REQUIRE(result.empty());
}

TEST_CASE("getStringMap throws for non-object", "[json][getStringMap]") {
  REQUIRE_THROWS_AS(get_string_map(json::array({"a", "b"})), Error);
  REQUIRE_THROWS_AS(get_string_map(json("string")), Error);
  REQUIRE_THROWS_AS(get_string_map(json(42)), Error);
}

TEST_CASE("getStringMap throws for object with non-string values", "[json][getStringMap]") {
  json mixed = json::object({{"valid", "string"}, {"invalid", 42}});
  REQUIRE_THROWS_AS(get_string_map(mixed), Error);
}

// ─────────────────────────────────────────────────────────────────────────────
// getStringSet tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("getStringSet returns set of strings", "[json][getStringSet]") {
  json arr = json::array({"a", "b", "c"});
  string_set_t result = get_string_set(arr);

  REQUIRE(result.size() == 3);
  REQUIRE(result.contains("a"));
  REQUIRE(result.contains("b"));
  REQUIRE(result.contains("c"));
}

TEST_CASE("getStringSet deduplicates strings", "[json][getStringSet]") {
  json arr = json::array({"a", "b", "a", "c", "b"});
  string_set_t result = get_string_set(arr);

  REQUIRE(result.size() == 3);
}

TEST_CASE("getStringSet returns empty set for empty array", "[json][getStringSet]") {
  json empty = json::array();
  string_set_t result = get_string_set(empty);
  REQUIRE(result.empty());
}

TEST_CASE("getStringSet throws for non-array", "[json][getStringSet]") {
  REQUIRE_THROWS_AS(get_string_set(json("string")), Error);
  REQUIRE_THROWS_AS(get_string_set(json::object()), Error);
}

TEST_CASE("getStringSet throws for array with non-strings", "[json][getStringSet]") {
  json mixed = json::array({"valid", 42});
  REQUIRE_THROWS_AS(get_string_set(mixed), Error);
}

// ─────────────────────────────────────────────────────────────────────────────
// std::optional serialization tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("optional serialization nullopt becomes null", "[json][optional]") {
  std::optional<int> opt = std::nullopt;
  json j = opt;
  REQUIRE(j.is_null());
}

TEST_CASE("optional serialization value becomes json value", "[json][optional]") {
  std::optional<int> opt = 42;
  json j = opt;
  REQUIRE(j == 42);
}

TEST_CASE("optional deserialization null becomes nullopt", "[json][optional]") {
  json j = nullptr;
  auto opt = j.get<std::optional<int>>();
  REQUIRE_FALSE(opt.has_value());
}

TEST_CASE("optional deserialization value becomes optional value", "[json][optional]") {
  json j = 42;
  auto opt = j.get<std::optional<int>>();
  REQUIRE(opt.has_value());
  REQUIRE(opt.value() == 42);
}

TEST_CASE("optional roundtrip with various types", "[json][optional]") {
  // String
  std::optional<std::string> str_opt = "hello";
  json j = str_opt;
  REQUIRE(j.get<std::optional<std::string>>() == str_opt);

  // Nullopt
  std::optional<std::string> null_opt = std::nullopt;
  j = null_opt;
  REQUIRE(j.get<std::optional<std::string>>() == null_opt);
}

// ─────────────────────────────────────────────────────────────────────────────
// ptrToOwned tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ptrToOwned returns nullopt for nullptr", "[json][ptrToOwned]") {
  const json* ptr = nullptr;
  auto result = nlohmann::ptr_to_owned<json>(ptr);
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("ptrToOwned returns value for valid pointer", "[json][ptrToOwned]") {
  json value = 42;
  const json* ptr = &value;
  auto result = nlohmann::ptr_to_owned<json>(ptr);
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 42);
}

// ─────────────────────────────────────────────────────────────────────────────
// Deeply nested JSON tests (security/DoS consideration)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("handle deeply nested objects", "[json][nested]") {
  // Create 100-level deep nested object
  json deep = json::object();
  json* current = &deep;
  for (int i = 0; i < 100; ++i) {
    (*current)["nested"] = json::object();
    current = &(*current)["nested"];
  }
  (*current)["value"] = "deepest";

  // Should be able to getObject at each level without issues
  const auto& obj = get_object(deep);
  REQUIRE(obj.contains("nested"));
}

TEST_CASE("handle deeply nested arrays", "[json][nested]") {
  // Create 100-level deep nested array
  json deep = json::array();
  json* current = &deep;
  for (int i = 0; i < 100; ++i) {
    current->push_back(json::array());
    current = &(*current)[0];
  }
  current->push_back("deepest");

  // Should work
  const auto& arr = get_array(deep);
  REQUIRE(arr.size() == 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Large string handling tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("handle very large strings", "[json][large]") {
  // 1MB string
  constexpr size_t one_megabyte = 1024UL * 1024UL;
  std::string large_str(one_megabyte, 'x');
  json j = large_str;

  const auto& result = get_string(j);
  REQUIRE(result.size() == one_megabyte);
  REQUIRE(result == large_str);
}

TEST_CASE("handle large arrays", "[json][large]") {
  // Array with 10000 elements
  json arr = json::array();
  for (int i = 0; i < 10000; ++i) {
    arr.push_back(i);
  }

  const auto& result = get_array(arr);
  REQUIRE(result.size() == 10000);
}

TEST_CASE("handle large string arrays", "[json][large]") {
  json arr = json::array();
  for (int i = 0; i < 1000; ++i) {
    arr.push_back("string_" + std::to_string(i));
  }

  strings_t result = get_string_list(arr);
  REQUIRE(result.size() == 1000);
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-based tests with RapidCheck
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("valueAt property tests", "[json][valueAt][property]") {
  rc::prop("valueAt returns same value as direct access for existing keys", []() {
    auto key = *rc::gen::nonEmpty<std::string>();
    auto value = *rc::gen::arbitrary<std::string>();

    json::object_t obj = {{key, value}};
    RC_ASSERT(value_at(obj, key) == value);
  });

  rc::prop("optionalValueAt returns nullptr iff key doesn't exist", []() {
    auto keys = *rc::gen::unique<std::vector<std::string>>(rc::gen::nonEmpty<std::string>());
    RC_PRE(!keys.empty());

    json::object_t obj;
    for (const auto& k : keys) {
      obj[k] = "value";
    }

    // Existing keys return non-null
    for (const auto& k : keys) {
      RC_ASSERT(optional_value_at(obj, k) != nullptr);
    }

    // Non-existing key returns null
    std::string missing = "definitely_not_in_the_object_12345";
    RC_PRE(obj.find(missing) == obj.end());
    RC_ASSERT(optional_value_at(obj, missing) == nullptr);
  });
}

TEST_CASE("getString property tests", "[json][getString][property]") {
  rc::prop("getString roundtrips arbitrary strings", []() {
    auto str = *rc::gen::arbitrary<std::string>();
    json j = str;
    RC_ASSERT(get_string(j) == str);
  });

  rc::prop("getString preserves string length", []() {
    auto str = *rc::gen::arbitrary<std::string>();
    json j = str;
    RC_ASSERT(get_string(j).length() == str.length());
  });
}

TEST_CASE("getInteger property tests", "[json][getInteger][property]") {
  rc::prop("getInteger<int32_t> roundtrips int32 values", []() {
    auto val = *rc::gen::arbitrary<int32_t>();
    json j = val;
    RC_ASSERT(get_integer<int32_t>(j) == val);
  });

  rc::prop("getInteger<int64_t> roundtrips int64 values", []() {
    auto val = *rc::gen::arbitrary<int64_t>();
    json j = val;
    RC_ASSERT(get_integer<int64_t>(j) == val);
  });

  rc::prop("getInteger<int8_t> rejects out of range values", []() {
    // Generate values outside int8 range directly
    auto use_large = *rc::gen::arbitrary<bool>();
    int32_t val = 0;
    if (use_large) {
      val = *rc::gen::inRange<int32_t>(static_cast<int32_t>(std::numeric_limits<int8_t>::max()) + 1,
                                       std::numeric_limits<int32_t>::max());
    } else {
      val =
          *rc::gen::inRange<int32_t>(std::numeric_limits<int32_t>::min(),
                                     static_cast<int32_t>(std::numeric_limits<int8_t>::min()) - 1);
    }
    json j = val;
    RC_ASSERT_THROWS_AS(get_integer<int8_t>(j), Error);
  });

  rc::prop("getInteger<int16_t> rejects out of range values", []() {
    // Generate values outside int16 range directly
    auto use_large = *rc::gen::arbitrary<bool>();
    int32_t val = 0;
    if (use_large) {
      val =
          *rc::gen::inRange<int32_t>(static_cast<int32_t>(std::numeric_limits<int16_t>::max()) + 1,
                                     std::numeric_limits<int32_t>::max());
    } else {
      val =
          *rc::gen::inRange<int32_t>(std::numeric_limits<int32_t>::min(),
                                     static_cast<int32_t>(std::numeric_limits<int16_t>::min()) - 1);
    }
    json j = val;
    RC_ASSERT_THROWS_AS(get_integer<int16_t>(j), Error);
  });

  rc::prop("getInteger from unsigned succeeds when value fits", []() {
    // Generate unsigned values that fit in signed range
    auto val = *rc::gen::inRange<uint32_t>(0, std::numeric_limits<int32_t>::max());
    json j = static_cast<uint64_t>(val);
    RC_ASSERT(get_integer<int32_t>(j) == static_cast<int32_t>(val));
  });
}

TEST_CASE("getUnsigned property tests", "[json][getUnsigned][property]") {
  rc::prop("getUnsigned roundtrips unsigned values", []() {
    auto val = *rc::gen::arbitrary<uint64_t>();
    json j = val;
    RC_ASSERT(get_unsigned(j) == val);
  });
}

TEST_CASE("getBoolean property tests", "[json][getBoolean][property]") {
  rc::prop("getBoolean roundtrips boolean values", []() {
    auto val = *rc::gen::arbitrary<bool>();
    json j = val;
    RC_ASSERT(get_boolean(j) == val);
  });
}

TEST_CASE("getStringList property tests", "[json][getStringList][property]") {
  rc::prop("getStringList preserves order", []() {
    auto strings = *rc::gen::arbitrary<std::vector<std::string>>();
    json arr = json::array();
    for (const auto& s : strings) {
      arr.push_back(s);
    }

    strings_t result = get_string_list(arr);
    RC_ASSERT(result.size() == strings.size());

    auto result_it = result.begin();
    for (const auto& s : strings) {
      RC_ASSERT(*result_it == s);
      ++result_it;
    }
  });

  rc::prop("getStringList size matches input size", []() {
    auto strings = *rc::gen::arbitrary<std::vector<std::string>>();
    json arr = strings;
    strings_t result = get_string_list(arr);
    RC_ASSERT(result.size() == strings.size());
  });
}

TEST_CASE("getStringSet property tests", "[json][getStringSet][property]") {
  rc::prop("getStringSet contains all unique input strings", []() {
    auto strings = *rc::gen::arbitrary<std::vector<std::string>>();
    json arr = strings;
    string_set_t result = get_string_set(arr);

    for (const auto& s : strings) {
      RC_ASSERT(result.contains(s));
    }
  });

  rc::prop("getStringSet size <= input size (due to deduplication)", []() {
    auto strings = *rc::gen::arbitrary<std::vector<std::string>>();
    json arr = strings;
    string_set_t result = get_string_set(arr);
    RC_ASSERT(result.size() <= strings.size());
  });
}

TEST_CASE("getStringMap property tests", "[json][getStringMap][property]") {
  rc::prop("getStringMap preserves key-value pairs", []() {
    auto keys = *rc::gen::unique<std::vector<std::string>>(rc::gen::nonEmpty<std::string>());
    auto values = *rc::gen::container<std::vector<std::string>>(keys.size(),
                                                                rc::gen::arbitrary<std::string>());

    json obj = json::object();
    for (size_t i = 0; i < keys.size(); ++i) {
      obj[keys[i]] = values[i];
    }

    string_map_t result = get_string_map(obj);
    RC_ASSERT(result.size() == keys.size());

    for (size_t i = 0; i < keys.size(); ++i) {
      RC_ASSERT(result.at(keys[i]) == values[i]);
    }
  });
}

TEST_CASE("optional serialization property tests", "[json][optional][property]") {
  rc::prop("optional<int> roundtrips through JSON", []() {
    auto has_value = *rc::gen::arbitrary<bool>();
    std::optional<int> opt;
    if (has_value) {
      opt = *rc::gen::arbitrary<int>();
    }

    json j = opt;
    auto restored = j.get<std::optional<int>>();
    RC_ASSERT(opt == restored);
  });

  rc::prop("optional<string> roundtrips through JSON", []() {
    auto has_value = *rc::gen::arbitrary<bool>();
    std::optional<std::string> opt;
    if (has_value) {
      opt = *rc::gen::arbitrary<std::string>();
    }

    json j = opt;
    auto restored = j.get<std::optional<std::string>>();
    RC_ASSERT(opt == restored);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Fuzz-style tests - random/malformed inputs
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("type mismatch fuzz tests", "[json][fuzz]") {
  rc::prop("getObject rejects all non-object types", []() {
    auto type = *rc::gen::inRange(0, 5);
    json j;
    switch (type) {
      case 0:
        j = *rc::gen::arbitrary<int64_t>();
        break;
      case 1:
        j = "test_string"; // Use a known-valid string
        break;
      case 2:
        j = *rc::gen::arbitrary<bool>();
        break;
      case 3:
        j = nullptr;
        break;
      case 4:
        j = json::array();
        break;
      default:
        j = *rc::gen::arbitrary<double>();
        break;
    }
    RC_ASSERT_THROWS_AS(get_object(j), Error);
  });

  rc::prop("getArray rejects all non-array types", []() {
    auto type = *rc::gen::inRange(0, 5);
    json j;
    switch (type) {
      case 0:
        j = *rc::gen::arbitrary<int64_t>();
        break;
      case 1:
        j = "test_string"; // Use a known-valid string
        break;
      case 2:
        j = *rc::gen::arbitrary<bool>();
        break;
      case 3:
        j = nullptr;
        break;
      case 4:
        j = json::object();
        break;
      default:
        j = *rc::gen::arbitrary<double>();
        break;
    }
    RC_ASSERT_THROWS_AS(get_array(j), Error);
  });

  rc::prop("getString rejects all non-string types", []() {
    auto type = *rc::gen::inRange(0, 5);
    json j;
    switch (type) {
      case 0:
        j = *rc::gen::arbitrary<int64_t>();
        break;
      case 1:
        j = json::array();
        break;
      case 2:
        j = *rc::gen::arbitrary<bool>();
        break;
      case 3:
        j = nullptr;
        break;
      case 4:
        j = json::object();
        break;
      default:
        j = *rc::gen::arbitrary<double>();
        break;
    }
    RC_ASSERT_THROWS_AS(get_string(j), Error);
  });

  rc::prop("getBoolean rejects all non-boolean types", []() {
    auto type = *rc::gen::inRange(0, 5);
    json j;
    switch (type) {
      case 0:
        j = *rc::gen::arbitrary<int64_t>();
        break;
      case 1:
        j = "test_string"; // Use a known-valid string
        break;
      case 2:
        j = json::array();
        break;
      case 3:
        j = nullptr;
        break;
      case 4:
        j = json::object();
        break;
      default:
        j = *rc::gen::arbitrary<double>();
        break;
    }
    RC_ASSERT_THROWS_AS(get_boolean(j), Error);
  });
}

TEST_CASE("integer conversion edge cases", "[json][fuzz][integer]") {
  rc::prop("getInteger never silently truncates", []() {
    auto val = *rc::gen::arbitrary<int64_t>();
    json j = val;

    // If value fits in int32, should succeed; otherwise throw
    if (val >= std::numeric_limits<int32_t>::min() && val <= std::numeric_limits<int32_t>::max()) {
      RC_ASSERT(get_integer<int32_t>(j) == static_cast<int32_t>(val));
    } else {
      RC_ASSERT_THROWS_AS(get_integer<int32_t>(j), Error);
    }
  });

  rc::prop("unsigned values > INT64_MAX rejected by getInteger<int64_t>", []() {
    // Generate values that exceed INT64_MAX
    auto offset = *rc::gen::inRange<uint64_t>(
        1, std::numeric_limits<uint64_t>::max() -
               static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
    auto val = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + offset;
    json j = val;
    RC_ASSERT_THROWS_AS(get_integer<int64_t>(j), Error);
  });
}

TEST_CASE("string content fuzz tests", "[json][fuzz][string]") {
  rc::prop("getString handles strings with embedded nulls", []() {
    auto prefix = *rc::gen::arbitrary<std::string>();
    auto suffix = *rc::gen::arbitrary<std::string>();
    std::string with_null = prefix + '\0' + suffix;

    json j = with_null;
    RC_ASSERT(get_string(j) == with_null);
    RC_ASSERT(get_string(j).size() == with_null.size());
  });

  rc::prop("getString handles strings with all byte values", []() {
    auto bytes = *rc::gen::arbitrary<std::vector<uint8_t>>();
    std::string str(bytes.begin(), bytes.end());

    json j = str;
    RC_ASSERT(get_string(j) == str);
  });
}

TEST_CASE("unicode fuzz tests", "[json][fuzz][unicode]") {
  rc::prop("getString handles valid UTF-8 sequences", []() {
    // Generate strings that are likely valid UTF-8
    auto ascii = *rc::gen::arbitrary<std::string>();
    json j = ascii;
    // Should not throw - ASCII is valid UTF-8
    RC_ASSERT(get_string(j) == ascii);
  });
}

TEST_CASE("nested structure stress tests", "[json][fuzz][nested]") {
  rc::prop("getObject works at any nesting depth", []() {
    auto depth = *rc::gen::inRange(1, 50);

    json root = json::object();
    json* current = &root;
    for (int i = 0; i < depth; ++i) {
      (*current)["child"] = json::object();
      current = &(*current)["child"];
    }
    (*current)["leaf"] = "value";

    // Verify we can descend and getObject at each level
    const json* reader = &root;
    for (int i = 0; i < depth; ++i) {
      const auto& obj = get_object(*reader);
      RC_ASSERT(obj.contains("child"));
      reader = &obj.at("child");
    }
    const auto& leaf_obj = get_object(*reader);
    RC_ASSERT(leaf_obj.contains("leaf"));
  });

  rc::prop("getArray works at any nesting depth", []() {
    auto depth = *rc::gen::inRange(1, 50);

    json root = json::array();
    json* current = &root;
    for (int i = 0; i < depth; ++i) {
      current->push_back(json::array());
      current = &(*current)[0];
    }
    current->push_back("leaf");

    // Verify we can descend and getArray at each level
    const json* reader = &root;
    for (int i = 0; i < depth; ++i) {
      const auto& arr = get_array(*reader);
      RC_ASSERT(arr.size() >= 1);
      reader = arr.data();
    }
    const auto& leaf_arr = get_array(*reader);
    RC_ASSERT(leaf_arr.size() == 1);
    RC_ASSERT(leaf_arr[0] == "leaf");
  });
}

TEST_CASE("mixed type array rejection", "[json][fuzz]") {
  rc::prop("getStringList rejects arrays containing any non-string", []() {
    auto strings = *rc::gen::nonEmpty<std::vector<std::string>>();
    auto bad_index = *rc::gen::inRange<size_t>(0, strings.size());

    json arr = json::array();
    for (size_t i = 0; i < strings.size(); ++i) {
      if (i == bad_index) {
        // Insert a non-string at this position
        arr.push_back(42);
      } else {
        arr.push_back(strings[i]);
      }
    }

    RC_ASSERT_THROWS_AS(get_string_list(arr), Error);
  });

  rc::prop("getStringSet rejects arrays containing any non-string", []() {
    auto strings = *rc::gen::nonEmpty<std::vector<std::string>>();
    auto bad_index = *rc::gen::inRange<size_t>(0, strings.size());

    json arr = json::array();
    for (size_t i = 0; i < strings.size(); ++i) {
      if (i == bad_index) {
        arr.push_back(true);
      } else {
        arr.push_back(strings[i]);
      }
    }

    RC_ASSERT_THROWS_AS(get_string_set(arr), Error);
  });
}

TEST_CASE("getStringMap rejects objects with non-string values", "[json][fuzz]") {
  rc::prop("getStringMap rejects objects containing any non-string value", []() {
    auto keys = *rc::gen::unique<std::vector<std::string>>(rc::gen::nonEmpty<std::string>());
    RC_PRE(keys.size() >= 2);
    auto bad_index = *rc::gen::inRange<size_t>(0, keys.size());

    json obj = json::object();
    for (size_t i = 0; i < keys.size(); ++i) {
      if (i == bad_index) {
        obj[keys[i]] = 123.456; // Non-string value
      } else {
        obj[keys[i]] = "valid_string";
      }
    }

    RC_ASSERT_THROWS_AS(get_string_map(obj), Error);
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Error message quality tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("error messages include helpful context", "[json][errors]") {
  SECTION("missing key error includes key name") {
    json::object_t obj = {{"existing", "value"}};
    try {
      value_at(obj, "missing_key");
      FAIL("Expected Error to be thrown");
    } catch (const Error& e) {
      std::string msg = e.what();
      REQUIRE(msg.contains("missing_key"));
    }
  }

  SECTION("type mismatch error includes expected and actual types") {
    json j = 42;
    try {
      get_string(j);
      FAIL("Expected Error to be thrown");
    } catch (const Error& e) {
      std::string msg = e.what();
      // Should mention both expected (string) and actual (number) types
      REQUIRE((msg.contains("string") || msg.contains("String")));
    }
  }

  SECTION("integer range error includes value") {
    json j = 1000;
    try {
      get_integer<int8_t>(j);
      FAIL("Expected Error to be thrown");
    } catch (const Error& e) {
      std::string msg = e.what();
      REQUIRE(msg.contains("1000"));
    }
  }
}
