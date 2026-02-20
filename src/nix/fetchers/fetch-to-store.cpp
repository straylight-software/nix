#include "nix/fetchers/fetch-to-store.h"

#include "nix/fetchers/fetch-settings.h"
#include "nix/fetchers/fetchers.h"
#include "nix/util/environment-variables.h"

namespace nix {

fetchers::cache_t::Key make_source_path_to_hash_cache_key(const std::string& fingerprint,
                                                  content_address_method_t method,
                                                  const std::string& path) {
  return fetchers::cache_t::Key{
      "sourcePathToHash",
      {{"fingerprint", fingerprint}, {"method", std::string{method.render()}}, {"path", path}}};
}

store_path_t fetch_to_store(const fetchers::settings_t& settings, store_t& store, const source_path_t& path,
                       FetchMode mode, std::string_view name, content_address_method_t method,
                       path_filter_t* filter, RepairFlag repair) {
  return fetch_to_store2(settings, store, path, mode, name, method, filter, repair).first;
}

std::pair<store_path_t, Hash> fetch_to_store2(const fetchers::settings_t& settings, store_t& store,
                                         const source_path_t& path, FetchMode mode,
                                         std::string_view name, content_address_method_t method,
                                         path_filter_t* filter, RepairFlag repair) {
  std::optional<fetchers::cache_t::Key> cache_key;

  auto [subpath, fingerprint] =
      filter ? std::pair<canon_path_t, std::optional<std::string>>{path.path, std::nullopt}
             : path.accessor->get_fingerprint(path.path);

  if (fingerprint) {
    cache_key = make_source_path_to_hash_cache_key(*fingerprint, method, subpath.abs());
    if (auto res = settings.get_cache()->lookup(*cache_key)) {
      auto hash = Hash::parse_sri(fetchers::get_str_attr(*res, "hash"));
      auto store_path = store.makeFixedOutputPathFromCA(
          name, ContentAddressWithReferences::fromParts(method, hash, {}));
      if (mode == FetchMode::DryRun || store.maybeQueryPathInfo(store_path)) {
        debug("source path '%s' cache hit in '%s' (hash '%s')", path,
              store.printStorePath(store_path), hash.to_string(hash_format_t::sri, true));
        return {store_path, hash};
      }
      debug("source path '%s' not in store", path);
    }
  } else {
    static auto barf = get_env("_NIX_TEST_BARF_ON_UNCACHEABLE").value_or("") == "1";
    if (barf && !filter &&
        !(path.to_string().starts_with("/") || path.to_string().starts_with("«path:/")))
      throw Error("source path '%s' is uncacheable (filter=%d)", path, (bool)filter);
    // FIXME: could still provide in-memory caching keyed on `SourcePath`.
    debug("source path '%s' is uncacheable", path);
  }

  activity_t act(*logger, lvl_chatty, act_unknown,
               fmt(mode == FetchMode::DryRun ? "hashing '%s'" : "copying '%s' to the store", path));

  auto filter2 = filter ? *filter : default_path_filter;

  auto [store_path, hash] =
      mode == FetchMode::DryRun
          ? ({
              auto [store_path, hash] =
                  store.computeStorePath(name, path, method, hash_algorithm_t::SHA256, {}, filter2);
              debug("hashed '%s' to '%s' (hash '%s')", path, store.printStorePath(store_path),
                    hash.to_string(hash_format_t::sri, true));
              std::make_pair(store_path, hash);
            })
          : ({
              // FIXME: ideally addToStore() would return the hash
              // right away (like computeStorePath()).
              auto store_path =
                  store.add_to_store(name, path, method, hash_algorithm_t::SHA256, {}, filter2, repair);
              auto info = store.queryPathInfo(store_path);
              assert(info->references.empty());
              auto hash = method == content_address_method_t::raw_t::nix_archive ? info->nar_hash : ({
                if (!info->ca || info->ca->method != method)
                  throw Error("path '%s' lacks a CA field", store.printStorePath(store_path));
                info->ca->hash;
              });
              debug("copied '%s' to '%s' (hash '%s')", path, store.printStorePath(store_path),
                    hash.to_string(hash_format_t::sri, true));
              std::make_pair(store_path, hash);
            });

  if (cache_key)
    settings.get_cache()->upsert(*cache_key, {{"hash", hash.to_string(hash_format_t::sri, true)}});

  return {store_path, hash};
}

} // namespace nix
