#include "nix/util/linux-namespaces.h"

#include <mutex>

#include <fcntl.h> // O_RDONLY
#include <sys/mount.h>
#include <sys/resource.h>

#include "nix/util/cgroup.h"
#include "nix/util/current-process.h"
#include "nix/util/file-system.h"
#include "nix/util/finally.h"
#include "nix/util/processes.h"
#include "nix/util/signals.h"
#include "nix/util/util.h"

namespace nix {

bool user_namespaces_supported() {
  static auto res = [&]() -> bool {
    if (!path_exists("/proc/self/ns/user")) {
      debug("'/proc/self/ns/user' does not exist; your kernel was likely built without "
            "CONFIG_USER_NS=y");
      return false;
    }

    Path max_user_namespaces = "/proc/sys/user/max_user_namespaces";
    if (!path_exists(max_user_namespaces) || trim(read_file(max_user_namespaces)) == "0") {
      debug("user namespaces appear to be disabled; check '/proc/sys/user/max_user_namespaces'");
      return false;
    }

    Path proc_sys_kernel_unprivileged_userns_clone = "/proc/sys/kernel/unprivileged_userns_clone";
    if (path_exists(proc_sys_kernel_unprivileged_userns_clone) &&
        trim(read_file(proc_sys_kernel_unprivileged_userns_clone)) == "0") {
      debug("user namespaces appear to be disabled; check "
            "'/proc/sys/kernel/unprivileged_userns_clone'");
      return false;
    }

    try {
      process_handle_t pid = start_process([&]() { _exit(0); }, {.clone_flags = CLONE_NEWUSER});

      auto r = pid.wait();
      assert(!r);
    } catch (sys_error_t& e) {
      debug("user namespaces do not work on this system: %s", e.msg());
      return false;
    }

    return true;
  }();
  return res;
}

bool mount_and_pid_namespaces_supported() {
  static auto res = [&]() -> bool {
    try {
      process_handle_t pid = start_process(
          [&]() {
            /* Make sure we don't remount the parent's /proc. */
            if (mount(0, "/", 0, MS_PRIVATE | MS_REC, 0) == -1) {
              _exit(1);
            }

            /* Test whether we can remount /proc. The kernel disallows
               this if /proc is not fully visible, i.e. if there are
               filesystems mounted on top of files inside /proc.  See
               https://lore.kernel.org/lkml/87tvsrjai0.fsf@xmission.com/T/. */
            if (mount("none", "/proc", "proc", 0, 0) == -1) {
              _exit(2);
            }

            _exit(0);
          },
          {.clone_flags =
               CLONE_NEWNS | CLONE_NEWPID | (user_namespaces_supported() ? CLONE_NEWUSER : 0)});

      if (pid.wait()) {
        debug("PID namespaces do not work on this system: cannot remount /proc");
        return false;
      }

    } catch (sys_error_t& e) {
      debug("mount namespaces do not work on this system: %s", e.msg());
      return false;
    }

    return true;
  }();
  return res;
}

//////////////////////////////////////////////////////////////////////

static auto_close_fd_t fd_saved_mount_namespace;
static auto_close_fd_t fd_saved_root;

void save_mount_namespace() {
  static std::once_flag done;
  std::call_once(done, []() {
    fd_saved_mount_namespace = open("/proc/self/ns/mnt", O_RDONLY);
    if (!fd_saved_mount_namespace) {
      throw sys_error_t("saving parent mount namespace");
    }

    fd_saved_root = open("/proc/self/root", O_RDONLY);
  });
}

void restore_mount_namespace() {
  try {
    auto saved_cwd = std::filesystem::current_path();

    if (fd_saved_mount_namespace && setns(fd_saved_mount_namespace.get(), CLONE_NEWNS) == -1) {
      throw sys_error_t("restoring parent mount namespace");
    }

    if (fd_saved_root) {
      if (fchdir(fd_saved_root.get())) {
        throw sys_error_t("chdir into saved root");
      }
      if (chroot(".")) {
        throw sys_error_t("chroot into saved root");
      }
    }

    if (chdir(saved_cwd.c_str()) == -1) {
      throw sys_error_t("restoring cwd");
    }
  } catch (Error& e) {
    debug(e.msg());
  }
}

void try_unshare_filesystem() {
  if (unshare(CLONE_FS) != 0 && errno != EPERM && errno != ENOSYS) {
    throw sys_error_t("unsharing filesystem state");
  }
}

} // namespace nix
