#pragma once
///@file

#include <boost/unordered/concurrent_flat_map.hpp>

#include "nix/store/store-api.h"
#include "nix/util/json-impls.h"

namespace nix {

struct dummy_store;

struct DummyStoreConfig : public std::enable_shared_from_this<DummyStoreConfig>,
                          virtual StoreConfig {
  DummyStoreConfig(const Params& params) : StoreConfig(params) {
    // Disable caching since this a temporary in-memory store.
    pathInfoCacheSize = 0;
  }

  DummyStoreConfig(std::string_view scheme, std::string_view authority, const Params& params)
      : DummyStoreConfig(params) {
    if (!authority.empty())
      throw UsageError("`%s` store URIs must not contain an authority part %s", scheme, authority);
  }

  setting_t<bool> read_only{this, true, "read-only",
                         R"(
          Make any sort of write fail instead of succeeding.
          No additional memory will be used, because no information needs to be stored.
        )"};

  static const std::string name() { return "Dummy Store"; }

  static std::string doc();

  static string_set_t uriSchemes() { return {"dummy"}; }

  /**
   * Same as `open_store`, just with a more precise return type.
   */
  ref<dummy_store> openDummyStore() const;

  ref<Store> open_store() const override;

  StoreReference getReference() const override {
    return {
        .variant =
            StoreReference::Specified{
                .scheme = *uriSchemes().begin(),
            },
        .params = getQueryParams(),
    };
  }
};

template <>
struct json_avoids_null<nix::DummyStoreConfig> : std::true_type {};

template <>
struct json_avoids_null<ref<nix::DummyStoreConfig>> : std::true_type {};

template <>
struct json_avoids_null<nix::dummy_store> : std::true_type {};

template <>
struct json_avoids_null<ref<nix::dummy_store>> : std::true_type {};

} // namespace nix

namespace nlohmann {

template <>
JSON_IMPL_INNER_TO(nix::DummyStoreConfig);
template <>
JSON_IMPL_INNER_FROM(nix::ref<nix::DummyStoreConfig>);
template <>
JSON_IMPL_INNER_TO(nix::dummy_store);
template <>
JSON_IMPL_INNER_FROM(nix::ref<nix::dummy_store>);

} // namespace nlohmann
