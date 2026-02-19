#pragma once

#include "nix/store/store-api.h"

namespace nix {

/**
 * Export multiple paths in the format expected by `nix-store
 * --import`. The paths will be sorted topologically.
 */
void export_paths(Store& store, const StorePathSet& paths, Sink& sink, unsigned int version);

/**
 * Import a sequence of NAR dumps created by `export_paths()` into the
 * Nix store.
 */
StorePaths import_paths(Store& store, Source& source, CheckSigsFlag check_sigs = CheckSigs);

} // namespace nix
