#pragma once
///@file

#include "nix/util/configuration.h"

namespace nix {

struct global_config_t : public abstract_config_t {
  typedef std::vector<Config*> config_registrations_t;

  static config_registrations_t& configRegistrations();

  bool set(const std::string& name, const std::string& value) override;

  void getSettings(std::map<std::string, setting_info_t>& res,
                   bool overriddenOnly = false) const override;

  void resetOverridden() override;

  nlohmann::json toJSON() override;

  std::string toKeyValue() override;

  void convertToArgs(Args& args, const std::string& category) override;

  struct Register {
    Register(Config* config);
  };
};

extern global_config_t globalConfig;

} // namespace nix
