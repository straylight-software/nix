#include "nix/store/log-store.h"

namespace nix {

std::optional<std::string> LogStore::getBuildLog(const store_path_t& path) {
  auto maybe_path = getBuildDerivationPath(path);
  if (!maybe_path)
    return std::nullopt;
  return getBuildLogExact(maybe_path.value());
}

} // namespace nix
