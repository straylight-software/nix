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

bool DummyStore::PathInfoAndContents::operator==(const PathInfoAndContents& other) const {
  return info == other.info && contents->root == other.contents->root;
}

bool DummyStore::operator==(const DummyStore& other) const {
  return contents == other.contents && derivations == other.derivations &&
         buildTrace == other.buildTrace;
}

namespace {

class whole_store_view_accessor_t : public SourceAccessor {
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
  callWithAccessorForPath(canon_path_t path,
                          std::invocable<memory_source_accessor_t&, const canon_path_t&> auto callback) {
    if (path.isRoot())
      return callback(rootPathAccessor, path);

    BaseName baseName(*path.begin());
    memory_source_accessor_t* res = nullptr;

    subdirs.cvisit(baseName, [&](const auto& kv) {
      path = path.removePrefix(canon_path_t{baseName});
      res = &*kv.second;
    });

    if (!res)
      res = &emptyAccessor;

    return callback(*res, path);
  }

public:
  whole_store_view_accessor_t() {
    memory_sink_t sink{rootPathAccessor};
    sink.createDirectory(canon_path_t::root);
  }

  void addObject(std::string_view baseName, ref<memory_source_accessor_t> accessor) {
    subdirs.emplace(baseName, std::move(accessor));
  }

  std::string readFile(const canon_path_t& path) override {
    return callWithAccessorForPath(path, [](SourceAccessor& accessor, const canon_path_t& path) {
      return accessor.readFile(path);
    });
  }

  void readFile(const canon_path_t& path, Sink& sink,
                std::function<void(uint64_t)> sizeCallback) override {
    return callWithAccessorForPath(path, [&](SourceAccessor& accessor, const canon_path_t& path) {
      return accessor.readFile(path, sink, sizeCallback);
    });
  }

  bool pathExists(const canon_path_t& path) override {
    return callWithAccessorForPath(path, [](SourceAccessor& accessor, const canon_path_t& path) {
      return accessor.pathExists(path);
    });
  }

  std::optional<stat_t> maybeLstat(const canon_path_t& path) override {
    return callWithAccessorForPath(path, [](SourceAccessor& accessor, const canon_path_t& path) {
      return accessor.maybeLstat(path);
    });
  }

  dir_entries_t readDirectory(const canon_path_t& path) override {
    return callWithAccessorForPath(path, [](SourceAccessor& accessor, const canon_path_t& path) {
      return accessor.readDirectory(path);
    });
  }

  std::string readLink(const canon_path_t& path) override {
    return callWithAccessorForPath(path, [](SourceAccessor& accessor, const canon_path_t& path) {
      return accessor.readLink(path);
    });
  }
};

} // namespace

ref<Store> DummyStoreConfig::openStore() const {
  return openDummyStore();
}

struct dummy_store_impl_t : DummyStore {
  using Config = DummyStoreConfig;

  /**
   * This view conceptually just borrows the file systems objects of
   * each store object from `contents`, and combines them together
   * into one store-wide source accessor.
   *
   * This is needed just in order to implement `Store::getFSAccessor`.
   */
  ref<whole_store_view_accessor_t> wholeStoreView = make_ref<whole_store_view_accessor_t>();

  dummy_store_impl_t(ref<const Config> config) : Store{*config}, DummyStore{config} {
    wholeStoreView->setPathDisplay(config->storeDir);
  }

