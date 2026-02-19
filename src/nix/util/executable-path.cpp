#include "nix/util/executable-path.h"

#include "nix/util/environment-variables.h"
#include "nix/util/file-path-impl.h"
#include "nix/util/strings-inline.h"
#include "nix/util/util.h"

namespace nix {

constexpr static const os_string_view_t path_var_separator{
    &executable_path_t::separator,
    1,
};

executable_path_t executable_path_t::load() {
  // "If PATH is unset or is set to null, the path search is
  // implementation-defined."
  // https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap08.html#tag_08_03
  return executable_path_t::parse(get_env_os(OS_STR("PATH")).value_or(OS_STR("")));
}

executable_path_t executable_path_t::parse(const os_string_t& path) {
  auto strings = path.empty()
                     ? (std::list<os_string_t>{})
                     : basic_split_string<std::list<os_string_t>, os_char_t>(path, path_var_separator);

  std::vector<std::filesystem::path> ret;
  ret.reserve(strings.size());

  std::transform(
      std::make_move_iterator(strings.begin()), std::make_move_iterator(strings.end()),
      std::back_inserter(ret), [](os_string_t&& str) {
        return std::filesystem::path{
            str.empty()
                // "A zero-length prefix is a legacy feature that
                // indicates the current working directory. It
                // appears as two adjacent <colon> characters
                // ("::"), as an initial <colon> preceding the rest
                // of the list, or as a trailing <colon> following
                // the rest of the list."
                // https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap08.html#tag_08_03
                ? OS_STR(".")
                : std::move(str),
        };
      });

  return {ret};
}

os_string_t executable_path_t::render() const {
  std::vector<path_view_ng_t> path2;
  path2.reserve(directories.size());
  for (auto& p : directories) {
    path2.push_back(p.native());
}
  return basic_concat_strings_sep(path_var_separator, path2);
}

std::optional<std::filesystem::path>
executable_path_t::find_name(const os_string_t& exe,
                         std::function<bool(const std::filesystem::path&)> is_executable) const {
  // "If the pathname being sought contains a <slash>, the search
  // through the path prefixes shall not be performed."
  // https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap08.html#tag_08_03
  assert(os_path_trait_t<std::filesystem::path::value_type>::rfind_path_sep(exe) == exe.npos);

  for (auto& dir : directories) {
    auto candidate = dir / exe;
    if (is_executable(candidate)) {
      return candidate.lexically_normal();
}
  }

  return std::nullopt;
}

std::filesystem::path
executable_path_t::find_path(const std::filesystem::path& exe,
                         std::function<bool(const std::filesystem::path&)> is_executable) const {
  // "If the pathname being sought contains a <slash>, the search
  // through the path prefixes shall not be performed."
  // https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap08.html#tag_08_03
  if (exe.filename() == exe) {
    auto res_opt = find_name(exe, is_executable);
    if (res_opt) {
      return *res_opt;
    } else {
      throw ExecutableLookupError("Could not find executable '%s'", exe.string());
}
  } else {
    return exe;
  }
}

} // namespace nix
