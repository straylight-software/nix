#include <boost/unordered/concurrent_flat_map.hpp>

#include "nix/store/dummy-store-impl.h"
#include "nix/store/realisation.h"
#include "nix/store/store-registration.h"
#include "nix/util/archive.h"
#include "nix/util/callback.h"
#include "nix/util/json-utils.h"
#include "nix/util/memory-source-accessor.h"

namespace nix {

std::string DummyStoreConfig::doc() {
  return
#include "dummy-store.md"
      ;
}

bool dummy_store::PathInfoAndContents::operator==(const PathInfoAndContents& other) const {
  return info == other.info && contents->root == other.contents->root;
}

bool dummy_store::operator==(const dummy_store& other) const {
  return contents == other.contents && derivations == other.derivations &&
         buildTrace == other.buildTrace;
}

namespace {

class whole_store_view_accessor_t : public source_accessor_t {
  using BaseName = std::string;

  /**
   * Map from store path basenames to corresponding accessors.
   */
  boost::concurrent_flat_map<BaseName, ref<memory_source_accessor_t>> subdirs;

  /**
   * Helper accessor for accessing just the canon_path_t::root.
   */
  memory_source_accessor_t rootPathAccessor;

  /**
   * Helper empty accessor.
   */
  memory_source_accessor_t emptyAccessor;

  auto
  call_with_accessor_for_path(canon_path_t path,
                          std::invocable<memory_source_accessor_t&, const canon_path_t&> auto callback) {
    if (path.is_root())
      return callback(rootPathAccessor, path);

    BaseName base_name(*path.begin());
    memory_source_accessor_t* res = nullptr;

    subdirs.cvisit(base_name, [&](const auto& kv) {
      path = path.remove_prefix(canon_path_t{base_name});
      res = &*kv.second;
    });

    if (!res)
      res = &emptyAccessor;

    return callback(*res, path);
  }

public:
  whole_store_view_accessor_t() {
    memory_sink_t sink{rootPathAccessor};
    sink.create_directory(canon_path_t::root);
  }

  void add_object(std::string_view base_name, ref<memory_source_accessor_t> accessor) {
    subdirs.emplace(base_name, std::move(accessor));
  }

  std::string read_file(const canon_path_t& path) override {
    return call_with_accessor_for_path(path, [](source_accessor_t& accessor, const canon_path_t& path) {
      return accessor.read_file(path);
    });
  }

  void read_file(const canon_path_t& path, sink_t& sink,
                std::function<void(uint64_t)> size_callback) override {
    return call_with_accessor_for_path(path, [&](source_accessor_t& accessor, const canon_path_t& path) {
      return accessor.read_file(path, sink, size_callback);
    });
  }

  bool path_exists(const canon_path_t& path) override {
    return call_with_accessor_for_path(path, [](source_accessor_t& accessor, const canon_path_t& path) {
      return accessor.path_exists(path);
    });
  }

  std::optional<stat_t> maybe_lstat(const canon_path_t& path) override {
    return call_with_accessor_for_path(path, [](source_accessor_t& accessor, const canon_path_t& path) {
      return accessor.maybe_lstat(path);
    });
  }

  dir_entries_t read_directory(const canon_path_t& path) override {
    return call_with_accessor_for_path(path, [](source_accessor_t& accessor, const canon_path_t& path) {
      return accessor.read_directory(path);
    });
  }

  std::string read_link(const canon_path_t& path) override {
    return call_with_accessor_for_path(path, [](source_accessor_t& accessor, const canon_path_t& path) {
      return accessor.read_link(path);
    });
  }
};

} // namespace

ref<store_t> DummyStoreConfig::open_store() const {
  return openDummyStore();
}

struct dummy_store_impl_t : dummy_store {
  using config_t = DummyStoreConfig;

