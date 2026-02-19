#include "nix/util/config-global.h"

#include <nlohmann/json.hpp>

namespace nix {

global_config_t::config_registrations_t& global_config_t::configRegistrations() {
  static global_config_t::config_registrations_t configRegistrations;
  return configRegistrations;
}

bool global_config_t::set(const std::string& name, const std::string& value) {
  for (auto& config : configRegistrations())
    if (config->set(name, value))
      return true;

  unknownSettings.emplace(name, value);

  return false;
}

void global_config_t::getSettings(std::map<std::string, setting_info_t>& res, bool overriddenOnly) const {
  for (auto& config : configRegistrations())
    config->getSettings(res, overriddenOnly);
}

void global_config_t::resetOverridden() {
  for (auto& config : configRegistrations())
    config->resetOverridden();
}

nlohmann::json global_config_t::toJSON() {
  auto res = nlohmann::json::object();
  for (const auto& config : configRegistrations())
    res.update(config->toJSON());
  return res;
}

std::string global_config_t::toKeyValue() {
  std::string res;
  std::map<std::string, Config::setting_info_t> settings;
  globalConfig.getSettings(settings);
  for (const auto& s : settings)
    res += fmt("%s = %s\n", s.first, s.second.value);
  return res;
}

void global_config_t::convertToArgs(Args& args, const std::string& category) {
  for (auto& config : configRegistrations())
    config->convertToArgs(args, category);
}

global_config_t globalConfig;

global_config_t::Register::Register(Config* config) {
  configRegistrations().emplace_back(config);
}

experimental_feature_settings_t experimentalFeatureSettings;

static global_config_t::Register rSettings(&experimentalFeatureSettings);

} // namespace nix
