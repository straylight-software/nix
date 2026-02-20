#pragma once
///@file

#include "nix/util/serialise.h"

namespace nix {

struct store_dir_config_t;
struct source_t;

// items being serialized
class store_path_t;
struct content_address_t;
struct DrvOutput;
struct realisation_t;

/**
 * Shared serializers between the worker protocol, serve protocol, and a
 * few others.
 *
 * This `struct` is basically just a `namespace`; We use a type rather
 * than a namespace just so we can use it as a template argument.
 */
struct CommonProto {
  /**
   * A unidirectional read connection, to be used by the read half of the
   * canonical serializers below.
   */
  struct ReadConn {
    source_t& from;
    bool shortStorePaths = false;
  };

  /**
   * A unidirectional write connection, to be used by the write half of the
   * canonical serializers below.
   */
  struct WriteConn {
    sink_t& to;
    bool shortStorePaths = false;
  };

  template <typename T>
  struct Serialise;

  /**
   * Wrapper function around `CommonProto::Serialise<T>::write` that allows us to
   * infer the type instead of having to write it down explicitly.
   */
  template <typename T>
  static void write(const store_dir_config_t& store, WriteConn conn, const T& t) {
    CommonProto::Serialise<T>::write(store, conn, t);
  }
};

#define DECLARE_COMMON_SERIALISER(T)                                                               \
  struct CommonProto::Serialise<T> {                                                               \
    static T read(const store_dir_config_t& store, CommonProto::ReadConn conn);                        \
    static void write(const store_dir_config_t& store, CommonProto::WriteConn conn, const T& str);     \
  }

template <>
DECLARE_COMMON_SERIALISER(std::string);
template <>
DECLARE_COMMON_SERIALISER(store_path_t);
template <>
DECLARE_COMMON_SERIALISER(content_address_t);
template <>
DECLARE_COMMON_SERIALISER(DrvOutput);
template <>
DECLARE_COMMON_SERIALISER(realisation_t);

#define COMMA_ ,
template <typename T>
DECLARE_COMMON_SERIALISER(std::vector<T>);
template <typename T, typename Compare>
DECLARE_COMMON_SERIALISER(std::set<T COMMA_ Compare>);
template <typename... Ts>
DECLARE_COMMON_SERIALISER(std::tuple<Ts...>);

template <typename K, typename V>
DECLARE_COMMON_SERIALISER(std::map<K COMMA_ V>);
#undef COMMA_

/**
 * These use the empty string for the null case, relying on the fact
 * that the underlying types never serialize to the empty string.
 *
 * We do this instead of a generic std::optional<T> instance because
 * ordinal tags (0 or 1, here) are a bit of a compatibility hazard. For
 * the same reason, we don't have a std::variant<T..> instances (ordinal
 * tags 0...n).
 *
 * We could the generic instances and then these as specializations for
 * compatibility, but that's proven a bit finnicky, and also makes the
 * worker protocol harder to implement in other languages where such
 * specializations may not be allowed.
 */
template <>
DECLARE_COMMON_SERIALISER(std::optional<store_path_t>);
template <>
DECLARE_COMMON_SERIALISER(std::optional<content_address_t>);

} // namespace nix
