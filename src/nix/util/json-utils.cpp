#include "nix/util/json-utils.h"

#include "nix/util/error.h"
#include "nix/util/types.h"
#include "nix/util/util.h"

namespace nix {

const nlohmann::json& value_at(const nlohmann::json::object_t& map, std::string_view key) {
  if (auto* p = optional_value_at(map, key)) {
    return *p;
  } else {
    throw Error("Expected JSON object to contain key '%s' but it doesn't: %s", key,
                nlohmann::json(map).dump());
  }
}

const nlohmann::json* optional_value_at(const nlohmann::json::object_t& map, std::string_view key) {
  return get(map, key);
}

const nlohmann::json* get_nullable(const nlohmann::json& value) {
  return value.is_null() ? nullptr : &value;
}

/**
 * Ensure the type of a JSON object is what you expect, failing with a
 * ensure type if it isn't.
 *
 * use before type conversions and element access to avoid ugly
 * exceptions, but only part of this module to define the other `get*`
 * functions. It is too cumbersome and easy to forget to expect regular
 * JSON code to use it directly.
 */
static const nlohmann::json& ensure_type(const nlohmann::json& value,
                                         nlohmann::json::value_type expected_type) {
  if (value.type() != expected_type) {
    throw Error("Expected JSON value to be of type '%s' but it is of type '%s': %s",
                nlohmann::json(expected_type).type_name(), value.type_name(), value.dump());
  }

  return value;
}

const nlohmann::json::object_t& get_object(const nlohmann::json& value) {
  return ensure_type(value, nlohmann::json::value_t::object)
      .get_ref<const nlohmann::json::object_t&>();
}

const nlohmann::json::array_t& get_array(const nlohmann::json& value) {
  return ensure_type(value, nlohmann::json::value_t::array)
      .get_ref<const nlohmann::json::array_t&>();
}

const nlohmann::json::string_t& get_string(const nlohmann::json& value) {
  return ensure_type(value, nlohmann::json::value_t::string)
      .get_ref<const nlohmann::json::string_t&>();
}

const nlohmann::json::number_unsigned_t& get_unsigned(const nlohmann::json& value) {
  if (auto ptr = value.get<const nlohmann::json::number_unsigned_t*>()) {
    return *ptr;
  }
  const char* type_name = value.type_name();
  if (type_name == nlohmann::json(0).type_name()) {
    type_name = value.is_number_float() ? "floating point number" : "signed integral number";
  }
  throw Error("Expected JSON value to be an unsigned integral number but it is of type '%s': %s",
              type_name, value.dump());
}

const nlohmann::json::boolean_t& get_boolean(const nlohmann::json& value) {
  return ensure_type(value, nlohmann::json::value_t::boolean)
      .get_ref<const nlohmann::json::boolean_t&>();
}

strings_t get_string_list(const nlohmann::json& value) {
  auto& json_array = get_array(value);

  strings_t string_list;

  for (const auto& elem : json_array) {
    string_list.push_back(get_string(elem));
  }

  return string_list;
}

string_map_t get_string_map(const nlohmann::json& value) {
  return get_map<std::string, std::less<>>(get_object(value), get_string);
}

string_set_t get_string_set(const nlohmann::json& value) {
  auto& json_array = get_array(value);

  string_set_t string_set;

  for (const auto& elem : json_array) {
    string_set.insert(get_string(elem));
  }

  return string_set;
}
} // namespace nix
