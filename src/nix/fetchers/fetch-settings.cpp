#include "nix/fetchers/fetch-settings.h"

#include "nix/util/config-global.h"

namespace nix::fetchers {

settings_t::settings_t() {}

} // namespace nix::fetchers

namespace nix {

fetchers::settings_t fetchSettings;

static global_config_t::Register rFetchSettings(&fetchSettings);

} // namespace nix
