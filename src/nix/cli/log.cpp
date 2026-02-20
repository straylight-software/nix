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

  void run(ref<store_t> store, ref<Installable> installable) override {
    settings.readOnlyMode = true;

    auto subs = get_default_substituters();

    subs.push_front(store);

    auto b = installable->toDerivedPath();

    // For compat with CLI today, TODO revisit
    auto one_up = std::visit(
        overloaded{
            [&](const derived_path_t::opaque_t& bo) { return make_ref<const SingleDerivedPath>(bo); },
            [&](const derived_path_t::Built& bfd) { return bfd.drv_path; },
        },
        b.path.raw());
    auto path = resolve_derived_path(*store, *one_up);

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
      write_full(get_standard_output(), *log);
      return;
    }

    throw Error("build log of '%s' is not available", installable->what());
  }
};

static auto r_cmd_log = registerCommand<cmd_log_t>("log");
