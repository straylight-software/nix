#include "nix/fetchers/cache.h"

#include <nlohmann/json.hpp>

#include "nix/fetchers/fetch-settings.h"
#include "nix/store/globals.h"
#include "nix/store/sqlite.h"
#include "nix/store/store-api.h"
#include "nix/util/sync.h"
#include "nix/util/users.h"

namespace nix::fetchers {

static const char* schema = R"sql(

create table if not exists cache_t (
    domain    text not null,
    key       text not null,
    value     text not null,
    timestamp integer not null,
    primary key (domain, key)
);
)sql";

// FIXME: we should periodically purge/nuke this cache to prevent it
// from growing too big.

struct cache_impl_t : cache_t {
  struct State {
    SQLite db;
    SQLiteStmt upsert, lookup;
  };

  sync_t<State> _state;

  cache_impl_t() {
    auto state(_state.lock());

    auto db_path = (get_cache_dir() / "fetcher-cache-v4.sqlite").string();
    create_dirs(dir_of(db_path));

    state->db = SQLite(db_path);
    state->db.isCache();
    state->db.exec(schema);

    state->upsert.create(
        state->db,
        "insert or replace into Cache(domain, key, value, timestamp) values (?, ?, ?, ?)");

    state->lookup.create(state->db,
                         "select value, timestamp from Cache where domain = ? and key = ?");
  }

  void upsert(const Key& key, const Attrs& value) override {
    _state.lock()
        ->upsert
        .use()(key.first)(attrs_to_json(key.second).dump())(attrs_to_json(value).dump())(time(0))
        .exec();
  }

  std::optional<Attrs> lookup(const Key& key) override {
    if (auto res = lookupExpired(key)) {
      return std::move(res->value);
    }
    return {};
  }

  std::optional<Attrs> lookupWithTTL(const Key& key) override {
    if (auto res = lookupExpired(key)) {
      if (!res->expired) {
        return std::move(res->value);
      }
      debug("ignoring expired cache entry '%s:%s'", key.first, attrs_to_json(key.second).dump());
    }
    return {};
  }

  std::optional<Result> lookupExpired(const Key& key) override {
    auto state(_state.lock());

    auto key_json = attrs_to_json(key.second).dump();

    auto stmt(state->lookup.use()(key.first)(key_json));
    if (!stmt.next()) {
      debug("did not find cache entry for '%s:%s'", key.first, key_json);
      return {};
    }

    auto value_json = stmt.getStr(0);
    auto timestamp = stmt.getInt(1);

    debug("using cache entry '%s:%s' -> '%s'", key.first, key_json, value_json);

    return Result{
        .expired = settings.tarballTtl.get() == 0 || timestamp + settings.tarballTtl < time(0),
        .value = json_to_attrs(nlohmann::json::parse(value_json)),
    };
  }

  void upsert(Key key, store_t& store, Attrs value, const store_path_t& store_path) override {
    /* Add the store prefix to the cache key to handle multiple
       store prefixes. */
    key.second.insert_or_assign("store", store.store_dir);

    value.insert_or_assign("storePath", (std::string)store_path.to_string());

    upsert(key, value);
  }

  std::optional<ResultWithStorePath> lookupStorePath(Key key, store_t& store,
                                                     bool allow_invalid) override {
    key.second.insert_or_assign("store", store.store_dir);

    auto res = lookupExpired(key);
    if (!res) {
      return std::nullopt;
    }

    auto store_path_s = get_str_attr(res->value, "storePath");
    res->value.erase("storePath");

    ResultWithStorePath res2(*res, store_path_t(store_path_s));

    store.addTempRoot(res2.store_path);
    if (!allow_invalid && !store.isValidPath(res2.store_path)) {
      // FIXME: we could try to substitute 'storePath'.
      debug("ignoring disappeared cache entry '%s:%s' -> '%s'", key.first,
            attrs_to_json(key.second).dump(), store.printStorePath(res2.store_path));
      return std::nullopt;
    }

    debug("using cache entry '%s:%s' -> '%s', '%s'", key.first, attrs_to_json(key.second).dump(),
          attrs_to_json(res2.value).dump(), store.printStorePath(res2.store_path));

    return res2;
  }

  std::optional<ResultWithStorePath> lookupStorePathWithTTL(Key key, store_t& store) override {
    auto res = lookupStorePath(std::move(key), store, false);
    return res && !res->expired ? res : std::nullopt;
  }
};

ref<cache_t> settings_t::get_cache() const {
  auto cache(_cache.lock());
  if (!*cache) {
    *cache = std::make_shared<cache_impl_t>();
  }
  return ref<cache_t>(*cache);
}

} // namespace nix::fetchers
