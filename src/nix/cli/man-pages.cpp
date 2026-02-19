#include "man-pages.h"

#include "cli-config-private.h"
#include "nix/util/current-process.h"
#include "nix/util/environment-variables.h"
#include "nix/util/file-system.h"

namespace nix {

std::filesystem::path getNixManDir() {
  return canonPath(NIX_MAN_DIR);
}

void showManPage(const std::string& name) {
  restoreProcessContext();
  setEnv("MANPATH", (getNixManDir().string() + ":").c_str());
  execlp("man", "man", name.c_str(), nullptr);
  if (errno == ENOENT) {
    // Not SysError because we don't want to suffix the errno, aka No such file or directory.
    throw Error("The '%1%' command was not found, but it is needed for '%2%' and some other '%3%' "
                "commands' help text. Perhaps you could install the '%1%' command?",
                "man", name.c_str(), "nix-*");
  }
  throw sys_error_t("command 'man %1%' failed", name.c_str());
}

} // namespace nix
