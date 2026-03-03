#include "nix/store/store-api.h"

#include "nix/store/derivations.h"
#include "nix/store/derived-path.h"
#include "nix/store/globals.h"
#include "nix/store/nar-info-disk-cache.h"
#include "nix/store/realisation.h"
#include "nix/store/store-open.h"
#include "nix/util/archive.h"
#include "nix/util/callback.h"
#include "nix/util/git.h"
#include "nix/util/logging.h"
#include "nix/util/posix-source-accessor.h"
#include "nix/util/signature/local-keys.h"
#include "nix/util/source-accessor.h"
#include "nix/util/thread-pool.h"
#include "nix/util/util.h"
// FIXME this should not be here, see TODO below on
// `addMultipleToStore`.
#include <filesystem>

#include <nlohmann/json.hpp>

#include "nix/store/worker-protocol.h"
#include "nix/util/signals.h"
#include "nix/util/strings.h"

using json = nlohmann::json;

namespace nix {

Path StoreConfigBase::getDefaultNixStoreDir() {
  return settings.nixStore;
}

store_config_t::store_config_t(const Params& params)
    : StoreConfigBase(params), store_dir_config_t{storeDir_} {}

bool store_dir_config_t::isInStore(path_view_t path) const {
  return is_in_dir(path, store_dir);
}

std::pair<store_path_t, Path> store_dir_config_t::toStorePath(path_view_t path) const {
  if (!isInStore(path)) {
    throw Error("path '%1%' is not in the Nix store", path);
  }
  auto slash = path.find('/', store_dir.size() + 1);
  if (slash == Path::npos) {
    return {parseStorePath(path), ""};
  } else {
    return {parseStorePath(path.substr(0, slash)), (Path)path.substr(slash)};
  }
}

Path store_t::followLinksToStore(std::string_view _path) const {
  Path path = abs_path(std::string(_path));

  // Limit symlink follows to prevent infinite loops
  unsigned int follow_count = 0;
  const unsigned int max_follow = 1024;

  while (!isInStore(path)) {
    if (!std::filesystem::is_symlink(path)) {
      break;
    }

    if (++follow_count >= max_follow) {
      throw Error("too many symbolic links encountered while resolving '%s'", _path);
    }

    auto target = read_link(path);
    path = abs_path(target, dir_of(path));
  }

  if (!isInStore(path)) {
    throw BadStorePath("path '%1%' is not in the Nix store", path);
  }
  return path;
}

store_path_t store_t::followLinksToStorePath(std::string_view path) const {
  return toStorePath(followLinksToStore(path)).first;
}

store_path_t store_t::add_to_store(std::string_view name, const source_path_t& path,
                                   content_address_method_t method, hash_algorithm_t hash_algo,
                                   const store_path_set_t& references, path_filter_t& filter,
                                   RepairFlag repair) {
  file_serialisation_method_t fsm;
  switch (method.getFileIngestionMethod()) {
    case file_ingestion_method_t::flat:
      fsm = file_serialisation_method_t::flat;
      break;
    case file_ingestion_method_t::nix_archive:
      fsm = file_serialisation_method_t::nix_archive;
      break;
    case file_ingestion_method_t::git:
      // Use NAR; Git is not a serialization method
      fsm = file_serialisation_method_t::nix_archive;
      break;
  }
  std::optional<store_path_t> store_path;
  auto sink = source_to_sink([&](source_t& source) {
    length_source_t lengthSource(source);
    store_path =
        add_to_store_from_dump(lengthSource, name, fsm, method, hash_algo, references, repair);
    if (settings.warnLargePathThreshold &&
        lengthSource.total() >= settings.warnLargePathThreshold) {
      static bool failOnLargePath = get_env("_NIX_TEST_FAIL_ON_LARGE_PATH").value_or("") == "1";
      if (failOnLargePath) {
        throw Error("doesn't copy large path '%s' to the store (%d)", path,
                    render_size(lengthSource.total()));
      }
      warn("copied large path '%s' to the store (%d)", path, render_size(lengthSource.total()));
    }
  });
  dump_path(path, *sink, fsm, filter);
  sink->finish();
  return store_path.value();
}

void store_t::addMultipleToStore(PathsSource&& paths_to_copy, activity_t& act, RepairFlag repair,
                                 CheckSigsFlag check_sigs) {
  std::atomic<size_t> nrDone{0};
  std::atomic<size_t> nrFailed{0};
  std::atomic<uint64_t> nrRunning{0};

  using PathWithInfo = std::pair<valid_path_info_t, std::unique_ptr<source_t>>;

  uint64_t bytesExpected = 0;

  std::map<store_path_t, PathWithInfo*> infosMap;
  store_path_set_t storePathsToAdd;
  for (auto& thingToAdd : paths_to_copy) {
    bytesExpected += thingToAdd.first.nar_size;
    infosMap.insert_or_assign(thingToAdd.first.path, &thingToAdd);
    storePathsToAdd.insert(thingToAdd.first.path);
  }

  act.set_expected(act_copy_path, bytesExpected);

  auto showProgress = [&, nrTotal = paths_to_copy.size()]() {
    act.progress(nrDone, nrTotal, nrRunning, nrFailed);
  };

  process_graph<store_path_t>(
      storePathsToAdd,

      [&](const store_path_t& path) {
        auto& [info, _] = *infosMap.at(path);

        if (isValidPath(info.path)) {
          nrDone++;
          showProgress();
          return store_path_set_t();
        }

        return info.references;
      },

      [&](const store_path_t& path) {
        check_interrupt();

        auto& [info_, source_] = *infosMap.at(path);
        auto info = info_;
        info.ultimate = false;

        /* Make sure that the source_t object is destroyed when
           we're done. In particular, a sink_to_source_t object must
           be destroyed to ensure that the destructors on its
           stack frame are run; this includes
           LegacySSHStore::nar_from_path()'s connection lock. */
        auto source = std::move(source_);

        if (!isValidPath(info.path)) {
          maintain_count_t<decltype(nrRunning)> mc(nrRunning);
          showProgress();
          try {
            add_to_store(info, *source, repair, check_sigs);
          } catch (Error& e) {
            nrFailed++;
            if (!settings.keep_going) {
              throw e;
            }
            printMsg(lvl_error, "could not copy %s: %s", printStorePath(path), e.what());
            showProgress();
            return;
          }
        }

        nrDone++;
        showProgress();
      });
}

void store_t::addMultipleToStore(source_t& source, RepairFlag repair, CheckSigsFlag check_sigs) {
  auto expected = read_num<uint64_t>(source);
  for (uint64_t i = 0; i < expected; ++i) {
    // FIXME we should not be using the worker protocol here, let
    // alone the worker protocol with a hard-coded version!
    auto info = WorkerProto::Serialise<valid_path_info_t>::read(*this, WorkerProto::ReadConn{
                                                                           .from = source,
                                                                           .version = 16,
                                                                       });
    info.ultimate = false;
    add_to_store(info, source, repair, check_sigs);
  }
}

/*
The aim of this function is to compute in one pass the correct valid_path_info_t for
the files that we are trying to add to the store. To accomplish that in one
pass, given the different kind of inputs that we can take (normal nar archives,
nar archives with non SHA-256 hashes, and flat files), we set up a net of sinks
and aliases. Also, since the dataflow is obfuscated by this, we include here a
graphviz diagram:

digraph graphname {
    node [shape=box]
    fileSource -> narSink
    narSink [style=dashed]
    narSink -> unusualHashTee [style = dashed, label = "Recursive && !SHA-256"]
    narSink -> narHashSink [style = dashed, label = "else"]
    unusualHashTee -> narHashSink
    unusualHashTee -> caHashSink
    fileSource -> parse_sink
    parse_sink [style=dashed]
    parse_sink-> fileSink [style = dashed, label = "Flat"]
    parse_sink -> blank [style = dashed, label = "Recursive"]
    fileSink -> caHashSink
}
*/
valid_path_info_t store_t::addToStoreSlow(std::string_view name, const source_path_t& src_path,
                                          content_address_method_t method,
                                          hash_algorithm_t hash_algo,
                                          const store_path_set_t& references,
                                          std::optional<Hash> expectedCAHash) {
  hash_sink_t narHashSink{hash_algorithm_t::SHA256};
  hash_sink_t caHashSink{hash_algo};

  /* Note that fileSink and unusualHashTee must be mutually exclusive, since
     they both write to caHashSink. Note that that requisite is currently true
     because the former is only used in the flat case. */
  regular_file_sink_t fileSink{caHashSink};
  tee_sink_t unusualHashTee{narHashSink, caHashSink};

  auto& narSink = method == content_address_method_t::raw_t::nix_archive &&
                          hash_algo != hash_algorithm_t::SHA256
                      ? static_cast<sink_t&>(unusualHashTee)
                      : narHashSink;

  /* Functionally, this means that fileSource will yield the content of
     src_path. The fact that we use scratchpadSink as a temporary buffer here
     is an implementation detail. */
  auto fileSource =
      sink_to_source([&](sink_t& scratchpadSink) { src_path.dump_path(scratchpadSink); });

  /* tapped provides the same data as fileSource, but we also write all the
     information to narSink. */
  tee_source_t tapped{*fileSource, narSink};

  null_file_system_object_sink_t blank;
  auto& parse_sink =
      method.getFileIngestionMethod() == file_ingestion_method_t::flat
          ? (file_system_object_sink_t&)fileSink
          : (file_system_object_sink_t&)blank; // for recursive or git we do recursive

  /* The information that flows from tapped (besides being replicated in
     narSink), is now put in parse_sink. */
  parse_dump(parse_sink, tapped);

  /* We extract the result of the computation from the sink by calling
     finish. */
  auto [nar_hash, nar_size] = narHashSink.finish();

  auto hash = method == content_address_method_t::raw_t::nix_archive &&
                      hash_algo == hash_algorithm_t::SHA256
                  ? nar_hash
              : method == content_address_method_t::raw_t::git
                  ? git::dump_hash(hash_algo, src_path).hash
                  : caHashSink.finish().hash;

  if (expectedCAHash && expectedCAHash != hash) {
    throw Error("hash mismatch for '%s'", src_path);
  }

  auto info = valid_path_info_t::makeFromCA(
      *this, name,
      ContentAddressWithReferences::fromParts(method, hash,
                                              {
                                                  .others = references,
                                                  .self = false,
                                              }),
      nar_hash);
  info.nar_size = nar_size;

  if (!isValidPath(info.path)) {
    auto source =
        sink_to_source([&](sink_t& scratchpadSink) { src_path.dump_path(scratchpadSink); });
    add_to_store(info, *source);
  }

  return info;
}

void store_t::nar_from_path(const store_path_t& path, sink_t& sink) {
  auto accessor = requireStoreObjectAccessor(path);
  source_path_t source_path{accessor};
  dump_path(source_path, sink, file_serialisation_method_t::nix_archive);
}

string_set_t store_t::config_t::getDefaultSystemFeatures() {
  auto res = settings.systemFeatures.get();

  if (experimental_feature_settings.is_enabled(xp_t::ca_derivations)) {
    res.insert("ca-derivations");
  }

  if (experimental_feature_settings.is_enabled(xp_t::recursive_nix)) {
    res.insert("recursive-nix");
  }

  return res;
}

store_t::store_t(const store_t::config_t& config)
    : store_dir_config_t{config},
      config{config},
      pathInfoCache(make_ref<decltype(pathInfoCache)::element_type>(
          straylight::nix::data::LRUCache<store_path_t, PathInfoCacheValue>(
              static_cast<std::size_t>(config.pathInfoCacheSize)))) {
  assert_lib_store_initialized();
}

StoreReference store_config_t::getReference() const {
  return {.variant = StoreReference::Auto{}};
}

bool store_t::PathInfoCacheValue::isKnownNow() {
  std::chrono::duration ttl = didExist() ? std::chrono::seconds(settings.ttlPositiveNarInfoCache)
                                         : std::chrono::seconds(settings.ttlNegativeNarInfoCache);

  return std::chrono::steady_clock::now() < time_point + ttl;
}

void store_t::invalidatePathInfoCacheFor(const store_path_t& path) {
  pathInfoCache->lock()->erase(path);
}

std::map<std::string, std::optional<store_path_t>>
store_t::queryStaticPartialDerivationOutputMap(const store_path_t& path) {
  std::map<std::string, std::optional<store_path_t>> outputs;
  auto drv = readInvalidDerivation(path);
  for (auto& [output_name, output] : drv.outputsAndOptPaths(*this)) {
    outputs.emplace(output_name, output.second);
  }
  return outputs;
}

std::map<std::string, std::optional<store_path_t>>
store_t::queryPartialDerivationOutputMap(const store_path_t& path, store_t* eval_store_) {
  auto& eval_store = eval_store_ ? *eval_store_ : *this;

  auto outputs = eval_store.queryStaticPartialDerivationOutputMap(path);

  if (!experimental_feature_settings.is_enabled(xp_t::ca_derivations)) {
    return outputs;
  }

  auto drv = eval_store.readInvalidDerivation(path);
  auto drv_hashes = static_output_hashes(*this, drv);
  for (auto& [output_name, hash] : drv_hashes) {
    auto realisation = query_realisation(DrvOutput{hash, output_name});
    if (realisation) {
      outputs.insert_or_assign(output_name, realisation->out_path);
    } else {
      // queryStaticPartialDerivationOutputMap is not guaranteed
      // to return std::nullopt for outputs which are not
      // statically known.
      outputs.insert({output_name, std::nullopt});
    }
  }

  return outputs;
}

OutputPathMap store_t::queryDerivationOutputMap(const store_path_t& path, store_t* eval_store) {
  auto resp = queryPartialDerivationOutputMap(path, eval_store);
  OutputPathMap result;
  for (auto& [outName, optOutPath] : resp) {
    if (!optOutPath) {
      throw MissingRealisation(printStorePath(path), outName);
    }
    result.insert_or_assign(outName, *optOutPath);
  }
  return result;
}

store_path_set_t store_t::queryDerivationOutputs(const store_path_t& path) {
  auto output_map = this->queryDerivationOutputMap(path);
  store_path_set_t output_paths;
  for (auto& i : output_map) {
    output_paths.emplace(std::move(i.second));
  }
  return output_paths;
}

void store_t::querySubstitutablePathInfos(const StorePathCAMap& paths,
                                          SubstitutablePathInfos& infos) {
  if (!settings.use_substitutes) {
    return;
  }

  for (auto& path : paths) {
    std::optional<Error> lastStoresException = std::nullopt;
    for (auto& sub : get_default_substituters()) {
      if (lastStoresException.has_value()) {
        logError(lastStoresException->info());
        lastStoresException.reset();
      }

      auto subPath(path.first);

      // Recompute store path so that we can use a different store root.
      if (path.second) {
        subPath = makeFixedOutputPathFromCA(
            path.first.name(), ContentAddressWithReferences::withoutRefs(*path.second));
        if (sub->store_dir == store_dir) {
          assert(subPath == path.first);
        }
        if (subPath != path.first) {
          debug("replaced path '%s' with '%s' for substituter '%s'", printStorePath(path.first),
                sub->printStorePath(subPath), sub->config.getHumanReadableURI());
        }
      } else if (sub->store_dir != store_dir) {
        continue;
      }

      debug("checking substituter '%s' for path '%s'", sub->config.getHumanReadableURI(),
            sub->printStorePath(subPath));
      try {
        auto info = sub->queryPathInfo(subPath);

        if (sub->store_dir != store_dir &&
            !(info->isContentAddressed(*sub) && info->references.empty())) {
          continue;
        }

        auto narInfo = std::dynamic_pointer_cast<const nar_info_t>(
            std::shared_ptr<const valid_path_info_t>(info));
        infos.insert_or_assign(path.first, SubstitutablePathInfo{
                                               .deriver = info->deriver,
                                               .references = info->references,
                                               .downloadSize = narInfo ? narInfo->file_size : 0,
                                               .nar_size = info->nar_size,
                                           });

        break; /* We are done. */
      } catch (InvalidPath&) {
      } catch (SubstituterDisabled&) {
      } catch (Error& e) {
        lastStoresException = std::make_optional(std::move(e));
      }
    }
    if (lastStoresException.has_value()) {
      if (!settings.try_fallback) {
        throw *lastStoresException;
      } else {
        logError(lastStoresException->info());
      }
    }
  }
}

bool store_t::isValidPath(const store_path_t& store_path) {
  auto res = pathInfoCache->lock()->get(store_path);
  if (res && res->isKnownNow()) {
    stats.narInfoReadAverted++;
    return res->didExist();
  }

  if (diskCache) {
    auto res = diskCache->lookupNarInfo(config.getReference().render(/*FIXME withParams=*/false),
                                        std::string(store_path.hash_part()));
    if (res.first != NarInfoDiskCache::oUnknown) {
      stats.narInfoReadAverted++;
      pathInfoCache->lock()->put(store_path, res.first == NarInfoDiskCache::oInvalid
                                                 ? PathInfoCacheValue{}
                                                 : PathInfoCacheValue{.value = res.second});
      return res.first == NarInfoDiskCache::oValid;
    }
  }

  bool valid = isValidPathUncached(store_path);

  if (diskCache && !valid) {
    // FIXME: handle valid = true case.
    diskCache->upsertNarInfo(config.getReference().render(/*FIXME withParams=*/false),
                             std::string(store_path.hash_part()), 0);
  }

  return valid;
}

/* Default implementation for stores that only implement
   query_path_info_uncached(). */
bool store_t::isValidPathUncached(const store_path_t& path) {
  try {
    queryPathInfo(path);
    return true;
  } catch (InvalidPath&) {
    return false;
  }
}

ref<const valid_path_info_t> store_t::queryPathInfo(const store_path_t& store_path) {
  std::promise<ref<const valid_path_info_t>> promise;

  queryPathInfo(store_path, {[&](std::future<ref<const valid_path_info_t>> result) {
                  try {
                    promise.set_value(result.get());
                  } catch (...) {
                    promise.set_exception(std::current_exception());
                  }
                }});

  return promise.get_future().get();
}

std::shared_ptr<const valid_path_info_t>
store_t::maybeQueryPathInfo(const store_path_t& store_path) {
  std::promise<std::shared_ptr<const valid_path_info_t>> promise;

  queryPathInfo(store_path, {[&](std::future<ref<const valid_path_info_t>> result) {
                  try {
                    promise.set_value(result.get());
                  } catch (InvalidPath&) {
                    promise.set_value(nullptr);
                  } catch (...) {
                    promise.set_exception(std::current_exception());
                  }
                }});

  return promise.get_future().get();
}

static bool good_store_path(const store_path_t& expected, const store_path_t& actual) {
  return expected.hash_part() == actual.hash_part() &&
         (expected.name() == store_t::MissingName || expected.name() == actual.name());
}

std::optional<std::shared_ptr<const valid_path_info_t>>
store_t::queryPathInfoFromClientCache(const store_path_t& store_path) {
  auto hash_part = std::string(store_path.hash_part());

  auto res = pathInfoCache->lock()->get(store_path);
  if (res && res->isKnownNow()) {
    stats.narInfoReadAverted++;
    if (res->didExist()) {
      return std::make_optional(res->value);
    } else {
      return std::make_optional(nullptr);
    }
  }

  if (diskCache) {
    auto res = diskCache->lookupNarInfo(config.getReference().render(/*FIXME withParams=*/false),
                                        hash_part);
    if (res.first != NarInfoDiskCache::oUnknown) {
      stats.narInfoReadAverted++;
      pathInfoCache->lock()->put(store_path, res.first == NarInfoDiskCache::oInvalid
                                                 ? PathInfoCacheValue{}
                                                 : PathInfoCacheValue{.value = res.second});
      if (res.first == NarInfoDiskCache::oInvalid ||
          !good_store_path(store_path, res.second->path)) {
        return std::make_optional(nullptr);
      }
      assert(res.second);
      return std::make_optional(res.second);
    }
  }

  return std::nullopt;
}

void store_t::queryPathInfo(const store_path_t& store_path,
                            Callback<ref<const valid_path_info_t>> callback) noexcept {
  auto hash_part = std::string(store_path.hash_part());

  try {
    auto r = queryPathInfoFromClientCache(store_path);
    if (r.has_value()) {
      std::shared_ptr<const valid_path_info_t>& info = *r;
      if (info) {
        return callback(ref(info));
      } else {
        throw InvalidPath("path '%s' is not valid", printStorePath(store_path));
      }
    }
  } catch (...) {
    return callback.rethrow();
  }

  auto callbackPtr = std::make_shared<decltype(callback)>(std::move(callback));

  query_path_info_uncached(
      store_path, {[this, store_path, hash_part,
                    callbackPtr](std::future<std::shared_ptr<const valid_path_info_t>> fut) {
        try {
          auto info = fut.get();

          if (diskCache) {
            diskCache->upsertNarInfo(config.getReference().render(/*FIXME withParams=*/false),
                                     hash_part, info);
          }

          pathInfoCache->lock()->put(store_path, PathInfoCacheValue{.value = info});

          if (!info || !good_store_path(store_path, info->path)) {
            stats.narInfoMissing++;
            throw InvalidPath("path '%s' is not valid", printStorePath(store_path));
          }

          (*callbackPtr)(ref<const valid_path_info_t>(info));
        } catch (...) {
          callbackPtr->rethrow();
        }
      }});
}

void store_t::query_realisation(
    const DrvOutput& id, Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept {
  try {
    if (diskCache) {
      auto [cacheOutcome, maybeCachedRealisation] = diskCache->lookupRealisation(
          config.getReference().render(/*FIXME: withParams=*/false), id);
      switch (cacheOutcome) {
        case NarInfoDiskCache::oValid:
          debug("Returning a cached realisation for %s", id.to_string());
          callback(maybeCachedRealisation);
          return;
        case NarInfoDiskCache::oInvalid:
          debug("Returning a cached missing realisation for %s", id.to_string());
          callback(nullptr);
          return;
        case NarInfoDiskCache::oUnknown:
          break;
      }
    }
  } catch (...) {
    return callback.rethrow();
  }

  auto callbackPtr = std::make_shared<decltype(callback)>(std::move(callback));

  query_realisation_uncached(
      id, {[this, id, callbackPtr](std::future<std::shared_ptr<const UnkeyedRealisation>> fut) {
        try {
          auto info = fut.get();

          if (diskCache) {
            if (info) {
              diskCache->upsertRealisation(config.getReference().render(/*FIXME withParams=*/false),
                                           {*info, id});
            } else {
              diskCache->upsertAbsentRealisation(
                  config.getReference().render(/*FIXME withParams=*/false), id);
            }
          }

          (*callbackPtr)(std::shared_ptr<const UnkeyedRealisation>(info));

        } catch (...) {
          callbackPtr->rethrow();
        }
      }});
}

std::shared_ptr<const UnkeyedRealisation> store_t::query_realisation(const DrvOutput& id) {
  using RealPtr = std::shared_ptr<const UnkeyedRealisation>;
  std::promise<RealPtr> promise;

  query_realisation(id, {[&](std::future<RealPtr> result) {
                      try {
                        promise.set_value(result.get());
                      } catch (...) {
                        promise.set_exception(std::current_exception());
                      }
                    }});

  return promise.get_future().get();
}

void store_t::substitutePaths(const store_path_set_t& paths) {
  std::vector<derived_path_t> paths2;
  for (auto& path : paths) {
    if (!path.is_derivation()) {
      paths2.emplace_back(derived_path_t::opaque_t{path});
    }
  }
  auto missing = query_missing(paths2);

  if (!missing.willSubstitute.empty()) {
    try {
      std::vector<derived_path_t> subs;
      for (auto& p : missing.willSubstitute) {
        subs.emplace_back(derived_path_t::opaque_t{p});
      }
      build_paths(subs);
    } catch (Error& e) {
      logWarning(e.info());
    }
  }
}

store_path_set_t store_t::queryValidPaths(const store_path_set_t& paths,
                                          SubstituteFlag maybeSubstitute) {
  struct State {
    size_t left;
    store_path_set_t valid;
    std::exception_ptr exc;
  };

  sync_t<State> state_(State{paths.size(), store_path_set_t()});

  std::condition_variable wakeup;
  thread_pool_t pool;

  auto doQuery = [&](const store_path_t& path) {
    check_interrupt();
    queryPathInfo(path, {[path, &state_, &wakeup](std::future<ref<const valid_path_info_t>> fut) {
                    bool exists = false;
                    std::exception_ptr newExc{};

                    try {
                      auto info = fut.get();
                      exists = true;
                    } catch (InvalidPath&) {
                    } catch (...) {
                      newExc = std::current_exception();
                    }

                    auto state(state_.lock());

                    if (exists) {
                      state->valid.insert(path);
                    }

                    if (newExc) {
                      state->exc = newExc;
                    }

                    assert(state->left);
                    if (!--state->left) {
                      wakeup.notify_one();
                    }
                  }});
  };

  for (auto& path : paths) {
    pool.enqueue(std::bind(doQuery, path));
  }

  pool.process();

  while (true) {
    auto state(state_.lock());
    if (!state->left) {
      if (state->exc) {
        std::rethrow_exception(state->exc);
      }
      return std::move(state->valid);
    }
    state.wait(wakeup);
  }
}

/* Return a string accepted by decode_valid_path_info() that
   registers the specified paths as valid.  Note: it's the
   responsibility of the caller to provide a closure. */
std::string store_t::makeValidityRegistration(const store_path_set_t& paths, bool showDerivers,
                                              bool showHash) {
  std::string s = "";

  for (auto& i : paths) {
    s += printStorePath(i) + "\n";

    auto info = queryPathInfo(i);

    if (showHash) {
      s += info->nar_hash.to_string(hash_format_t::base16, false) + "\n";
      s += fmt("%1%\n", info->nar_size);
    }

    auto deriver = showDerivers && info->deriver ? printStorePath(*info->deriver) : "";
    s += deriver + "\n";

    s += fmt("%1%\n", info->references.size());

    for (auto& j : info->references) {
      s += printStorePath(j) + "\n";
    }
  }

  return s;
}

store_path_set_t store_t::exportReferences(const store_path_set_t& store_paths,
                                           const store_path_set_t& inputPaths) {
  store_path_set_t paths;

  for (auto& store_path : store_paths) {
    if (!inputPaths.count(store_path)) {
      throw build_error_t(build_result_t::Failure::InputRejected,
                          "cannot export references of path '%s' because it is not in the input "
                          "closure of the derivation",
                          printStorePath(store_path));
    }

    computeFSClosure({store_path}, paths);
  }

  /* If there are derivations in the graph, then include their
     outputs as well.  This is useful if you want to do things
     like passing all build-time dependencies of some path to a
     derivation that builds a NixOS DVD image. */
  auto paths2 = paths;

  for (auto& j : paths2) {
    if (j.is_derivation()) {
      derivation_t drv = derivationFromPath(j);
      for (auto& k : drv.outputsAndOptPaths(*this)) {
        if (!k.second.second) {
          /* FIXME: I am confused why we are calling
             `computeFSClosure` on the output path, rather than
             derivation itself. That doesn't seem right to me, so I
             won't try to implemented this for CA derivations. */
          throw UnimplementedError("exportReferences on CA derivations is not yet implemented");
        }
        computeFSClosure(*k.second.second, paths);
      }
    }
  }

  return paths;
}

const store_t::Stats& store_t::get_stats() {
  stats.pathInfoCacheSize = pathInfoCache->read_lock()->size();
  return stats;
}

static std::string make_copy_path_message(const store_config_t& src_cfg,
                                          const store_config_t& dst_cfg,
                                          std::string_view store_path) {
  auto src = src_cfg.getReference();
  auto dst = dst_cfg.getReference();

  auto is_shorthand = [](const StoreReference& ref) {
    /* At this point StoreReference **must** be resolved. */
    const auto& specified = std::visit(
        overloaded{
            [](const StoreReference::Auto&) -> const StoreReference::Specified& { unreachable(); },
            [](const StoreReference::Specified& specified) -> const StoreReference::Specified& {
              return specified;
            }},
        ref.variant);
    const auto& scheme = specified.scheme;
    return (scheme == "local" || scheme == "unix") && specified.authority.empty();
  };

  if (is_shorthand(src)) {
    return fmt("copying path '%s' to '%s'", store_path, dst_cfg.getHumanReadableURI());
  }

  if (is_shorthand(dst)) {
    return fmt("copying path '%s' from '%s'", store_path, src_cfg.getHumanReadableURI());
  }

  return fmt("copying path '%s' from '%s' to '%s'", store_path, src_cfg.getHumanReadableURI(),
             dst_cfg.getHumanReadableURI());
}

void copy_store_path(store_t& src_store, store_t& dst_store, const store_path_t& store_path,
                     RepairFlag repair, CheckSigsFlag check_sigs) {
  /* Bail out early (before starting a download from src_store) if
     dst_store already has this path. */
  if (!repair && dst_store.isValidPath(store_path)) {
    return;
  }

  const auto& src_cfg = src_store.config;
  const auto& dst_cfg = dst_store.config;
  auto store_path_s = src_store.printStorePath(store_path);
  activity_t act(*logger, lvl_info, act_copy_path,
                 make_copy_path_message(src_cfg, dst_cfg, store_path_s),
                 logger_t::fields_t{logger_t::field_t{store_path_s},
                                    logger_t::field_t{src_cfg.getHumanReadableURI()},
                                    logger_t::field_t{dst_cfg.getHumanReadableURI()}});
  push_activity_t pact(act.id_);

  auto info = src_store.queryPathInfo(store_path);

  uint64_t total = 0;

  // recompute store path on the chance dstStore does it differently
  if (info->ca && info->references.empty()) {
    auto info2 = make_ref<valid_path_info_t>(*info);
    info2->path = dst_store.makeFixedOutputPathFromCA(info->path.name(),
                                                      info->contentAddressWithReferences().value());
    if (dst_store.store_dir == src_store.store_dir) {
      assert(info->path == info2->path);
    }
    info = info2;
  }

  if (info->ultimate) {
    auto info2 = make_ref<valid_path_info_t>(*info);
    info2->ultimate = false;
    info = info2;
  }

  auto source = sink_to_source(
      [&](sink_t& sink) {
        lambda_sink_t progress_sink([&](std::string_view data) {
          total += data.size();
          act.progress(total, info->nar_size);
        });
        tee_sink_t tee{sink, progress_sink};
        src_store.nar_from_path(store_path, tee);
      },
      [&]() {
        throw EndOfFile("NAR for '%s' fetched from '%s' is incomplete",
                        src_store.printStorePath(store_path),
                        src_store.config.getHumanReadableURI());
      });

  dst_store.add_to_store(*info, *source, repair, check_sigs);
}

std::map<store_path_t, store_path_t> copy_paths(store_t& src_store, store_t& dst_store,
                                                const RealisedPath::Set& paths, RepairFlag repair,
                                                CheckSigsFlag check_sigs,
                                                SubstituteFlag substitute) {
  store_path_set_t store_paths;
  std::set<realisation_t> toplevelRealisations;
  for (auto& path : paths) {
    store_paths.insert(path.path());
    if (auto* realisation = std::get_if<realisation_t>(&path.raw)) {
      experimental_feature_settings.require(xp_t::ca_derivations);
      toplevelRealisations.insert(*realisation);
    }
  }

  auto paths_map = copy_paths(src_store, dst_store, store_paths, repair, check_sigs, substitute);

  try {
    // Copy the realisation closure
    process_graph<realisation_t>(
        realisation_t::closure(src_store, toplevelRealisations),
        [&](const realisation_t& current) -> std::set<realisation_t> {
          std::set<realisation_t> children;
          for (const auto& [drvOutput, _] : current.dependentRealisations) {
            auto currentChild = src_store.query_realisation(drvOutput);
            if (!currentChild) {
              throw Error("incomplete realisation closure: '%s' is a "
                          "dependency of '%s' but isn't registered",
                          drvOutput.to_string(), current.id.to_string());
            }
            children.insert({*currentChild, drvOutput});
          }
          return children;
        },
        [&](const realisation_t& current) -> void {
          dst_store.register_drv_output(current, check_sigs);
        });
  } catch (missing_experimental_feature_t& e) {
    // Don't fail if the remote doesn't support CA derivations is it might
    // not be within our control to change that, and we might still want
    // to at least copy the output paths.
    if (e.missing_feature == xp_t::ca_derivations) {
      ignore_exception_except_interrupt();
    } else {
      throw;
    }
  }

  return paths_map;
}

std::map<store_path_t, store_path_t> copy_paths(store_t& src_store, store_t& dst_store,
                                                const store_path_set_t& store_paths,
                                                RepairFlag repair, CheckSigsFlag check_sigs,
                                                SubstituteFlag substitute) {
  auto valid = dst_store.queryValidPaths(store_paths, substitute);

  store_path_set_t missing;
  for (auto& path : store_paths) {
    if (!valid.count(path)) {
      missing.insert(path);
    }
  }

  activity_t act(*logger, lvl_info, act_copy_paths, fmt("copying %d paths", missing.size()));

  // In the general case, `addMultipleToStore` requires a sorted list of
  // store paths to add, so sort them right now
  auto sorted_missing = src_store.topoSortPaths(missing);
  std::reverse(sorted_missing.begin(), sorted_missing.end());

  std::map<store_path_t, store_path_t> paths_map;
  for (auto& path : store_paths) {
    paths_map.insert_or_assign(path, path);
  }

  store_t::PathsSource paths_to_copy;

  auto compute_store_path_for_dst = [&](const valid_path_info_t& currentPathInfo) -> store_path_t {
    auto storePathForSrc = currentPathInfo.path;
    auto storePathForDst = storePathForSrc;
    if (currentPathInfo.ca && currentPathInfo.references.empty()) {
      storePathForDst = dst_store.makeFixedOutputPathFromCA(
          currentPathInfo.path.name(), currentPathInfo.contentAddressWithReferences().value());
      if (dst_store.store_dir == src_store.store_dir) {
        assert(storePathForDst == storePathForSrc);
      }
      if (storePathForDst != storePathForSrc) {
        debug("replaced path '%s' to '%s' for substituter '%s'",
              src_store.printStorePath(storePathForSrc), dst_store.printStorePath(storePathForDst),
              dst_store.config.getHumanReadableURI());
      }
    }
    return storePathForDst;
  };

  for (auto& missingPath : sorted_missing) {
    auto info = src_store.queryPathInfo(missingPath);

    auto storePathForDst = compute_store_path_for_dst(*info);
    paths_map.insert_or_assign(missingPath, storePathForDst);

    valid_path_info_t infoForDst = *info;
    infoForDst.path = storePathForDst;

    auto source = sink_to_source([&, nar_size = info->nar_size](sink_t& sink) {
      // We can reasonably assume that the copy will happen whenever we
      // read the path, so log something about that at that point
      uint64_t total = 0;
      const auto& src_cfg = src_store.config;
      const auto& dst_cfg = dst_store.config;
      auto store_path_s = src_store.printStorePath(missingPath);
      activity_t act(*logger, lvl_info, act_copy_path,
                     make_copy_path_message(src_cfg, dst_cfg, store_path_s),
                     logger_t::fields_t{logger_t::field_t{store_path_s},
                                        logger_t::field_t{src_cfg.getHumanReadableURI()},
                                        logger_t::field_t{dst_cfg.getHumanReadableURI()}});
      push_activity_t pact(act.id_);

      lambda_sink_t progress_sink([&](std::string_view data) {
        total += data.size();
        act.progress(total, nar_size);
      });
      tee_sink_t tee{sink, progress_sink};

      src_store.nar_from_path(missingPath, tee);
    });
    paths_to_copy.emplace_back(std::move(infoForDst), std::move(source));
  }

  dst_store.addMultipleToStore(std::move(paths_to_copy), act, repair, check_sigs);

  return paths_map;
}

void copy_closure(store_t& src_store, store_t& dst_store, const RealisedPath::Set& paths,
                  RepairFlag repair, CheckSigsFlag check_sigs, SubstituteFlag substitute) {
  if (&src_store == &dst_store) {
    return;
  }

  RealisedPath::Set closure;
  RealisedPath::closure(src_store, paths, closure);

  copy_paths(src_store, dst_store, closure, repair, check_sigs, substitute);
}

void copy_closure(store_t& src_store, store_t& dst_store, const store_path_set_t& store_paths,
                  RepairFlag repair, CheckSigsFlag check_sigs, SubstituteFlag substitute) {
  if (&src_store == &dst_store) {
    return;
  }

  store_path_set_t closure;
  src_store.computeFSClosure(store_paths, closure);
  copy_paths(src_store, dst_store, closure, repair, check_sigs, substitute);
}

std::optional<valid_path_info_t> decode_valid_path_info(const store_t& store, std::istream& str,
                                                        std::optional<hash_result_t> hash_given) {
  std::string path;
  getline(str, path);
  if (str.eof()) {
    return {};
  }
  if (!hash_given) {
    std::string s;
    getline(str, s);
    auto nar_hash = Hash::parse_any(s, hash_algorithm_t::SHA256);
    getline(str, s);
    auto nar_size = string2_int<uint64_t>(s);
    if (!nar_size) {
      throw Error("number expected");
    }
    hash_given = {nar_hash, *nar_size};
  }
  valid_path_info_t info(store.parseStorePath(path), {store, hash_given->hash});
  info.nar_size = hash_given->num_bytes_digested;
  std::string deriver;
  getline(str, deriver);
  if (deriver != "") {
    info.deriver = store.parseStorePath(deriver);
  }
  std::string s;
  getline(str, s);
  auto n = string2_int<int>(s);
  if (!n) {
    throw Error("number expected");
  }
  while ((*n)--) {
    getline(str, s);
    info.references.insert(store.parseStorePath(s));
  }
  if (!str || str.eof()) {
    throw Error("missing input");
  }
  return std::optional<valid_path_info_t>(std::move(info));
}

std::string store_dir_config_t::show_paths(const store_path_set_t& paths) const {
  std::string s;
  for (auto& i : paths) {
    if (s.size() != 0) {
      s += ", ";
    }
    s += "'" + printStorePath(i) + "'";
  }
  return s;
}

std::string show_paths(const std::set<std::filesystem::path> paths) {
  return concat_strings_sep(", ", quote_fs_paths(paths));
}

std::string show_paths(const path_set_t& paths) {
  return concat_strings_sep(", ", quote_strings(paths));
}

derivation_t store_t::derivationFromPath(const store_path_t& drv_path) {
  ensure_path(drv_path);
  return read_derivation(drv_path);
}

static derivation_t read_derivation_common(store_t& store, const store_path_t& drv_path,
                                           bool require_valid_path) {
  auto accessor = store.requireStoreObjectAccessor(drv_path, require_valid_path);
  try {
    return parse_derivation(store, accessor->read_file(canon_path_t::root),
                            derivation_t::nameFromPath(drv_path));
  } catch (FormatError& e) {
    throw Error("error parsing derivation '%s': %s", store.printStorePath(drv_path), e.msg());
  }
}

std::optional<store_path_t> store_t::getBuildDerivationPath(const store_path_t& path) {
  if (!path.is_derivation()) {
    try {
      auto info = queryPathInfo(path);
      if (!info->deriver) {
        return std::nullopt;
      }
      return *info->deriver;
    } catch (InvalidPath&) {
      return std::nullopt;
    }
  }

  if (!experimental_feature_settings.is_enabled(xp_t::ca_derivations) || !isValidPath(path)) {
    return path;
  }

  auto drv = read_derivation(path);
  if (!drv.type().hasKnownOutputPaths()) {
    // The build log is actually attached to the corresponding
    // resolved derivation, so we need to get it first
    auto resolvedDrv = drv.try_resolve(*this);
    if (resolvedDrv) {
      return ::nix::write_derivation(*this, *resolvedDrv, NoRepair, true);
    }
  }

  return path;
}

derivation_t store_t::read_derivation(const store_path_t& drv_path) {
  return read_derivation_common(*this, drv_path, true);
}

derivation_t store_t::readInvalidDerivation(const store_path_t& drv_path) {
  return read_derivation_common(*this, drv_path, false);
}

void store_t::signPathInfo(valid_path_info_t& info) {
  // FIXME: keep secret keys in memory.

  auto secretKeyFiles = settings.secretKeyFiles;

  for (auto& secret_key_file : secretKeyFiles.get()) {
    secret_key_t secret_key(read_file(secret_key_file));
    local_signer_t signer(std::move(secret_key));
    info.sign(*this, signer);
  }
}

void store_t::signRealisation(realisation_t& realisation) {
  // FIXME: keep secret keys in memory.

  auto secretKeyFiles = settings.secretKeyFiles;

  for (auto& secret_key_file : secretKeyFiles.get()) {
    secret_key_t secret_key(read_file(secret_key_file));
    local_signer_t signer(std::move(secret_key));
    realisation.sign(realisation.id, signer);
  }
}

} // namespace nix
