#pragma once

#include <filesystem>

#include "nix/util/canon-path.h"
#include "nix/util/hash.h"
#include "nix/util/ref.h"

namespace nix {

struct sink_t;

/**
 * Note there is a decent chance this type soon goes away because the problem is solved another way.
 * See the discussion in https://github.com/NixOS/nix/pull/9985.
 */
enum class symlink_resolution_t {
  /**
   * Resolve symlinks in the ancestors only.
   *
   * Only the last component of the result is possibly a symlink.
   */
  ancestors,

  /**
   * Resolve symlinks fully, realpath(3)-style.
   *
   * No component of the result will be a symlink.
   */
  full,
};

make_error(FileNotFound, Error);

/**
 * A read-only filesystem abstraction. This is used by the Nix
 * evaluator and elsewhere for accessing sources in various
 * filesystem-like entities (such as the real filesystem, tarballs or
 * git repositories).
 */
struct source_accessor_t : std::enable_shared_from_this<source_accessor_t> {
  const size_t number;

  std::string display_prefix, display_suffix;

  source_accessor_t();

  virtual ~source_accessor_t() = default;

  /**
   * Return the contents of a file as a string.
   *
   * @note Unlike Unix, this method should *not* follow symlinks. Nix
   * by default wants to manipulate symlinks explicitly, and not
   * implicitly follow them, as they are frequently untrusted user data
   * and thus may point to arbitrary locations. Acting on the targets
   * targets of symlinks should only occasionally be done, and only
   * with care.
   */
  [[nodiscard]] virtual auto read_file(const canon_path_t& path) -> std::string;

  /**
   * Write the contents of a file as a sink. `size_callback` must be
   * called with the size of the file before any data is written to
   * the sink.
   *
   * @note Like the other `read_file`, this method should *not* follow
   * symlinks.
   *
   * @note subclasses of `source_accessor_t` need to implement at least
   * one of the `read_file()` variants.
   */
  virtual auto read_file(
      const canon_path_t& path, sink_t& sink,
      std::function<void(uint64_t)> size_callback = [](uint64_t size) {}) -> void;

  [[nodiscard]] virtual auto path_exists(const canon_path_t& path) -> bool;

  enum Type {
    t_regular,
    t_symlink,
    t_directory,
    /**
      Any other node types that may be encountered on the file system, such as device nodes,
      sockets, named pipe, and possibly even more exotic things.

      Responsible for `"unknown"` from `builtins.read_file_type "/dev/null"`.

      Unlike `DT_UNKNOWN`, this must not be used for deferring the lookup of types.
    */
    t_char,
    t_block,
    t_socket,
    t_fifo,
    t_unknown
  };

  struct stat_t {
    Type type = t_unknown;

    /**
     * For regular files only: the size of the file. Not all
     * accessors return this since it may be too expensive to
     * compute.
     */
    std::optional<uint64_t> file_size{};

    /**
     * For regular files only: whether this is an executable.
     */
    bool is_executable = false;

    /**
     * For regular files only: the position of the contents of this
     * file in the NAR. Only returned by NAR accessors.
     */
    std::optional<uint64_t> nar_offset{};

    [[nodiscard]] auto is_not_nar_serialisable() -> bool;
    [[nodiscard]] auto type_string() -> std::string;
  };

  [[nodiscard]] virtual auto lstat(const canon_path_t& path) -> stat_t;

  [[nodiscard]] virtual auto maybe_lstat(const canon_path_t& path) -> std::optional<stat_t> = 0;

  using dir_entry_t = std::optional<Type>;

  using dir_entries_t = std::map<std::string, dir_entry_t>;

  /**
   * @note Like `read_file`, this method should *not* follow symlinks.
   */
  [[nodiscard]] virtual auto read_directory(const canon_path_t& path) -> dir_entries_t = 0;

  [[nodiscard]] virtual auto read_link(const canon_path_t& path) -> std::string = 0;

