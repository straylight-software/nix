#include "nix/util/config-global.h"

#include <nlohmann/json.hpp>

namespace nix {

global_config_t::config_registrations_t& global_config_t::config_registrations() {
  static global_config_t::config_registrations_t config_registrations;
  return config_registrations;
}

bool global_config_t::set(const std::string& name, const std::string& value) {
  for (auto& config : config_registrations()) {
    if (config->set(name, value)) {
      return true;
    }
  }

  unknownSettings_.emplace(name, value);

  return false;
}

void global_config_t::get_settings(std::map<std::string, setting_info_t>& res,
                                   bool overridden_only) const {
  for (auto& config : config_registrations()) {
    config->get_settings(res, overridden_only);
  }
}

void global_config_t::reset_overridden() {
  for (auto& config : config_registrations()) {
    config->reset_overridden();
  }
}

nlohmann::json global_config_t::to_json() {
  auto res = nlohmann::json::object();
  for (const auto& config : config_registrations()) {
    res.update(config->to_json());
  }
  return res;
}

std::string global_config_t::to_key_value() {
  std::string res;
  std::map<std::string, config_t::setting_info_t> settings;
  global_config.get_settings(settings);
  for (const auto& s : settings) {
    res += fmt("%s = %s\n", s.first, s.second.value);
  }
  return res;
}

void global_config_t::convert_to_args(args_t& args, const std::string& category) {
  for (auto& config : config_registrations()) {
    config->convert_to_args(args, category);
  }
}

global_config_t global_config;

global_config_t::Register::Register(config_t* config) {
  config_registrations().emplace_back(config);
}

experimental_feature_settings_t experimental_feature_settings;

static global_config_t::Register r_settings(&experimental_feature_settings);

} // namespace nix
