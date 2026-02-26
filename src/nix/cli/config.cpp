#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/store-api.h"
#include "nix/util/config-global.h"

struct cmd_config_t : nix::NixMultiCommand {
  cmd_config_t() : NixMultiCommand("config", nix::RegisterCommand::getCommandsFor({"config"})) {}

  std::string description() override { return "manipulate the Nix configuration"; }

  nix::category_t category() override { return nix::catUtility; }
};

struct cmd_config_show_t : nix::command_t, nix::MixJSON {
  std::optional<std::string> name;

  cmd_config_show_t() {
    expect_args({
        .label = {"name"},
        .optional = true,
        .handler = {&name},
    });
  }

  std::string description() override {
    return "show the Nix configuration or the value of a specific setting";
  }

  nix::category_t category() override { return nix::catUtility; }

  void run() override {
    if (name) {
      if (json) {
        throw nix::UsageError("'--json' is not supported when specifying a setting name");
      }

      std::map<std::string, nix::config_t::setting_info_t> settings;
      nix::global_config.get_settings(settings);
      auto setting = settings.find(*name);

      if (setting == settings.end()) {
        throw nix::Error("could not find setting '%1%'", *name);
      } else {
        const auto& value = setting->second.value_;
        nix::logger->cout("%s", value);
      }

      return;
    }

    if (json) {
      // FIXME: use appropriate JSON types (bool, ints, etc).
      nix::printJSON(nix::global_config.to_json());
    } else {
      nix::logger->cout("%s", nix::global_config.to_key_value());
    }
  }
};

static auto r_cmd_config = nix::registerCommand<cmd_config_t>("config");
static auto r_show_config = nix::registerCommand2<cmd_config_show_t>({"config", "show"});
