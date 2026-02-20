#pragma once

#include <map>
#include <string>
#include <variant>

#include "nix/store/content-address.h"
#include "nix/store/path.h"
#include "nix/util/configuration.h"
#include "nix/util/hash.h"

namespace nix {

struct source_path_t;

make_error(BadStorePath, Error);
make_error(BadStorePathName, BadStorePath);

/**
 * @todo This should just be inherited by `store_config_t`. However, it
 * would be a huge amount of churn if `store_t` didn't have these methods
 * anymore, forcing a bunch of code to go from `store.method(...)` to
 * `store.config.method(...)`.
 *
 * @todo this should not have "config" in its name, because it no longer
 * uses the configuration system for `store_dir` --- in fact, `store_dir`
 * isn't even owned, but a mere reference. But doing that rename would
 * cause a bunch of churn.
 */
struct store_dir_config_t {
  const Path& store_dir;

  // pure methods

  store_path_t parseStorePath(std::string_view path) const;

  std::optional<store_path_t> maybeParseStorePath(std::string_view path) const;

  std::string printStorePath(const store_path_t& path) const;

  /**
   * deprecated
   *
   * \todo remove
   */
  store_path_set_t parseStorePathSet(const path_set_t& paths) const;

  path_set_t printStorePathSet(const store_path_set_t& path) const;

  /**
   * Display a set of paths in human-readable form (i.e., between quotes
   * and separated by commas).
   */
  std::string show_paths(const store_path_set_t& paths) const;

  /**
   * @return true if *path* is in the Nix store (but not the Nix
   * store itself).
   */
  bool isInStore(path_view_t path) const;

  /**
   * @return true if *path* is a store path, i.e. a direct child of the
   * Nix store.
   */
  bool isStorePath(std::string_view path) const;

  /**
   * Split a path like `/nix/store/<hash>-<name>/<bla>` into
   * `/nix/store/<hash>-<name>` and `/<bla>`.
   */
  std::pair<store_path_t, Path> toStorePath(path_view_t path) const;

  /**
   * Constructs a unique store path name.
   */
  store_path_t makeStorePath(std::string_view type, std::string_view hash,
                             std::string_view name) const;
  store_path_t makeStorePath(std::string_view type, const Hash& hash, std::string_view name) const;

  store_path_t makeOutputPath(std::string_view id, const Hash& hash, std::string_view name) const;

  store_path_t makeFixedOutputPath(std::string_view name, const FixedOutputInfo& info) const;

  store_path_t makeFixedOutputPathFromCA(std::string_view name,
                                         const ContentAddressWithReferences& ca) const;

  /**
   * Read-only variant of add_to_store(). It returns the store
   * path for the given file system object.
   */
  std::pair<store_path_t, Hash>
  computeStorePath(std::string_view name, const source_path_t& path,
                   content_address_method_t method = content_address_method_t::raw_t::nix_archive,
                   hash_algorithm_t hash_algo = hash_algorithm_t::SHA256,
                   const store_path_set_t& references = {},
                   path_filter_t& filter = default_path_filter) const;
};

} // namespace nix
