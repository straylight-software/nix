#pragma once
/**
 * @file
 *
 * Utilities for working with the file system and file paths.
 */

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "nix/util/file-descriptor.h"
#include "nix/util/file-path.h"
#include "nix/util/types.h"
#ifdef _WIN32
#  include <windef.h>
#endif

#include <functional>
#include <optional>

/**
 * Polyfill for MinGW
 *
 * Windows does in fact support symlinks, but the C runtime interfaces predate this.
 *
 * @todo get rid of this, and stop using `stat` when we want `lstat` too.
 */
#ifndef S_ISLNK
#  define S_ISLNK(m) false
#endif

namespace nix {

struct Sink;
struct Source;

/**
 * Return whether the path denotes an absolute path.
 */
bool is_absolute(path_view_t path);

/**
 * @return An absolutized path, resolving paths relative to the
 * specified directory, or the current directory otherwise.  The path
 * is also canonicalised.
 *
 * In the process of being deprecated for `std::filesystem::absolute`.
 */
Path abs_path(path_view_t path, std::optional<path_view_t> dir = {}, bool resolve_symlinks = false);

inline Path abs_path(const Path& path, std::optional<path_view_t> dir = {},
                    bool resolve_symlinks = false) {
  return abs_path(path_view_t{path}, dir, resolve_symlinks);
}

std::filesystem::path abs_path(const std::filesystem::path& path,
                              const std::filesystem::path* dir = nullptr,
                              bool resolve_symlinks = false);

/**
 * Canonicalise a path by removing all `.` or `..` components and
 * double or trailing slashes.  Optionally resolves all symlink
 * components such that each component of the resulting path is *not*
 * a symbolic link.
 *
 * In the process of being deprecated for
 * `std::filesystem::path::lexically_normal` (for the `resolve_symlinks =
 * false` case), and `std::filesystem::weakly_canonical` (for the
 * `resolve_symlinks = true` case).
 */
Path canon_path(path_view_t path, bool resolve_symlinks = false);

/**
 * @return The directory part of the given canonical path, i.e.,
 * everything before the final `/`.  If the path is the root or an
 * immediate child thereof (e.g., `/foo`), this means `/`
 * is returned.
 *
 * In the process of being deprecated for
 * `std::filesystem::path::parent_path`.
 */
Path dir_of(const path_view_t path);

/**
 * @return the base name of the given canonical path, i.e., everything
 * following the final `/` (trailing slashes are removed).
 *
 * In the process of being deprecated for
 * `std::filesystem::path::filename`.
 */
std::string_view base_name_of(std::string_view path);

/**
 * Check whether 'path' is a descendant of 'dir'. Both paths must be
 * canonicalized.
 */
bool is_in_dir(const std::filesystem::path& path, const std::filesystem::path& dir);

/**
 * Check whether 'path' is equal to 'dir' or a descendant of
 * 'dir'. Both paths must be canonicalized.
 */
bool is_dir_or_in_dir(const std::filesystem::path& path, const std::filesystem::path& dir);

/**
 * Get status of `path`.
 */
struct stat stat(const Path& path);
struct stat lstat(const Path& path);
/**
 * `lstat` the given path if it exists.
 * @return std::nullopt if the path doesn't exist, or an optional containing the result of `lstat`
 * otherwise
 */
std::optional<struct stat> maybe_lstat(const Path& path);

/**
 * @return true iff the given path exists.
 */
bool path_exists(const std::filesystem::path& path);

/**
 * Canonicalize a path except for the last component.
 *
 * This is useful for getting the canonical location of a symlink.
 *
 * Consider the case where `foo/l` is a symlink. `canonical("foo/l")` will
 * resolve the symlink `l` to its target.
 * `make_parent_canonical("foo/l")` will not resolve the symlink `l` to its target,
 * but does ensure that the returned parent part of the path, `foo` is resolved
 * to `canonical("foo")`, and can therefore be retrieved without traversing any
 * symlinks.
 *
 * If a relative path is passed, it will be made absolute, so that the parent
 * can always be canonicalized.
 */
std::filesystem::path make_parent_canonical(const std::filesystem::path& path);

/**
 * A version of path_exists that returns false on a permission error.
 * Useful for inferring default paths across directories that might not
 * be readable.
 * @return true iff the given path can be accessed and exists
 */
bool path_accessible(const std::filesystem::path& path);

/**
 * Read the contents (target) of a symbolic link.  The result is not
 * in any way canonicalised.
 *
 * In the process of being deprecated for
 * `std::filesystem::read_symlink`.
 */
Path read_link(const Path& path);

/**
 * Read the contents (target) of a symbolic link.  The result is not
 * in any way canonicalised.
 */
std::filesystem::path read_link(const std::filesystem::path& path);

/**
 * Open a `descriptor_t` with read-only access to the given directory.
 */
descriptor_t open_directory(const std::filesystem::path& path);

/**
 * Read the contents of a file into a string.
 */
std::string read_file(const Path& path);
std::string read_file(const std::filesystem::path& path);
void read_file(const Path& path, Sink& sink, bool memory_map = true);

enum struct fs_sync_t { yes, No };

/**
 * Write a string to a file.
 */
void write_file(const Path& path, std::string_view s, mode_t mode = 0666, fs_sync_t sync = fs_sync_t::No);

static inline void write_file(const std::filesystem::path& path, std::string_view s,
                             mode_t mode = 0666, fs_sync_t sync = fs_sync_t::No) {
  return write_file(path.string(), s, mode, sync);
}

void write_file(const Path& path, Source& source, mode_t mode = 0666, fs_sync_t sync = fs_sync_t::No);

static inline void write_file(const std::filesystem::path& path, Source& source, mode_t mode = 0666,
                             fs_sync_t sync = fs_sync_t::No) {
  return write_file(path.string(), source, mode, sync);
}

void write_file(auto_close_fd_t& fd, const Path& orig_path, std::string_view s, mode_t mode = 0666,
               fs_sync_t sync = fs_sync_t::No);

/**
 * Flush a path's parent directory to disk.
 */
void sync_parent(const Path& path);

/**
 * Flush a file or entire directory tree to disk.
 */
void recursive_sync(const Path& path);

/**
 * Delete a path; i.e., in the case of a directory, it is deleted
 * recursively. It's not an error if the path does not exist. The
 * second variant returns the number of bytes and blocks freed.
 */
void delete_path(const std::filesystem::path& path);

void delete_path(const std::filesystem::path& path, uint64_t& bytes_freed);

/**
 * Create a directory and all its parents, if necessary.
 *
 * Wrapper around `std::filesystem::create_directories` to handle exceptions.
 */
void create_dirs(const std::filesystem::path& path);

/**
 * Create a single directory.
 */
void create_dir(const Path& path, mode_t mode = 0755);

/**
 * Set the access and modification times of the given path, not
 * following symlinks.
 *
 * @param accessed_time Specified in seconds.
 *
 * @param modification_time Specified in seconds.
 *
 * @param is_symlink Whether the file in question is a symlink. Used for
 * fallback code where we don't have `lutimes` or similar. if
 * `std::optional` is passed, the information will be recomputed if it
 * is needed. Race conditions are possible so be careful!
 */
void set_write_time(const std::filesystem::path& path, time_t accessed_time, time_t modification_time,
                  std::optional<bool> is_symlink = std::nullopt);

/**
 * Convenience wrapper that takes all arguments from the `struct stat`.
 */
void set_write_time(const std::filesystem::path& path, const struct stat& st);

/**
 * Create a symlink.
 *
 */
void create_symlink(const Path& target, const Path& link);

/**
 * Atomically create or replace a symlink.
 */
void replace_symlink(const std::filesystem::path& target, const std::filesystem::path& link);

inline void replace_symlink(const Path& target, const Path& link) {
  return replace_symlink(std::filesystem::path{target}, std::filesystem::path{link});
}

/**
 * Similar to 'renameFile', but fallback to a copy+remove if `src` and `dst`
 * are on a different filesystem.
 *
 * Beware that this might not be atomic because of the copy that happens behind
 * the scenes
 */
void move_file(const Path& src, const Path& dst);

/**
 * Recursively copy the content of `old_path` to `new_path`. If `and_delete` is
 * `true`, then also remove `old_path` (making this equivalent to `move_file`, but
 * with the guaranty that the destination will be “fresh”, with no stale inode
 * or file descriptor pointing to it).
 */
void copy_file(const std::filesystem::path& from, const std::filesystem::path& to, bool and_delete);

/**
 * Automatic cleanup of resources.
 */
class auto_delete_t {
  std::filesystem::path _path;
  bool del;
  bool recursive;

public:
  auto_delete_t();

