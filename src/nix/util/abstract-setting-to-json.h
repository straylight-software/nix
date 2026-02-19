#pragma once
///@file

#include <nlohmann/json.hpp>

#include "nix/util/configuration.h"
#include "nix/util/json-utils.h"

namespace nix {
template <typename T>
std::map<std::string, nlohmann::json> base_setting_t<T>::toJSONObject() const {
  auto obj = abstract_setting_t::toJSONObject();
  obj.emplace("value", value);
  obj.emplace("defaultValue", defaultValue);
  obj.emplace("documentDefault", documentDefault);
  return obj;
}
} // namespace nix
