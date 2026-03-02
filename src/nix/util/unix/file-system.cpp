#include "nix/util/file-system.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>

#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "nix/util/util-unix-config-private.h"

namespace nix {

descriptor_t open_directory(const std::filesystem::path& path) {
  return open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

void set_write_time(const std::filesystem::path& path, time_t accessed_time,
                    time_t modification_time, std::optional<bool> opt_is_symlink) {
  // Would be nice to use std::filesystem unconditionally, but
  // doesn't support access time just modification time.
  //
  // System clock vs File clock issues also make that annoying.
#if HAVE_UTIMENSAT && HAVE_DECL_AT_SYMLINK_NOFOLLOW
  struct timespec times[2] = {
      {
          .tv_sec = accessed_time,
          .tv_nsec = 0,
      },
      {
          .tv_sec = modification_time,
          .tv_nsec = 0,
      },
  };
  if (utimensat(AT_FDCWD, path.c_str(), times, AT_SYMLINK_NOFOLLOW) == -1) {
    throw sys_error_t("changing modification time of %s (using `utimensat`)", path);
  }
#else
  struct timeval times[2] = {
      {
          .tv_sec = accessed_time,
          .tv_usec = 0,
      },
      {
          .tv_sec = modification_time,
          .tv_usec = 0,
      },
  };
#  if HAVE_LUTIMES
  if (lutimes(path.c_str(), times) == -1) {
    throw sys_error_t("changing modification time of %s", path);
  }
#  else
  bool is_symlink = opt_is_symlink ? *opt_is_symlink : std::filesystem::is_symlink(path);

  if (!is_symlink) {
    if (utimes(path.c_str(), times) == -1) {
      throw sys_error_t("changing modification time of %s (not a symlink)", path);
    }
  } else {
    throw Error("Cannot change modification time of symlink %s", path);
  }
#  endif
#endif
}

} // namespace nix
