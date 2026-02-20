#pragma once
///@file

#include <variant>

#include <nlohmann/json_fwd.hpp>

#include "nix/store/outputs-spec.h"
#include "nix/store/path.h"
#include "nix/util/configuration.h"
#include "nix/util/json-impls.h"
#include "nix/util/ref.h"

namespace nix {

struct store_dir_config_t;

/**
 * An opaque derived path.
 *
 * opaque_t derived paths are just store paths, and fully evaluated. They
 * cannot be simplified further. Since they are opaque, they cannot be
 * built, but they can fetched.
 */
struct DerivedPathOpaque {
  store_path_t path;

  std::string to_string(const store_dir_config_t& store) const;
  static DerivedPathOpaque parse(const store_dir_config_t& store, std::string_view);

  bool operator==(const DerivedPathOpaque&) const = default;
  auto operator<=>(const DerivedPathOpaque&) const = default;
};

struct SingleDerivedPath;

/**
 * A single derived path that is built from a derivation
 *
 * Built derived paths are pair of a derivation and an output name. They are
 * evaluated by building the derivation, and then taking the resulting output
 * path of the given output name.
 */
struct SingleDerivedPathBuilt {
  ref<const SingleDerivedPath> drv_path;
  OutputName output;

  /**
   * Get the store path this is ultimately derived from (by realising
   * and projecting outputs).
   *
   * Note that this is *not* a property of the store object being
   * referred to, but just of this path --- how we happened to be
   * referring to that store object. In other words, this means this
   * function breaks "referential transparency". It should therefore
   * be used only with great care.
   */
  const store_path_t& getBaseStorePath() const;

  /**
   * Uses `^` as the separator
   */
  std::string to_string(const store_dir_config_t& store) const;
  /**
   * Uses `!` as the separator
   */
  std::string to_string_legacy(const store_dir_config_t& store) const;
  /**
   * The caller splits on the separator, so it works for both variants.
   *
   * @param xp_settings Stop-gap to avoid globals during unit tests.
   */
  static SingleDerivedPathBuilt
  parse(const store_dir_config_t& store, ref<const SingleDerivedPath> drv_path,
        OutputNameView outputs,
        const experimental_feature_settings_t& xp_settings = experimental_feature_settings);

  bool operator==(const SingleDerivedPathBuilt&) const noexcept;
  std::strong_ordering operator<=>(const SingleDerivedPathBuilt&) const noexcept;
};

using _SingleDerivedPathRaw = std::variant<DerivedPathOpaque, SingleDerivedPathBuilt>;

/**
 * A "derived path" is a very simple sort of expression (not a Nix
 * language expression! But an expression in a the general sense) that
 * evaluates to (concrete) store path. It is either:
 *
 * - opaque, in which case it is just a concrete store path with
 *   possibly no known derivation
 *
 * - built, in which case it is a pair of a derivation path and an
 *   output name.
 */
struct SingleDerivedPath : _SingleDerivedPathRaw {
  using raw_t = _SingleDerivedPathRaw;
  using raw_t::raw_t;

  using opaque_t = DerivedPathOpaque;
  using Built = SingleDerivedPathBuilt;

  inline const raw_t& raw() const { return static_cast<const raw_t&>(*this); }

  bool operator==(const SingleDerivedPath&) const = default;
  auto operator<=>(const SingleDerivedPath&) const = default;

  /**
   * Get the store path this is ultimately derived from (by realising
   * and projecting outputs).
   *
   * Note that this is *not* a property of the store object being
   * referred to, but just of this path --- how we happened to be
   * referring to that store object. In other words, this means this
   * function breaks "referential transparency". It should therefore
   * be used only with great care.
   */
  const store_path_t& getBaseStorePath() const;

  /**
   * Uses `^` as the separator
   */
  std::string to_string(const store_dir_config_t& store) const;
  /**
   * Uses `!` as the separator
   */
  std::string to_string_legacy(const store_dir_config_t& store) const;
  /**
   * Uses `^` as the separator
   *
   * @param xp_settings Stop-gap to avoid globals during unit tests.
   */
  static SingleDerivedPath
  parse(const store_dir_config_t& store, std::string_view,
        const experimental_feature_settings_t& xp_settings = experimental_feature_settings);
  /**
   * Uses `!` as the separator
   *
   * @param xp_settings Stop-gap to avoid globals during unit tests.
   */
  static SingleDerivedPath
  parseLegacy(const store_dir_config_t& store, std::string_view,
              const experimental_feature_settings_t& xp_settings = experimental_feature_settings);
};

static inline ref<SingleDerivedPath> makeConstantStorePathRef(store_path_t drv_path) {
  return make_ref<SingleDerivedPath>(SingleDerivedPath::opaque_t{drv_path});
}

/**
 * A set of derived paths that are built from a derivation
 *
 * Built derived paths are pair of a derivation and some output names.
 * They are evaluated by building the derivation, and then replacing the
 * output names with the resulting outputs.
 *
 * Note that does mean a derived store paths evaluates to multiple
 * opaque paths, which is sort of icky as expressions are supposed to
 * evaluate to single values. Perhaps this should have just a single
 * output name.
 */
struct DerivedPathBuilt {
  ref<const SingleDerivedPath> drv_path;
  OutputsSpec outputs;

