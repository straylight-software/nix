#pragma once

#include "nix/util/source-accessor.h"

namespace nix {

struct source_path_t;

/**
 * A source accessor that uses the Unix filesystem.
 */
class posix_source_accessor_t : virtual public SourceAccessor {
  /**
   * Optional root path to prefix all operations into the native file
   * system. This allows prepending funny things like `C:\` that
   * `canon_path_t` intentionally doesn't support.
   */
  const std::filesystem::path root;

  const bool track_last_modified = false;

public:
  posix_source_accessor_t();
  posix_source_accessor_t(std::filesystem::path&& root, bool track_last_modified = false);

  /**
   * The most recent mtime seen by lstat(). This is a hack to
   * support dump_path_and_get_mtime(). Should remove this eventually.
   */
  time_t mtime = 0;

  void read_file(const canon_path_t& path, Sink& sink,
                std::function<void(uint64_t)> size_callback) override;

  auto path_exists(const canon_path_t& path) -> bool override;

  std::optional<stat_t> maybe_lstat(const canon_path_t& path) override;

  auto read_directory(const canon_path_t& path) -> dir_entries_t override;

  std::string read_link(const canon_path_t& path) override;

  std::optional<std::filesystem::path> get_physical_path(const canon_path_t& path) override;

  /**
   * Create a `posix_source_accessor_t` and `source_path_t` corresponding to
   * some native path.
   *
   * @param Whether the accessor should return a non-null get_last_modified.
   * When true the accessor must be used only by a single thread.
   *
   * The `posix_source_accessor_t` is rooted as far up the tree as
   * possible, (e.g. on Windows it could scoped to a drive like
   * `C:\`). This allows more `..` parent accessing to work.
   *
   * @note When `path` is trusted user input, canonicalize it using
   * `std::filesystem::canonical`, `make_parent_canonical`, `std::filesystem::weakly_canonical`, etc,
   * as appropriate for the use case. At least weak canonicalization is
   * required for the `source_path_t` to do anything useful at the location it
   * points to.
   *
   * @note A canonicalizing behavior is not built in `create_at_root` so that
   * callers do not accidentally introduce symlink-related security vulnerabilities.
   * Furthermore, `create_at_root` does not know whether the file pointed to by
   * `path` should be resolved if it is itself a symlink. In other words,
   * `create_at_root` can not decide between aforementioned `canonical`, `make_parent_canonical`, etc.
   * for its callers.
   *
   * See
   * [`std::filesystem::path::root_path`](https://en.cppreference.com/w/cpp/filesystem/path/root_path)
   * and
   * [`std::filesystem::path::relative_path`](https://en.cppreference.com/w/cpp/filesystem/path/relative_path).
   */
  static auto create_at_root(const std::filesystem::path& path, bool track_last_modified = false) -> source_path_t;

  std::optional<std::time_t> get_last_modified() override {
    return track_last_modified ? std::optional{mtime} : std::nullopt;
  }

  void invalidate_cache(const canon_path_t& path) override;

private:
  /**
   * Throw an error if `path` or any of its ancestors are symlinks.
   */
  void assert_no_symlinks(canon_path_t path);

  std::optional<struct stat> cached_lstat(const canon_path_t& path);

  std::filesystem::path make_abs_path(const canon_path_t& path);
};

} // namespace nix
