#include "nix/flake/settings.h"

#include <vector>

#include "nix/expr/eval-settings.h"
#include "nix/expr/eval.h"
#include "nix/flake/flake-primops.h"

namespace nix::flake {

settings_t::settings_t() {}

void settings_t::configureEvalSettings(nix::EvalSettings& eval_settings) const {
  eval_settings.extraPrimOps.emplace_back(primops::get_flake(*this));
  eval_settings.extraPrimOps.emplace_back(primops::parse_flake_ref);
  eval_settings.extraPrimOps.emplace_back(primops::flake_ref_to_string);
}

} // namespace nix::flake
