#pragma once
///@file

#include "nix/store/store-api.h"

namespace nix {

/** Rewrite a closure of store paths to be completely content addressed.
 */
std::map<StorePath, StorePath> make_content_addressed(Store& src_store, Store& dst_store,
                                                    const StorePathSet& root_paths);

/** Rewrite a closure of a store path to be completely content addressed.
 *
 * This is a convenience function for the case where you only have one root path.
 */
StorePath make_content_addressed(Store& src_store, Store& dst_store, const StorePath& root_path);

} // namespace nix
