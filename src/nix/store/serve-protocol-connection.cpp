#include "nix/store/serve-protocol-connection.h"

#include "nix/store/build-result.h"
#include "nix/store/derivations.h"
#include "nix/store/serve-protocol-impl.h"

namespace nix {

ServeProto::Version ServeProto::BasicClientConnection::handshake(buffered_sink_t& to, Source& from,
                                                                 ServeProto::Version localVersion,
                                                                 std::string_view host) {
  to << SERVE_MAGIC_1 << localVersion;
  to.flush();

  unsigned int magic = read_int(from);
  if (magic != SERVE_MAGIC_2)
    throw Error("'nix-store --serve' protocol mismatch from '%s'", host);
  auto remoteVersion = read_int(from);
  if (GET_PROTOCOL_MAJOR(remoteVersion) != 0x200 || GET_PROTOCOL_MINOR(remoteVersion) < 5)
    throw Error("unsupported 'nix-store --serve' protocol version on '%s'", host);
  return std::min(remoteVersion, localVersion);
}

ServeProto::Version ServeProto::BasicServerConnection::handshake(buffered_sink_t& to, Source& from,
                                                                 ServeProto::Version localVersion) {
  unsigned int magic = read_int(from);
  if (magic != SERVE_MAGIC_1)
    throw Error("protocol mismatch");
  to << SERVE_MAGIC_2 << localVersion;
  to.flush();
  auto remoteVersion = read_int(from);
  return std::min(remoteVersion, localVersion);
}

StorePathSet ServeProto::BasicClientConnection::queryValidPaths(const StoreDirConfig& store,
                                                                bool lock,
                                                                const StorePathSet& paths,
                                                                SubstituteFlag maybeSubstitute) {
  to << ServeProto::command_t::QueryValidPaths << lock << maybeSubstitute;
  write(store, *this, paths);
  to.flush();

  return Serialise<StorePathSet>::read(store, *this);
}

std::map<StorePath, UnkeyedValidPathInfo>
ServeProto::BasicClientConnection::queryPathInfos(const StoreDirConfig& store,
                                                  const StorePathSet& paths) {
  std::map<StorePath, UnkeyedValidPathInfo> infos;

  to << ServeProto::command_t::QueryPathInfos;
  ServeProto::write(store, *this, paths);
  to.flush();

  while (true) {
    auto store_path_s = read_string(from);
    if (store_path_s == "")
      break;

    auto store_path = store.parseStorePath(store_path_s);
    assert(paths.count(store_path) == 1);
    auto info = ServeProto::Serialise<UnkeyedValidPathInfo>::read(store, *this);
    infos.insert_or_assign(std::move(store_path), std::move(info));
  }

  return infos;
}

void ServeProto::BasicClientConnection::putBuildDerivationRequest(
    const StoreDirConfig& store, const StorePath& drv_path, const BasicDerivation& drv,
    const ServeProto::BuildOptions& options) {
  to << ServeProto::command_t::BuildDerivation << store.printStorePath(drv_path);
  write_derivation(to, store, drv);

  ServeProto::write(store, *this, options);

  to.flush();
}

BuildResult
ServeProto::BasicClientConnection::getBuildDerivationResponse(const StoreDirConfig& store) {
  return ServeProto::Serialise<BuildResult>::read(store, *this);
}

void ServeProto::BasicClientConnection::nar_from_path(const StoreDirConfig& store,
                                                    const StorePath& path,
                                                    std::function<void(Source&)> fun) {
  to << ServeProto::command_t::DumpStorePath << store.printStorePath(path);
  to.flush();

  fun(from);
}

void ServeProto::BasicClientConnection::import_paths(const StoreDirConfig& store,
                                                    std::function<void(Sink&)> fun) {
  to << ServeProto::command_t::ImportPaths;
  fun(to);
  to.flush();

  if (read_int(from) != 1)
    throw Error("remote machine failed to import closure");
}

} // namespace nix
