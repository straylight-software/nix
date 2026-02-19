#pragma once
///@file

#include <filesystem>

#include "nix/util/json-non-null.h"
#include "nix/util/os-string.h"
#include "nix/util/types.h"

namespace nix {

/**
 * Paths are just `std::filesystem::path`s.
 *
 * @todo drop `NG` suffix and replace the ones in `types.hh`.
 */
typedef std::list<std::filesystem::path> paths_ng_t;
typedef std::set<std::filesystem::path> path_set_ng_t;

/**
 * Stop gap until `std::filesystem::path_view` from P1030R6 exists in a
 * future C++ standard.
 *
 * @todo drop `NG` suffix and replace the one in `types.hh`.
 */
struct path_view_ng_t : os_string_view_t {
  using string_view = os_string_view_t;

  using string_view::string_view;

  path_view_ng_t(const std::filesystem::path& path) : os_string_view_t{path.native()} {}

  path_view_ng_t(const os_string_t& path) : os_string_view_t{path} {}

  const string_view& native() const { return *this; }

  string_view& native() { return *this; }
};

std::optional<std::filesystem::path> maybePath(path_view_t path);

std::filesystem::path pathNG(path_view_t path);

template <>
struct json_avoids_null<std::filesystem::path> : std::true_type {};

} // namespace nix