  auto_delete_t(auto_delete_t&& x) noexcept {
    _path = std::move(x._path);
    del = x.del;
    recursive = x.recursive;
    x.del = false;
  }

  auto_delete_t(const std::filesystem::path& p, bool recursive = true);
  auto_delete_t(const auto_delete_t&) = delete;
  auto_delete_t& operator=(auto_delete_t&&) = delete;
  auto_delete_t& operator=(const auto_delete_t&) = delete;
  ~auto_delete_t();

  void cancel();

  void reset(const std::filesystem::path& p, bool recursive = true);

  const std::filesystem::path& path() const { return _path; }

  path_view_ng_t view() const { return _path; }

  operator const std::filesystem::path&() const { return _path; }

  operator path_view_ng_t() const { return _path; }
};

struct dir_deleter_t {
  void operator()(DIR* dir) const { closedir(dir); }
};

typedef std::unique_ptr<DIR, dir_deleter_t> auto_close_dir_t;

/**
 * Create a temporary directory.
 */
std::filesystem::path create_temp_dir(const std::filesystem::path& tmp_root = "",
                                    const std::string& prefix = "nix", mode_t mode = 0755);

/**
 * Create an anonymous readable/writable temporary file, returning a file handle.
 * On UNIX there resulting file isn't linked to any path on the filesystem.
 */
auto_close_fd_t create_anonymous_temp_file();

/**
 * Create a temporary file, returning a file handle and its path.
 */
std::pair<auto_close_fd_t, Path> create_temp_file(const Path& prefix = "nix");

/**
 * Return `TMPDIR`, or the default temporary directory if unset or empty.
 */
std::filesystem::path default_temp_dir();

/**
 * Interpret `exe` as a location in the ambient file system and return
 * whether it resolves to a file that is executable.
 */
bool is_executable_file_ambient(const std::filesystem::path& exe);

/**
 * Return temporary path constructed by appending a suffix to a root path.
 *
 * The constructed path looks like `<root><suffix>-<pid>-<unique>`. To create a
 * path nested in a directory, provide a suffix starting with `/`.
 */
std::filesystem::path make_temp_path(const std::filesystem::path& root,
                                   const std::string& suffix = ".tmp");

/**
 * Used in various places.
 */
typedef std::function<bool(const Path& path)> path_filter_t;

extern path_filter_t default_path_filter;

/**
 * Change permissions of a file only if necessary.
 *
 * @details
 * Skip chmod call if the directory already has the requested permissions.
 * This is to avoid failing when the executing user lacks permissions to change the
 * directory's permissions even if it would be no-op.
 *
 * @param path Path to the file to change the permissions for.
 * @param mode New file mode.
 * @param mask Used for checking if the file already has requested permissions.
 *
 * @return true if permissions changed, false otherwise.
 */
bool chmod_if_needed(const std::filesystem::path& path, mode_t mode,
                   mode_t mask = S_IRWXU | S_IRWXG | S_IRWXO);

/**
 * @brief A directory iterator that can be used to iterate over the
 * contents of a directory. It is similar to std::filesystem::directory_iterator
 * but throws NixError on failure instead of std::filesystem::filesystem_error.
 */
class directory_iterator_t {
public:
  // --- Iterator Traits ---
  using iterator_category = std::input_iterator_tag;
  using value_type = std::filesystem::directory_entry;
  using difference_type = std::ptrdiff_t;
  using pointer = const std::filesystem::directory_entry*;
  using reference = const std::filesystem::directory_entry&;