  virtual auto dump_path(const canon_path_t& path, sink_t& sink,
                         path_filter_t& filter = default_path_filter) -> void;

  [[nodiscard]] auto hash_path(const canon_path_t& path,
                               path_filter_t& filter = default_path_filter,
                               hash_algorithm_t ha = hash_algorithm_t::SHA256) -> Hash;

  /**
   * Return a corresponding path in the root filesystem, if
   * possible. This is only possible for filesystems that are
   * materialized in the root filesystem.
   */
  [[nodiscard]] virtual auto get_physical_path(const canon_path_t& /*path*/)
      -> std::optional<std::filesystem::path> {
    return std::nullopt;
  }

  [[nodiscard]] auto operator==(const source_accessor_t& other) const -> bool {
    return number == other.number;
  }

  [[nodiscard]] auto operator<=>(const source_accessor_t& other) const {
    return number <=> other.number;
  }

  auto set_path_display(std::string display_prefix, std::string display_suffix = "") -> void;

  [[nodiscard]] virtual auto show_path(const canon_path_t& path) -> std::string;

  /**
   * Resolve any symlinks in `path` according to the given
   * resolution mode.
   *
   * @param mode might only be a temporary solution for this.
   * See the discussion in https://github.com/NixOS/nix/pull/9985.
   */
  [[nodiscard]] auto resolve_symlinks(const canon_path_t& path,
                                      symlink_resolution_t mode = symlink_resolution_t::full)
      -> canon_path_t;

  /**
   * A string that uniquely represents the contents of this
   * accessor. This is used for caching lookups (see `fetch_to_store()`).
   */
  std::optional<std::string> fingerprint;

  /**
   * Return the fingerprint for `path`. This is usually the
   * fingerprint of the current accessor, but for composite
   * accessors (like `mounted_source_accessor_t`), we want to return the
   * fingerprint of the "inner" accessor if the current one lacks a
   * fingerprint.
   *
   * So this method is intended to return the most-outer accessor
   * that has a fingerprint for `path`. It also returns the path that `path`
   * corresponds to in that accessor.
   *
   * For example: in a `mounted_source_accessor_t` that has
   * `/nix/store/foo` mounted,
   * `get_fingerprint("/nix/store/foo/bar")` will return the path
   * `/bar` and the fingerprint of the `/nix/store/foo` accessor.
   */
  [[nodiscard]] virtual auto get_fingerprint(const canon_path_t& path)
      -> std::pair<canon_path_t, std::optional<std::string>> {
    return {path, fingerprint};
  }

  /**
   * Return the maximum last-modified time of the files in this
   * tree, if available.
   */
  [[nodiscard]] virtual auto get_last_modified() -> std::optional<time_t> { return std::nullopt; }

  /**
   * Invalidate any cached value the accessor may have for the specified path.
   */
  virtual auto invalidate_cache(const canon_path_t& path) -> void {}
};

/**
 * Return a source accessor that contains only an empty root directory.
 */
auto make_empty_source_accessor() -> ref<source_accessor_t>;

/**
 * Exception thrown when accessing a filtered path (see
 * `FilteringSourceAccessor`).
 */
make_error(RestrictedPathError, Error);

/**
 * Return an accessor for the root filesystem.
 */
auto get_fs_source_accessor() -> ref<source_accessor_t>;

/**
 * Construct an accessor for the filesystem rooted at `root`. Note
 * that it is not possible to escape `root` by appending `..` path
 * elements, and that absolute symlinks are resolved relative to
 * `root`.
 */
auto make_fs_source_accessor(std::filesystem::path root) -> ref<source_accessor_t>;

/**
 * Construct an accessor that presents a "union" view of a vector of
 * underlying accessors. Earlier accessors take precedence over later.
 */
[[nodiscard]] auto make_union_source_accessor(std::vector<ref<source_accessor_t>>&& accessors)
    -> ref<source_accessor_t>;

} // namespace nix
