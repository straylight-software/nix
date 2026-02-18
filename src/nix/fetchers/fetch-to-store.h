#pragma once

#include "nix/fetchers/cache.h"
#include "nix/store/store-api.h"
#include "nix/util/file-content-address.h"
#include "nix/util/file-system.h"
#include "nix/util/repair-flag.h"
#include "nix/util/source-path.h"

namespace nix {

enum struct FetchMode { DryRun, Copy };

/**
 * Copy the `path` to the Nix store.
 */
StorePath fetchToStore(const fetchers::Settings& settings, Store& store, const SourcePath& path,
                       FetchMode mode, std::string_view name = "source",
                       ContentAddressMethod method = ContentAddressMethod::Raw::NixArchive,
                       PathFilter* filter = nullptr, RepairFlag repair = NoRepair);

std::pair<StorePath, Hash>
fetchToStore2(const fetchers::Settings& settings, Store& store, const SourcePath& path,
              FetchMode mode, std::string_view name = "source",
              ContentAddressMethod method = ContentAddressMethod::Raw::NixArchive,
              PathFilter* filter = nullptr, RepairFlag repair = NoRepair);

fetchers::Cache::Key makeSourcePathToHashCacheKey(const std::string& fingerprint,
                                                  ContentAddressMethod method,
                                                  const std::string& path);

} // namespace nix
