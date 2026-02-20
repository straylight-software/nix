#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/store-api.h"
#include "nix/util/config-global.h"

using namespace nix;

struct cmd_config_t : NixMultiCommand {
  cmd_config_t() : NixMultiCommand("config", RegisterCommand::getCommandsFor({"config"})) {}

  std::string description() override { return "manipulate the Nix configuration"; }

  category_t category() override { return catUtility; }
};

struct cmd_config_show_t : command_t, MixJSON {
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

  category_t category() override { return catUtility; }

  void run() override {
    if (name) {
      if (json) {
        throw UsageError("'--json' is not supported when specifying a setting name");
      }

      std::map<std::string, config_t::setting_info_t> settings;
      global_config.get_settings(settings);
      auto setting = settings.find(*name);

      if (setting == settings.end()) {
        throw Error("could not find setting '%1%'", *name);
      } else {
        const auto& value = setting->second.value_;
        logger->cout("%s", value);
      }

      return;
    }

    if (json) {
      // FIXME: use appropriate JSON types (bool, ints, etc).
      printJSON(global_config.to_json());
    } else {
      logger->cout("%s", global_config.to_key_value());
    }
  }
};

static auto r_cmd_config = registerCommand<cmd_config_t>("config");
static auto r_show_config = registerCommand2<cmd_config_show_t>({"config", "show"});
