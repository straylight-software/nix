#include "nix/store/binary-cache-store.h"

#include <chrono>
#include <fstream>
#include <future>
#include <regex>

#include <nlohmann/json.hpp>

#include "nix/store/derivations.h"
#include "nix/store/globals.h"
#include "nix/store/nar-info-disk-cache.h"
#include "nix/store/nar-info.h"
#include "nix/store/remote-fs-accessor.h"
#include "nix/util/archive.h"
#include "nix/util/callback.h"
#include "nix/util/compression.h"
#include "nix/util/nar-accessor.h"
#include "nix/util/signals.h"
#include "nix/util/source-accessor.h"
#include "nix/util/strings.h"
#include "nix/util/sync.h"
#include "nix/util/thread-pool.h"

// Shadow parsing: Continuity verified parsers (Checkpoint 2)
#include "continuity/nix/nix_formats.h"

namespace nix {

binary_cache_store::binary_cache_store(config_t& config) : config{config} {
  if (config.secret_key_file != "") {
    signers.push_back(
        std::make_unique<local_signer_t>(secret_key_t{read_file(config.secret_key_file)}));
  }

  if (config.secretKeyFiles.get() != "") {
    for (const auto& keyPath : tokenize_string<strings_t>(config.secretKeyFiles.get(), ",")) {
      signers.push_back(std::make_unique<local_signer_t>(secret_key_t{read_file(keyPath)}));
    }
  }

  string_sink_t sink;
  sink << nar_version_magic1;
  narMagic = sink.str();
}

void binary_cache_store::init() {
  auto cacheInfo = getNixCacheInfo();
  if (!cacheInfo) {
    upsert_file(cacheInfoFile, "StoreDir: " + store_dir + "\n", "text/x-nix-cache-info");
  } else {
    for (auto& line : tokenize_string<strings_t>(*cacheInfo, "\n")) {
      size_t colon = line.find(':');
      if (colon == std::string::npos) {
        continue;
      }
      auto name = line.substr(0, colon);
      auto value = trim(line.substr(colon + 1, std::string::npos));
      if (name == "StoreDir") {
        if (value != store_dir) {
          throw Error("binary cache '%s' is for Nix stores with prefix '%s', not '%s'",
                      config.getHumanReadableURI(), value, store_dir);
        }
      } else if (name == "WantMassQuery") {
        config.want_mass_query.set_default(value == "1");
      } else if (name == "Priority") {
        config.priority.set_default(std::stoi(value));
      }
    }
  }
}

std::optional<std::string> binary_cache_store::getNixCacheInfo() {
  return getFile(cacheInfoFile);
}

void binary_cache_store::upsert_file(const std::string& path, std::string&& data,
                                     const std::string& mime_type, uint64_t size_hint) {
  string_source_t source{data};
  upsert_file(path, source, mime_type, size_hint);
}

void binary_cache_store::getFile(const std::string& path,
                                 Callback<std::optional<std::string>> callback) noexcept {
  try {
    callback(getFile(path));
  } catch (...) {
    callback.rethrow();
  }
}

void binary_cache_store::getFile(const std::string& path, sink_t& sink) {
  std::promise<std::optional<std::string>> promise;
  getFile(path, {[&](std::future<std::optional<std::string>> result) {
            try {
              promise.set_value(result.get());
            } catch (...) {
              promise.set_exception(std::current_exception());
            }
          }});
  sink(*promise.get_future().get());
}

std::optional<std::string> binary_cache_store::getFile(const std::string& path) {
  string_sink_t sink;
  try {
    getFile(path, sink);
  } catch (NoSuchBinaryCacheFile&) {
    return std::nullopt;
  }
  return std::move(sink.str());
}

std::string binary_cache_store::narInfoFileFor(const store_path_t& store_path) {
  return std::string(store_path.hash_part()) + ".narinfo";
}

void binary_cache_store::writeNarInfo(ref<nar_info_t> narInfo) {
  auto narInfoFile = narInfoFileFor(narInfo->path);

  upsert_file(narInfoFile, narInfo->to_string(*this), "text/x-nix-narinfo");

  pathInfoCache->lock()->upsert(narInfo->path,
                                PathInfoCacheValue{.value = std::shared_ptr<nar_info_t>(narInfo)});

  if (diskCache) {
    diskCache->upsertNarInfo(config.getReference().render(/*FIXME withParams=*/false),
                             std::string(narInfo->path.hash_part()),
                             std::shared_ptr<nar_info_t>(narInfo));
  }
}

ref<const valid_path_info_t>
binary_cache_store::addToStoreCommon(source_t& nar_source, RepairFlag repair,
                                     CheckSigsFlag check_sigs,
                                     std::function<valid_path_info_t(hash_result_t)> mkInfo) {
  auto fdTemp = create_anonymous_temp_file();

  auto now1 = std::chrono::steady_clock::now();

  /* Read the NAR simultaneously into a compression_sink_t+FileSink (to
     write the compressed NAR to disk), into a hash_sink_t (to get the
     NAR hash), and into a nar_accessor_t (to get the NAR listing). */
  hash_sink_t fileHashSink{hash_algorithm_t::SHA256};
  std::shared_ptr<source_accessor_t> narAccessor;
  hash_sink_t narHashSink{hash_algorithm_t::SHA256};
  {
    fd_sink_t fileSink(fdTemp.get());
    tee_sink_t teeSinkCompressed{fileSink, fileHashSink};
    auto compression_sink = make_compression_sink(
        config.compression, teeSinkCompressed, config.parallelCompression, config.compressionLevel);
    tee_sink_t teeSinkUncompressed{*compression_sink, narHashSink};
    tee_source_t teeSource{nar_source, teeSinkUncompressed};
    narAccessor = make_nar_accessor(teeSource);
    compression_sink->finish();
    fileSink.flush();
  }

  auto now2 = std::chrono::steady_clock::now();

  auto info = mkInfo(narHashSink.finish());
  auto narInfo = make_ref<nar_info_t>(info);
  narInfo->compression = config.compression;
  auto [fileHash, file_size] = fileHashSink.finish();
  narInfo->fileHash = fileHash;
  narInfo->file_size = file_size;
  narInfo->url = "nar/" + narInfo->fileHash->to_string(hash_format_t::nix32, false) + ".nar" +
                 (config.compression == "xz"      ? ".xz"
                  : config.compression == "bzip2" ? ".bz2"
                  : config.compression == "zstd"  ? ".zst"
                  : config.compression == "lzip"  ? ".lzip"
                  : config.compression == "lz4"   ? ".lz4"
                  : config.compression == "br"    ? ".br"
                                                  : "");

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now2 - now1).count();
  printMsg(lvl_talkative,
           "copying path '%1%' (%2% bytes, compressed %3$.1f%% in %4% ms) to binary cache",
           printStorePath(narInfo->path), info.nar_size,
           ((1.0 - (double)file_size / info.nar_size) * 100.0), duration);

