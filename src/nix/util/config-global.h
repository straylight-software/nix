#pragma once
///@file

#include "nix/util/configuration.h"

namespace nix {

struct global_config_t : public abstract_config_t {
  typedef std::vector<config_t*> config_registrations_t;

  static config_registrations_t& config_registrations();

  bool set(const std::string& name, const std::string& value) override;

  void get_settings(std::map<std::string, setting_info_t>& res,
                   bool overridden_only = false) const override;

  void reset_overridden() override;

  nlohmann::json to_json() override;

  std::string to_key_value() override;

  void convert_to_args(Args& args, const std::string& category) override;

  struct Register {
    Register(config_t* config);
  };
};

extern global_config_t global_config;

} // namespace nix
