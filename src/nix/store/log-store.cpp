#include "nix/store/log-store.h"

namespace nix {

std::optional<std::string> LogStore::getBuildLog(const StorePath& path) {
  auto maybe_path = getBuildDerivationPath(path);
  if (!maybe_path)
    return std::nullopt;
  return getBuildLogExact(maybe_path.value());
}

} // namespace nix