  /* Verify that all references are valid. This may do some .narinfo
     reads, but typically they'll already be cached. */
  for (auto& ref : info.references) {
    try {
      if (ref != info.path) {
        queryPathInfo(ref);
      }
    } catch (InvalidPath&) {
      throw Error("cannot add '%s' to the binary cache because the reference '%s' is not valid",
                  printStorePath(info.path), printStorePath(ref));
    }
  }

  /* Optionally write a JSON file containing a listing of the
     contents of the NAR. */
  if (config.writeNARListing) {
    nlohmann::json j = {
        {"version", 1},
        {"root", list_nar_deep(*narAccessor, canon_path_t::root)},
    };

    upsert_file(std::string(info.path.hash_part()) + ".ls", j.dump(), "application/json");
  }

  /* Optionally maintain an index of DWARF debug info files
     consisting of JSON files named 'debuginfo/<build-id>' that
     specify the NAR file and member containing the debug info. */
  if (config.writeDebugInfo) {
    canon_path_t buildIdDir("lib/debug/.build-id");

    if (auto st = narAccessor->maybe_lstat(buildIdDir);
        st && st->type == source_accessor_t::t_directory) {
      thread_pool_t threadPool(25);

      auto doFile = [&](std::string member, std::string key, std::string target) {
        check_interrupt();

        nlohmann::json json;
        json["archive"] = target;
        json["member"] = member;

        // FIXME: or should we overwrite? The previous link may point
        // to a GC'ed file, so overwriting might be useful...
        if (file_exists(key)) {
          return;
        }

        printMsg(lvl_talkative, "creating debuginfo link from '%s' to '%s'", key, target);

        upsert_file(key, json.dump(), "application/json");
      };

      std::regex regex1("^[0-9a-f]{2}$");
      std::regex regex2("^[0-9a-f]{38}\\.debug$");

      for (auto& [s1, _type] : narAccessor->read_directory(buildIdDir)) {
        auto dir = buildIdDir / s1;

        if (narAccessor->lstat(dir).type != source_accessor_t::t_directory ||
            !std::regex_match(s1, regex1)) {
          continue;
        }

        for (auto& [s2, _type] : narAccessor->read_directory(dir)) {
          auto debugPath = dir / s2;

          if (narAccessor->lstat(debugPath).type != source_accessor_t::t_regular ||
              !std::regex_match(s2, regex2)) {
            continue;
          }

          auto buildId = s1 + s2;

          std::string key = "debuginfo/" + buildId;
          std::string target = "../" + narInfo->url;

          threadPool.enqueue(std::bind(doFile, std::string(debugPath.rel()), key, target));
        }
      }

      threadPool.process();
    }
  }

