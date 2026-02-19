#pragma once
///@file

#include <filesystem>

#include "nix/util/file-descriptor.h"

namespace nix {

/**
 * Open (possibly create) a lock file and return the file descriptor.
 * -1 is returned if create is false and the lock could not be opened
 * because it doesn't exist.  Any other error throws an exception.
 */
auto_close_fd_t openLockFile(const std::filesystem::path& path, bool create);

/**
 * Delete an open lock file.
 */
void deleteLockFile(const std::filesystem::path& path, descriptor_t desc);

enum LockType { ltRead, ltWrite, ltNone };

bool lockFile(descriptor_t desc, LockType lockType, bool wait);

class PathLocks {
private:
  typedef std::pair<descriptor_t, std::filesystem::path> FDPair;
  std::list<FDPair> fds;
  bool deletePaths;

public:
  PathLocks();
  PathLocks(const std::set<std::filesystem::path>& paths, const std::string& waitMsg = "");
  bool lockPaths(const std::set<std::filesystem::path>& _paths, const std::string& waitMsg = "",
                 bool wait = true);
  ~PathLocks();
  void unlock();
  void setDeletion(bool deletePaths);
};

struct FdLock {
  descriptor_t desc;
  bool acquired = false;

  FdLock(descriptor_t desc, LockType lockType, bool wait, std::string_view waitMsg);

  ~FdLock() {
    if (acquired)
      lockFile(desc, ltNone, false);
  }
};

} // namespace nix
