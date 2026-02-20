#pragma once
///@file

#include "nix/store/store-api.h"

namespace nix {

/**
 * Helper to try downcasting a store_t with a nice method if it fails.
 *
 * This is basically an alternative to the user-facing part of
 * store_t::unsupported that allows us to still have a nice message but
 * better interface design.
 */
template <typename T>
T& require(store_t& store) {
  auto* castedStore = dynamic_cast<T*>(&store);
  if (!castedStore)
    throw UsageError("%s not supported by store '%s'", T::operation_name,
                     store.config.getHumanReadableURI());
  return *castedStore;
}

} // namespace nix
