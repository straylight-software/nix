#include "nix/store/remote-store.h"

#include <nlohmann/json.hpp>

#include "nix/store/build-result.h"
#include "nix/store/derivations.h"
#include "nix/store/filetransfer.h"
#include "nix/store/gc-store.h"
#include "nix/store/globals.h"
#include "nix/store/path-with-outputs.h"
#include "nix/store/remote-fs-accessor.h"
#include "nix/store/remote-store-connection.h"
#include "nix/store/worker-protocol-impl.h"
#include "nix/store/worker-protocol.h"
#include "nix/util/archive.h"
#include "nix/util/callback.h"
#include "nix/util/finally.h"
#include "nix/util/git.h"
#include "nix/util/logging.h"
#include "nix/util/pool.h"
#include "nix/util/serialise.h"
#include "nix/util/signals.h"
#include "nix/util/util.h"

namespace nix {

/* TODO: Separate these store types into different files, give them better names */
remote_store::remote_store(const config_t& config)
    : store_t{config},
      config{config},
      connections(make_ref<pool_t<Connection>>(
          std::max(1, config.maxConnections.get()),
          [this]() {
            auto conn = openConnectionWrapper();
            try {
              initConnection(*conn);
            } catch (...) {
              failed = true;
              throw;
            }
            return conn;
          },
          [this](const ref<Connection>& r) {
            return r->to.good() && r->from.good() &&
                   std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now() - r->start_time)
                           .count() < this->config.maxConnectionAge;
          })) {}

ref<remote_store::Connection> remote_store::openConnectionWrapper() {
  if (failed)
    throw Error("opening a connection to remote store '%s' previously failed",
                config.getHumanReadableURI());
  try {
    return open_connection();
  } catch (...) {
    failed = true;
    throw;
  }
}

void remote_store::initConnection(Connection& conn) {
  /* Send the magic greeting, check for the reply. */
  try {
    conn.from.set_end_of_file_error("Nix daemon disconnected unexpectedly (maybe it crashed?)");

    string_sink_t saved;
    tee_source_t tee(conn.from, saved);
    try {
      auto [protoVersion, features] = WorkerProto::BasicClientConnection::handshake(
          conn.to, tee, PROTOCOL_VERSION, WorkerProto::allFeatures);
      if (protoVersion < MINIMUM_PROTOCOL_VERSION)
        throw Error("the Nix daemon version is too old");
      conn.protoVersion = protoVersion;
      conn.features = features;
    } catch (SerialisationError& e) {
      /* In case the other side is waiting for our input, close
         it. */
      conn.closeWrite();
      {
        null_sink_t nullSink;
        tee.drain_into(nullSink);
      }
      throw Error("protocol mismatch, got '%s'", chomp(saved.str()));
    }

    static_cast<WorkerProto::ClientHandshakeInfo&>(conn) = conn.postHandshake(*this);

    for (auto& feature : conn.features)
      debug("negotiated feature '%s'", feature);

    auto ex = conn.processStderrReturn();
    if (ex)
      std::rethrow_exception(ex);
  } catch (Error& e) {
    throw Error("cannot open connection to remote store '%s': %s", config.getHumanReadableURI(),
                e.what());
  }

  setOptions(conn);
}

void remote_store::setOptions(Connection& conn) {
  conn.to << WorkerProto::Op::SetOptions << settings.keep_failed << settings.keep_going
          << settings.try_fallback << static_cast<uint64_t>(verbosity) << settings.max_build_jobs
          << settings.max_silent_time << true
          << static_cast<uint64_t>(settings.verbose_build ? lvl_error : lvl_vomit)
          << 0 // obsolete log type
          << 0 /* obsolete print build trace */
          << settings.build_cores << settings.use_substitutes;

  std::map<std::string, nix::config_t::setting_info_t> overrides;
  settings.get_settings(overrides, true); // libstore settings
  file_transfer_settings.get_settings(overrides, true);
  overrides.erase(settings.keep_failed.name);
  overrides.erase(settings.keep_going.name);
  overrides.erase(settings.try_fallback.name);
  overrides.erase(settings.max_build_jobs.name);
  overrides.erase(settings.max_silent_time.name);
  overrides.erase(settings.build_cores.name);
  overrides.erase(settings.use_substitutes.name);
  overrides.erase(logger_settings.show_trace.name);
  overrides.erase(experimental_feature_settings.experimental_features.name);
  overrides.erase("plugin-files");
  // Prevent recursive remote building (NixOS/nix#10740): clear builders on remote
  // to avoid deadlocks from cyclic builder configurations (A→B→A).
  overrides[settings.builders.name] = {.value_ = "", .description_ = ""};
  conn.to << overrides.size();
  for (auto& i : overrides)
    conn.to << i.first << i.second.value_;

  auto ex = conn.processStderrReturn();
  if (ex)
    std::rethrow_exception(ex);
}

