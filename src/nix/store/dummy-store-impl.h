#pragma once
///@file

#include <boost/unordered/concurrent_flat_map.hpp>

#include "nix/store/derivations.h"
#include "nix/store/dummy-store.h"

namespace nix {

struct memory_source_accessor_t;

/**
 * Enough of the Dummy store_t exposed for sake of writing unit tests
 */
struct dummy_store : virtual store_t {
  using config_t = DummyStoreConfig;

  ref<const config_t> config;

  struct PathInfoAndContents {
    UnkeyedValidPathInfo info;
    ref<memory_source_accessor_t> contents;

    bool operator==(const PathInfoAndContents&) const;
  };

  /**
   * This map conceptually owns the file system objects for each
   * store object.
   */
  boost::concurrent_flat_map<store_path_t, PathInfoAndContents> contents;

  /**
   * This map conceptually owns every derivation, allowing us to
   * avoid "on-disk drv format" serialization round-trips.
   */
  boost::concurrent_flat_map<store_path_t, derivation_t> derivations;

  /**
   * The build trace maps the pair of a content-addressing (fixed or
   * floating) derivations an one of its output to a
   * (content-addressed) store object.
   *
   * It is [curried](https://en.wikipedia.org/wiki/Currying), so we
   * instead having a single output with a `DrvOutput` key, we have an
   * outer map for the derivation, and inner maps for the outputs of a
   * given derivation.
   */
  boost::concurrent_flat_map<Hash, std::map<std::string, UnkeyedRealisation>> buildTrace;

  dummy_store(ref<const config_t> config) : store_t{*config}, config(config) {}

  bool operator==(const dummy_store&) const;
};

template <>
struct json_avoids_null<dummy_store::PathInfoAndContents> : std::true_type {};

} // namespace nix

JSON_IMPL(nix::dummy_store::PathInfoAndContents)
