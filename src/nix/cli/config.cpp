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
    expectArgs({
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

      std::map<std::string, Config::setting_info_t> settings;
      globalConfig.getSettings(settings);
      auto setting = settings.find(*name);

      if (setting == settings.end()) {
        throw Error("could not find setting '%1%'", *name);
      } else {
        const auto& value = setting->second.value;
        logger->cout("%s", value);
      }

      return;
    }

    if (json) {
      // FIXME: use appropriate JSON types (bool, ints, etc).
      printJSON(globalConfig.toJSON());
    } else {
      logger->cout("%s", globalConfig.toKeyValue());
    }
  }
};

static auto rCmdConfig = registerCommand<cmd_config_t>("config");
static auto rShowConfig = registerCommand2<cmd_config_show_t>({"config", "show"});