remote_store::ConnectionHandle::~ConnectionHandle() {
  if (!daemonException && std::uncaught_exceptions()) {
    handle.markBad();
    debug("closing daemon connection because of an exception");
  }
}

void remote_store::ConnectionHandle::processStderr(sink_t* sink, source_t* source, bool flush,
                                                   bool block) {
  handle->processStderr(&daemonException, sink, source, flush, block);
}

remote_store::ConnectionHandle remote_store::getConnection() {
  return ConnectionHandle(connections->get());
}

void remote_store::setOptions() {
  setOptions(*(getConnection().handle));
}

bool remote_store::isValidPathUncached(const store_path_t& path) {
  auto conn(getConnection());
  conn->to << WorkerProto::Op::IsValidPath;
  WorkerProto::write(*this, *conn, path);
  conn.processStderr();
  return read_int(conn->from);
}

store_path_set_t remote_store::queryValidPaths(const store_path_set_t& paths,
                                               SubstituteFlag maybeSubstitute) {
  auto conn(getConnection());
  return conn->queryValidPaths(*this, &conn.daemonException, paths, maybeSubstitute);
}

store_path_set_t remote_store::query_all_valid_paths() {
  auto conn(getConnection());
  conn->to << WorkerProto::Op::QueryAllValidPaths;
  conn.processStderr();
  return WorkerProto::Serialise<store_path_set_t>::read(*this, *conn);
}

store_path_set_t remote_store::querySubstitutablePaths(const store_path_set_t& paths) {
  auto conn(getConnection());
  conn->to << WorkerProto::Op::QuerySubstitutablePaths;
  WorkerProto::write(*this, *conn, paths);
  conn.processStderr();
  return WorkerProto::Serialise<store_path_set_t>::read(*this, *conn);
}

void remote_store::querySubstitutablePathInfos(const StorePathCAMap& paths_map,
                                               SubstitutablePathInfos& infos) {
  if (paths_map.empty())
    return;

  auto conn(getConnection());

  conn->to << WorkerProto::Op::QuerySubstitutablePathInfos;
  if (GET_PROTOCOL_MINOR(conn->protoVersion) < 22) {
    store_path_set_t paths;
    for (auto& path : paths_map)
      paths.insert(path.first);
    WorkerProto::write(*this, *conn, paths);
  } else
    WorkerProto::write(*this, *conn, paths_map);
  conn.processStderr();
  size_t count = read_num<size_t>(conn->from);
  for (size_t n = 0; n < count; n++) {
    SubstitutablePathInfo& info(infos[WorkerProto::Serialise<store_path_t>::read(*this, *conn)]);
    info.deriver = WorkerProto::Serialise<std::optional<store_path_t>>::read(*this, *conn);
    info.references = WorkerProto::Serialise<store_path_set_t>::read(*this, *conn);
    info.downloadSize = read_long_long(conn->from);
    info.nar_size = read_long_long(conn->from);
  }
}

void remote_store::query_path_info_uncached(
    const store_path_t& path,
    Callback<std::shared_ptr<const valid_path_info_t>> callback) noexcept {
  try {
    auto info = ({
      auto conn(getConnection());
      conn->queryPathInfo(*this, &conn.daemonException, path);
    });
    if (!info)
      callback(nullptr);
    else
      callback(std::make_shared<valid_path_info_t>(store_path_t{path}, *info));
  } catch (...) {
    callback.rethrow();
  }
}

void remote_store::query_referrers(const store_path_t& path, store_path_set_t& referrers) {
  auto conn(getConnection());
  conn->to << WorkerProto::Op::QueryReferrers;
  WorkerProto::write(*this, *conn, path);
  conn.processStderr();
  for (auto& i : WorkerProto::Serialise<store_path_set_t>::read(*this, *conn))
    referrers.insert(i);
}

