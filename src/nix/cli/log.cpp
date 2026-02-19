#include "nix/cmd/command.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/globals.h"
#include "nix/store/log-store.h"
#include "nix/store/store-open.h"

using namespace nix;

struct cmd_log_t : InstallableCommand {
  std::string description() override {
    return "show the build log of the specified packages or paths, if available";
  }

  std::string doc() override {
    return
#include "log.md"
        ;
  }

  category_t category() override { return catSecondary; }

  void run(ref<Store> store, ref<Installable> installable) override {
    settings.readOnlyMode = true;

    auto subs = getDefaultSubstituters();

    subs.push_front(store);

    auto b = installable->toDerivedPath();

    // For compat with CLI today, TODO revisit
    auto oneUp = std::visit(
        overloaded{
            [&](const DerivedPath::opaque_t& bo) { return make_ref<const SingleDerivedPath>(bo); },
            [&](const DerivedPath::Built& bfd) { return bfd.drvPath; },
        },
        b.path.raw());
    auto path = resolveDerivedPath(*store, *oneUp);

    RunPager pager;
    for (auto& sub : subs) {
      auto* logSubP = dynamic_cast<LogStore*>(&*sub);
      if (!logSubP) {
        printInfo("Skipped '%s' which does not support retrieving build logs",
                  sub->config.getHumanReadableURI());
        continue;
      }
      auto& logSub = *logSubP;

      auto log = logSub.getBuildLog(path);
      if (!log)
        continue;
      logger->stop();
      printInfo("got build log for '%s' from '%s'", installable->what(),
                logSub.config.getHumanReadableURI());
      writeFull(getStandardOutput(), *log);
      return;
    }

    throw Error("build log of '%s' is not available", installable->what());
  }
};

static auto rCmdLog = registerCommand<cmd_log_t>("log");