  // Default constructor (represents end iterator)
  directory_iterator_t() noexcept = default;

  // Constructor taking a path
  explicit directory_iterator_t(const std::filesystem::path& p);

  reference operator*() const {
    // Accessing the value itself doesn't typically throw filesystem_error
    // after successful construction/increment, but underlying operations might.
    // If directory_entry methods called via -> could throw, add try-catch there.
    return *it_;
  }

  pointer operator->() const { return &(*it_); }

  directory_iterator_t& operator++();

  // Postfix increment operator
  directory_iterator_t operator++(int) {
    directory_iterator_t temp = *this;
    ++(*this); // Uses the prefix increment's try-catch logic
    return temp;
  }

  // Equality comparison
  friend bool operator==(const directory_iterator_t& a, const directory_iterator_t& b) noexcept {
    return a.it_ == b.it_;
  }

  // Inequality comparison
  friend bool operator!=(const directory_iterator_t& a, const directory_iterator_t& b) noexcept {
    return !(a == b);
  }

  // Allow direct use in range-based for loops if iterating over an instance
  directory_iterator_t begin() const { return *this; }

  directory_iterator_t end() const { return directory_iterator_t{}; }


private:
  std::filesystem::directory_iterator it_;
};

#ifdef __FreeBSD__
class AutoUnmount {
  Path path;
  bool del;

public:
  AutoUnmount(Path&);
  AutoUnmount();
  ~AutoUnmount();
  void cancel();
};
#endif

} // namespace nix
