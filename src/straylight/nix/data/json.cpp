// straylight::nix::data::json - Implementation

#include "straylight/nix/data/json.h"

#include <nlohmann/json.hpp>

namespace straylight::nix::data::json {

// =============================================================================
// boxed_value_t implementation
// =============================================================================

boxed_value_t::boxed_value_t() : ptr(std::make_unique<value_t>()) {}

boxed_value_t::boxed_value_t(value_t v) : ptr(std::make_unique<value_t>(std::move(v))) {}

boxed_value_t::boxed_value_t(const boxed_value_t& other)
    : ptr(other.ptr ? std::make_unique<value_t>(*other.ptr) : nullptr) {}

boxed_value_t& boxed_value_t::operator=(const boxed_value_t& other) {
  if (this != &other) {
    ptr = other.ptr ? std::make_unique<value_t>(*other.ptr) : nullptr;
  }
  return *this;
}

boxed_value_t::~boxed_value_t() = default;

// =============================================================================
// array_t implementation
// =============================================================================

void array_t::push_back(value_t v) {
  elements.emplace_back(std::move(v));
}

// =============================================================================
// object_t implementation
// =============================================================================

const value_t* object_t::get(std::string_view key) const {
  for (const auto& [k, v] : members) {
    if (k == key) {
      return v.ptr.get();
    }
  }
  return nullptr;
}

value_t* object_t::get(std::string_view key) {
  for (auto& [k, v] : members) {
    if (k == key) {
      return v.ptr.get();
    }
  }
  return nullptr;
}

bool object_t::contains(std::string_view key) const {
  return get(key) != nullptr;
}

void object_t::insert(std::string key, value_t value) {
  members.emplace_back(std::move(key), boxed_value_t(std::move(value)));
}

void object_t::insert_or_assign(std::string key, value_t value) {
  for (auto& [k, v] : members) {
    if (k == key) {
      *v.ptr = std::move(value);
      return;
    }
  }
  members.emplace_back(std::move(key), boxed_value_t(std::move(value)));
}

// =============================================================================
// value_t implementation
// =============================================================================

double value_t::as_double() const {
  if (auto* d = std::get_if<double>(&data)) {
    return *d;
  }
  if (auto* i = std::get_if<std::int64_t>(&data)) {
    return static_cast<double>(*i);
  }
  if (auto* u = std::get_if<std::uint64_t>(&data)) {
    return static_cast<double>(*u);
  }
  return std::get<double>(data);
}

std::optional<bool> value_t::get_bool() const {
  if (auto* p = std::get_if<bool>(&data)) {
    return *p;
  }
  return std::nullopt;
}

std::optional<std::int64_t> value_t::get_int() const {
  if (auto* p = std::get_if<std::int64_t>(&data)) {
    return *p;
  }
  return std::nullopt;
}

std::optional<std::uint64_t> value_t::get_uint() const {
  if (auto* p = std::get_if<std::uint64_t>(&data)) {
    return *p;
  }
  return std::nullopt;
}

std::optional<double> value_t::get_double() const {
  if (auto* p = std::get_if<double>(&data)) {
    return *p;
  }
  if (auto* i = std::get_if<std::int64_t>(&data)) {
    return static_cast<double>(*i);
  }
  if (auto* u = std::get_if<std::uint64_t>(&data)) {
    return static_cast<double>(*u);
  }
  return std::nullopt;
}

std::optional<std::string_view> value_t::get_string() const {
  if (auto* p = std::get_if<std::string>(&data)) {
    return *p;
  }
  return std::nullopt;
}

const array_t* value_t::get_array() const {
  return std::get_if<array_t>(&data);
}

const object_t* value_t::get_object() const {
  return std::get_if<object_t>(&data);
}

const value_t* value_t::get(std::string_view key) const {
  if (auto* obj = get_object()) {
    return obj->get(key);
  }
  return nullptr;
}

const value_t* value_t::get(std::size_t index) const {
  if (auto* arr = get_array()) {
    if (index < arr->size()) {
      return &(*arr)[index];
    }
  }
  return nullptr;
}

// =============================================================================
// Conversion from nlohmann::json
// =============================================================================

static value_t from_nlohmann(const nlohmann::json& j) {
  switch (j.type()) {
    case nlohmann::json::value_t::null:
      return null;
    case nlohmann::json::value_t::boolean:
      return j.get<bool>();
    case nlohmann::json::value_t::number_integer:
      return j.get<std::int64_t>();
    case nlohmann::json::value_t::number_unsigned:
      return j.get<std::uint64_t>();
    case nlohmann::json::value_t::number_float:
      return j.get<double>();
    case nlohmann::json::value_t::string:
      return j.get<std::string>();
    case nlohmann::json::value_t::array: {
      array_t arr;
      arr.elements.reserve(j.size());
      for (const auto& elem : j) {
        arr.elements.emplace_back(from_nlohmann(elem));
      }
      return arr;
    }
    case nlohmann::json::value_t::object: {
      object_t obj;
      obj.members.reserve(j.size());
      for (const auto& [key, val] : j.items()) {
        obj.members.emplace_back(key, boxed_value_t(from_nlohmann(val)));
      }
      return obj;
    }
    default:
      return null;
  }
}

// =============================================================================
// Conversion to nlohmann::json
// =============================================================================

static nlohmann::json to_nlohmann(const value_t& v) {
  return std::visit(
      [](auto&& arg) -> nlohmann::json {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, null_t>) {
          return nullptr;
        } else if constexpr (std::is_same_v<T, bool>) {
          return arg;
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          return arg;
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
          return arg;
        } else if constexpr (std::is_same_v<T, double>) {
          return arg;
        } else if constexpr (std::is_same_v<T, std::string>) {
          return arg;
        } else if constexpr (std::is_same_v<T, array_t>) {
          auto j = nlohmann::json::array();
          for (const auto& elem : arg.elements) {
            j.push_back(to_nlohmann(*elem));
          }
          return j;
        } else if constexpr (std::is_same_v<T, object_t>) {
          auto j = nlohmann::json::object();
          for (const auto& [key, val] : arg.members) {
            j[key] = to_nlohmann(*val);
          }
          return j;
        }
      },
      v.data);
}

// =============================================================================
// Parsing and serialization
// =============================================================================

result_t<value_t> parse(std::string_view input) {
  try {
    auto j = nlohmann::json::parse(input);
    return {from_nlohmann(j)};
  } catch (const nlohmann::json::parse_error& e) {
    return {parse_error_t{.message = e.what(), .offset = e.byte}};
  }
}

result_t<value_t> parse(std::span<const std::byte> input) {
  return parse(std::string_view(reinterpret_cast<const char*>(input.data()), input.size()));
}

std::string to_string(const value_t& value) {
  return to_nlohmann(value).dump();
}

std::string to_string_pretty(const value_t& value, int indent) {
  return to_nlohmann(value).dump(indent);
}

} // namespace straylight::nix::data::json
