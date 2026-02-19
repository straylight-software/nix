#pragma once
///@file

#include <nlohmann/json.hpp>

#include "nix/util/configuration.h"
#include "nix/util/json-utils.h"

namespace nix {
template <typename T>
std::map<std::string, nlohmann::json> base_setting_t<T>::to_json_object() const {
  auto obj = abstract_setting_t::to_json_object();
  obj.emplace("value", value);
  obj.emplace("defaultValue", default_value);
  obj.emplace("documentDefault", document_default);
  return obj;
}
} // namespace nix
