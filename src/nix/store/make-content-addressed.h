#pragma once
///@file

#include "nix/store/store-api.h"

namespace nix {

/** Rewrite a closure of store paths to be completely content addressed.
 */
std::map<store_path_t, store_path_t> make_content_addressed(store_t& src_store, store_t& dst_store,
                                                    const store_path_set_t& root_paths);

/** Rewrite a closure of a store path to be completely content addressed.
 *
 * This is a convenience function for the case where you only have one root path.
 */
store_path_t make_content_addressed(store_t& src_store, store_t& dst_store, const store_path_t& root_path);

} // namespace nix
