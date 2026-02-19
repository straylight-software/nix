#pragma once

#include "nix/store/store-api.h"

namespace nix {

struct AsyncPathWriter {
  virtual StorePath add_path(std::string contents, std::string name, StorePathSet references,
                            RepairFlag repair, bool read_only = false) = 0;

  virtual void waitForPath(const StorePath& path) = 0;

  virtual void waitForAllPaths() = 0;

  static ref<AsyncPathWriter> make(ref<Store> store);
};

} // namespace nix
