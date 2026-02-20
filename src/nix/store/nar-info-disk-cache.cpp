#include "nix/store/nar-info-disk-cache.h"

#include <sqlite3.h>

#include <nlohmann/json.hpp>

#include "nix/store/globals.h"
#include "nix/store/sqlite.h"
#include "nix/util/strings.h"
#include "nix/util/sync.h"
#include "nix/util/users.h"

namespace nix {

static const char* schema = R"sql(

create table if not exists BinaryCaches (
    id        integer primary key autoincrement not null,
    url       text unique not null,
    timestamp integer not null,
    store_dir  text not null,
    want_mass_query integer not null,
    priority  integer not null
);

create table if not exists NARs (
    cache            integer not null,
    hash_part         text not null,
    name_part         text,
    url              text,
    compression      text,
    fileHash         text,
    file_size         integer,
    nar_hash          text,
    nar_size          integer,
    refs             text,
    deriver          text,
    sigs             text,
    ca               text,
    timestamp        integer not null,
    present          integer not null,
    primary key (cache, hash_part),
    foreign key (cache) references BinaryCaches(id) on delete cascade
);

create table if not exists Realisations (
    cache integer not null,
    output_id text not null,
    content blob, -- Json serialisation of the realisation, or null if the realisation is absent
    timestamp        integer not null,
    primary key (cache, output_id),
    foreign key (cache) references BinaryCaches(id) on delete cascade
);

create table if not exists LastPurge (
    dummy            text primary key,
    value            integer
);

)sql";

class nar_info_disk_cache_impl_t : public NarInfoDiskCache {
public:
  /* How often to purge expired entries from the cache. */
  const int purge_interval = 24 * 3600;

  /* How long to cache binary cache info (i.e. /nix-cache-info) */
  const int cache_info_ttl = 7 * 24 * 3600;

  struct cache_t {
    int id;
    Path store_dir;
    bool want_mass_query;
    int priority;
  };

  struct State {
    SQLite db;
    SQLiteStmt insert_cache, query_cache, insert_nar, insert_missing_nar, query_nar, insert_realisation,
        insert_missing_realisation, query_realisation, purge_cache;
    std::map<std::string, cache_t> caches;
  };

  sync_t<State> _state;

  nar_info_disk_cache_impl_t(Path db_path = (get_cache_dir() / "binary-cache-v7.sqlite").string()) {
    auto state(_state.lock());

    create_dirs(dir_of(db_path));

    state->db = SQLite(db_path);

    state->db.isCache();

    state->db.exec(schema);

    state->insert_cache.create(
        state->db, "insert into BinaryCaches(url, timestamp, storeDir, wantMassQuery, priority) "
                   "values (?1, ?2, ?3, ?4, ?5) on conflict (url) do update set timestamp = ?2, "
                   "storeDir = ?3, wantMassQuery = ?4, priority = ?5 returning id;");

    state->query_cache.create(state->db, "select id, storeDir, wantMassQuery, priority from "
                                        "BinaryCaches where url = ? and timestamp > ?");

    state->insert_nar.create(state->db, "insert or replace into NARs(cache, hashPart, namePart, "
                                       "url, compression, fileHash, fileSize, narHash, "
                                       "narSize, refs, deriver, sigs, ca, timestamp, present) "
                                       "values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1)");

    state->insert_missing_nar.create(
        state->db,
        "insert or replace into NARs(cache, hashPart, timestamp, present) values (?, ?, ?, 0)");

    state->query_nar.create(
        state->db, "select present, namePart, url, compression, fileHash, fileSize, narHash, "
                   "narSize, refs, deriver, sigs, ca from NARs where cache = ? and hashPart = ? "
                   "and ((present = 0 and timestamp > ?) or (present = 1 and timestamp > ?))");

    state->insert_realisation.create(state->db,
                                    R"(
                insert or replace into Realisations(cache, output_id, content, timestamp)
                    values (?, ?, ?, ?)
            )");

    state->insert_missing_realisation.create(state->db,
                                           R"(
                insert or replace into Realisations(cache, output_id, timestamp)
                    values (?, ?, ?)
            )");

    state->query_realisation.create(state->db,
                                   R"(
                select content from Realisations
                    where cache = ? and output_id = ?  and
                        ((content is null and timestamp > ?) or
                         (content is not null and timestamp > ?))
            )");

    /* Periodically purge expired entries from the database. */
    retrySQLite<void>([&]() {
      auto now = time(0);

      SQLiteStmt query_last_purge(state->db, "select value from LastPurge");
      auto query_last_purge_(query_last_purge.use());

      if (!query_last_purge_.next() || query_last_purge_.getInt(0) < now - purge_interval) {
        SQLiteStmt(state->db, "delete from NARs where ((present = 0 and timestamp < ?) or (present "
                              "= 1 and timestamp < ?))")
            .use()
            // Use a minimum TTL to prevent --refresh from
            // nuking the entire disk cache.
            (now - std::max(settings.ttlNegativeNarInfoCache.get(), 3600U))(
                now - std::max(settings.ttlPositiveNarInfoCache.get(), 30 * 24 * 3600U))
            .exec();

        debug("deleted %d entries from the NAR info disk cache", sqlite3_changes(state->db));

        SQLiteStmt(state->db, "insert or replace into LastPurge(dummy, value) values ('', ?)")
            .use()(now)
            .exec();
      }
    });
  }

