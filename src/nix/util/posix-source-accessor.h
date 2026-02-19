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

  const bool trackLastModified = false;

public:
  posix_source_accessor_t();
  posix_source_accessor_t(std::filesystem::path&& root, bool trackLastModified = false);

  /**
   * The most recent mtime seen by lstat(). This is a hack to
   * support dumpPathAndGetMtime(). Should remove this eventually.
   */
  time_t mtime = 0;

  void readFile(const canon_path_t& path, Sink& sink,
                std::function<void(uint64_t)> sizeCallback) override;

  bool pathExists(const canon_path_t& path) override;

  std::optional<stat_t> maybeLstat(const canon_path_t& path) override;

  dir_entries_t readDirectory(const canon_path_t& path) override;

  std::string readLink(const canon_path_t& path) override;

  std::optional<std::filesystem::path> getPhysicalPath(const canon_path_t& path) override;

  /**
   * Create a `posix_source_accessor_t` and `source_path_t` corresponding to
   * some native path.
   *
   * @param Whether the accessor should return a non-null getLastModified.
   * When true the accessor must be used only by a single thread.
   *
   * The `posix_source_accessor_t` is rooted as far up the tree as
   * possible, (e.g. on Windows it could scoped to a drive like
   * `C:\`). This allows more `..` parent accessing to work.
   *
   * @note When `path` is trusted user input, canonicalize it using
   * `std::filesystem::canonical`, `makeParentCanonical`, `std::filesystem::weakly_canonical`, etc,
   * as appropriate for the use case. At least weak canonicalization is
   * required for the `source_path_t` to do anything useful at the location it
   * points to.
   *
   * @note A canonicalizing behavior is not built in `createAtRoot` so that
   * callers do not accidentally introduce symlink-related security vulnerabilities.
   * Furthermore, `createAtRoot` does not know whether the file pointed to by
   * `path` should be resolved if it is itself a symlink. In other words,
   * `createAtRoot` can not decide between aforementioned `canonical`, `makeParentCanonical`, etc.
   * for its callers.
   *
   * See
   * [`std::filesystem::path::root_path`](https://en.cppreference.com/w/cpp/filesystem/path/root_path)
   * and
   * [`std::filesystem::path::relative_path`](https://en.cppreference.com/w/cpp/filesystem/path/relative_path).
   */
  static source_path_t createAtRoot(const std::filesystem::path& path, bool trackLastModified = false);

  std::optional<std::time_t> getLastModified() override {
    return trackLastModified ? std::optional{mtime} : std::nullopt;
  }

  void invalidateCache(const canon_path_t& path) override;

private:
  /**
   * Throw an error if `path` or any of its ancestors are symlinks.
   */
  void assertNoSymlinks(canon_path_t path);

  std::optional<struct stat> cachedLstat(const canon_path_t& path);

  std::filesystem::path makeAbsPath(const canon_path_t& path);
};

} // namespace nix