store_path_set_t remote_store::queryValidDerivers(const store_path_t& path) {
  auto conn(getConnection());
  conn->to << WorkerProto::Op::QueryValidDerivers;
  WorkerProto::write(*this, *conn, path);
  conn.processStderr();
  return WorkerProto::Serialise<store_path_set_t>::read(*this, *conn);
}

store_path_set_t remote_store::queryDerivationOutputs(const store_path_t& path) {
  if (GET_PROTOCOL_MINOR(getProtocol()) >= 0x16) {
    return store_t::queryDerivationOutputs(path);
  }
  auto conn(getConnection());
  conn->to << WorkerProto::Op::QueryDerivationOutputs;
  WorkerProto::write(*this, *conn, path);
  conn.processStderr();
  return WorkerProto::Serialise<store_path_set_t>::read(*this, *conn);
}

std::map<std::string, std::optional<store_path_t>>
remote_store::queryPartialDerivationOutputMap(const store_path_t& path, store_t* eval_store_) {
  if (GET_PROTOCOL_MINOR(getProtocol()) >= 0x16) {
    if (!eval_store_) {
      auto conn(getConnection());
      conn->to << WorkerProto::Op::QueryDerivationOutputMap;
      WorkerProto::write(*this, *conn, path);
      conn.processStderr();
      return WorkerProto::Serialise<std::map<std::string, std::optional<store_path_t>>>::read(
          *this, *conn);
    } else {
      auto& eval_store = *eval_store_;
      auto outputs = eval_store.queryStaticPartialDerivationOutputMap(path);
      // union with the first branch overriding the statically-known ones
      // when non-`std::nullopt`.
      for (auto&& [output_name, optPath] : queryPartialDerivationOutputMap(path, nullptr)) {
        if (optPath)
          outputs.insert_or_assign(std::move(output_name), std::move(optPath));
        else
          outputs.insert({std::move(output_name), std::nullopt});
      }
      return outputs;
    }
  } else {
    auto& eval_store = eval_store_ ? *eval_store_ : *this;
    // Fallback for old daemon versions.
    // For floating-CA derivations (and their co-dependencies) this is an
    // under-approximation as it only returns the paths that can be inferred
    // from the derivation itself (and not the ones that are known because
    // the have been built), but as old stores don't handle floating-CA
    // derivations this shouldn't matter
    return eval_store.queryStaticPartialDerivationOutputMap(path);
  }
}

std::optional<store_path_t> remote_store::queryPathFromHashPart(const std::string& hash_part) {
  auto conn(getConnection());
  conn->to << WorkerProto::Op::QueryPathFromHashPart << hash_part;
  conn.processStderr();
  return WorkerProto::Serialise<std::optional<store_path_t>>::read(*this, *conn);
}