  cache_t& get_cache(State& state, const std::string& uri) {
    auto i = state.caches.find(uri);
    if (i == state.caches.end())
      unreachable();
    return i->second;
  }

private:
  std::optional<cache_t> query_cache_raw(State& state, const std::string& uri) {
    auto i = state.caches.find(uri);
    if (i == state.caches.end()) {
      auto query_cache(state.query_cache.use()(uri)(time(0) - cache_info_ttl));
      if (!query_cache.next())
        return std::nullopt;
      auto cache = cache_t{
          .id = (int)query_cache.getInt(0),
          .store_dir = query_cache.getStr(1),
          .want_mass_query = query_cache.getInt(2) != 0,
          .priority = (int)query_cache.getInt(3),
      };
      state.caches.emplace(uri, cache);
    }
    return get_cache(state, uri);
  }

public:
  int createCache(const std::string& uri, const Path& store_dir, bool want_mass_query,
                  int priority) override {
    return retrySQLite<int>([&]() {
      auto state(_state.lock());
      SQLiteTxn txn(state->db);

      // To avoid the race, we have to check if maybe someone hasn't yet created
      // the cache for this URI in the meantime.
      auto cache(query_cache_raw(*state, uri));

      if (cache)
        return cache->id;

      cache_t ret{
          .id = -1, // set below
          .store_dir = store_dir,
          .want_mass_query = want_mass_query,
          .priority = priority,
      };

      {
        auto r(state->insert_cache.use()(uri)(time(0))(store_dir)(want_mass_query)(priority));
        if (!r.next()) {
          unreachable();
        }
        ret.id = (int)r.getInt(0);
      }

      state->caches[uri] = ret;

      txn.commit();
      return ret.id;
    });
  }

  std::optional<CacheInfo> upToDateCacheExists(const std::string& uri) override {
    return retrySQLite<std::optional<CacheInfo>>([&]() -> std::optional<CacheInfo> {
      auto state(_state.lock());
      auto cache(query_cache_raw(*state, uri));
      if (!cache)
        return std::nullopt;
      return CacheInfo{
          .id = cache->id, .want_mass_query = cache->want_mass_query, .priority = cache->priority};
    });
  }

  std::pair<Outcome, std::shared_ptr<nar_info_t>> lookupNarInfo(const std::string& uri,
                                                             const std::string& hash_part) override {
    return retrySQLite<std::pair<Outcome, std::shared_ptr<nar_info_t>>>(
        [&]() -> std::pair<Outcome, std::shared_ptr<nar_info_t>> {
          auto state(_state.lock());

          auto& cache(get_cache(*state, uri));

          auto now = time(0);

          auto query_nar(
              state->query_nar.use()(cache.id)(hash_part)(now - settings.ttlNegativeNarInfoCache)(
                  now - settings.ttlPositiveNarInfoCache));

          if (!query_nar.next())
            return {oUnknown, 0};

          if (!query_nar.getInt(0))
            return {oInvalid, 0};

          auto name_part = query_nar.getStr(1);
          auto narInfo = make_ref<nar_info_t>(cache.store_dir, store_path_t(hash_part + "-" + name_part),
                                           Hash::parse_any_prefixed(query_nar.getStr(6)));
          narInfo->url = query_nar.getStr(2);
          narInfo->compression = query_nar.getStr(3);
          if (!query_nar.isNull(4))
            narInfo->fileHash = Hash::parse_any_prefixed(query_nar.getStr(4));
          narInfo->file_size = query_nar.getInt(5);
          narInfo->nar_size = query_nar.getInt(7);
          for (auto& r : tokenize_string<strings_t>(query_nar.getStr(8), " "))
            narInfo->references.insert(store_path_t(r));
          if (!query_nar.isNull(9))
            narInfo->deriver = store_path_t(query_nar.getStr(9));
          for (auto& sig : tokenize_string<strings_t>(query_nar.getStr(10), " "))
            narInfo->sigs.insert(sig);
          narInfo->ca = content_address_t::parseOpt(query_nar.getStr(11));

          return {oValid, narInfo};
        });
  }

