#pragma once
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
void dump_path(const Path& path, Sink& sink, path_filter_t& filter = default_path_filter);

/**
 * Same as dump_path(), but returns the last modified date of the path.
 */
time_t dump_path_and_get_mtime(const Path& path, Sink& sink, path_filter_t& filter = default_path_filter);

/**
 * Dump an archive with a single file with these contents.
 *
 * @param s Contents of the file.
 */
void dump_string(std::string_view s, Sink& sink);

void parse_dump(file_system_object_sink_t& sink, Source& source);

void restore_path(const std::filesystem::path& path, Source& source, bool start_fsync = false);

/**
 * Read a NAR from 'source' and write it to 'sink'.
 */
void copy_nar(Source& source, Sink& sink);

inline constexpr std::string_view nar_version_magic1 = "nix-archive-1";

inline constexpr std::string_view case_hack_suffix = "~nix~case~hack~";

} // namespace nix
