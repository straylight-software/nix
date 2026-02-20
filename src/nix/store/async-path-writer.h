#pragma once

#include "nix/store/store-api.h"

namespace nix {

struct AsyncPathWriter {
  virtual store_path_t add_path(std::string contents, std::string name, store_path_set_t references,
                            RepairFlag repair, bool read_only = false) = 0;

  virtual void waitForPath(const store_path_t& path) = 0;

  virtual void waitForAllPaths() = 0;

  static ref<AsyncPathWriter> make(ref<store_t> store);
};

} // namespace nix