  std::pair<Outcome, std::shared_ptr<realisation_t>> lookupRealisation(const std::string& uri,
                                                                     const DrvOutput& id) override {
    return retrySQLite<std::pair<Outcome, std::shared_ptr<realisation_t>>>(
        [&]() -> std::pair<Outcome, std::shared_ptr<realisation_t>> {
          auto state(_state.lock());

          auto& cache(get_cache(*state, uri));

          auto now = time(0);

          auto query_realisation(state->query_realisation.use()(cache.id)(id.to_string())(
              now - settings.ttlNegativeNarInfoCache)(now - settings.ttlPositiveNarInfoCache));

          if (!query_realisation.next())
            return {oUnknown, 0};

          if (query_realisation.isNull(0))
            return {oInvalid, 0};

          try {
            return {
                oValid,
                std::make_shared<realisation_t>(nlohmann::json::parse(query_realisation.getStr(0))),
            };
          } catch (Error& e) {
            e.add_trace({}, "while parsing the local disk cache");
            throw;
          }
        });
  }

  void upsertNarInfo(const std::string& uri, const std::string& hash_part,
                     std::shared_ptr<const valid_path_info_t> info) override {
    retrySQLite<void>([&]() {
      auto state(_state.lock());

      auto& cache(get_cache(*state, uri));

      if (info) {
        auto narInfo = std::dynamic_pointer_cast<const nar_info_t>(info);

        // assert(hashPart == storePathToHash(info->path));

        state->insert_nar
            .use()(cache.id)(hash_part)(std::string(info->path.name()))(
                narInfo ? narInfo->url : "", narInfo != 0)(narInfo ? narInfo->compression : "",
                                                           narInfo != 0)(
                narInfo && narInfo->fileHash ? narInfo->fileHash->to_string(hash_format_t::nix32, true)
                                             : "",
                narInfo && narInfo->fileHash)(narInfo ? narInfo->file_size : 0,
                                              narInfo != 0 && narInfo->file_size)(
                info->nar_hash.to_string(hash_format_t::nix32, true))(info->nar_size)(
                concat_strings_sep(" ", info->shortRefs()))(
                info->deriver ? std::string(info->deriver->to_string()) : "", (bool)info->deriver)(
                concat_strings_sep(" ", info->sigs))(render_content_address(info->ca))(time(0))
            .exec();

      } else {
        state->insert_missing_nar.use()(cache.id)(hash_part)(time(0)).exec();
      }
    });
  }

  void upsertRealisation(const std::string& uri, const realisation_t& realisation) override {
    retrySQLite<void>([&]() {
      auto state(_state.lock());

      auto& cache(get_cache(*state, uri));

      state->insert_realisation
          .use()(cache.id)(realisation.id.to_string())(
              static_cast<nlohmann::json>(realisation).dump())(time(0))
          .exec();
    });
  }

  virtual void upsertAbsentRealisation(const std::string& uri, const DrvOutput& id) override {
    retrySQLite<void>([&]() {
      auto state(_state.lock());

      auto& cache(get_cache(*state, uri));
      state->insert_missing_realisation.use()(cache.id)(id.to_string())(time(0)).exec();
    });
  }
};

ref<NarInfoDiskCache> get_nar_info_disk_cache() {
  static ref<NarInfoDiskCache> cache = make_ref<nar_info_disk_cache_impl_t>();
  return cache;
}

ref<NarInfoDiskCache> get_test_nar_info_disk_cache(Path db_path) {
  return make_ref<nar_info_disk_cache_impl_t>(db_path);
}

} // namespace nix
