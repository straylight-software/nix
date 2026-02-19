#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/main/shared.h"
#include "nix/store/store-api.h"
#include "nix/util/finally.h"

using namespace nix;

struct cmd_info_store_t : StoreCommand, MixJSON {
  std::string description() override { return "test whether a store can be accessed"; }

  std::string doc() override {
    return
#include "store-info.md"
        ;
  }

  void run(ref<Store> store) override {
    if (!json) {
      notice("Store URL: %s", store->config.getReference().render(/*withParams=*/true));
      store->connect();
      if (auto version = store->getVersion())
        notice("Version: %s", *version);
      if (auto trusted = store->isTrustedClient())
        notice("Trusted: %s", *trusted);
    } else {
      nlohmann::json res;
      finally_t printRes([&]() { printJSON(res); });

      res["url"] = store->config.getReference().render(/*withParams=*/true);
      store->connect();
      if (auto version = store->getVersion())
        res["version"] = *version;
      if (auto trusted = store->isTrustedClient())
        res["trusted"] = *trusted;
    }
  }
};

static auto rCmdInfoStore = registerCommand2<cmd_info_store_t>({"store", "info"});
