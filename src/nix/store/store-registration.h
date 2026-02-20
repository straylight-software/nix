#pragma once
/**
 * @file
 *
 * Infrastructure for "registering" store implementations. Used by the
 * store implementation definitions themselves but not by consumers of
 * those implementations.
 *
 * Consumers of an arbitrary store from a URL/JSON configuration instead
 * just need the definitions `nix/store/store-open.hh`; those do use this
 * but only as an implementation. Consumers of a specific extra type of
 * store can skip both these, and just use the definition of the store
 * in question directly.
 */

#include "nix/store/store-api.h"

namespace nix {

struct StoreFactory {
  /**
   * Documentation for this type of store.
   */
  std::string doc;

  /**
   * URIs with these schemes should be handled by this factory
   */
  string_set_t uriSchemes;

  /**
   * An experimental feature this type store is gated, if it is to be
   * experimental.
   */
  std::optional<experimental_feature_t> experimental_feature;

  /**
   * The `authorityPath` parameter is `<authority>/<path>`, or really
   * whatever comes after `<scheme>://` and before `?<query-params>`.
   */
  std::function<ref<store_config_t>(std::string_view scheme, std::string_view authorityPath,
                                    const store_t::config_t::Params& params)>
      parseConfig;

  /**
   * Just for dumping the defaults. Kind of awkward this exists,
   * because it means we cannot require fields to be manually
   * specified so easily.
   */
  std::function<ref<store_config_t>()> getConfig;
};

struct Implementations {
  using Map = std::map<std::string, StoreFactory>;

  static Map& registered();

  template <typename TConfig>
  static void add() {
    StoreFactory factory{
        .doc = TConfig::doc(),
        .uriSchemes = TConfig::uriSchemes(),
        .experimental_feature = TConfig::experimental_feature(),
        .parseConfig = ([](auto scheme, auto uri, auto& params) -> ref<store_config_t> {
          return make_ref<TConfig>(scheme, uri, params);
        }),
        .getConfig = ([]() -> ref<store_config_t> {
          return make_ref<TConfig>(store_t::config_t::Params{});
        }),
    };
    auto [it, didInsert] = registered().insert({TConfig::name(), std::move(factory)});
    if (!didInsert) {
      throw Error("Already registered store with name '%s'", it->first);
    }
  }
};

template <typename TConfig>
struct RegisterStoreImplementation {
  RegisterStoreImplementation() { Implementations::add<TConfig>(); }
};

} // namespace nix
