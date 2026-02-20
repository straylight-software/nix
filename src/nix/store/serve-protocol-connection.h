#pragma once
///@file

#include "nix/store/serve-protocol.h"
#include "nix/store/store-api.h"

namespace nix {

struct ServeProto::BasicClientConnection {
  fd_sink_t to;
  fd_source_t from;
  ServeProto::Version remoteVersion;

  /**
   * Establishes connection, negotiating version.
   *
   * @return the version provided by the other side of the
   * connection.
   *
   * @param to Taken by reference to allow for various error handling
   * mechanisms.
   *
   * @param from Taken by reference to allow for various error
   * handling mechanisms.
   *
   * @param localVersion Our version which is sent over
   *
   * @param host Just used to add context to thrown exceptions.
   */
  static ServeProto::Version handshake(buffered_sink_t& to, source_t& from,
                                       ServeProto::Version localVersion, std::string_view host);

  /**
   * Coercion to `ServeProto::ReadConn`. This makes it easy to use the
   * factored out serve protocol serializers with a
   * `LegacySSHStore::Connection`.
   *
   * The serve protocol connection types are unidirectional, unlike
   * this type.
   */
  operator ServeProto::ReadConn() {
    return ServeProto::ReadConn{
        .from = from,
        .version = remoteVersion,
    };
  }

  /**
   * Coercion to `ServeProto::WriteConn`. This makes it easy to use the
   * factored out serve protocol serializers with a
   * `LegacySSHStore::Connection`.
   *
   * The serve protocol connection types are unidirectional, unlike
   * this type.
   */
  operator ServeProto::WriteConn() {
    return ServeProto::WriteConn{
        .to = to,
        .version = remoteVersion,
    };
  }

  store_path_set_t queryValidPaths(const store_dir_config_t& remoteStore, bool lock,
                                   const store_path_set_t& paths, SubstituteFlag maybeSubstitute);

  std::map<store_path_t, UnkeyedValidPathInfo> queryPathInfos(const store_dir_config_t& store,
                                                              const store_path_set_t& paths);
  ;

  void putBuildDerivationRequest(const store_dir_config_t& store, const store_path_t& drv_path,
                                 const basic_derivation_t& drv,
                                 const ServeProto::BuildOptions& options);

  /**
   * Get the response, must be paired with
   * `putBuildDerivationRequest`.
   */
  build_result_t getBuildDerivationResponse(const store_dir_config_t& store);

  void nar_from_path(const store_dir_config_t& store, const store_path_t& path,
                     std::function<void(source_t&)> fun);

  void import_paths(const store_dir_config_t& store, std::function<void(sink_t&)> fun);
};

struct ServeProto::BasicServerConnection {
  /**
   * Establishes connection, negotiating version.
   *
   * @return the version provided by the other side of the
   * connection.
   *
   * @param to Taken by reference to allow for various error handling
   * mechanisms.
   *
   * @param from Taken by reference to allow for various error
   * handling mechanisms.
   *
   * @param localVersion Our version which is sent over
   */
  static ServeProto::Version handshake(buffered_sink_t& to, source_t& from,
                                       ServeProto::Version localVersion);
};

} // namespace nix
