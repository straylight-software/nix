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

using namespace nix;

struct CmdUpgradeNix : MixDryRun, StoreCommand {
  /**
   * This command is stable before the others
   */
  std::optional<ExperimentalFeature> experimentalFeature() override { return std::nullopt; }

  std::string description() override { return "deprecated in favor of determinate-nixd upgrade"; }

  std::string doc() override {
    return
#include "upgrade-nix.md"
        ;
  }

  Category category() override { return catNixInstallation; }

  void run(ref<Store> store) override {
    throw Error("The upgrade-nix command isn't available in Determinate Nix; use %s instead",
                "sudo determinate-nixd upgrade");
  }
};

static auto rCmdUpgradeNix = registerCommand<CmdUpgradeNix>("upgrade-nix");
