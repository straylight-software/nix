#include "nix/store/pathlocks.h"

#include <cerrno>
#include <cstdlib>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "nix/util/signals.h"
#include "nix/util/sync.h"
#include "nix/util/util.h"

namespace nix {

auto_close_fd_t open_lock_file(const std::filesystem::path& path, bool create) {
  auto_close_fd_t fd;

  fd = open(path.c_str(), O_CLOEXEC | O_RDWR | (create ? O_CREAT : 0), 0600);
  if (!fd && (create || errno != ENOENT))
    throw sys_error_t("opening lock file %1%", path);

  return fd;
}

void delete_lock_file(const std::filesystem::path& path, descriptor_t desc) {
  /* Get rid of the lock file.  Have to be careful not to introduce
     races.  Write a (meaningless) token to the file to indicate to
     other processes waiting on this lock that the lock is stale
     (deleted). */
  unlink(path.c_str());
  write_full(desc, "d");
  /* Note that the result of unlink() is ignored; removing the lock
     file is an optimisation, not a necessity. */
}

bool lock_file(descriptor_t desc, LockType lock_type, bool wait) {
  int type;
  if (lock_type == ltRead)
    type = LOCK_SH;
  else if (lock_type == ltWrite)
    type = LOCK_EX;
  else if (lock_type == ltNone)
    type = LOCK_UN;
  else
    unreachable();

  if (wait) {
    while (flock(desc, type) != 0) {
      check_interrupt();
      if (errno != EINTR)
        throw sys_error_t("acquiring/releasing lock");
      else
        return false;
    }
  } else {
    while (flock(desc, type | LOCK_NB) != 0) {
      check_interrupt();
      if (errno == EWOULDBLOCK)
        return false;
      if (errno != EINTR)
        throw sys_error_t("acquiring/releasing lock");
    }
  }

  return true;
}

bool PathLocks::lockPaths(const std::set<std::filesystem::path>& paths, const std::string& waitMsg,
                          bool wait) {
  assert(fds.empty());

  /* Note that `fds' is built incrementally so that the destructor
     will only release those locks that we have already acquired. */

  /* Acquire the lock for each path in sorted order. This ensures
     that locks are always acquired in the same order, thus
     preventing deadlocks. */
  for (auto& path : paths) {
    check_interrupt();
    std::filesystem::path lockPath = path + ".lock";

    debug("locking path %1%", path);

    auto_close_fd_t fd;

    while (1) {
      /* Open/create the lock file. */
      fd = open_lock_file(lockPath, true);

      /* Acquire an exclusive lock. */
      if (!lock_file(fd.get(), ltWrite, false)) {
        if (wait) {
          if (waitMsg != "")
            printError(waitMsg);
          lock_file(fd.get(), ltWrite, true);
        } else {
          /* Failed to lock this path; release all other
             locks. */
          unlock();
          return false;
        }
      }

      debug("lock acquired on %1%", lockPath);

      /* Check that the lock file hasn't become stale (i.e.,
         hasn't been unlinked). */
      struct stat st;
      if (fstat(fd.get(), &st) == -1)
        throw sys_error_t("statting lock file %1%", lockPath);
      if (st.st_size != 0)
        /* This lock file has been unlinked, so we're holding
           a lock on a deleted file.  This means that other
           processes may create and acquire a lock on
           `lockPath', and proceed.  So we must retry. */
        debug("open lock file %1% has become stale", lockPath);
      else
        break;
    }

    /* use borrow so that the descriptor isn't closed. */
    fds.push_back(FDPair(fd.release(), lockPath));
  }

  return true;
}

void PathLocks::unlock() {
  for (auto& i : fds) {
    if (deletePaths)
      delete_lock_file(i.second, i.first);

    if (close(i.first) == -1)
      printError("error (ignored): cannot close lock file on %1%", i.second);

    debug("lock released on %1%", i.second);
  }

  fds.clear();
}

FdLock::FdLock(descriptor_t desc, LockType lock_type, bool wait, std::string_view waitMsg)
    : desc(desc) {
  if (wait) {
    if (!lock_file(desc, lock_type, false)) {
      printInfo("%s", waitMsg);
      acquired = lock_file(desc, lock_type, true);
    }
  } else
    acquired = lock_file(desc, lock_type, false);
}

} // namespace nix
