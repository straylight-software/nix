#include "nix/store/pathlocks.h"

#include <cerrno>
#include <cstdlib>

#include "nix/util/signals.h"
#include "nix/util/sync.h"
#include "nix/util/util.h"

namespace nix {

PathLocks::PathLocks() : deletePaths(false) {}

PathLocks::PathLocks(const std::set<std::filesystem::path>& paths, const std::string& waitMsg)
    : deletePaths(false) {
  lockPaths(paths, waitMsg);
}

PathLocks::~PathLocks() {
  try {
    unlock();
  } catch (...) {
    ignore_exception_in_destructor();
  }
}

void PathLocks::setDeletion(bool deletePaths) {
  this->deletePaths = deletePaths;
}

} // namespace nix
