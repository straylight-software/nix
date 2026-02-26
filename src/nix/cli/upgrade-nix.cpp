#include "nix/cmd/command.h"
#include "nix/expr/attr-path.h"
#include "nix/expr/eval-settings.h"
#include "nix/expr/eval.h"
#include "nix/main/common-args.h"
#include "nix/store/filetransfer.h"
#include "nix/store/globals.h"
#include "nix/store/names.h"
#include "nix/store/store-api.h"
#include "nix/util/executable-path.h"
#include "nix/util/processes.h"
#include "self-exe.h"

struct cmd_upgrade_nix_t : nix::MixDryRun, nix::StoreCommand {
  /**
   * This command is stable before the others
   */
  std::optional<nix::experimental_feature_t> experimental_feature() override {
    return std::nullopt;
  }

  std::string description() override { return "deprecated in favor of determinate-nixd upgrade"; }

  std::string doc() override {
    return
#include "upgrade-nix.md"
        ;
  }

  nix::category_t category() override { return nix::catNixInstallation; }

  void run(nix::ref<nix::store_t> store) override {
    throw nix::Error("The upgrade-nix command isn't available in Determinate Nix; use %s instead",
                     "sudo determinate-nixd upgrade");
  }
};

static auto r_cmd_upgrade_nix = nix::registerCommand<cmd_upgrade_nix_t>("upgrade-nix");
