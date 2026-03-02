#include "nix/store/worker-protocol-connection.h"

#include "nix/store/build-result.h"
#include "nix/store/derivations.h"
#include "nix/store/worker-protocol-impl.h"

namespace nix {

const WorkerProto::FeatureSet WorkerProto::allFeatures{
    {std::string(WorkerProto::featureQueryActiveBuilds)}};

WorkerProto::BasicClientConnection::~BasicClientConnection() {
  try {
    to.flush();
  } catch (...) {
    ignore_exception_in_destructor();
  }
}

static logger_t::fields_t read_fields(source_t& from) {
  logger_t::fields_t fields;
  size_t size = read_int(from);
  for (size_t n = 0; n < size; n++) {
    auto type = (decltype(logger_t::field_t::type_))read_int(from);
    if (type == logger_t::field_t::t_int) {
      fields.push_back(logger_t::field_t(read_num<uint64_t>(from)));
    } else if (type == logger_t::field_t::t_string) {
      fields.push_back(logger_t::field_t(read_string(from)));
    } else {
      throw Error("got unsupported field type %x from Nix daemon", (int)type);
    }
  }
  return fields;
}

std::exception_ptr WorkerProto::BasicClientConnection::processStderrReturn(sink_t* sink,
                                                                           source_t* source,
                                                                           bool flush, bool block) {
  if (flush) {
    to.flush();
  }

  std::exception_ptr ex;

  while (true) {
    if (!block && !from.has_data()) {
      break;
    }

    auto msg = read_num<uint64_t>(from);

    if (msg == STDERR_WRITE) {
      auto s = read_string(from);
      if (!sink) {
        throw Error("no sink");
      }
      (*sink)(s);
    }

    else if (msg == STDERR_READ) {
      if (!source) {
        throw Error("no source");
      }
      size_t len = read_num<size_t>(from);
      auto buf = std::make_unique<char[]>(len);
      write_string({(const char*)buf.get(), source->read(buf.get(), len)}, to);
      to.flush();
    }

    else if (msg == STDERR_ERROR) {
      if (GET_PROTOCOL_MINOR(protoVersion) >= 26) {
        ex = std::make_exception_ptr(read_error(from));
      } else {
        auto error = read_string(from);
        unsigned int status = read_int(from);
        ex = std::make_exception_ptr(Error(status, error));
      }
      break;
    }

    else if (msg == STDERR_NEXT) {
      printError(chomp(read_string(from)));
    }

    else if (msg == STDERR_START_ACTIVITY) {
      auto act = read_num<activity_id_t>(from);
      auto lvl = (verbosity_t)read_int(from);
      auto type = (activity_type_t)read_int(from);
      auto s = read_string(from);
      auto fields = read_fields(from);
      auto parent = read_num<activity_id_t>(from);
      logger->start_activity(act, lvl, type, s, fields, parent);
    }

    else if (msg == STDERR_STOP_ACTIVITY) {
      auto act = read_num<activity_id_t>(from);
      logger->stop_activity(act);
    }

    else if (msg == STDERR_RESULT) {
      auto act = read_num<activity_id_t>(from);
      auto type = (result_type_t)read_int(from);
      auto fields = read_fields(from);
      logger->result(act, type, fields);
    }

    else if (msg == STDERR_LAST) {
      assert(block);
      break;
    }

    else {
      throw Error("got unknown message type %x from Nix daemon", msg);
    }
  }

  if (!ex) {
    return ex;
  } else {
    try {
      std::rethrow_exception(ex);
    } catch (const Error& e) {
      // Nix versions before #4628 did not have an adequate
      // behavior for reporting that the derivation format was
      // upgraded. To avoid having to add compatibility logic in
      // many places, we expect to catch almost all occurrences of
      // the old incomprehensible error here, so that we can
      // explain to users what's going on when their daemon is
      // older than #4628 (2023).
      if (experimental_feature_settings.is_enabled(xp_t::dynamic_derivations) &&
          GET_PROTOCOL_MINOR(protoVersion) <= 35) {
        auto m = e.msg();
        if (m.find("parsing derivation") != std::string::npos &&
            m.find("expected string") != std::string::npos &&
            m.find("Derive([") != std::string::npos) {
          return std::make_exception_ptr(
              Error("%s, this might be because the daemon is too old to understand dependencies on "
                    "dynamic derivations. Check to see if the raw derivation is in the form '%s'",
                    std::move(m), "Drv WithVersion(..)"));
        }
      }
      return std::current_exception();
    }
  }
}

void WorkerProto::BasicClientConnection::processStderr(bool* daemonException, sink_t* sink,
                                                       source_t* source, bool flush, bool block) {
  auto ex = processStderrReturn(sink, source, flush, block);
  if (ex) {
    *daemonException = true;
    std::rethrow_exception(ex);
  }
}

static WorkerProto::FeatureSet intersect_features(const WorkerProto::FeatureSet& a,
                                                  const WorkerProto::FeatureSet& b) {
  WorkerProto::FeatureSet res;
  for (auto& x : a) {
    if (b.contains(x)) {
      res.insert(x);
    }
  }
  return res;
}

std::tuple<WorkerProto::Version, WorkerProto::FeatureSet>
WorkerProto::BasicClientConnection::handshake(buffered_sink_t& to, source_t& from,
                                              WorkerProto::Version localVersion,
                                              const WorkerProto::FeatureSet& supportedFeatures) {
  to << WORKER_MAGIC_1 << localVersion;
  to.flush();

  unsigned int magic = read_int(from);
  if (magic != WORKER_MAGIC_2) {
    throw Error("nix-daemon protocol mismatch from");
  }
  auto daemonVersion = read_int(from);

  if (GET_PROTOCOL_MAJOR(daemonVersion) != GET_PROTOCOL_MAJOR(PROTOCOL_VERSION)) {
    throw Error("Nix daemon protocol version not supported");
  }
  if (GET_PROTOCOL_MINOR(daemonVersion) < 10) {
    throw Error("the Nix daemon version is too old");
  }

  auto protoVersion = std::min(daemonVersion, localVersion);

  /* Exchange features. */
  WorkerProto::FeatureSet daemonFeatures;
  if (GET_PROTOCOL_MINOR(protoVersion) >= 38) {
    to << supportedFeatures;
    to.flush();
    daemonFeatures = read_strings<WorkerProto::FeatureSet>(from);
  }

  return {protoVersion, intersect_features(daemonFeatures, supportedFeatures)};
}

std::tuple<WorkerProto::Version, WorkerProto::FeatureSet>
WorkerProto::BasicServerConnection::handshake(buffered_sink_t& to, source_t& from,
                                              WorkerProto::Version localVersion,
                                              const WorkerProto::FeatureSet& supportedFeatures) {
  unsigned int magic = read_int(from);
  if (magic != WORKER_MAGIC_1) {
    throw Error("protocol mismatch");
  }
  to << WORKER_MAGIC_2 << localVersion;
  to.flush();
  auto client_version = read_int(from);

  auto protoVersion = std::min(client_version, localVersion);

  /* Exchange features. */
  WorkerProto::FeatureSet clientFeatures;
  if (GET_PROTOCOL_MINOR(protoVersion) >= 38) {
    clientFeatures = read_strings<WorkerProto::FeatureSet>(from);
    to << supportedFeatures;
    to.flush();
  }

  return {protoVersion, intersect_features(clientFeatures, supportedFeatures)};
}

WorkerProto::ClientHandshakeInfo
WorkerProto::BasicClientConnection::postHandshake(const store_dir_config_t& store) {
  WorkerProto::ClientHandshakeInfo res;

  if (GET_PROTOCOL_MINOR(protoVersion) >= 14) {
    // Obsolete CPU affinity.
    to << 0;
  }

  if (GET_PROTOCOL_MINOR(protoVersion) >= 11) {
    to << false; // obsolete reserveSpace
  }

  if (GET_PROTOCOL_MINOR(protoVersion) >= 33) {
    to.flush();
  }

  return WorkerProto::Serialise<ClientHandshakeInfo>::read(store, *this);
}

void WorkerProto::BasicServerConnection::postHandshake(const store_dir_config_t& store,
                                                       const ClientHandshakeInfo& info) {
  if (GET_PROTOCOL_MINOR(protoVersion) >= 14 && read_int(from)) {
    // Obsolete CPU affinity.
    read_int(from);
  }

  if (GET_PROTOCOL_MINOR(protoVersion) >= 11) {
    read_int(from); // obsolete reserveSpace
  }

  WorkerProto::write(store, *this, info);
}

std::optional<UnkeyedValidPathInfo>
WorkerProto::BasicClientConnection::queryPathInfo(const store_dir_config_t& store,
                                                  bool* daemonException, const store_path_t& path) {
  to << WorkerProto::Op::QueryPathInfo << store.printStorePath(path);
  try {
    processStderr(daemonException);
  } catch (Error& e) {
    // Ugly backwards compatibility hack.
    if (e.msg().find("is not valid") != std::string::npos) {
      return std::nullopt;
    }
    throw;
  }
  if (GET_PROTOCOL_MINOR(protoVersion) >= 17) {
    bool valid;
    from >> valid;
    if (!valid) {
      return std::nullopt;
    }
  }
  return WorkerProto::Serialise<UnkeyedValidPathInfo>::read(store, *this);
}

store_path_set_t WorkerProto::BasicClientConnection::queryValidPaths(
    const store_dir_config_t& store, bool* daemonException, const store_path_set_t& paths,
    SubstituteFlag maybeSubstitute) {
  assert(GET_PROTOCOL_MINOR(protoVersion) >= 12);
  to << WorkerProto::Op::QueryValidPaths;
  WorkerProto::write(store, *this, paths);
  if (GET_PROTOCOL_MINOR(protoVersion) >= 27) {
    to << maybeSubstitute;
  }
  processStderr(daemonException);
  return WorkerProto::Serialise<store_path_set_t>::read(store, *this);
}

void WorkerProto::BasicClientConnection::addTempRoot(const store_dir_config_t& store,
                                                     bool* daemonException,
                                                     const store_path_t& path) {
  to << WorkerProto::Op::AddTempRoot << store.printStorePath(path);
  processStderr(daemonException);
  read_int(from);
}

void WorkerProto::BasicClientConnection::putBuildDerivationRequest(const store_dir_config_t& store,
                                                                   bool* daemonException,
                                                                   const store_path_t& drv_path,
                                                                   const basic_derivation_t& drv,
                                                                   BuildMode build_mode) {
  to << WorkerProto::Op::BuildDerivation << store.printStorePath(drv_path);
  write_derivation(to, store, drv);
  to << build_mode;
}

build_result_t
WorkerProto::BasicClientConnection::getBuildDerivationResponse(const store_dir_config_t& store,
                                                               bool* daemonException) {
  return WorkerProto::Serialise<build_result_t>::read(store, *this);
}

void WorkerProto::BasicClientConnection::nar_from_path(const store_dir_config_t& store,
                                                       bool* daemonException,
                                                       const store_path_t& path,
                                                       std::function<void(source_t&)> fun) {
  to << WorkerProto::Op::NarFromPath << store.printStorePath(path);
  processStderr(daemonException);

  fun(from);
}

} // namespace nix