  /**
   * This view conceptually just borrows the file systems objects of
   * each store object from `contents`, and combines them together
   * into one store-wide source accessor.
   *
   * This is needed just in order to implement `store_t::getFSAccessor`.
   */
  ref<whole_store_view_accessor_t> whole_store_view = make_ref<whole_store_view_accessor_t>();

  dummy_store_impl_t(ref<const config_t> config) : store_t{*config}, dummy_store{config} {
    whole_store_view->set_path_display(config->store_dir);
  }

  void
  query_path_info_uncached(const store_path_t& path,
                        Callback<std::shared_ptr<const valid_path_info_t>> callback) noexcept override {
    if (path.is_derivation()) {
      if (auto accessor_ = getMemoryFSAccessor(path)) {
        ref<memory_source_accessor_t> accessor = ref{std::move(accessor_)};
        /* compute path info on demand */
        auto nar_hash = hash_path({accessor, canon_path_t::root}, file_serialisation_method_t::nix_archive,
                                hash_algorithm_t::SHA256);
        auto info =
            std::make_shared<valid_path_info_t>(path, UnkeyedValidPathInfo{*this, nar_hash.hash});
        info->nar_size = nar_hash.num_bytes_digested;
        info->ca = content_address_t{
            .method = content_address_method_t::raw_t::Text,
            .hash = hash_string(
                hash_algorithm_t::SHA256,
                std::get<memory_source_accessor_t::file_t::regular>(accessor->root->raw).contents),
        };
        callback(std::move(info));
        return;
      }
    } else {
      if (contents.cvisit(path, [&](const auto& kv) {
            callback(std::make_shared<valid_path_info_t>(store_path_t{kv.first}, kv.second.info));
          }))
        return;
    }

    callback(nullptr);
  }

  /**
   * Do this to avoid `query_path_info_uncached` computing `path_info_t`
   * that we don't need just to return a `bool`.
   */
  bool isValidPathUncached(const store_path_t& path) override {
    return path.is_derivation() ? derivations.contains(path) : store_t::isValidPathUncached(path);
  }

  /**
   * The dummy store is incapable of *not* trusting! :)
   */
  std::optional<TrustedFlag> isTrustedClient() override { return Trusted; }

  std::optional<store_path_t> queryPathFromHashPart(const std::string& hash_part) override {
    unsupported("queryPathFromHashPart");
  }

  void add_to_store(const valid_path_info_t& info, source_t& source, RepairFlag repair,
                  CheckSigsFlag check_sigs) override {
    if (config->read_only)
      unsupported("addToStore");

    if (repair)
      throw Error("repairing is not supported for '%s' store", config->getHumanReadableURI());

    if (check_sigs)
      throw Error("checking signatures is not supported for '%s' store",
                  config->getHumanReadableURI());

    auto accessor = make_ref<memory_source_accessor_t>();
    memory_sink_t tempSink{*accessor};
    parse_dump(tempSink, source);
    auto path = info.path;

    if (info.path.is_derivation()) {
      warn("back compat supporting `addToStore` for inserting derivations in dummy store");
      write_derivation(parse_derivation(*this, accessor->read_file(canon_path_t::root),
                                      derivation_t::nameFromPath(info.path)));
      return;
    }

    contents.insert({
        path,
        PathInfoAndContents{
            std::move(info),
            accessor,
        },
    });
    whole_store_view->add_object(path.to_string(), accessor);
  }

