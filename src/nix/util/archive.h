#ifndef NIX_UTIL_ARCHIVE_H
#define NIX_UTIL_ARCHIVE_H
///@file

#include "nix/util/fs-sink.h"
#include "nix/util/serialise.h"
#include "nix/util/types.h"

namespace nix {

/**
 * dump_path creates a Nix archive of the specified path.
 *
 * @param path the file system data to dump. Dumping is recursive so if
 * this is a directory we dump it and all its children.
 *
 * @param [out] sink The serialised archive is fed into this sink.
 *
 * @param filter Can be used to skip certain files.
 *
 * The format is as follows:
 *
 * ```
 * IF path points to a REGULAR FILE:
 *   dump(path) = attrs(
 *     [ ("type", "regular")
 *     , ("contents", contents(path))
 *     ])
 *
 * IF path points to a DIRECTORY:
 *   dump(path) = attrs(
 *     [ ("type", "directory")
 *     , ("entries", concat(map(f, sort(entries(path)))))
 *     ])
 *     where f(fn) = attrs(
 *       [ ("name", fn)
 *       , ("file", dump(path + "/" + fn))
 *       ])
 *
 * where:
 *
 *   attrs(as) = concat(map(attr, as)) + encN(0)
 *   attrs((a, b)) = encS(a) + encS(b)
 *
 *   encS(s) = encN(len(s)) + s + (padding until next 64-bit boundary)
 *
 *   encN(n) = 64-bit little-endian encoding of n.
 *
 *   contents(path) = the contents of a regular file.
 *
 *   sort(strings) = lexicographic sort by 8-bit value (strcmp).
 *
 *   entries(path) = the entries of a directory, without `.` and
 *   `..`.
 *
 *   `+` denotes string concatenation.
 * ```
 */
auto dump_path(const Path& path, sink_t& sink, path_filter_t& filter = default_path_filter) -> void;

/**
 * Same as dump_path(), but returns the last modified date of the path.
 */
[[nodiscard]] auto dump_path_and_get_mtime(const Path& path, sink_t& sink,
                                           path_filter_t& filter = default_path_filter) -> time_t;

/**
 * Dump an archive with a single file with these contents.
 *
 * @param str Contents of the file.
 */
auto dump_string(std::string_view str, sink_t& sink) -> void;

auto parse_dump(file_system_object_sink_t& sink, source_t& source) -> void;

auto restore_path(const std::filesystem::path& path, source_t& source, bool start_fsync = false)
    -> void;

/**
 * Read a NAR from 'source' and write it to 'sink'.
 */
auto copy_nar(source_t& source, sink_t& sink) -> void;

inline constexpr std::string_view nar_version_magic1 = "nix-archive-1";

inline constexpr std::string_view case_hack_suffix = "~nix~case~hack~";

} // namespace nix

#endif // NIX_UTIL_ARCHIVE_H
