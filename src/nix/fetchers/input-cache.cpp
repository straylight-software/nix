#include "nix/fetchers/input-cache.h"

#include "nix/fetchers/registry.h"
#include "nix/util/source-path.h"
#include "nix/util/sync.h"

namespace nix::fetchers {

InputCache::CachedResult InputCache::get_accessor(const settings_t& settings, store_t& store,
                                                  const input_t& original_input,
                                                  UseRegistries use_registries) {
  auto fetched = lookup(original_input);
  input_t resolved_input = original_input;

  if (!fetched) {
    if (original_input.isDirect()) {
      auto [accessor, lockedInput] = original_input.get_accessor(settings, store);
      fetched.emplace(CachedInput{.lockedInput = lockedInput, .accessor = accessor});
    } else {
      if (use_registries != UseRegistries::No) {
        auto [res, extra_attrs] =
            lookup_in_registries(settings, store, original_input, use_registries);
        resolved_input = std::move(res);
        fetched = lookup(resolved_input);
        if (!fetched) {
          auto [accessor, lockedInput] = resolved_input.get_accessor(settings, store);
          fetched.emplace(CachedInput{
              .lockedInput = lockedInput, .accessor = accessor, .extra_attrs = extra_attrs});
        }
        upsert(resolved_input, *fetched);
      } else {
        throw Error("'%s' is an indirect flake reference, but registry lookups are not allowed",
                    original_input.to_string());
      }
    }
    upsert(original_input, *fetched);
  }

  debug("got tree '%s' from '%s'", fetched->accessor, fetched->lockedInput.to_string());

  return {fetched->accessor, resolved_input, fetched->lockedInput, fetched->extra_attrs};
}

struct input_cache_impl_t : InputCache {
  sync_t<std::map<input_t, CachedInput>> cache_;

  std::optional<CachedInput> lookup(const input_t& original_input) const override {
    auto cache(cache_.read_lock());
    auto i = cache->find(original_input);
    if (i == cache->end())
      return std::nullopt;
    debug("mapping '%s' to previously seen input '%s' -> '%s", original_input.to_string(),
          i->first.to_string(), i->second.lockedInput.to_string());
    return i->second;
  }

  void upsert(input_t key, CachedInput cached_input) override {
    cache_.lock()->insert_or_assign(std::move(key), std::move(cached_input));
  }

  void clear() override { cache_.lock()->clear(); }
};

ref<InputCache> InputCache::create() {
  return make_ref<input_cache_impl_t>();
}

} // namespace nix::fetchers
