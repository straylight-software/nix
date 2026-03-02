#include <atomic>

#include "nix/cmd/command.h"
#include "nix/main/shared.h"
#include "nix/store/log-store.h"
#include "nix/store/store-api.h"
#include "nix/store/store-cast.h"
#include "nix/util/sync.h"
#include "nix/util/thread-pool.h"

struct cmd_copy_log_t : virtual nix::CopyCommand, virtual nix::InstallablesCommand {
  std::string description() override { return "copy build logs between Nix stores"; }

  std::string doc() override {
    return
#include "store-copy-log.md"
        ;
  }

  void run(nix::ref<nix::store_t> src_store, nix::Installables&& installables) override {
    auto& src_log_store = nix::require<nix::LogStore>(*src_store);

    auto dst_store = getDstStore();
    auto& dst_log_store = nix::require<nix::LogStore>(*dst_store);

    for (auto& drv_path : nix::Installable::toDerivations(getEvalStore(), installables, true)) {
      if (auto log = src_log_store.getBuildLog(drv_path)) {
        dst_log_store.addBuildLog(drv_path, *log);
      } else {
        throw nix::Error("build log for '%s' is not available",
                         src_store->printStorePath(drv_path));
      }
    }
  }
};

static auto r_cmd_copy_log = nix::registerCommand2<cmd_copy_log_t>({"store", "copy-log"});