  store_path_t
  add_to_store_from_dump(source_t& source, std::string_view name,
                     file_serialisation_method_t dump_method = file_serialisation_method_t::nix_archive,
                     content_address_method_t hash_method = file_ingestion_method_t::nix_archive,
                     hash_algorithm_t hash_algo = hash_algorithm_t::SHA256,
                     const store_path_set_t& references = store_path_set_t(),
                     RepairFlag repair = NoRepair) override {
    if (is_derivation(name))
      throw Error("Do not insert derivation into dummy store with `addToStoreFromDump`");

    if (config->read_only)
      unsupported("addToStoreFromDump");

    if (repair)
      throw Error("repairing is not supported for '%s' store", config->getHumanReadableURI());

    auto temp = make_ref<memory_source_accessor_t>();

    {
      memory_sink_t tempSink{*temp};

      // TODO factor this out into `restorePath`, same todo on it.
      switch (dump_method) {
        case file_serialisation_method_t::nix_archive:
          parse_dump(tempSink, source);
          break;
        case file_serialisation_method_t::flat: {
          // Replace root dir with file so next part succeeds.
          temp->root = memory_source_accessor_t::file_t::regular{};
          tempSink.create_regular_file(canon_path_t::root, [&](auto& sink) { source.drain_into(sink); });
          break;
        }
      }
    }

    auto hash =
        hash_path({temp, canon_path_t::root}, hash_method.getFileIngestionMethod(), hash_algo).first;
    auto nar_hash =
        hash_path({temp, canon_path_t::root}, file_ingestion_method_t::nix_archive, hash_algorithm_t::SHA256);

    auto info =
        valid_path_info_t::makeFromCA(*this, name,
                                  ContentAddressWithReferences::fromParts(
                                      hash_method, std::move(hash),
                                      {
                                          .others = references,
                                          // caller is not capable of creating a self-reference,
                                          // because this is content-addressed without modulus
                                          .self = false,
                                      }),
                                  std::move(nar_hash.first));

    info.nar_size = nar_hash.second.value();

    auto path = info.path;
    auto accessor = make_ref<memory_source_accessor_t>(std::move(*temp));
    contents.insert({
        path,
        PathInfoAndContents{
            std::move(info),
            accessor,
        },
    });
    whole_store_view->add_object(path.to_string(), accessor);

    return path;
  }

  store_path_t write_derivation(const derivation_t& drv, RepairFlag repair = NoRepair) override {
    auto drv_path = ::nix::write_derivation(*this, drv, repair, /*readonly=*/true);

    if (!derivations.contains(drv_path) || repair) {
      if (config->read_only)
        unsupported("writeDerivation");
      derivations.insert({drv_path, drv});
    }

    return drv_path;
  }

  derivation_t read_derivation(const store_path_t& drv_path) override {
    if (std::optional res = get_concurrent(derivations, drv_path))
      return *res;
    else
      throw Error("derivation '%s' is not valid", printStorePath(drv_path));
  }

  /**
   * No such thing as an "invalid derivation" with the dummy store
   */
  derivation_t readInvalidDerivation(const store_path_t& drv_path) override {
    return read_derivation(drv_path);
  }

  void register_drv_output(const realisation_t& output) override {
    buildTrace.insert_or_visit(
        {output.id.drvHash, {{output.id.output_name, output}}},
        [&](auto& kv) { kv.second.insert_or_assign(output.id.output_name, output); });
  }

