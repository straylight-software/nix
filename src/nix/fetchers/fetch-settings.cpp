#include "nix/fetchers/fetch-settings.h"

#include "nix/util/config-global.h"

namespace nix::fetchers {

Settings::Settings() {}

} // namespace nix::fetchers

namespace nix {

fetchers::Settings fetchSettings;

static GlobalConfig::Register rFetchSettings(&fetchSettings);

} // namespace nix