  /**
   * Get the store path this is ultimately derived from (by realising
   * and projecting outputs).
   *
   * Note that this is *not* a property of the store object being
   * referred to, but just of this path --- how we happened to be
   * referring to that store object. In other words, this means this
   * function breaks "referential transparency". It should therefore
   * be used only with great care.
   */
  const store_path_t& getBaseStorePath() const;

  /**
   * Uses `^` as the separator
   */
  std::string to_string(const store_dir_config_t& store) const;
  /**
   * Uses `!` as the separator
   */
  std::string to_string_legacy(const store_dir_config_t& store) const;
  /**
   * The caller splits on the separator, so it works for both variants.
   *
   * @param xp_settings Stop-gap to avoid globals during unit tests.
   */
  static DerivedPathBuilt
  parse(const store_dir_config_t& store, ref<const SingleDerivedPath>, std::string_view,
        const experimental_feature_settings_t& xp_settings = experimental_feature_settings);

  bool operator==(const DerivedPathBuilt&) const noexcept;
  // TODO libc++ 16 (used by darwin) missing `std::set::operator <=>`, can't do yet.
  bool operator<(const DerivedPathBuilt&) const noexcept;
};

using _DerivedPathRaw = std::variant<DerivedPathOpaque, DerivedPathBuilt>;

/**
 * A "derived path" is a very simple sort of expression that evaluates
 * to one or more (concrete) store paths. It is either:
 *
 * - opaque, in which case it is just a single concrete store path with
 *   possibly no known derivation
 *
 * - built, in which case it is a pair of a derivation path and some
 *   output names.
 */
struct derived_path_t : _DerivedPathRaw {
  using raw_t = _DerivedPathRaw;
  using raw_t::raw_t;

  using opaque_t = DerivedPathOpaque;
  using Built = DerivedPathBuilt;

  inline const raw_t& raw() const { return static_cast<const raw_t&>(*this); }

  /**
   * Get the store path this is ultimately derived from (by realising
   * and projecting outputs).
   *
   * Note that this is *not* a property of the store object being
   * referred to, but just of this path --- how we happened to be
   * referring to that store object. In other words, this means this
   * function breaks "referential transparency". It should therefore
   * be used only with great care.
   */
  const store_path_t& getBaseStorePath() const;

  /**
   * Uses `^` as the separator
   */
  std::string to_string(const store_dir_config_t& store) const;
  /**
   * Uses `!` as the separator
   */
  std::string to_string_legacy(const store_dir_config_t& store) const;
  /**
   * Uses `^` as the separator
   *
   * @param xp_settings Stop-gap to avoid globals during unit tests.
   */
  static derived_path_t
  parse(const store_dir_config_t& store, std::string_view,
        const experimental_feature_settings_t& xp_settings = experimental_feature_settings);
  /**
   * Uses `!` as the separator
   *
   * @param xp_settings Stop-gap to avoid globals during unit tests.
   */
  static derived_path_t
  parseLegacy(const store_dir_config_t& store, std::string_view,
              const experimental_feature_settings_t& xp_settings = experimental_feature_settings);

  /**
   * Convert a `SingleDerivedPath` to a `derived_path_t`.
   */
  static derived_path_t fromSingle(const SingleDerivedPath&);
};

using DerivedPaths = std::vector<derived_path_t>;

/**
 * Used by various parser functions to require experimental features as
 * needed.
 *
 * Somewhat unfortunate this cannot just be an implementation detail for
 * this module.
 *
 * @param xp_settings Stop-gap to avoid globals during unit tests.
 */
void drv_require_experiment(
    const SingleDerivedPath& drv,
    const experimental_feature_settings_t& xp_settings = experimental_feature_settings);
} // namespace nix

JSON_IMPL(nix::SingleDerivedPath::opaque_t)
JSON_IMPL_WITH_XP_FEATURES(nix::SingleDerivedPath::Built)
JSON_IMPL_WITH_XP_FEATURES(nix::SingleDerivedPath)
JSON_IMPL_WITH_XP_FEATURES(nix::derived_path_t::Built)
JSON_IMPL_WITH_XP_FEATURES(nix::derived_path_t)
