#pragma once
///@file

#include "nix/util/file-system.h"

namespace nix {

make_error(ExecutableLookupError, Error);

/**
 * @todo rename, it is not just good for executable paths, but also
 * other lists of paths.
 */
struct executable_path_t {
  std::vector<std::filesystem::path> directories;

  constexpr static const os_char_t separator =
#ifdef WIN32
      L';'
#else
      ':'
#endif
      ;

  /**
   * Parse `path` into a list of paths.
   *
   * On Unix we split on `:`, on Windows we split on `;`.
   *
   * For Unix, this is according to the POSIX spec for `PATH`.
   * https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap08.html#tag_08_03
   */
  static executable_path_t parse(const os_string_t& path);

  /**
   * Load the `PATH` environment variable and `parse` it.
   */
  static executable_path_t load();

  /**
   * Opposite of `parse`
   */
  os_string_t render() const;

  /**
   * Search for an executable.
   *
   * For Unix, this is according to the POSIX spec for `PATH`.
   * https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap08.html#tag_08_03
   *
   * @param exe This must just be a name, and not contain any `/` (or
   * `\` on Windows). in case it does, per the spec no lookup should
   * be performed, and the path (it is not just a file name) as is.
   * This is the caller's respsonsibility.
   *
   * This is a pure function, except for the default `is_executable`
   * argument, which uses the ambient file system to check if a file is
   * executable (and exists).
   *
   * @return path to a resolved executable
   */
  std::optional<std::filesystem::path>
  find_name(const os_string_t& exe, std::function<bool(const std::filesystem::path&)> is_executable_file =
                                    is_executable_file_ambient) const;

  /**
   * Like the `find_name` but also allows a file path as input.
   *
   * This implements the full POSIX spec: if the path is just a name,
   * it searches like the above. Otherwise, it returns the path as is.
   * If (in the name case) the search fails, an exception is thrown.
   */
  std::filesystem::path find_path(const std::filesystem::path& exe,
                                 std::function<bool(const std::filesystem::path&)> is_executable =
                                     is_executable_file_ambient) const;

  bool operator==(const executable_path_t&) const = default;
};

} // namespace nix
