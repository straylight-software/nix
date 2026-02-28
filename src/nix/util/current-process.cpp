#include "nix/util/current-process.h"

#include <algorithm>
#include <cstring>

#include <math.h>
#include <sys/stat.h>

#include "nix/util/environment-variables.h"
#include "nix/util/file-system.h"
#include "nix/util/finally.h"
#include "nix/util/processes.h"
#include "nix/util/signals.h"
#include "nix/util/util.h"

#ifdef __APPLE__
#  include <mach-o/dyld.h>
#endif

#ifdef __linux__
#  include <mutex>

#  include "nix/util/cgroup.h"
#  include "nix/util/linux-namespaces.h"
#endif

#ifdef __FreeBSD__
#  include <sys/param.h>
#  include <sys/sysctl.h>
#endif

namespace nix {

unsigned int get_max_cpu() {
#ifdef __linux__
  try {
    auto cgroup_fs = get_cgroup_fs();
    if (!cgroup_fs) {
      return 0;
    }

    auto cpu_file = *cgroup_fs + "/" + get_current_cgroup() + "/cpu.max";

    auto cpu_max = read_file(cpu_file);
    auto cpu_max_parts = tokenize_string<std::vector<std::string>>(cpu_max, " \n");

    if (cpu_max_parts.size() != 2) {
      return 0;
    }

    auto quota = cpu_max_parts[0];
    auto period = cpu_max_parts[1];
    if (quota != "max") {
      return std::ceil(std::stoi(quota) / std::stof(period));
    }
  } catch (Error&) {
    ignore_exception_in_destructor(lvl_debug);
  }
#endif

  return 0;
}

//////////////////////////////////////////////////////////////////////

#ifndef _WIN32
size_t saved_stack_size = 0;
static mode_t saved_umask = 0;
static bool saved_umask_valid = false;

void save_umask() {
  // Get current umask by setting and restoring (there's no way to just read it)
  saved_umask = ::umask(0);
  ::umask(saved_umask);
  saved_umask_valid = true;
}

void restore_umask() {
  if (saved_umask_valid) {
    ::umask(saved_umask);
  }
}

void set_stack_size(size_t stack_size) {
  struct rlimit limit;
  if (getrlimit(RLIMIT_STACK, &limit) == 0 && static_cast<size_t>(limit.rlim_cur) < stack_size) {
    saved_stack_size = limit.rlim_cur;
    if (limit.rlim_max < static_cast<rlim_t>(stack_size)) {
      if (get_env("_NIX_TEST_NO_ENVIRONMENT_WARNINGS") != "1") {
        logger->log(
            lvl_warn,
            hint_fmt_t("Stack size hard limit is %1%, which is less than the desired %2%. If "
                       "possible, increase the hard limit, e.g. with 'ulimit -Hs %3%'.",
                       limit.rlim_max, stack_size, stack_size / 1024)
                .str());
      }
    }
    auto requested_size = std::min(static_cast<rlim_t>(stack_size), limit.rlim_max);
    limit.rlim_cur = requested_size;
    if (setrlimit(RLIMIT_STACK, &limit) != 0) {
      logger->log(lvl_error,
                  hint_fmt_t("Failed to increase stack size from %1% to %2% (desired: %3%, "
                             "maximum allowed: %4%): %5%",
                             saved_stack_size, requested_size, stack_size, limit.rlim_max,
                             std::strerror(errno))
                      .str());
    }
  }
}
#endif

void restore_process_context(bool restore_mounts) {
#ifndef _WIN32
  unix::restore_signals();
  // Restore the original umask (NixOS/nix#15306)
  // nix sets umask(0022) for store operations, but we should restore the
  // user's original umask when spawning user processes like nix-shell/develop
  restore_umask();
#endif
  if (restore_mounts) {
#ifdef __linux__
    restore_mount_namespace();
#endif
  }

#ifndef _WIN32
  if (saved_stack_size) {
    struct rlimit limit;
    if (getrlimit(RLIMIT_STACK, &limit) == 0) {
      limit.rlim_cur = saved_stack_size;
      setrlimit(RLIMIT_STACK, &limit);
    }
  }
#endif
}

//////////////////////////////////////////////////////////////////////

std::optional<Path> get_self_exe() {
  static auto cached = []() -> std::optional<Path> {
#if defined(__linux__) || defined(__GNU__)
    return read_link(std::filesystem::path{"/proc/self/exe"});
#elif defined(__APPLE__)
    char buf[1024];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0)
      return buf;
    else
      return std::nullopt;
#elif defined(__FreeBSD__)
    int sysctlName[] = {
        CTL_KERN,
        KERN_PROC,
        KERN_PROC_PATHNAME,
        -1,
    };
    size_t path_len = 0;
    if (sysctl(sysctlName, sizeof(sysctlName) / sizeof(sysctlName[0]), nullptr, &path_len, nullptr,
               0) < 0) {
      return std::nullopt;
    }

    std::vector<char> path(path_len);
    if (sysctl(sysctlName, sizeof(sysctlName) / sizeof(sysctlName[0]), path.data(), &path_len,
               nullptr, 0) < 0) {
      return std::nullopt;
    }

    // FreeBSD's sysctl(KERN_PROC_PATHNAME) includes the null terminator in
    // pathLen. Strip it to prevent Nix evaluation errors when the path is
    // serialized to JSON and evaluated as a Nix string.
    path.pop_back();

    return Path(path.begin(), path.end());
#else
    return std::nullopt;
#endif
  }();
  return cached;
}

} // namespace nix
