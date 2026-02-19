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
extern const string_set_t networkProxyVariables;

/**
 * Heuristically check if there is a proxy connection by checking for defined
 * proxy variables.
 */
bool haveNetworkProxyConnection();

} // namespace nix
