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
StorePath fetch_to_store(const fetchers::settings_t& settings, Store& store, const source_path_t& path,
                       FetchMode mode, std::string_view name = "source",
                       ContentAddressMethod method = ContentAddressMethod::raw_t::nix_archive,
                       path_filter_t* filter = nullptr, RepairFlag repair = NoRepair);

std::pair<StorePath, Hash>
fetch_to_store2(const fetchers::settings_t& settings, Store& store, const source_path_t& path,
              FetchMode mode, std::string_view name = "source",
              ContentAddressMethod method = ContentAddressMethod::raw_t::nix_archive,
              path_filter_t* filter = nullptr, RepairFlag repair = NoRepair);

fetchers::cache_t::Key make_source_path_to_hash_cache_key(const std::string& fingerprint,
                                                  ContentAddressMethod method,
                                                  const std::string& path);

} // namespace nix