  void query_realisation_uncached(
      const DrvOutput& drvOutput,
      Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept override {
    bool visited = false;
    buildTrace.cvisit(drvOutput.drvHash, [&](const auto& kv) {
      if (auto it = kv.second.find(drvOutput.output_name); it != kv.second.end()) {
        visited = true;
        callback(std::make_shared<UnkeyedRealisation>(it->second));
      }
    });

    if (!visited)
      callback(nullptr);
  }

  std::shared_ptr<memory_source_accessor_t> getMemoryFSAccessor(const store_path_t& path,
                                                            bool require_valid_path = true) {
    std::shared_ptr<memory_source_accessor_t> res;
    if (path.is_derivation())
      derivations.cvisit(path, [&](const auto& kv) {
        /* compute path info on demand */
        auto res2 = make_ref<memory_source_accessor_t>();
        res2->root = memory_source_accessor_t::file_t::regular{
            .contents = kv.second.unparse(*this, false),
        };
        res = std::move(res2).get_ptr();
      });
    else
      contents.cvisit(path, [&](const auto& kv) { res = kv.second.contents.get_ptr(); });
    return res;
  }

  std::shared_ptr<source_accessor_t> getFSAccessor(const store_path_t& path,
                                                bool require_valid_path = true) override {
    return getMemoryFSAccessor(path, require_valid_path);
  }

  ref<source_accessor_t> getFSAccessor(bool require_valid_path) override { return whole_store_view; }
};

ref<dummy_store> dummy_store::config_t::openDummyStore() const {
  return make_ref<dummy_store_impl_t>(ref{shared_from_this()});
}

static RegisterStoreImplementation<dummy_store::config_t> reg_dummy_store;

} // namespace nix

namespace nlohmann {

using namespace nix;

dummy_store::PathInfoAndContents
adl_serializer<dummy_store::PathInfoAndContents>::from_json(const json& json) {
  auto& obj = get_object(json);
  return dummy_store::PathInfoAndContents{
      .info = value_at(obj, "info"),
      .contents = make_ref<memory_source_accessor_t>(value_at(obj, "contents")),
  };
}

void adl_serializer<dummy_store::PathInfoAndContents>::to_json(
    json& json, const dummy_store::PathInfoAndContents& val) {
  json = {
      {"info", val.info},
      {"contents", *val.contents},
  };
}

ref<DummyStoreConfig> adl_serializer<ref<dummy_store::config_t>>::from_json(const json& json) {
  auto& obj = get_object(json);
  auto cfg = make_ref<dummy_store::config_t>(dummy_store::config_t::Params{});
  const_cast<path_setting_t&>(cfg->storeDir_).set(get_string(value_at(obj, "store")));
  cfg->read_only = true;
  return cfg;
}

void adl_serializer<DummyStoreConfig>::to_json(json& json, const DummyStoreConfig& val) {
  json = {
      {"store", val.store_dir},
  };
}

ref<dummy_store> adl_serializer<ref<dummy_store>>::from_json(const json& json) {
  auto& obj = get_object(json);
  ref<dummy_store> res =
      adl_serializer<ref<DummyStoreConfig>>::from_json(value_at(obj, "config"))->openDummyStore();
  for (auto& [k, v] : get_object(value_at(obj, "contents")))
    res->contents.insert({store_path_t{k}, v});
  for (auto& [k, v] : get_object(value_at(obj, "derivations")))
    res->derivations.insert({store_path_t{k}, v});
  for (auto& [k0, v] : get_object(value_at(obj, "buildTrace"))) {
    for (auto& [k1, v2] : get_object(v)) {
      UnkeyedRealisation realisation = v2;
      res->buildTrace.insert_or_visit(
          {
              Hash::parse_explicit_format_unprefixed(k0, hash_algorithm_t::SHA256, hash_format_t::base64),
              {{k1, realisation}},
          },
          [&](auto& kv) { kv.second.insert_or_assign(k1, realisation); });
    }
  }
  return res;
}

void adl_serializer<dummy_store>::to_json(json& json, const dummy_store& val) {
  json = {
      {"config", *val.config},
      {"contents",
       [&] {
         auto obj = json::object();
         val.contents.cvisit_all([&](const auto& kv) {
           auto& [k, v] = kv;
           obj[k.to_string()] = v;
         });
         return obj;
       }()},
      {"derivations",
       [&] {
         auto obj = json::object();
         val.derivations.cvisit_all([&](const auto& kv) {
           auto& [k, v] = kv;
           obj[k.to_string()] = v;
         });
         return obj;
       }()},
      {"buildTrace",
       [&] {
         auto obj = json::object();
         val.buildTrace.cvisit_all([&](const auto& kv) {
           auto& [k, v] = kv;
           auto& obj2 = obj[k.to_string(hash_format_t::base64, false)] = json::object();
           for (auto& [k2, v2] : kv.second)
             obj2[k2] = v2;
         });
         return obj;
       }()},
  };
}

} // namespace nlohmann
