#include "nix/store/legacy-ssh-store.h"

#include "nix/store/build-result.h"
#include "nix/store/common-ssh-store-config.h"
#include "nix/store/derivations.h"
#include "nix/store/globals.h"
#include "nix/store/path-with-outputs.h"
#include "nix/store/remote-store.h"
#include "nix/store/serve-protocol-connection.h"
#include "nix/store/serve-protocol-impl.h"
#include "nix/store/serve-protocol.h"
#include "nix/store/ssh.h"
#include "nix/store/store-api.h"
#include "nix/store/store-registration.h"
#include "nix/util/archive.h"
#include "nix/util/callback.h"
#include "nix/util/pool.h"

namespace nix {

LegacySSHStoreConfig::LegacySSHStoreConfig(std::string_view scheme, std::string_view authority,
                                           const Params& params)
    : store_config_t(params),
      CommonSSHStoreConfig(scheme, parsed_url_t::authority_t::parse(authority), params) {}

std::string LegacySSHStoreConfig::doc() {
  return
#include "legacy-ssh-store.md"
      ;
}

struct LegacySSHStore::Connection : public ServeProto::BasicClientConnection {
  std::unique_ptr<SSHMaster::Connection> sshConn;
  bool good = true;
};

LegacySSHStore::LegacySSHStore(ref<const config_t> config)
    : store_t{*config},
      config{config},
      connections(make_ref<pool_t<Connection>>(
          std::max(1, (int)config->maxConnections), [this]() { return open_connection(); },
          [](const ref<Connection>& r) { return r->good; })),
      master(config->createSSHMaster(
          // Use SSH master only if using more than 1 connection.
          connections->capacity() > 1, config->logFD)) {}

ref<LegacySSHStore::Connection> LegacySSHStore::open_connection() {
  auto conn = make_ref<Connection>();
  strings_t command = config->remoteProgram.get();
  command.push_back("--serve");
  command.push_back("--write");
  if (config->remoteStore.get() != "") {
    command.push_back("--store");
    command.push_back(config->remoteStore.get());
  }
  conn->sshConn = master.startCommand(std::move(command), std::list{config->extraSshArgs});
  if (config->connPipeSize) {
    conn->sshConn->trySetBufferSize(*config->connPipeSize);
  }
  conn->to = fd_sink_t(conn->sshConn->in.get());
  conn->from = fd_source_t(conn->sshConn->out.get());

  string_sink_t saved;
  tee_source_t tee(conn->from, saved);
  try {
    conn->remoteVersion = ServeProto::BasicClientConnection::handshake(
        conn->to, tee, SERVE_PROTOCOL_VERSION, config->authority.host());
  } catch (SerialisationError& e) {
    // in.close(): Don't let the remote block on us not writing.
    conn->sshConn->in.close();
    {
      null_sink_t nullSink;
      tee.drain_into(nullSink);
    }
    throw Error("'nix-store --serve' protocol mismatch from '%s', got '%s'",
                config->authority.host(), chomp(saved.str()));
  } catch (EndOfFile& e) {
    throw Error("cannot connect to '%1%'", config->authority.host());
  }

  return conn;
};

StoreReference LegacySSHStoreConfig::getReference() const {
  return {
      .variant =
          StoreReference::Specified{
              .scheme = *uriSchemes().begin(),
              .authority = authority.to_string(),
          },
      .params = getQueryParams(),
  };
}

std::map<store_path_t, UnkeyedValidPathInfo>
LegacySSHStore::queryPathInfosUncached(const store_path_set_t& paths) {
  auto conn(connections->get());

  debug("querying remote host '%s' for info on '%s'", config->authority.host(),
        concat_strings_sep(", ", printStorePathSet(paths)));

  auto infos = conn->queryPathInfos(*this, paths);

  for (const auto& [_, info] : infos) {
    if (info.nar_hash == Hash::dummy)
      throw Error("NAR hash is now mandatory");
  }

  return infos;
}

void LegacySSHStore::query_path_info_uncached(
    const store_path_t& path,
    Callback<std::shared_ptr<const valid_path_info_t>> callback) noexcept {
  try {
    auto infos = queryPathInfosUncached({path});

    switch (infos.size()) {
      case 0:
        return callback(nullptr);
      case 1: {
        auto& [path2, info] = *infos.begin();

        assert(path == path2);
        return callback(std::make_shared<valid_path_info_t>(std::move(path), std::move(info)));
      }
      default:
        throw Error("More path infos returned than queried");
    }
  } catch (...) {
    callback.rethrow();
  }
}

void LegacySSHStore::add_to_store(const valid_path_info_t& info, source_t& source,
                                  RepairFlag repair, CheckSigsFlag check_sigs) {
  debug("adding path '%s' to remote host '%s'", printStorePath(info.path),
        config->authority.host());

  auto conn(connections->get());

  conn->to << ServeProto::command_t::AddToStoreNar << printStorePath(info.path)
           << (info.deriver ? printStorePath(*info.deriver) : "")
           << info.nar_hash.to_string(hash_format_t::base16, false);
  ServeProto::write(*this, *conn, info.references);
  conn->to << info.registrationTime << info.nar_size << info.ultimate << info.sigs
           << render_content_address(info.ca);
  try {
    copy_nar(source, conn->to);
  } catch (...) {
    conn->good = false;
    throw;
  }
  conn->to.flush();

  if (read_int(conn->from) != 1)
    throw Error("failed to add path '%s' to remote host '%s'", printStorePath(info.path),
                config->authority.host());
}

void LegacySSHStore::nar_from_path(const store_path_t& path, sink_t& sink) {
  nar_from_path(path, [&](auto& source) { copy_nar(source, sink); });
}

void LegacySSHStore::nar_from_path(const store_path_t& path, std::function<void(source_t&)> fun) {
  auto conn(connections->get());
  conn->nar_from_path(*this, path, fun);
}

static ServeProto::BuildOptions build_settings() {
  return {
      .max_silent_time = settings.max_silent_time,
      .buildTimeout = settings.buildTimeout,
      .maxLogSize = settings.maxLogSize,
      .nrRepeats = 0, // buildRepeat hasn't worked for ages anyway
      .enforceDeterminism = 0,
      .keep_failed = settings.keep_failed,
  };
}

build_result_t LegacySSHStore::buildDerivation(const store_path_t& drv_path,
                                               const basic_derivation_t& drv,
                                               BuildMode build_mode) {
  auto conn(connections->get());

  conn->putBuildDerivationRequest(*this, drv_path, drv, build_settings());

  return conn->getBuildDerivationResponse(*this);
}

std::function<build_result_t()>
LegacySSHStore::buildDerivationAsync(const store_path_t& drv_path, const basic_derivation_t& drv,
                                     const ServeProto::BuildOptions& options) {
  // Until we have C++23 std::move_only_function
  auto conn = std::make_shared<pool_t<Connection>::Handle>(connections->get());
  (*conn)->putBuildDerivationRequest(*this, drv_path, drv, options);

  return [this, conn]() -> build_result_t { return (*conn)->getBuildDerivationResponse(*this); };
}

void LegacySSHStore::build_paths(const std::vector<derived_path_t>& drv_paths, BuildMode build_mode,
                                 std::shared_ptr<store_t> eval_store) {
  if (eval_store && eval_store.get() != this)
    throw Error("building on an SSH store is incompatible with '--eval-store'");

  auto conn(connections->get());

  conn->to << ServeProto::command_t::BuildPaths;
  strings_t ss;
  for (auto& p : drv_paths) {
    auto sOrDrvPath = StorePathWithOutputs::tryFromDerivedPath(p);
    std::visit(overloaded{
                   [&](const StorePathWithOutputs& s) { ss.push_back(s.to_string(*this)); },
                   [&](const store_path_t& drv_path) {
                     throw Error("wanted to fetch '%s' but the legacy ssh protocol doesn't support "
                                 "merely substituting drv files via the build paths command. It "
                                 "would build them instead. Try using ssh-ng://",
                                 printStorePath(drv_path));
                   },
                   [&](std::monostate) {
                     throw Error("wanted build derivation that is itself a build product, but the "
                                 "legacy ssh protocol doesn't support that. Try using ssh-ng://");
                   },
               },
               sOrDrvPath);
  }
  conn->to << ss;

  ServeProto::write(*this, *conn, build_settings());

  conn->to.flush();

  auto status = read_int(conn->from);
  if (!build_result_t::Success::statusIs(status)) {
    build_result_t::Failure failure{
        .status = (build_result_t::Failure::Status)status,
    };
    conn->from >> failure.errorMsg;
    throw Error(failure.status, std::move(failure.errorMsg));
  }
}

void LegacySSHStore::computeFSClosure(const store_path_set_t& paths, store_path_set_t& out,
                                      bool flipDirection, bool includeOutputs,
                                      bool includeDerivers) {
  if (flipDirection || includeDerivers) {
    store_t::computeFSClosure(paths, out, flipDirection, includeOutputs, includeDerivers);
    return;
  }

  auto conn(connections->get());

  conn->to << ServeProto::command_t::QueryClosure << includeOutputs;
  ServeProto::write(*this, *conn, paths);
  conn->to.flush();

  for (auto& i : ServeProto::Serialise<store_path_set_t>::read(*this, *conn))
    out.insert(i);
}

store_path_set_t LegacySSHStore::queryValidPaths(const store_path_set_t& paths,
                                                 SubstituteFlag maybeSubstitute) {
  auto conn(connections->get());
  return conn->queryValidPaths(*this, false, paths, maybeSubstitute);
}

store_path_set_t LegacySSHStore::queryValidPaths(const store_path_set_t& paths, bool lock,
                                                 SubstituteFlag maybeSubstitute) {
  auto conn(connections->get());
  return conn->queryValidPaths(*this, lock, paths, maybeSubstitute);
}

void LegacySSHStore::connect() {
  auto conn(connections->get());
}

unsigned int LegacySSHStore::getProtocol() {
  auto conn(connections->get());
  return conn->remoteVersion;
}

::pid_t LegacySSHStore::get_connection_pid() {
  auto conn(connections->get());
#ifndef _WIN32
  return conn->sshConn->sshPid;
#else
  // TODO: Implement
  return 0;
#endif
}

LegacySSHStore::ConnectionStats LegacySSHStore::getConnectionStats() {
  auto conn(connections->get());
  return {
      .bytesReceived = conn->from.bytes_read(),
      .bytesSent = conn->to.written(),
  };
}

/**
 * The legacy ssh protocol doesn't support checking for trusted-user.
 * Try using ssh-ng:// instead if you want to know.
 */
std::optional<TrustedFlag> LegacySSHStore::isTrustedClient() {
  return std::nullopt;
}

ref<store_t> LegacySSHStore::config_t::open_store() const {
  return make_ref<LegacySSHStore>(ref{shared_from_this()});
}

static RegisterStoreImplementation<LegacySSHStore::config_t> reg_legacy_ssh_store;

} // namespace nix