  void
  queryPathInfoUncached(const StorePath& path,
                        Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept override {
    if (path.isDerivation()) {
      if (auto accessor_ = getMemoryFSAccessor(path)) {
        ref<memory_source_accessor_t> accessor = ref{std::move(accessor_)};
        /* compute path info on demand */
        auto narHash = hashPath({accessor, canon_path_t::root}, file_serialisation_method_t::NixArchive,
                                hash_algorithm_t::SHA256);
        auto info =
            std::make_shared<ValidPathInfo>(path, UnkeyedValidPathInfo{*this, narHash.hash});
        info->narSize = narHash.numBytesDigested;
        info->ca = ContentAddress{
            .method = ContentAddressMethod::raw_t::Text,
            .hash = hashString(
                hash_algorithm_t::SHA256,
                std::get<memory_source_accessor_t::file_t::Regular>(accessor->root->raw).contents),
        };
        callback(std::move(info));
        return;
      }
    } else {
      if (contents.cvisit(path, [&](const auto& kv) {
            callback(std::make_shared<ValidPathInfo>(StorePath{kv.first}, kv.second.info));
          }))
        return;
    }

    callback(nullptr);
  }

  /**
   * Do this to avoid `queryPathInfoUncached` computing `PathInfo`
   * that we don't need just to return a `bool`.
   */
  bool isValidPathUncached(const StorePath& path) override {
    return path.isDerivation() ? derivations.contains(path) : Store::isValidPathUncached(path);
  }

  /**
   * The dummy store is incapable of *not* trusting! :)
   */
  std::optional<TrustedFlag> isTrustedClient() override { return Trusted; }

  std::optional<StorePath> queryPathFromHashPart(const std::string& hashPart) override {
    unsupported("queryPathFromHashPart");
  }

  void addToStore(const ValidPathInfo& info, Source& source, RepairFlag repair,
                  CheckSigsFlag checkSigs) override {
    if (config->readOnly)
      unsupported("addToStore");

    if (repair)
      throw Error("repairing is not supported for '%s' store", config->getHumanReadableURI());

    if (checkSigs)
      throw Error("checking signatures is not supported for '%s' store",
                  config->getHumanReadableURI());

    auto accessor = make_ref<memory_source_accessor_t>();
    memory_sink_t tempSink{*accessor};
    parseDump(tempSink, source);
    auto path = info.path;

    if (info.path.isDerivation()) {
      warn("back compat supporting `addToStore` for inserting derivations in dummy store");
      writeDerivation(parseDerivation(*this, accessor->readFile(canon_path_t::root),
                                      Derivation::nameFromPath(info.path)));
      return;
    }

    contents.insert({
        path,
        PathInfoAndContents{
            std::move(info),
            accessor,
        },
    });
    wholeStoreView->addObject(path.to_string(), accessor);
  }

  StorePath
  addToStoreFromDump(Source& source, std::string_view name,
                     file_serialisation_method_t dumpMethod = file_serialisation_method_t::NixArchive,
                     ContentAddressMethod hashMethod = file_ingestion_method_t::NixArchive,
                     hash_algorithm_t hashAlgo = hash_algorithm_t::SHA256,
                     const StorePathSet& references = StorePathSet(),
                     RepairFlag repair = NoRepair) override {
    if (isDerivation(name))
      throw Error("Do not insert derivation into dummy store with `addToStoreFromDump`");

    if (config->readOnly)
      unsupported("addToStoreFromDump");

    if (repair)
      throw Error("repairing is not supported for '%s' store", config->getHumanReadableURI());

    auto temp = make_ref<memory_source_accessor_t>();

    {
      memory_sink_t tempSink{*temp};

      // TODO factor this out into `restorePath`, same todo on it.
      switch (dumpMethod) {
        case file_serialisation_method_t::NixArchive:
          parseDump(tempSink, source);
          break;
        case file_serialisation_method_t::Flat: {
          // Replace root dir with file so next part succeeds.
          temp->root = memory_source_accessor_t::file_t::Regular{};
          tempSink.createRegularFile(canon_path_t::root, [&](auto& sink) { source.drainInto(sink); });
          break;
        }
      }
    }

    auto hash =
        hashPath({temp, canon_path_t::root}, hashMethod.getFileIngestionMethod(), hashAlgo).first;
    auto narHash =
        hashPath({temp, canon_path_t::root}, file_ingestion_method_t::NixArchive, hash_algorithm_t::SHA256);

    auto info =
        ValidPathInfo::makeFromCA(*this, name,
                                  ContentAddressWithReferences::fromParts(
                                      hashMethod, std::move(hash),
                                      {
                                          .others = references,
                                          // caller is not capable of creating a self-reference,
                                          // because this is content-addressed without modulus
                                          .self = false,
                                      }),
                                  std::move(narHash.first));

    info.narSize = narHash.second.value();

    auto path = info.path;
    auto accessor = make_ref<memory_source_accessor_t>(std::move(*temp));
    contents.insert({
        path,
        PathInfoAndContents{
            std::move(info),
            accessor,
        },
    });
    wholeStoreView->addObject(path.to_string(), accessor);

    return path;
  }

  StorePath writeDerivation(const Derivation& drv, RepairFlag repair = NoRepair) override {
    auto drvPath = ::nix::writeDerivation(*this, drv, repair, /*readonly=*/true);

    if (!derivations.contains(drvPath) || repair) {
      if (config->readOnly)
        unsupported("writeDerivation");
      derivations.insert({drvPath, drv});
    }

    return drvPath;
  }

  Derivation readDerivation(const StorePath& drvPath) override {
    if (std::optional res = getConcurrent(derivations, drvPath))
      return *res;
    else
      throw Error("derivation '%s' is not valid", printStorePath(drvPath));
  }

  /**
   * No such thing as an "invalid derivation" with the dummy store
   */
  Derivation readInvalidDerivation(const StorePath& drvPath) override {
    return readDerivation(drvPath);
  }

  void registerDrvOutput(const Realisation& output) override {
    buildTrace.insert_or_visit(
        {output.id.drvHash, {{output.id.outputName, output}}},
        [&](auto& kv) { kv.second.insert_or_assign(output.id.outputName, output); });
  }

  void queryRealisationUncached(
      const DrvOutput& drvOutput,
      Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept override {
    bool visited = false;
    buildTrace.cvisit(drvOutput.drvHash, [&](const auto& kv) {
      if (auto it = kv.second.find(drvOutput.outputName); it != kv.second.end()) {
        visited = true;
        callback(std::make_shared<UnkeyedRealisation>(it->second));
      }
    });

    if (!visited)
      callback(nullptr);
  }

  std::shared_ptr<memory_source_accessor_t> getMemoryFSAccessor(const StorePath& path,
                                                            bool requireValidPath = true) {
    std::shared_ptr<memory_source_accessor_t> res;
    if (path.isDerivation())
      derivations.cvisit(path, [&](const auto& kv) {
        /* compute path info on demand */
        auto res2 = make_ref<memory_source_accessor_t>();
        res2->root = memory_source_accessor_t::file_t::Regular{
            .contents = kv.second.unparse(*this, false),
        };
        res = std::move(res2).get_ptr();
      });
    else
      contents.cvisit(path, [&](const auto& kv) { res = kv.second.contents.get_ptr(); });
    return res;
  }

  std::shared_ptr<SourceAccessor> getFSAccessor(const StorePath& path,
                                                bool requireValidPath = true) override {
    return getMemoryFSAccessor(path, requireValidPath);
  }

  ref<SourceAccessor> getFSAccessor(bool requireValidPath) override { return wholeStoreView; }
};

ref<DummyStore> DummyStore::Config::openDummyStore() const {
  return make_ref<dummy_store_impl_t>(ref{shared_from_this()});
}

static RegisterStoreImplementation<DummyStore::Config> regDummyStore;

} // namespace nix

namespace nlohmann {

using namespace nix;

DummyStore::PathInfoAndContents
adl_serializer<DummyStore::PathInfoAndContents>::from_json(const json& json) {
  auto& obj = getObject(json);
  return DummyStore::PathInfoAndContents{
      .info = valueAt(obj, "info"),
      .contents = make_ref<memory_source_accessor_t>(valueAt(obj, "contents")),
  };
}

void adl_serializer<DummyStore::PathInfoAndContents>::to_json(
    json& json, const DummyStore::PathInfoAndContents& val) {
  json = {
      {"info", val.info},
      {"contents", *val.contents},
  };
}

ref<DummyStoreConfig> adl_serializer<ref<DummyStore::Config>>::from_json(const json& json) {
  auto& obj = getObject(json);
  auto cfg = make_ref<DummyStore::Config>(DummyStore::Config::Params{});
  const_cast<path_setting_t&>(cfg->storeDir_).set(getString(valueAt(obj, "store")));
  cfg->readOnly = true;
  return cfg;
}

void adl_serializer<DummyStoreConfig>::to_json(json& json, const DummyStoreConfig& val) {
  json = {
      {"store", val.storeDir},
  };
}

ref<DummyStore> adl_serializer<ref<DummyStore>>::from_json(const json& json) {
  auto& obj = getObject(json);
  ref<DummyStore> res =
      adl_serializer<ref<DummyStoreConfig>>::from_json(valueAt(obj, "config"))->openDummyStore();
  for (auto& [k, v] : getObject(valueAt(obj, "contents")))
    res->contents.insert({StorePath{k}, v});
  for (auto& [k, v] : getObject(valueAt(obj, "derivations")))
    res->derivations.insert({StorePath{k}, v});
  for (auto& [k0, v] : getObject(valueAt(obj, "buildTrace"))) {
    for (auto& [k1, v2] : getObject(v)) {
      UnkeyedRealisation realisation = v2;
      res->buildTrace.insert_or_visit(
          {
              Hash::parseExplicitFormatUnprefixed(k0, hash_algorithm_t::SHA256, hash_format_t::Base64),
              {{k1, realisation}},
          },
          [&](auto& kv) { kv.second.insert_or_assign(k1, realisation); });
    }
  }
  return res;
}

void adl_serializer<DummyStore>::to_json(json& json, const DummyStore& val) {
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
           auto& obj2 = obj[k.to_string(hash_format_t::Base64, false)] = json::object();
           for (auto& [k2, v2] : kv.second)
             obj2[k2] = v2;
         });
         return obj;
       }()},
  };
}

} // namespace nlohmann
