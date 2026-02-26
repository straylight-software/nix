#include "nix/cmd/command.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/globals.h"
#include "nix/store/log-store.h"
#include "nix/store/store-open.h"

using nix::fmt;
using nix::logger;

struct cmd_log_t : nix::InstallableCommand {
  std::string description() override {
    return "show the build log of the specified packages or paths, if available";
  }

  std::string doc() override {
    return
#include "log.md"
        ;
  }

  category_t category() override { return nix::catSecondary; }

  void run(nix::ref<nix::store_t> store, nix::ref<nix::Installable> installable) override {
    nix::settings.readOnlyMode = true;

    auto subs = nix::get_default_substituters();

    subs.push_front(store);

    auto b = installable->toDerivedPath();

    // For compat with CLI today, TODO revisit
    auto one_up =
        std::visit(nix::overloaded{
                       [&](const nix::derived_path_t::opaque_t& bo) {
                         return nix::make_ref<const nix::SingleDerivedPath>(bo);
                       },
                       [&](const nix::derived_path_t::Built& bfd) { return bfd.drv_path; },
                   },
                   b.path.raw());
    auto path = nix::resolve_derived_path(*store, *one_up);

    nix::RunPager pager;
    for (auto& sub : subs) {
      auto* logSubP = dynamic_cast<nix::LogStore*>(&*sub);
      if (!logSubP) {
        printInfo("Skipped '%s' which does not support retrieving build logs",
                  sub->config.getHumanReadableURI());
        continue;
      }
      auto& logSub = *logSubP;

      auto log = logSub.getBuildLog(path);
      if (!log)
        continue;
      nix::logger->stop();
      printInfo("got build log for '%s' from '%s'", installable->what(),
                logSub.config.getHumanReadableURI());
      nix::write_full(nix::get_standard_output(), *log);
      return;
    }

    throw nix::Error("build log of '%s' is not available", installable->what());
  }
};

static auto r_cmd_log = nix::registerCommand<cmd_log_t>("log");
