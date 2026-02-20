#pragma once
///@file

#include "nix/util/configuration.h"

namespace nix {

struct global_config_t : public abstract_config_t {
  using config_registrations_t = std::vector<config_t*>;

  static auto config_registrations() -> config_registrations_t&;

  auto set(const std::string& name, const std::string& value) -> bool override;

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
