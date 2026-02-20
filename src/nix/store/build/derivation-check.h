#pragma once
///@file

#include "nix/store/derivation-options.h"
#include "nix/store/derivations.h"
#include "nix/store/path-info.h"

namespace nix {

/**
 * Check that outputs meets the requirements specified by the
 * 'outputChecks' attribute (or the legacy
 * '{allowed,disallowed}{References,Requisites}' attributes).
 *
 * The outputs may not be valid yet, hence outputs needs to contain all
 * needed info like the NAR size. However, the external (not other
 * output) references of the output must be valid, so we can compute the
 * closure size.
 */
void check_outputs(store_t& store, const store_path_t& drv_path,
                  const decltype(derivation_t::outputs)& drv_outputs,
                  const decltype(derivation_options_t<store_path_t>::output_checks)& drv_options,
                  const std::map<std::string, valid_path_info_t>& outputs, activity_t& act);

} // namespace nix