  /* Atomically write the NAR file. */
  if (repair || !file_exists(narInfo->url)) {
    fd_source_t source{fdTemp.get()};
    source.restart(); /* Seek back to the start of the file. */
    stats.narWrite++;
    upsert_file(narInfo->url, source, "application/x-nix-nar", narInfo->file_size);
  } else {
    stats.narWriteAverted++;
  }

  stats.narWriteBytes += info.nar_size;
  stats.narWriteCompressedBytes += file_size;
  stats.narWriteCompressionTimeMs += duration;

  narInfo->sign(*this, signers);

  /* Atomically write the NAR info file.*/
  writeNarInfo(narInfo);

  stats.narInfoWrite++;

  return narInfo;
}

void binary_cache_store::add_to_store(const valid_path_info_t& info, source_t& nar_source,
                                      RepairFlag repair, CheckSigsFlag check_sigs) {
  if (!repair && isValidPath(info.path)) {
    // FIXME: copyNAR -> null sink
    nar_source.drain();
    return;
  }

  addToStoreCommon(nar_source, repair, check_sigs, {[&](hash_result_t nar) {
                     /* FIXME reinstate these, once we can correctly do hash modulo sink as
                        needed. We need to throw here in case we uploaded a corrupted store path. */
                     // assert(info.narHash == nar.first);
                     // assert(info.narSize == nar.second);
                     return info;
                   }});
}

store_path_t binary_cache_store::add_to_store_from_dump(source_t& dump, std::string_view name,
                                                        file_serialisation_method_t dump_method,
                                                        content_address_method_t hash_method,
                                                        hash_algorithm_t hash_algo,
                                                        const store_path_set_t& references,
                                                        RepairFlag repair) {
  std::optional<Hash> caHash;
  std::string nar;

  // Calculating Git hash from NAR stream not yet implemented. May not
  // be possible to implement in single-pass if the NAR is in an
  // inconvenient order. Could fetch after uploading, however.
  if (hash_method.getFileIngestionMethod() == file_ingestion_method_t::git) {
    unsupported("addToStoreFromDump");
  }

  if (auto* dump2p = dynamic_cast<string_source_t*>(&dump)) {
    auto& dump2 = *dump2p;
    // Hack, this gives us a "replayable" source so we can compute
    // multiple hashes more easily.
    //
    // Only calculate if the dump is in the right format, however.
    if (static_cast<file_ingestion_method_t>(dump_method) == hash_method.getFileIngestionMethod()) {
      caHash = hash_string(hash_algorithm_t::SHA256, dump2.view());
    }
    switch (dump_method) {
      case file_serialisation_method_t::nix_archive:
        // The dump is already NAR in this case, just use it.
        nar = std::string(dump2.view());
        break;
      case file_serialisation_method_t::flat: {
        // The dump is Flat, so we need to convert it to NAR with a
        // single file.
        string_sink_t s;
        dump_string(dump2.view(), s);
        nar = std::move(s.str());
        break;
      }
    }
  } else {
    // Otherwise, we have to do th same hashing as NAR so our single
    // hash will suffice for both purposes.
    if (dump_method != file_serialisation_method_t::nix_archive ||
        hash_algo != hash_algorithm_t::SHA256) {
      unsupported("addToStoreFromDump");
    }
  }
  string_source_t narDump{nar};

  // Use `narDump` if we wrote to `nar`.
  source_t& narDump2 = nar.size() > 0 ? static_cast<source_t&>(narDump) : dump;

  return addToStoreCommon(narDump2, repair, CheckSigs,
                          [&](hash_result_t nar) {
                            auto info = valid_path_info_t::makeFromCA(
                                *this, name,
                                ContentAddressWithReferences::fromParts(
                                    hash_method, caHash ? *caHash : nar.hash,
                                    {
                                        .others = references,
                                        // caller is not capable of creating a self-reference,
                                        // because this is content-addressed without modulus
                                        .self = false,
                                    }),
                                nar.hash);
                            info.nar_size = nar.num_bytes_digested;
                            return info;
                          })
      ->path;
}

