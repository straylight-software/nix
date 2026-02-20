#pragma once
///@file

#include "nix/store/derived-path.h"
#include "nix/store/realisation.h"

namespace nix {

struct SingleBuiltPath;

struct SingleBuiltPathBuilt {
  ref<SingleBuiltPath> drv_path;
  std::pair<std::string, store_path_t> output;

  SingleDerivedPathBuilt discardOutputPath() const;

  std::string to_string(const store_dir_config_t& store) const;
  static SingleBuiltPathBuilt parse(const store_dir_config_t& store, std::string_view,
                                    std::string_view);
  nlohmann::json to_json(const store_dir_config_t& store) const;

  bool operator==(const SingleBuiltPathBuilt&) const noexcept;
  std::strong_ordering operator<=>(const SingleBuiltPathBuilt&) const noexcept;
};

using _SingleBuiltPathRaw = std::variant<DerivedPathOpaque, SingleBuiltPathBuilt>;

struct SingleBuiltPath : _SingleBuiltPathRaw {
  using raw_t = _SingleBuiltPathRaw;
  using raw_t::raw_t;

  using opaque_t = DerivedPathOpaque;
  using Built = SingleBuiltPathBuilt;

  bool operator==(const SingleBuiltPath&) const = default;
  auto operator<=>(const SingleBuiltPath&) const = default;

  inline const raw_t& raw() const { return static_cast<const raw_t&>(*this); }

  store_path_t out_path() const;

  SingleDerivedPath discardOutputPath() const;

  static SingleBuiltPath parse(const store_dir_config_t& store, std::string_view);
  nlohmann::json to_json(const store_dir_config_t& store) const;
};

static inline ref<SingleBuiltPath> staticDrv(store_path_t drv_path) {
  return make_ref<SingleBuiltPath>(SingleBuiltPath::opaque_t{drv_path});
}

/**
 * A built derived path with hints in the form of optional concrete output paths.
 *
 * See 'BuiltPath' for more an explanation.
 */
struct BuiltPathBuilt {
  ref<SingleBuiltPath> drv_path;
  std::map<std::string, store_path_t> outputs;

  bool operator==(const BuiltPathBuilt&) const noexcept;
  // TODO libc++ 16 (used by darwin) missing `std::map::operator <=>`, can't do yet.
  // std::strong_ordering operator <=> (const BuiltPathBuilt &) const noexcept;

  std::string to_string(const store_dir_config_t& store) const;
  static BuiltPathBuilt parse(const store_dir_config_t& store, std::string_view, std::string_view);
  nlohmann::json to_json(const store_dir_config_t& store) const;
};

using _BuiltPathRaw = std::variant<derived_path_t::opaque_t, BuiltPathBuilt>;

/**
 * A built path. Similar to a derived_path_t, but enriched with the corresponding
 * output path(s).
 */
struct BuiltPath : _BuiltPathRaw {
  using raw_t = _BuiltPathRaw;
  using raw_t::raw_t;

  using opaque_t = DerivedPathOpaque;
  using Built = BuiltPathBuilt;

  bool operator==(const BuiltPath&) const = default;

  // TODO libc++ 16 (used by darwin) missing `std::map::operator <=>`, can't do yet.
  // auto operator <=> (const BuiltPath &) const = default;

  inline const raw_t& raw() const { return static_cast<const raw_t&>(*this); }

  store_path_set_t out_paths() const;
  RealisedPath::Set toRealisedPaths(store_t& store) const;

  nlohmann::json to_json(const store_dir_config_t& store) const;
};

using BuiltPaths = std::vector<BuiltPath>;

} // namespace nix
