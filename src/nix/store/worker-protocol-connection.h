#pragma once
///@file

#include "nix/store/store-api.h"
#include "nix/store/worker-protocol.h"

namespace nix {

struct WorkerProto::BasicConnection {
  /**
   * Send with this.
   */
  fd_sink_t to;

  /**
   * Receive with this.
   */
  fd_source_t from;

  /**
   * The protocol version agreed by both sides.
   */
  WorkerProto::Version protoVersion;

  /**
   * The set of features that both sides support.
   */
  FeatureSet features;

  /**
   * Coercion to `WorkerProto::ReadConn`. This makes it easy to use the
   * factored out serve protocol serializers with a
   * `LegacySSHStore::Connection`.
   *
   * The serve protocol connection types are unidirectional, unlike
   * this type.
   */
  operator WorkerProto::ReadConn() {
    return WorkerProto::ReadConn{
        .from = from,
        .version = protoVersion,
    };
  }

  /**
   * Coercion to `WorkerProto::WriteConn`. This makes it easy to use the
   * factored out serve protocol serializers with a
   * `LegacySSHStore::Connection`.
   *
   * The serve protocol connection types are unidirectional, unlike
   * this type.
   */
  operator WorkerProto::WriteConn() {
    return WorkerProto::WriteConn{
        .to = to,
        .version = protoVersion,
    };
  }
};

struct WorkerProto::BasicClientConnection : WorkerProto::BasicConnection {
  /**
   * Flush to direction
   */
  virtual ~BasicClientConnection();

  virtual void closeWrite() = 0;

  std::exception_ptr processStderrReturn(sink_t* sink = 0, source_t* source = 0, bool flush = true,
                                         bool block = true);

  void processStderr(bool* daemonException, sink_t* sink = 0, source_t* source = 0, bool flush = true,
                     bool block = true);

  /**
   * Establishes connection, negotiating version.
   *
   * @return The minimum version supported by both sides and the set
   * of protocol features supported by both sides.
   *
   * @param to Taken by reference to allow for various error handling
   * mechanisms.
   *
   * @param from Taken by reference to allow for various error
   * handling mechanisms.
   *
   * @param localVersion Our version which is sent over.
   *
   * @param supportedFeatures The protocol features that we support.
   */
  // FIXME: this should probably be a constructor.
  static std::tuple<Version, FeatureSet> handshake(buffered_sink_t& to, source_t& from,
                                                   WorkerProto::Version localVersion,
                                                   const FeatureSet& supportedFeatures);

  /**
   * After calling handshake, must call this to exchange some basic
   * information about the connection.
   */
  ClientHandshakeInfo postHandshake(const store_dir_config_t& store);

  void addTempRoot(const store_dir_config_t& remoteStore, bool* daemonException, const store_path_t& path);

  store_path_set_t queryValidPaths(const store_dir_config_t& remoteStore, bool* daemonException,
                               const store_path_set_t& paths, SubstituteFlag maybeSubstitute);

  std::optional<UnkeyedValidPathInfo> queryPathInfo(const store_dir_config_t& store,
                                                    bool* daemonException, const store_path_t& path);

  void putBuildDerivationRequest(const store_dir_config_t& store, bool* daemonException,
                                 const store_path_t& drv_path, const basic_derivation_t& drv,
                                 BuildMode build_mode);

  /**
   * Get the response, must be paired with
   * `putBuildDerivationRequest`.
   */
  build_result_t getBuildDerivationResponse(const store_dir_config_t& store, bool* daemonException);

  void nar_from_path(const store_dir_config_t& store, bool* daemonException, const store_path_t& path,
                   std::function<void(source_t&)> fun);
};

struct WorkerProto::BasicServerConnection : WorkerProto::BasicConnection {
  /**
   * Establishes connection, negotiating version.
   *
   * @return The version provided by the other side of the
   * connection.
   *
   * @param to Taken by reference to allow for various error handling
   * mechanisms.
   *
   * @param from Taken by reference to allow for various error
   * handling mechanisms.
   *
   * @param localVersion Our version which is sent over.
   *
   * @param supportedFeatures The protocol features that we support.
   */
  // FIXME: this should probably be a constructor.
  static std::tuple<Version, FeatureSet> handshake(buffered_sink_t& to, source_t& from,
                                                   WorkerProto::Version localVersion,
                                                   const FeatureSet& supportedFeatures);

  /**
   * After calling handshake, must call this to exchange some basic
   * information about the connection.
   */
  void postHandshake(const store_dir_config_t& store, const ClientHandshakeInfo& info);
};

} // namespace nix