bool binary_cache_store::isValidPathUncached(const store_path_t& store_path) {
  // FIXME: this only checks whether a .narinfo with a matching hash
  // part exists. So ‘f4kb...-foo’ matches ‘f4kb...-bar’, even
  // though they shouldn't. Not easily fixed.
  return file_exists(narInfoFileFor(store_path));
}

std::optional<store_path_t>
binary_cache_store::queryPathFromHashPart(const std::string& hash_part) {
  auto pseudoPath = store_path_t(hash_part + "-" + MissingName);
  try {
    auto info = queryPathInfo(pseudoPath);
    return info->path;
  } catch (InvalidPath&) {
    return std::nullopt;
  }
}

void binary_cache_store::nar_from_path(const store_path_t& store_path, sink_t& sink) {
  auto info = queryPathInfo(store_path).cast<const nar_info_t>();

  uint64_t nar_size = 0;

  // Compute hash of decompressed NAR for validation
  hash_sink_t nar_hash_sink{info->nar_hash.algo()};

  // Track whether validation has been done (to handle both sync and coroutine paths)
  bool validated = false;

  auto validate = [&]() {
    if (validated) {
      return;
    }
    validated = true;

    auto [computed_hash, computed_size] = nar_hash_sink.finish();

    if (computed_hash != info->nar_hash) {
      throw Error("hash mismatch in NAR fetched from binary cache for path '%s';\n  expected: %s\n "
                  " got:      %s",
                  printStorePath(store_path), info->nar_hash.to_string(hash_format_t::nix32, true),
                  computed_hash.to_string(hash_format_t::nix32, true));
    }

    if (info->nar_size && computed_size != info->nar_size) {
      throw Error("size mismatch in NAR fetched from binary cache for path '%s';\n  expected: %d\n "
                  " got:      %d",
                  printStorePath(store_path), info->nar_size, computed_size);
    }
  };

  lambda_sink_t uncompressedSink{[&](std::string_view data) {
                                   nar_size += data.size();
                                   nar_hash_sink(data);
                                   sink(data);
                                 },
                                 [&]() {
                                   // Validate hash before recording stats. In coroutine path,
                                   // this runs in destructor - only validate if no exception
                                   // is already unwinding to avoid std::terminate
                                   if (!std::uncaught_exceptions()) {
                                     validate();
                                   }
                                   stats.narRead++;
                                   // stats.narReadCompressedBytes += nar->size(); // FIXME
                                   stats.narReadBytes += nar_size;
                                 }};

  auto decompressor = make_decompression_sink(info->compression, uncompressedSink);

  try {
    getFile(info->url, *decompressor);
  } catch (NoSuchBinaryCacheFile& e) {
    throw SubstituteGone(std::move(e.info()));
  }

  decompressor->finish();

  // For synchronous calls, validate here (coroutine calls will validate in cleanup)
  validate();
}

