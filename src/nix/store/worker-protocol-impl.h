#pragma once
/**
 * @file
 *
 * Template implementations (as opposed to mere declarations).
 *
 * This file is an example of the "impl.h" pattern. See the
 * contributing guide.
 */

#include "nix/store/length-prefixed-protocol-helper.h"
#include "nix/store/worker-protocol.h"

namespace nix {

/* protocol-agnostic templates */

#define WORKER_USE_LENGTH_PREFIX_SERIALISER(TEMPLATE, T)                                           \
  TEMPLATE T WorkerProto::Serialise<T>::read(const store_dir_config_t& store,                          \
                                             WorkerProto::ReadConn conn) {                         \
    return LengthPrefixedProtoHelper<WorkerProto, T>::read(store, conn);                           \
  }                                                                                                \
  TEMPLATE void WorkerProto::Serialise<T>::write(const store_dir_config_t& store,                      \
                                                 WorkerProto::WriteConn conn, const T& t) {        \
    LengthPrefixedProtoHelper<WorkerProto, T>::write(store, conn, t);                              \
  }

WORKER_USE_LENGTH_PREFIX_SERIALISER(template <typename T>, std::vector<T>)
#define COMMA_ ,
WORKER_USE_LENGTH_PREFIX_SERIALISER(template <typename T COMMA_ typename Compare>,
                                    std::set<T COMMA_ Compare>)
#undef COMMA_
WORKER_USE_LENGTH_PREFIX_SERIALISER(template <typename... Ts>, std::tuple<Ts...>)

#define WORKER_USE_LENGTH_PREFIX_SERIALISER_COMMA ,
WORKER_USE_LENGTH_PREFIX_SERIALISER(
    template <typename K WORKER_USE_LENGTH_PREFIX_SERIALISER_COMMA typename V>,
    std::map<K WORKER_USE_LENGTH_PREFIX_SERIALISER_COMMA V>)

/**
 * use `CommonProto` where possible.
 */
template <typename T>
struct WorkerProto::Serialise {
  static T read(const store_dir_config_t& store, WorkerProto::ReadConn conn) {
    return CommonProto::Serialise<T>::read(
        store, CommonProto::ReadConn{.from = conn.from, .shortStorePaths = conn.shortStorePaths});
  }

  static void write(const store_dir_config_t& store, WorkerProto::WriteConn conn, const T& t) {
    CommonProto::Serialise<T>::write(
        store, CommonProto::WriteConn{.to = conn.to, .shortStorePaths = conn.shortStorePaths}, t);
  }
};

/* protocol-specific templates */

} // namespace nix