ref<const valid_path_info_t> remote_store::addCAToStore(source_t& dump, std::string_view name,
                                                        content_address_method_t ca_method,
                                                        hash_algorithm_t hash_algo,
                                                        const store_path_set_t& references,
                                                        RepairFlag repair) {
  std::optional<ConnectionHandle> conn_(getConnection());
  auto& conn = *conn_;

  if (GET_PROTOCOL_MINOR(conn->protoVersion) >= 25) {
    conn->to << WorkerProto::Op::AddToStore << name << ca_method.renderWithAlgo(hash_algo);
    WorkerProto::write(*this, *conn, references);
    conn->to << repair;

    // The dump source may invoke the store, so we need to make some room.
    connections->incCapacity();
    {
      finally_t cleanup([&]() { connections->decCapacity(); });
      conn.withFramedSink([&](sink_t& sink) { dump.drain_into(sink); });
    }

    return make_ref<valid_path_info_t>(
        WorkerProto::Serialise<valid_path_info_t>::read(*this, *conn));
  } else {
    if (repair)
      throw Error(
          "repairing is not supported when building through the Nix daemon protocol < 1.25");

    switch (ca_method.raw) {
      case content_address_method_t::raw_t::Text: {
        if (hash_algo != hash_algorithm_t::SHA256)
          throw UnimplementedError("When adding text-hashed data called '%s', only SHA-256 is "
                                   "supported but '%s' was given",
                                   name, print_hash_algo(hash_algo));
        std::string s = dump.drain();
        conn->to << WorkerProto::Op::AddTextToStore << name << s;
        WorkerProto::write(*this, *conn, references);
        conn.processStderr();
        break;
      }
      case content_address_method_t::raw_t::flat:
      case content_address_method_t::raw_t::nix_archive:
      case content_address_method_t::raw_t::git:
      default: {
        auto fim = ca_method.getFileIngestionMethod();
        conn->to << WorkerProto::Op::AddToStore << name
                 << ((hash_algo == hash_algorithm_t::SHA256 &&
                      fim == file_ingestion_method_t::nix_archive)
                         ? 0
                         : 1) /* backwards compatibility hack */
                 << (fim == file_ingestion_method_t::nix_archive ? 1 : 0)
                 << print_hash_algo(hash_algo);

        try {
          conn->to.reset_written();
          connections->incCapacity();
          {
            finally_t cleanup([&]() { connections->decCapacity(); });
            if (fim == file_ingestion_method_t::nix_archive) {
              dump.drain_into(conn->to);
            } else {
              std::string contents = dump.drain();
              dump_string(contents, conn->to);
            }
          }
          conn.processStderr();
        } catch (sys_error_t& e) {
          /* Daemon closed while we were sending the path. Probably OOM
            or I/O error. */
          if (e.err_no() == EPIPE)
            try {
              conn.processStderr();
            } catch (EndOfFile& e) {
            }
          throw;
        }
        break;
      }
    }
    auto path = WorkerProto::Serialise<store_path_t>::read(*this, *conn);
    // Release our connection to prevent a deadlock in queryPathInfo().
    conn_.reset();
    return queryPathInfo(path);
  }
}

store_path_t remote_store::add_to_store_from_dump(source_t& dump, std::string_view name,
                                                  file_serialisation_method_t dump_method,
                                                  content_address_method_t hash_method,
                                                  hash_algorithm_t hash_algo,
                                                  const store_path_set_t& references,
                                                  RepairFlag repair) {
  file_serialisation_method_t fsm;
  switch (hash_method.getFileIngestionMethod()) {
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
    default:
      throw Error("unsupported file ingestion method: %d",
                  static_cast<int>(hash_method.getFileIngestionMethod()));
  }
  if (fsm != dump_method)
    unsupported("RemoteStore::addToStoreFromDump doesn't support this `dumpMethod` `hashMethod` "
                "combination");
  auto store_path = addCAToStore(dump, name, hash_method, hash_algo, references, repair)->path;
  invalidatePathInfoCacheFor(store_path);
  return store_path;
}

void remote_store::add_to_store(const valid_path_info_t& info, source_t& source, RepairFlag repair,
                                CheckSigsFlag check_sigs) {
  auto conn(getConnection());

  conn->to << WorkerProto::Op::AddToStoreNar;
  WorkerProto::write(*this, *conn, info.path);
  WorkerProto::write(*this, *conn, info.deriver);
  conn->to << info.nar_hash.to_string(hash_format_t::base16, false);
  WorkerProto::write(*this, *conn, info.references);
  conn->to << info.registrationTime << info.nar_size << info.ultimate << info.sigs
           << render_content_address(info.ca) << repair << !check_sigs;

  if (GET_PROTOCOL_MINOR(conn->protoVersion) >= 23) {
    conn.withFramedSink([&](sink_t& sink) { copy_nar(source, sink); });
  } else if (GET_PROTOCOL_MINOR(conn->protoVersion) >= 21) {
    conn.processStderr(0, &source);
  } else {
    copy_nar(source, conn->to);
    conn.processStderr(0, nullptr);
  }
}

void remote_store::addMultipleToStore(PathsSource&& paths_to_copy, activity_t& act,
                                      RepairFlag repair, CheckSigsFlag check_sigs) {
  // `addMultipleToStore` is single threaded
  size_t bytesExpected = 0;
  for (auto& [path_info, _] : paths_to_copy) {
    bytesExpected += path_info.nar_size;
  }
  act.set_expected(act_copy_path, bytesExpected);

  auto source = sink_to_source([&](sink_t& sink) {
    size_t nrTotal = paths_to_copy.size();
    sink << nrTotal;
    // Reverse, so we can release memory at the original start
    std::reverse(paths_to_copy.begin(), paths_to_copy.end());
    while (!paths_to_copy.empty()) {
      act.progress(nrTotal - paths_to_copy.size(), nrTotal, size_t(1), size_t(0));

      auto& [path_info, pathSource] = paths_to_copy.back();
      WorkerProto::Serialise<valid_path_info_t>::write(*this,
                                                       WorkerProto::WriteConn{
                                                           .to = sink,
                                                           .version = 16,
                                                       },
                                                       path_info);
      pathSource->drain_into(sink);
      paths_to_copy.pop_back();
    }
  });

  addMultipleToStore(*source, repair, check_sigs);
}