void binary_cache_store::query_path_info_uncached(
    const store_path_t& store_path,
    Callback<std::shared_ptr<const valid_path_info_t>> callback) noexcept {
  auto uri = config.getReference().render(/*FIXME withParams=*/false);
  auto store_path_s = printStorePath(store_path);
  logger_t::fields_t fields;
  fields.push_back(logger_t::field_t(store_path_s));
  fields.push_back(logger_t::field_t(uri));
  auto act = std::make_shared<activity_t>(
      *logger, lvl_talkative, act_query_path_info,
      fmt("querying info about '%s' on '%s'", store_path_s, uri), fields);
  push_activity_t pact(act->id_);

  auto narInfoFile = narInfoFileFor(store_path);

  auto callbackPtr = std::make_shared<decltype(callback)>(std::move(callback));

  getFile(narInfoFile, {[=, this](std::future<std::optional<std::string>> fut) {
            try {
              auto data = fut.get();

              if (!data) {
                return (*callbackPtr)({});
              }

              stats.narInfoRead++;

              // ═══════════════════════════════════════════════════════════════
              // Checkpoint 3: Continuity verified parser is PRIMARY
              // Legacy parser shadows in debug builds for correctness assertion
              // ═══════════════════════════════════════════════════════════════

              auto continuity_result = continuity::nix::parse_narinfo(*data);
              if (!continuity_result.is_ok()) {
                // Continuity parse failed - fall through to legacy for error handling
                // TODO[b7r6]: !! clean this up !! - once we trust Continuity fully,
                // this should throw directly with continuity_result.error
                throw Error("Continuity narinfo parse failed for '%s': %s", narInfoFile,
                            continuity_result.error.value_or("incomplete input"));
              }

              // Convert Continuity result to legacy nar_info_t
              auto info = std::make_shared<nar_info_t>(
                  from_continuity_narinfo(*this, *continuity_result.value));

#ifndef NDEBUG
              // Shadow parse with legacy - assert equivalence
              try {
                nar_info_t legacy_info(*this, *data, narInfoFile);

                // Assert key fields match
                assert(info->path == legacy_info.path);
                assert(info->nar_hash == legacy_info.nar_hash);
                assert(info->nar_size == legacy_info.nar_size);
                assert(info->url == legacy_info.url);
                assert(info->compression == legacy_info.compression);
                assert(info->file_size == legacy_info.file_size);
                assert(info->references == legacy_info.references);
                assert(info->deriver == legacy_info.deriver);
                assert(info->sigs == legacy_info.sigs);
                // Note: fileHash and ca may differ in representation, check separately
                assert(info->fileHash.has_value() == legacy_info.fileHash.has_value());
                assert(info->ca.has_value() == legacy_info.ca.has_value());
              } catch (const Error& e) {
                // Legacy parse failed but Cornell succeeded - log but continue
                // This indicates Cornell is more permissive or legacy has a bug
                warn("Shadow parse divergence for '%s': Cornell succeeded but legacy failed: %s",
                     narInfoFile, e.what());
              }
#endif

              (*callbackPtr)((std::shared_ptr<valid_path_info_t>)info);

              (void)act; // force Activity into this lambda to ensure it stays alive
            } catch (...) {
              callbackPtr->rethrow();
            }
          }});
}

store_path_t binary_cache_store::add_to_store(std::string_view name, const source_path_t& path,
                                              content_address_method_t method,
                                              hash_algorithm_t hash_algo,
                                              const store_path_set_t& references,
                                              path_filter_t& filter, RepairFlag repair) {
  /* FIXME: Make binary_cache_store::addToStoreCommon support
     non-recursive+sha256 so we can just use the default
     implementation of this method in terms of add_to_store_from_dump. */

  auto h = hash_path(path, method.getFileIngestionMethod(), hash_algo, filter).first;

  auto source = sink_to_source([&](sink_t& sink) { path.dump_path(sink, filter); });
  return addToStoreCommon(*source, repair, CheckSigs,
                          [&](hash_result_t nar) {
                            auto info = valid_path_info_t::makeFromCA(
                                *this, name,
                                ContentAddressWithReferences::fromParts(
                                    method, h,
                                    {
                                        .others = references,
                                        // caller is not capable of creating a self-reference,
                                        // because this is content-addressed without modulus
                                        .self = false,
                                    }),
                                nar.hash);
                            info.nar_size = nar.num_bytes_digested;
                            return info;
                          })
      ->path;
}

std::string binary_cache_store::makeRealisationPath(const DrvOutput& id) {
  return realisationsPrefix + "/" + id.to_string() + ".doi";
}

