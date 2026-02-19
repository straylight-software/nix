#include "man-pages.h"

#include "cli-config-private.h"
#include "nix/util/current-process.h"
#include "nix/util/environment-variables.h"
#include "nix/util/file-system.h"

namespace nix {

std::filesystem::path get_nix_man_dir() {
  return canon_path(NIX_MAN_DIR);
}

void show_man_page(const std::string& name) {
  restore_process_context();
  set_env("MANPATH", (get_nix_man_dir().string() + ":").c_str());
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
