#include "nix/store/build/child.h"

#include <fcntl.h>
#include <unistd.h>

#include "nix/util/current-process.h"
#include "nix/util/logging.h"

namespace nix {

void common_child_init() {
  logger = make_simple_logger();

  const static std::string path_null_device = "/dev/null";
  restore_process_context(false);

  /* Put the child in a separate session (and thus a separate
     process group) so that it has no controlling terminal (meaning
     that e.g. ssh cannot open /dev/tty) and it doesn't receive
     terminal signals. */
  if (setsid() == -1) {
    throw sys_error_t("creating a new session");
  }

  /* Dup stderr to stdout. */
  if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
    throw sys_error_t("cannot dup stderr into stdout");
  }

  /* Reroute stdin to /dev/null. */
  int fd_dev_null = open(path_null_device.c_str(), O_RDWR);
  if (fd_dev_null == -1) {
    throw sys_error_t("cannot open '%1%'", path_null_device);
  }
  if (dup2(fd_dev_null, STDIN_FILENO) == -1) {
    throw sys_error_t("cannot dup null device into stdin");
  }
  close(fd_dev_null);
}

} // namespace nix
