#include "self-exe.h"

#include "cli-config-private.h"
#include "nix/store/globals.h"
#include "nix/util/current-process.h"
#include "nix/util/file-system.h"

namespace nix {

std::filesystem::path get_nix_bin(std::optional<std::string_view> binaryNameOpt) {
  auto getBinaryName = [&] { return binaryNameOpt ? *binaryNameOpt : "nix"; };

  // If the environment variable is set, use it unconditionally.
  if (auto envOpt = get_env_non_empty("NIX_BIN_DIR")) {
    return std::filesystem::path{*envOpt} / std::string{getBinaryName()};
  }

  // Try OS tricks, if available, to get to the path of this Nix, and
  // see if we can find the right executable next to that.
  if (auto selfOpt = get_self_exe()) {
    std::filesystem::path path{*selfOpt};
    if (binaryNameOpt) {
      path = path.parent_path() / std::string{*binaryNameOpt};
    }
    if (std::filesystem::exists(path)) {
      return path;
    }
  }

  // If `nix` exists at the hardcoded fallback path, use it.
  {
    auto path = std::filesystem::path{NIX_BIN_DIR} / std::string{getBinaryName()};
    if (std::filesystem::exists(path)) {
      return path;
    }
  }

  // return just the name, hoping the exe is on the `PATH`
  return getBinaryName();
}

} // namespace nix
