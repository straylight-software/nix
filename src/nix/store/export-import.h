#pragma once

#include "nix/store/store-api.h"

namespace nix {

/**
 * Export multiple paths in the format expected by `nix-store
 * --import`. The paths will be sorted topologically.
 */
void export_paths(store_t& store, const store_path_set_t& paths, sink_t& sink, unsigned int version);

/**
 * Import a sequence of NAR dumps created by `export_paths()` into the
 * Nix store.
 */
store_paths_t import_paths(store_t& store, source_t& source, CheckSigsFlag check_sigs = CheckSigs);

} // namespace nix
