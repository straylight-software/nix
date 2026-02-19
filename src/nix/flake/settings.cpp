#include "nix/flake/settings.h"

#include <vector>

#include "nix/expr/eval-settings.h"
#include "nix/expr/eval.h"
#include "nix/flake/flake-primops.h"

namespace nix::flake {

Settings::Settings() {}

void Settings::configureEvalSettings(nix::EvalSettings& evalSettings) const {
  evalSettings.extraPrimOps.emplace_back(primops::getFlake(*this));
  evalSettings.extraPrimOps.emplace_back(primops::parseFlakeRef);
  evalSettings.extraPrimOps.emplace_back(primops::flakeRefToString);
}

} // namespace nix::flake