void remote_store::addMultipleToStore(source_t& source, RepairFlag repair,
                                      CheckSigsFlag check_sigs) {
  if (GET_PROTOCOL_MINOR(getConnection()->protoVersion) >= 32) {
    auto conn(getConnection());
    conn->to << WorkerProto::Op::AddMultipleToStore << repair << !check_sigs;
    conn.withFramedSink([&](sink_t& sink) { source.drain_into(sink); });
  } else
    store_t::addMultipleToStore(source, repair, check_sigs);
}

void remote_store::register_drv_output(const realisation_t& info) {
  auto conn(getConnection());
  conn->to << WorkerProto::Op::RegisterDrvOutput;
  if (GET_PROTOCOL_MINOR(conn->protoVersion) < 31) {
    WorkerProto::write(*this, *conn, info.id);
    conn->to << std::string(info.out_path.to_string());
  } else {
    WorkerProto::write(*this, *conn, info);
  }
  conn.processStderr();
}

void remote_store::query_realisation_uncached(
    const DrvOutput& id, Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept {
  try {
    auto conn(getConnection());

    if (GET_PROTOCOL_MINOR(conn->protoVersion) < 27) {
      warn("the daemon is too old to support content-addressing derivations, please upgrade it to "
           "2.4");
      return callback(nullptr);
    }

    conn->to << WorkerProto::Op::QueryRealisation;
    WorkerProto::write(*this, *conn, id);
    conn.processStderr();

    auto real = [&]() -> std::shared_ptr<const UnkeyedRealisation> {
      if (GET_PROTOCOL_MINOR(conn->protoVersion) < 31) {
        auto out_paths = WorkerProto::Serialise<std::set<store_path_t>>::read(*this, *conn);
        if (out_paths.empty())
          return nullptr;
        return std::make_shared<const UnkeyedRealisation>(
            UnkeyedRealisation{.out_path = *out_paths.begin()});
      } else {
        auto realisations = WorkerProto::Serialise<std::set<realisation_t>>::read(*this, *conn);
        if (realisations.empty())
          return nullptr;
        return std::make_shared<const UnkeyedRealisation>(*realisations.begin());
      }
    }();

    callback(std::shared_ptr<const UnkeyedRealisation>(real));
  } catch (...) {
    return callback.rethrow();
  }
}

void remote_store::copyDrvsFromEvalStore(const std::vector<derived_path_t>& paths,
                                         std::shared_ptr<store_t> eval_store) {
  if (eval_store && eval_store.get() != this) {
    /* The remote doesn't have a way to access eval_store, so copy
       the .drvs. */
    RealisedPath::Set drvPaths2;
    for (const auto& i : paths) {
      std::visit(overloaded{
                     [&](const derived_path_t::opaque_t& bp) {
                       // Do nothing, path is hopefully there already
                     },
                     [&](const derived_path_t::Built& bp) {
                       drvPaths2.insert(bp.drv_path->getBaseStorePath());
                     },
                 },
                 i.raw());
    }
    copy_closure(*eval_store, *this, drvPaths2);
  }
}

void remote_store::build_paths(const std::vector<derived_path_t>& drv_paths, BuildMode build_mode,
                               std::shared_ptr<store_t> eval_store) {
  copyDrvsFromEvalStore(drv_paths, eval_store);

  auto conn(getConnection());
  conn->to << WorkerProto::Op::BuildPaths;
  WorkerProto::write(*this, *conn, drv_paths);
  conn->to << build_mode;
  conn.processStderr();
  read_int(conn->from);
}

std::vector<keyed_build_result_t>
remote_store::build_paths_with_results(const std::vector<derived_path_t>& paths,
                                       BuildMode build_mode, std::shared_ptr<store_t> eval_store) {
  copyDrvsFromEvalStore(paths, eval_store);

  std::optional<ConnectionHandle> conn_(getConnection());
  auto& conn = *conn_;

  if (GET_PROTOCOL_MINOR(conn->protoVersion) >= 34) {
    conn->to << WorkerProto::Op::BuildPathsWithResults;
    WorkerProto::write(*this, *conn, paths);
    conn->to << build_mode;
    conn.processStderr();
    return WorkerProto::Serialise<std::vector<keyed_build_result_t>>::read(*this, *conn);
  } else {
    // Avoid deadlock.
    conn_.reset();

    // Note: this throws an exception if a build/substitution
    // fails, but meh.
    build_paths(paths, build_mode, eval_store);

    std::vector<keyed_build_result_t> results;

    for (auto& path : paths) {
      std::visit(
          overloaded{[&](const derived_path_t::opaque_t& bo) {
                       results.push_back(keyed_build_result_t{
                           {.inner{build_result_t::Success{
                               .status = build_result_t::Success::Substituted,
                           }}},
                           /* .path = */ bo,
                       });
                     },
                     [&](const derived_path_t::Built& bfd) {
                       build_result_t::Success success{
                           .status = build_result_t::Success::Built,
                       };

                       OutputPathMap outputs;
                       auto drv_path = resolve_derived_path(*eval_store, *bfd.drv_path);
                       auto drv = eval_store->read_derivation(drv_path);
                       const auto output_hashes =
                           static_output_hashes(*eval_store, drv); // FIXME: expensive
                       auto built = resolve_derived_path(*this, bfd, &*eval_store);
                       for (auto& [output, output_path] : built) {
                         auto outputHash = get(output_hashes, output);
                         if (!outputHash)
                           throw Error("the derivation '%s' doesn't have an output named '%s'",
                                       printStorePath(drv_path), output);
                         auto output_id = DrvOutput{*outputHash, output};
                         if (experimental_feature_settings.is_enabled(xp_t::ca_derivations)) {
                           auto realisation = query_realisation(output_id);
                           if (!realisation)
                             throw MissingRealisation(output_id);
                           success.built_outputs.emplace(output,
                                                         realisation_t{*realisation, output_id});
                         } else {
                           success.built_outputs.emplace(output, realisation_t{
                                                                     UnkeyedRealisation{
                                                                         .out_path = output_path,
                                                                     },
                                                                     output_id,
                                                                 });
                         }
                       }

                       results.push_back(keyed_build_result_t{
                           {.inner = std::move(success)},
                           /* .path = */ bfd,
                       });
                     }},
          path.raw());
    }

    return results;
  }
}

build_result_t remote_store::buildDerivation(const store_path_t& drv_path,
                                             const basic_derivation_t& drv, BuildMode build_mode) {
  auto conn(getConnection());
  conn->putBuildDerivationRequest(*this, &conn.daemonException, drv_path, drv, build_mode);
  conn.processStderr();
  return WorkerProto::Serialise<build_result_t>::read(*this, *conn);
}

void remote_store::ensure_path(const store_path_t& path) {
  auto conn(getConnection());
  conn->to << WorkerProto::Op::EnsurePath;
  WorkerProto::write(*this, *conn, path);
  conn.processStderr();
  read_int(conn->from);
}

void remote_store::addTempRoot(const store_path_t& path) {
  auto conn(getConnection());
  conn->addTempRoot(*this, &conn.daemonException, path);
}

Roots remote_store::findRoots(bool censor) {
  auto conn(getConnection());
  conn->to << WorkerProto::Op::FindRoots;
  conn.processStderr();
  size_t count = read_num<size_t>(conn->from);
  Roots result;
  while (count--) {
    Path link = read_string(conn->from);
    result[WorkerProto::Serialise<store_path_t>::read(*this, *conn)].emplace(link);
  }
  return result;
}

void remote_store::collectGarbage(const GCOptions& options, GCResults& results) {
  auto conn(getConnection());

  conn->to << WorkerProto::Op::CollectGarbage;
  WorkerProto::write(*this, *conn, options.action);
  WorkerProto::write(*this, *conn, options.pathsToDelete);
  conn->to << options.ignoreLiveness
           << options.maxFreed
           /* removed options */
           << 0 << 0 << 0;

  conn.processStderr();

  results.paths = read_strings<path_set_t>(conn->from);
  results.bytes_freed = read_long_long(conn->from);
  read_long_long(conn->from); // obsolete

  pathInfoCache->lock()->clear();
}

void remote_store::optimiseStore() {
  auto conn(getConnection());
  conn->to << WorkerProto::Op::OptimiseStore;
  conn.processStderr();
  read_int(conn->from);
}

bool remote_store::verifyStore(bool check_contents, RepairFlag repair) {
  auto conn(getConnection());
  conn->to << WorkerProto::Op::VerifyStore << check_contents << repair;
  conn.processStderr();
  return read_int(conn->from);
}

void remote_store::addSignatures(const store_path_t& store_path, const string_set_t& sigs) {
  auto conn(getConnection());
  conn->to << WorkerProto::Op::AddSignatures;
  WorkerProto::write(*this, *conn, store_path);
  conn->to << sigs;
  conn.processStderr();
  read_int(conn->from);
}

MissingPaths remote_store::query_missing(const std::vector<derived_path_t>& targets) {
  {
    auto conn(getConnection());
    if (GET_PROTOCOL_MINOR(conn->protoVersion) < 19)
      // Don't hold the connection handle in the fallback case
      // to prevent a deadlock.
      goto fallback;
    conn->to << WorkerProto::Op::QueryMissing;
    WorkerProto::write(*this, *conn, targets);
    conn.processStderr();
    MissingPaths res;
    res.willBuild = WorkerProto::Serialise<store_path_set_t>::read(*this, *conn);
    res.willSubstitute = WorkerProto::Serialise<store_path_set_t>::read(*this, *conn);
    res.unknown = WorkerProto::Serialise<store_path_set_t>::read(*this, *conn);
    conn->from >> res.downloadSize >> res.nar_size;
    return res;
  }

fallback:
  return store_t::query_missing(targets);
}

void remote_store::addBuildLog(const store_path_t& drv_path, std::string_view log) {
  auto conn(getConnection());
  conn->to << WorkerProto::Op::AddBuildLog << drv_path.to_string();
  string_source_t source(log);
  conn.withFramedSink([&](sink_t& sink) { source.drain_into(sink); });
  read_int(conn->from);
}

std::vector<ActiveBuildInfo> remote_store::queryActiveBuilds() {
  auto conn(getConnection());
  if (!conn->features.count(WorkerProto::featureQueryActiveBuilds))
    throw Error("remote store does not support querying active builds");
  conn->to << WorkerProto::Op::QueryActiveBuilds;
  conn.processStderr();
  return nlohmann::json::parse(read_string(conn->from)).get<std::vector<ActiveBuildInfo>>();
}

std::optional<std::string> remote_store::getVersion() {
  auto conn(getConnection());
  return conn->daemonNixVersion;
}

void remote_store::connect() {
  auto conn(getConnection());
}

unsigned int remote_store::getProtocol() {
  auto conn(connections->get());
  return conn->protoVersion;
}

std::optional<TrustedFlag> remote_store::isTrustedClient() {
  auto conn(getConnection());
  return conn->remoteTrustsUs;
}

void remote_store::flushBadConnections() {
  connections->flushBad();
}

void remote_store::nar_from_path(const store_path_t& path, sink_t& sink) {
  auto conn(getConnection());
  conn->nar_from_path(*this, &conn.daemonException, path,
                      [&](source_t& source) { copy_nar(conn->from, sink); });
}

ref<RemoteFSAccessor> remote_store::getRemoteFSAccessor(bool require_valid_path) {
  return make_ref<RemoteFSAccessor>(ref<store_t>(shared_from_this()), require_valid_path);
}

ref<source_accessor_t> remote_store::getFSAccessor(bool require_valid_path) {
  return getRemoteFSAccessor(require_valid_path);
}

std::shared_ptr<source_accessor_t> remote_store::getFSAccessor(const store_path_t& path,
                                                               bool require_valid_path) {
  return getRemoteFSAccessor(require_valid_path)->accessObject(path);
}

void remote_store::ConnectionHandle::withFramedSink(std::function<void(sink_t& sink)> fun) {
  (*this)->to.flush();

  {
    framed_sink_t sink((*this)->to, [&]() {
      /* Periodically process stderr messages and exceptions
         from the daemon. */
      processStderr(nullptr, nullptr, false, false);
    });
    fun(sink);
    sink.flush();
  }

  processStderr(nullptr, nullptr, false);
}

} // namespace nix
