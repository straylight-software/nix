#pragma once
///@file

#include <string_view>

#include "nix/util/json-impls.h"
#include "nix/util/json-non-null.h"
#include "nix/util/types.h"

namespace nix {

struct Hash;

/**
 * Check whether a name is a valid store path name.
 *
 * @throws BadStorePathName if the name is invalid. The message is of the format "name %s is not
 * valid, for this specific reason".
 */
void check_name(std::string_view name);

/**
 * \ref StorePath "Store path" is the fundamental reference type of Nix.
 * A store paths refers to a Store object.
 *
 * See store/store-path.html for more information on a
 * conceptual level.
 */
class StorePath {
  std::string base_name;

public:
  /**
   * Size of the hash part of store paths, in base-32 characters.
   */
  constexpr static size_t HashLen = 32; // i.e. 160 bits

  constexpr static size_t MaxPathLen = 211;

  StorePath() = delete;

  /** @throws BadStorePath */
  StorePath(std::string_view base_name);

  /** @throws BadStorePath */
  StorePath(const Hash& hash, std::string_view name);

  std::string_view to_string() const noexcept { return base_name; }

  bool operator==(const StorePath& other) const noexcept = default;
  auto operator<=>(const StorePath& other) const noexcept = default;

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

  static StorePath dummy;

  static StorePath random(std::string_view name);
};

typedef std::set<StorePath> StorePathSet;
typedef std::vector<StorePath> StorePaths;

/**
 * The file extension of \ref nix::Derivation derivations when serialized
 * into store objects.
 */
constexpr std::string_view drvExtension = ".drv";

template <>
struct json_avoids_null<StorePath> : std::true_type {};

} // namespace nix

namespace std {

template <>
struct hash<nix::StorePath> {
  std::size_t operator()(const nix::StorePath& path) const noexcept {
    return *(std::size_t*)path.to_string().data();
  }
};

} // namespace std

namespace nix {

inline std::size_t hash_value(const StorePath& path) {
  return std::hash<StorePath>{}(path);
}

} // namespace nix

JSON_IMPL(nix::StorePath)