void binary_cache_store::query_realisation_uncached(
    const DrvOutput& id, Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept {
  auto outputInfoFilePath = makeRealisationPath(id);

  auto callbackPtr = std::make_shared<decltype(callback)>(std::move(callback));

  Callback<std::optional<std::string>> newCallback = {
      [=](std::future<std::optional<std::string>> fut) {
        try {
          auto data = fut.get();
          if (!data) {
            return (*callbackPtr)({});
          }

          std::shared_ptr<const UnkeyedRealisation> realisation;
          try {
            realisation = std::make_shared<const UnkeyedRealisation>(nlohmann::json::parse(*data));
          } catch (Error& e) {
            e.add_trace({}, "while parsing file '%s' as a realisation for key '%s'",
                        outputInfoFilePath, id.to_string());
            throw;
          }
          return (*callbackPtr)(std::move(realisation));
        } catch (...) {
          callbackPtr->rethrow();
        }
      }};

  getFile(outputInfoFilePath, std::move(newCallback));
}

void binary_cache_store::register_drv_output(const realisation_t& info) {
  if (diskCache) {
    diskCache->upsertRealisation(config.getReference().render(/*FIXME withParams=*/false), info);
  }
  upsert_file(makeRealisationPath(info.id), static_cast<nlohmann::json>(info).dump(),
              "application/json");
}

ref<RemoteFSAccessor> binary_cache_store::getRemoteFSAccessor(bool require_valid_path) {
  return make_ref<RemoteFSAccessor>(ref<store_t>(shared_from_this()), require_valid_path,
                                    config.localNarCache);
}

ref<source_accessor_t> binary_cache_store::getFSAccessor(bool require_valid_path) {
  return getRemoteFSAccessor(require_valid_path);
}

std::shared_ptr<source_accessor_t> binary_cache_store::getFSAccessor(const store_path_t& store_path,
                                                                     bool require_valid_path) {
  return getRemoteFSAccessor(require_valid_path)->accessObject(store_path);
}

void binary_cache_store::addSignatures(const store_path_t& store_path, const string_set_t& sigs) {
  /* Note: This operation is inherently racy for remote binary caches since
     there is no distributed locking mechanism. We mitigate the race by:

     1. Reading current narinfo directly from remote (bypassing all caches)
     2. Merging all existing signatures with new signatures
     3. Writing the merged result

     For S3 specifically, eventual consistency means we may still lose
     signatures in rare cases if S3 returns a stale cached version. However,
     by always fetching fresh and merging, we ensure monotonic progress:
     signatures are never lost from the local perspective.

     The only complete solution would require:
     - Conditional writes (If-Match ETag) with retry-on-conflict, or
     - A separate coordination service (e.g., DynamoDB for S3)
     Neither is currently supported by the binary cache interface.

     Since signatures are only additive (never removed), and signing is typically
     done by a single trusted entity, the practical impact is minimal. Lost
     signatures can always be re-added. */

  // Helper to fetch path info bypassing all caches (memory and disk)
  auto fetchFresh = [&]() -> std::shared_ptr<const valid_path_info_t> {
    std::promise<std::shared_ptr<const valid_path_info_t>> promise;
    query_path_info_uncached(store_path,
                             {[&](std::future<std::shared_ptr<const valid_path_info_t>> result) {
                               try {
                                 promise.set_value(result.get());
                               } catch (...) {
                                 promise.set_exception(std::current_exception());
                               }
                             }});
    return promise.get_future().get();
  };

  // Fetch fresh from remote, bypassing caches
  auto currentInfo = fetchFresh();
  if (!currentInfo) {
    throw InvalidPath("path '%s' is not valid", printStorePath(store_path));
  }

  auto narInfo = make_ref<nar_info_t>((nar_info_t&)*currentInfo);

  // Add the new signatures (set merge is idempotent)
  narInfo->sigs.insert(sigs.begin(), sigs.end());

  writeNarInfo(narInfo);
}

std::optional<std::string> binary_cache_store::getBuildLogExact(const store_path_t& path) {
  auto logPath = "log/" + std::string(base_name_of(printStorePath(path)));

  debug("fetching build log from binary cache '%s/%s'", config.getHumanReadableURI(), logPath);

  return getFile(logPath);
}

void binary_cache_store::addBuildLog(const store_path_t& drv_path, std::string_view log) {
  assert(drv_path.is_derivation());

  upsert_file("log/" + std::string(drv_path.to_string()),
              (std::string)log, // FIXME: don't copy
              "text/plain; charset=utf-8");
}

} // namespace nix
