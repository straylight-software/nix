#include "nix/store/make-content-addressed.h"

#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/main/common-args.h"
#include "nix/store/store-open.h"

using namespace nix;

using nlohmann::json;

struct CmdMakeContentAddressed : virtual CopyCommand, virtual StorePathsCommand, MixJSON {
  CmdMakeContentAddressed() { realiseMode = Realise::Outputs; }

  std::string description() override {
    return "rewrite a path or closure to content-addressed form";
  }

  std::string doc() override {
    return
#include "make-content-addressed.md"
        ;
  }

  void run(ref<Store> srcStore, StorePaths&& storePaths) override {
    auto dstStore = dstUri.empty() ? openStore() : openStore(dstUri);

    auto remappings = makeContentAddressed(*srcStore, *dstStore,
                                           StorePathSet(storePaths.begin(), storePaths.end()));

    if (json) {
      auto jsonRewrites = json::object();
      for (auto& path : storePaths) {
        auto i = remappings.find(path);
        assert(i != remappings.end());
        jsonRewrites[srcStore->printStorePath(path)] = srcStore->printStorePath(i->second);
      }
      auto json = json::object();
      json["rewrites"] = jsonRewrites;
      printJSON(json);
    } else {
      for (auto& path : storePaths) {
        auto i = remappings.find(path);
        assert(i != remappings.end());
        notice("rewrote '%s' to '%s'", srcStore->printStorePath(path),
               srcStore->printStorePath(i->second));
      }
    }
  }
};

static auto rCmdMakeContentAddressed =
    registerCommand2<CmdMakeContentAddressed>({"store", "make-content-addressed"});
