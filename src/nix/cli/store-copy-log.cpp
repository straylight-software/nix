#include <atomic>

#include "nix/cmd/command.h"
#include "nix/main/shared.h"
#include "nix/store/log-store.h"
#include "nix/store/store-api.h"
#include "nix/store/store-cast.h"
#include "nix/util/sync.h"
#include "nix/util/thread-pool.h"

using namespace nix;

struct cmd_copy_log_t : virtual CopyCommand, virtual InstallablesCommand {
  std::string description() override { return "copy build logs between Nix stores"; }

  std::string doc() override {
    return
#include "store-copy-log.md"
        ;
  }

  void run(ref<store_t> src_store, Installables&& installables) override {
    auto& src_log_store = require<LogStore>(*src_store);

    auto dst_store = getDstStore();
    auto& dst_log_store = require<LogStore>(*dst_store);

    for (auto& drv_path : Installable::toDerivations(getEvalStore(), installables, true)) {
      if (auto log = src_log_store.getBuildLog(drv_path))
        dst_log_store.addBuildLog(drv_path, *log);
      else
        throw Error("build log for '%s' is not available", src_store->printStorePath(drv_path));
    }
  }
};

static auto r_cmd_copy_log = registerCommand2<cmd_copy_log_t>({"store", "copy-log"});
