#include "nix/util/file-path.h"

#include <algorithm>
#include <codecvt>
#include <iostream>
#include <locale>

#include "nix/util/util.h"

namespace nix {

std::optional<std::filesystem::path> maybePath(PathView path) {
  return {path};
}

std::filesystem::path pathNG(PathView path) {
  return path;
}

} // namespace nix
