#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/main/common-args.h"

using namespace nix;

struct cmd_realisation_t : NixMultiCommand {
  cmd_realisation_t()
      : NixMultiCommand("realisation", RegisterCommand::getCommandsFor({"realisation"})) {}

  std::string description() override { return "manipulate a Nix realisation"; }

  category_t category() override { return catUtility; }
};

static auto r_cmd_realisation = registerCommand<cmd_realisation_t>("realisation");

struct cmd_realisation_info_t : BuiltPathsCommand, MixJSON {
  std::string description() override {
    return "query information about one or several realisations";
  }

  std::string doc() override {
    return
#include "realisation/info.md"
        ;
  }

  category_t category() override { return catSecondary; }

  void run(ref<store_t> store, BuiltPaths&& paths, BuiltPaths&& root_paths) override {
    experimental_feature_settings.require(xp_t::ca_derivations);
    RealisedPath::Set realisations;

    for (auto& builtPath : paths) {
      auto theseRealisations = builtPath.toRealisedPaths(*store);
      realisations.insert(theseRealisations.begin(), theseRealisations.end());
    }

    if (json) {
      nlohmann::json res = nlohmann::json::array();
      for (auto& path : realisations) {
        nlohmann::json currentPath;
        if (auto realisation = std::get_if<realisation_t>(&path.raw))
          currentPath = *realisation;
        else
          currentPath["opaquePath"] = store->printStorePath(path.path());

        res.push_back(currentPath);
      }
      printJSON(res);
    } else {
      for (auto& path : realisations) {
        if (auto realisation = std::get_if<realisation_t>(&path.raw)) {
          logger->cout("%s %s", realisation->id.to_string(),
                       store->printStorePath(realisation->out_path));
        } else
          logger->cout("%s", store->printStorePath(path.path()));
      }
    }
  }
};

static auto r_cmd_realisation_info = registerCommand2<cmd_realisation_info_t>({"realisation", "info"});
