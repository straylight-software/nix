#include "nix/fetchers/input-cache.h"

#include "nix/fetchers/registry.h"
#include "nix/util/source-path.h"
#include "nix/util/sync.h"

namespace nix::fetchers {

InputCache::CachedResult InputCache::get_accessor(const settings_t& settings, store_t& store,
                                                  const input_t& original_input,
                                                  UseRegistries use_registries) {
  // First, check if we have a complete cache entry for the original input.
  // This is critical for issue #9570 - we must return cached results without re-fetching.
  auto fetched = lookup(original_input);
  input_t resolved_input = original_input;

  if (fetched) {
    // Cache hit - return immediately without any fetching.
    // This fixes #9570 where inputs were being re-fetched despite cache hits.
    debug("cache hit for '%s' -> '%s'", original_input.to_string(),
          fetched->lockedInput.to_string());
    return {fetched->accessor, resolved_input, fetched->lockedInput, fetched->extra_attrs};
  }

  // Cache miss - need to fetch
  if (original_input.isDirect()) {
    auto [accessor, lockedInput] = original_input.get_accessor(settings, store);
    fetched.emplace(CachedInput{.lockedInput = lockedInput, .accessor = accessor});
  } else {
    if (use_registries != UseRegistries::No) {
      // Check if we've already resolved this input through registry lookup.
      // This addresses issue #9339 by caching registry resolution per session.
      auto cachedResolution = lookupRegistryResolution(original_input);
      if (cachedResolution) {
        resolved_input = cachedResolution->resolved_input;
        // Try to find the resolved input in cache
        fetched = lookup(resolved_input);
        if (fetched) {
          // Merge extra_attrs from registry resolution
          for (auto& [k, v] : cachedResolution->extra_attrs)
            if (!fetched->extra_attrs.contains(k))
              fetched->extra_attrs.insert_or_assign(k, v);
        }
      }

      if (!fetched) {
        // No cached resolution or resolved input not in cache - do full lookup
        if (!cachedResolution) {
          auto [res, extra_attrs] =
              lookup_in_registries(settings, store, original_input, use_registries);
          resolved_input = std::move(res);
          // Cache the registry resolution for future lookups (#9339)
          upsertRegistryResolution(
              original_input,
              CachedRegistryLookup{.resolved_input = resolved_input, .extra_attrs = extra_attrs});
          // Check if resolved input is cached
          fetched = lookup(resolved_input);
          if (fetched) {
            for (auto& [k, v] : extra_attrs)
              if (!fetched->extra_attrs.contains(k))
                fetched->extra_attrs.insert_or_assign(k, v);
          }
        }

        if (!fetched) {
          auto [accessor, lockedInput] = resolved_input.get_accessor(settings, store);
          auto cachedRes = lookupRegistryResolution(original_input);
          Attrs extra_attrs = cachedRes ? cachedRes->extra_attrs : Attrs{};
          fetched.emplace(CachedInput{
              .lockedInput = lockedInput, .accessor = accessor, .extra_attrs = extra_attrs});
        }
      }
      upsert(resolved_input, *fetched);
    } else {
      throw Error("'%s' is an indirect flake reference, but registry lookups are not allowed",
                  original_input.to_string());
    }
  }
  upsert(original_input, *fetched);

  debug("got tree '%s' from '%s'", fetched->accessor, fetched->lockedInput.to_string());

  return {fetched->accessor, resolved_input, fetched->lockedInput, fetched->extra_attrs};
}

struct input_cache_impl_t : InputCache {
  sync_t<std::map<input_t, CachedInput>> cache_;
  sync_t<std::map<std::string, CachedLockFile>> lockFileCache_;
  sync_t<std::map<input_t, CachedRegistryLookup>> registryCache_;

  std::optional<CachedInput> lookup(const input_t& original_input) const override {
    auto cache(cache_.read_lock());
    auto i = cache->find(original_input);
    if (i == cache->end())
      return std::nullopt;
    debug("cache hit: mapping '%s' to previously seen input '%s' -> '%s'",
          original_input.to_string(), i->first.to_string(), i->second.lockedInput.to_string());
    return i->second;
  }

  void upsert(input_t key, CachedInput cached_input) override {
    cache_.lock()->insert_or_assign(std::move(key), std::move(cached_input));
  }

  void clear() override {
    cache_.lock()->clear();
    lockFileCache_.lock()->clear();
    registryCache_.lock()->clear();
  }

  // Lock file cache implementation for #9339
  std::optional<CachedLockFile> lookupLockFile(const std::string& path) const override {
    auto cache(lockFileCache_.read_lock());
    auto i = cache->find(path);
    if (i == cache->end())
      return std::nullopt;
    debug("lock file cache hit for '%s'", path);
    return i->second;
  }

  void upsertLockFile(std::string path, CachedLockFile lock_file) override {
    lockFileCache_.lock()->insert_or_assign(std::move(path), std::move(lock_file));
  }

  // Registry resolution cache implementation for #9339
  std::optional<CachedRegistryLookup>
  lookupRegistryResolution(const input_t& input) const override {
    auto cache(registryCache_.read_lock());
    auto i = cache->find(input);
    if (i == cache->end())
      return std::nullopt;
    debug("registry resolution cache hit for '%s' -> '%s'", input.to_string(),
          i->second.resolved_input.to_string());
    return i->second;
  }

  void upsertRegistryResolution(input_t key, CachedRegistryLookup result) override {
    registryCache_.lock()->insert_or_assign(std::move(key), std::move(result));
  }
};

ref<InputCache> InputCache::create() {
  return make_ref<input_cache_impl_t>();
}

} // namespace nix::fetchers
