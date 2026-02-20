#pragma once
///@file

#include <nlohmann/json.hpp>

#include "nix/util/error.h"
#include "nix/util/json-non-null.h"
#include "nix/util/types.h"

namespace nix {

enum struct experimental_feature_t;

/**
 * Get the value of a json object at a key safely, failing with a nice
 * error if the key does not exist.
 *
 * use instead of nlohmann::json::at() to avoid ugly exceptions.
 */
const nlohmann::json& value_at(const nlohmann::json::object_t& map, std::string_view key);

/**
 * @return A pointer to the value assiocated with `key` if `value`
 * contains `key`, otherwise return  `nullptr` (not JSON `null`!).
 */
const nlohmann::json* optional_value_at(const nlohmann::json::object_t& value,
                                        std::string_view key);

/**
 * Prevents bugs; see `get` for the same trick.
 */
const nlohmann::json& value_at(nlohmann::json::object_t&& map, std::string_view key) = delete;
const nlohmann::json* optional_value_at(nlohmann::json::object_t&& value,
                                        std::string_view key) = delete;

/**
 * Downcast the json object, failing with a nice error if the conversion fails.
 * See https://json.nlohmann.me/features/types/
 */
const nlohmann::json* get_nullable(const nlohmann::json& value);
const nlohmann::json::object_t& get_object(const nlohmann::json& value);
const nlohmann::json::array_t& get_array(const nlohmann::json& value);
const nlohmann::json::string_t& get_string(const nlohmann::json& value);
const nlohmann::json::number_unsigned_t& get_unsigned(const nlohmann::json& value);

template <typename T>
auto get_integer(const nlohmann::json& value)
    -> std::enable_if_t<std::is_signed_v<T> && std::is_integral_v<T>, T> {
  if (auto ptr = value.get_ptr<const nlohmann::json::number_unsigned_t*>()) {
    if (*ptr <= std::make_unsigned_t<T>(std::numeric_limits<T>::max())) {
      return *ptr;
    }
  } else if (auto ptr = value.get_ptr<const nlohmann::json::number_integer_t*>()) {
    if (*ptr >= std::numeric_limits<T>::min() && *ptr <= std::numeric_limits<T>::max()) {
      return *ptr;
    }
  } else {
    auto type_name = value.is_number_float() ? "floating point number" : value.type_name();
    throw Error("Expected JSON value to be an integral number but it is of type '%s': %s",
                type_name, value.dump());
  }
  throw Error("Out of range: JSON value '%s' cannot be casted to %d-bit integer", value.dump(),
              8 * sizeof(T));
}

template <typename... args_t>
std::map<std::string, args_t...> get_map(const nlohmann::json::object_t& json_object, auto&& f) {
  std::map<std::string, args_t...> map;

  for (const auto& [key, value] : json_object)
    map.insert_or_assign(key, f(value));

  return map;
}

const nlohmann::json::boolean_t& get_boolean(const nlohmann::json& value);
strings_t get_string_list(const nlohmann::json& value);
string_map_t get_string_map(const nlohmann::json& value);
string_set_t get_string_set(const nlohmann::json& value);

} // namespace nix

namespace nlohmann {

/**
 * This "instance" is widely requested, see
 * https://github.com/nlohmann/json/issues/1749, but momentum has stalled
 * out. Writing there here in Nix as a stop-gap.
 *
 * We need to make sure the underlying type does not use `null` for this to
 * round trip. We do that with a static assert.
 */
template <typename T>
struct adl_serializer<std::optional<T>> {
  /**
   * @brief Convert a JSON type to an `optional<T>` treating
   *        `null` as `std::nullopt`.
   */
  static void from_json(const json& json, std::optional<T>& t) {
    static_assert(nix::json_avoids_null<T>::value,
                  "null is already in use for underlying type's JSON");
    t = json.is_null() ? std::nullopt : std::make_optional(json.template get<T>());
  }

  /**
   *  @brief Convert an optional type to a JSON type  treating `std::nullopt`
   *         as `null`.
   */
  static void to_json(json& json, const std::optional<T>& t) {
    static_assert(nix::json_avoids_null<T>::value,
                  "null is already in use for underlying type's JSON");
    if (t)
      json = *t;
    else
      json = nullptr;
  }
};

template <typename T>
static inline std::optional<T> ptr_to_owned(const json* ptr) {
  if (ptr)
    return std::optional{*ptr};
  else
    return std::nullopt;
}

} // namespace nlohmann
