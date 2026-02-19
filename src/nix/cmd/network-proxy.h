#pragma once
///@file

#include "nix/util/types.h"

namespace nix {

/**
 * environment_t variables relating to network proxying. These are used by
 * a few misc commands.
 *
 * See the environment_t section of https://curl.se/docs/manpage.html for details.
 */
extern const string_set_t network_proxy_variables;

/**
 * Heuristically check if there is a proxy connection by checking for defined
 * proxy variables.
 */
bool have_network_proxy_connection();

} // namespace nix
