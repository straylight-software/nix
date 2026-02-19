#include "nix/util/file-path.h"

#include <algorithm>
#include <codecvt>
#include <iostream>
#include <locale>

#include "nix/util/util.h"

namespace nix {

std::optional<std::filesystem::path> maybe_path(path_view_t path) {
  return {path};
}

std::filesystem::path path_ng(path_view_t path) {
  return path;
}

} // namespace nix
