#include "nix/store/log-store.h"

namespace nix {

std::optional<std::string> LogStore::getBuildLog(const StorePath& path) {
  auto maybePath = getBuildDerivationPath(path);
  if (!maybePath)
    return std::nullopt;
  return getBuildLogExact(maybePath.value());
}

} // namespace nix
