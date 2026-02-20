#pragma once
///@file

#include "nix/store/store-api.h"

namespace nix {

struct LogStore : public virtual store_t {
  inline static std::string operation_name = "Build log storage and retrieval";

  /**
   * Return the build log of the specified store path, if available,
   * or null otherwise.
   */
  std::optional<std::string> getBuildLog(const store_path_t& path);

  virtual std::optional<std::string> getBuildLogExact(const store_path_t& path) = 0;

  virtual void addBuildLog(const store_path_t& path, std::string_view log) = 0;

  static LogStore& require(store_t& store);
};

} // namespace nix
