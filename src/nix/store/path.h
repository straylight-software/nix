#pragma once
///@file

#include <string_view>

#include "nix/util/hash.h"
#include "nix/util/json-impls.h"
#include "nix/util/json-non-null.h"
#include "nix/util/types.h"

namespace nix {

/**
 * Check whether a name is a valid store path name.
 *
 * @throws BadStorePathName if the name is invalid. The message is of the format "name %s is not
 * valid, for this specific reason".
 */
void check_name(std::string_view name);

/**
 * \ref store_path_t "store_t path" is the fundamental reference type of Nix.
 * A store paths refers to a store_t object.
 *
 * See store/store-path.html for more information on a
 * conceptual level.
 */
class store_path_t {
  std::string base_name;

public:
  /**
   * Size of the hash part of store paths, in base-32 characters.
   */
  constexpr static size_t HashLen = 32; // i.e. 160 bits

  constexpr static size_t MaxPathLen = 211;

  store_path_t() = delete;

  /** @throws BadStorePath */
  store_path_t(std::string_view base_name);

  /** @throws BadStorePath */
  store_path_t(const Hash& hash, std::string_view name);

  std::string_view to_string() const noexcept { return base_name; }

  bool operator==(const store_path_t& other) const noexcept = default;
  auto operator<=>(const store_path_t& other) const noexcept = default;

  /**
   * Check whether a file name ends with the extension for derivations.
   */
  bool is_derivation() const noexcept;

  /**
   * Throw an exception if `is_derivation` is false.
   */
  void requireDerivation() const;

  std::string_view name() const { return std::string_view(base_name).substr(HashLen + 1); }

  std::string_view hash_part() const { return std::string_view(base_name).substr(0, HashLen); }

  static store_path_t dummy;

  static store_path_t random(std::string_view name);
};

using store_path_set_t = std::set<store_path_t>;
using store_paths_t = std::vector<store_path_t>;

/**
 * The file extension of \ref nix::derivation_t derivations when serialized
 * into store objects.
 */
constexpr std::string_view drvExtension = ".drv";

template <>
struct json_avoids_null<store_path_t> : std::true_type {};

} // namespace nix

namespace std {

template <>
struct hash<nix::store_path_t> {
  std::size_t operator()(const nix::store_path_t& path) const noexcept {
    return *(std::size_t*)path.to_string().data();
  }
};

} // namespace std

namespace nix {

inline std::size_t hash_value(const store_path_t& path) {
  return std::hash<store_path_t>{}(path);
}

} // namespace nix

JSON_IMPL(nix::store_path_t)
