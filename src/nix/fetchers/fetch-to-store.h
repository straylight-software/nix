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
store_path_t
fetch_to_store(const fetchers::settings_t& settings, store_t& store, const source_path_t& path,
               FetchMode mode, std::string_view name = "source",
               content_address_method_t method = content_address_method_t::raw_t::nix_archive,
               path_filter_t* filter = nullptr, RepairFlag repair = NoRepair);

std::pair<store_path_t, Hash>
fetch_to_store2(const fetchers::settings_t& settings, store_t& store, const source_path_t& path,
                FetchMode mode, std::string_view name = "source",
                content_address_method_t method = content_address_method_t::raw_t::nix_archive,
                path_filter_t* filter = nullptr, RepairFlag repair = NoRepair);

fetchers::cache_t::Key make_source_path_to_hash_cache_key(const std::string& fingerprint,
                                                          content_address_method_t method,
                                                          const std::string& path);

} // namespace nix
