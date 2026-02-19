#include "nix/fetchers/fetch-settings.h"

#include "nix/util/config-global.h"

namespace nix::fetchers {

settings_t::settings_t() {}

} // namespace nix::fetchers

namespace nix {

fetchers::settings_t fetch_settings;

static global_config_t::Register r_fetch_settings(&fetch_settings);

} // namespace nix
