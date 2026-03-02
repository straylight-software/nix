#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/main/shared.h"
#include "nix/store/store-api.h"
#include "nix/util/finally.h"

// Required for notice macro
using nix::fmt;
using nix::logger;

struct cmd_info_store_t : nix::StoreCommand, nix::MixJSON {
  std::string description() override { return "test whether a store can be accessed"; }

  std::string doc() override {
    return
#include "store-info.md"
        ;
  }

  void run(nix::ref<nix::store_t> store) override {
    if (!json) {
      notice("store_t URL: %s", store->config.getReference().render(/*withParams=*/true));
      store->connect();
      if (auto version = store->getVersion()) {
        notice("Version: %s", *version);
      }
      if (auto trusted = store->isTrustedClient()) {
        notice("Trusted: %s", *trusted);
      }
    } else {
      nlohmann::json res;
      auto print_res = finally_t([&]() { printJSON(res); });

      res["url"] = store->config.getReference().render(/*withParams=*/true);
      store->connect();
      if (auto version = store->getVersion()) {
        res["version"] = *version;
      }
      if (auto trusted = store->isTrustedClient()) {
        res["trusted"] = *trusted;
      }
    }
  }
};

static auto r_cmd_info_store = nix::registerCommand2<cmd_info_store_t>({"store", "info"});
