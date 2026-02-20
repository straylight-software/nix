#pragma once
///@file

#include <optional>
#include <string>

#include "nix/store/content-address.h"
#include "nix/store/path.h"
#include "nix/util/hash.h"
#include "nix/util/signature/signer.h"

namespace nix {

class store_t;
struct store_dir_config_t;

/**
 * JSON format version for path info output.
 */
enum class PathInfoJsonFormat {
  /// Legacy format with string hashes and full store paths
  V1 = 1,
  /// New format with structured hashes and store path base names
  V2 = 2,
};

/**
 * Convert an integer version number to PathInfoJsonFormat.
 * Throws Error if the version is not supported.
 */
PathInfoJsonFormat parse_path_info_json_format(uint64_t version);

struct SubstitutablePathInfo {
  std::optional<store_path_t> deriver;
  store_path_set_t references;
  /**
   * 0 = unknown or inapplicable
   */
  uint64_t downloadSize;
  /**
   * 0 = unknown
   */
  uint64_t nar_size;
};

using SubstitutablePathInfos = std::map<store_path_t, SubstitutablePathInfo>;

/**
 * Information about a store object.
 *
 * See `store/store-object` and `protocols/json/store-object-info` in
 * the Nix manual
 */
struct UnkeyedValidPathInfo {
  /**
   * The store directory this store object belongs to.
   *
   * This supports relocatable store objects where different objects
   * may have different store directories.
   */
  std::string store_dir;

  /**
   * Path to derivation that produced this store object, if known.
   */
  std::optional<store_path_t> deriver;

  /**
   * \todo document this
   */
  Hash nar_hash;

  /**
   * Other store objects this store object refers to.
   */
  store_path_set_t references;

  /**
   * When this store object was registered in the store that contains
   * it, if known.
   */
  time_t registrationTime = 0;

  /**
   * 0 = unknown
   */
  uint64_t nar_size = 0;

  /**
   * internal use only: SQL primary key for on-disk store objects with
   * `LocalStore`.
   *
   * @todo Remove, layer violation
   */
  uint64_t id = 0;

  /**
   * Whether the path is ultimately trusted, that is, it's a
   * derivation output that was built locally.
   */
  bool ultimate = false;

  string_set_t sigs; // note: not necessarily verified

  /**
   * If non-empty, an assertion that the path is content-addressed,
   * i.e., that the store path is computed from a cryptographic hash
   * of the contents of the path, plus some other bits of data like
   * the "name" part of the path. Such a path doesn't need
   * signatures, since we don't have to trust anybody's claim that
   * the path is the output of a particular derivation. (In the
   * extensional store model, we have to trust that the *contents*
   * of an output path of a derivation were actually produced by
   * that derivation. In the intensional model, we have to trust
   * that a particular output path was produced by a derivation; the
   * path then implies the contents.)
   *
   * Ideally, the content-addressability assertion would just be a Boolean,
   * and the store path would be computed from the name component, 'narHash'
   * and 'references'. However, we support many types of content addresses.
   */
  std::optional<content_address_t> ca;

  UnkeyedValidPathInfo(const UnkeyedValidPathInfo& other) = default;

  UnkeyedValidPathInfo(const store_dir_config_t& store, Hash nar_hash);

  UnkeyedValidPathInfo(std::string store_dir, Hash nar_hash)
      : store_dir(std::move(store_dir)), nar_hash(std::move(nar_hash)) {}

  bool operator==(const UnkeyedValidPathInfo&) const noexcept;

  /**
   * @todo return `std::strong_ordering` once `id` is removed
   */
  std::weak_ordering operator<=>(const UnkeyedValidPathInfo&) const noexcept;

  virtual ~UnkeyedValidPathInfo() {}

  /**
   * @param store If non-null, store paths are rendered as full paths.
   *              If null, store paths are rendered as base names.
   * @param includeImpureInfo If true, variable elements such as the
   *                          registration time are included.
   * @param format JSON format version. Version 1 uses string hashes and
   *               string content addresses. Version 2 uses structured
   *               hashes and structured content addresses.
   */
  virtual nlohmann::json to_json(const store_dir_config_t* store, bool includeImpureInfo,
                                 PathInfoJsonFormat format) const;
  static UnkeyedValidPathInfo from_json(const store_dir_config_t* store,
                                        const nlohmann::json& json);
};

struct valid_path_info_t : virtual UnkeyedValidPathInfo {
  store_path_t path;

  bool operator==(const valid_path_info_t&) const = default;
  auto operator<=>(const valid_path_info_t&) const = default;

  /**
   * Return a fingerprint of the store path to be used in binary
   * cache signatures. It contains the store path, the base-32
   * SHA-256 hash of the NAR serialisation of the path, the size of
   * the NAR, and the sorted references. The size field is strictly
   * speaking superfluous, but might prevent endless/excessive data
   * attacks.
   */
  std::string fingerprint(const store_dir_config_t& store) const;

  void sign(const store_t& store, const signer_t& signer);
  void sign(const store_t& store, const std::vector<std::unique_ptr<signer_t>>& signers);

  /**
   * @return The `ContentAddressWithReferences` that determines the
   * store path for a content-addressed store object, `std::nullopt`
   * for an input-addressed store object.
   */
  std::optional<ContentAddressWithReferences> contentAddressWithReferences() const;

  /**
   * @return true iff the path is verifiably content-addressed.
   */
  bool isContentAddressed(const store_dir_config_t& store) const;

  static const size_t maxSigs = std::numeric_limits<size_t>::max();

  /**
   * Return the number of signatures on this .narinfo that were
   * produced by one of the specified keys, or maxSigs if the path
   * is content-addressed.
   */
  size_t checkSignatures(const store_dir_config_t& store, const public_keys_t& public_keys) const;

  /**
   * Verify a single signature.
   */
  bool checkSignature(const store_dir_config_t& store, const public_keys_t& public_keys,
                      const std::string& sig) const;

  /**
   * References as store path basenames, including a self reference if it has one.
   */
  strings_t shortRefs() const;

  valid_path_info_t(store_path_t&& path, UnkeyedValidPathInfo info)
      : UnkeyedValidPathInfo(info), path(std::move(path)) {}

  valid_path_info_t(const store_path_t& path, UnkeyedValidPathInfo info)
      : valid_path_info_t(store_path_t{path}, std::move(info)) {}

  static valid_path_info_t makeFromCA(const store_dir_config_t& store, std::string_view name,
                                      ContentAddressWithReferences&& ca, Hash nar_hash);
};

static_assert(std::is_move_assignable_v<valid_path_info_t>);
static_assert(std::is_copy_assignable_v<valid_path_info_t>);
static_assert(std::is_copy_constructible_v<valid_path_info_t>);
static_assert(std::is_move_constructible_v<valid_path_info_t>);

using ValidPathInfos = std::map<store_path_t, valid_path_info_t>;

} // namespace nix

JSON_IMPL(nix::PathInfoJsonFormat)
JSON_IMPL(nix::UnkeyedValidPathInfo)
JSON_IMPL(nix::valid_path_info_t)
