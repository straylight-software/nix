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

  void run(ref<Store> srcStore, Installables&& installables) override {
    auto& srcLogStore = require<LogStore>(*srcStore);

    auto dstStore = getDstStore();
    auto& dstLogStore = require<LogStore>(*dstStore);

    for (auto& drvPath : Installable::toDerivations(getEvalStore(), installables, true)) {
      if (auto log = srcLogStore.getBuildLog(drvPath))
        dstLogStore.addBuildLog(drvPath, *log);
      else
        throw Error("build log for '%s' is not available", srcStore->printStorePath(drvPath));
    }
  }
};

static auto rCmdCopyLog = registerCommand2<cmd_copy_log_t>({"store", "copy-log"});
