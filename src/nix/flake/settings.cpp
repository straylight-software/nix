#include "nix/flake/settings.h"

#include <vector>

#include "nix/expr/eval-settings.h"
#include "nix/expr/eval.h"
#include "nix/flake/flake-primops.h"

namespace nix::flake {

settings_t::settings_t() {}

void settings_t::configureEvalSettings(nix::EvalSettings& evalSettings) const {
  evalSettings.extraPrimOps.emplace_back(primops::getFlake(*this));
  evalSettings.extraPrimOps.emplace_back(primops::parseFlakeRef);
  evalSettings.extraPrimOps.emplace_back(primops::flakeRefToString);
}

} // namespace nix::flake
