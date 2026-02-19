#pragma once

#include <filesystem>

#include "nix/util/canon-path.h"
#include "nix/util/hash.h"
#include "nix/util/ref.h"

namespace nix {

struct Sink;

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
  Ancestors,

  /**
   * Resolve symlinks fully, realpath(3)-style.
   *
   * No component of the result will be a symlink.
   */
  Full,
};

MakeError(FileNotFound, Error);

/**
 * A read-only filesystem abstraction. This is used by the Nix
 * evaluator and elsewhere for accessing sources in various
 * filesystem-like entities (such as the real filesystem, tarballs or
 * Git repositories).
 */
struct SourceAccessor : std::enable_shared_from_this<SourceAccessor> {
  const size_t number;

  std::string displayPrefix, displaySuffix;

  SourceAccessor();

  virtual ~SourceAccessor() {}

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
  virtual std::string readFile(const canon_path_t& path);

  /**
   * Write the contents of a file as a sink. `sizeCallback` must be
   * called with the size of the file before any data is written to
   * the sink.
   *
   * @note Like the other `readFile`, this method should *not* follow
   * symlinks.
   *
   * @note subclasses of `SourceAccessor` need to implement at least
   * one of the `readFile()` variants.
   */
  virtual void readFile(
      const canon_path_t& path, Sink& sink,
      std::function<void(uint64_t)> sizeCallback = [](uint64_t size) {});

  virtual bool pathExists(const canon_path_t& path);

  enum Type {
    tRegular,
    tSymlink,
    tDirectory,
    /**
      Any other node types that may be encountered on the file system, such as device nodes,
      sockets, named pipe, and possibly even more exotic things.

      Responsible for `"unknown"` from `builtins.readFileType "/dev/null"`.

      Unlike `DT_UNKNOWN`, this must not be used for deferring the lookup of types.
    */
    tChar,
    tBlock,
    tSocket,
    tFifo,
    tUnknown
  };

  struct stat_t {
    Type type = tUnknown;

    /**
     * For regular files only: the size of the file. Not all
     * accessors return this since it may be too expensive to
     * compute.
     */
    std::optional<uint64_t> fileSize;

    /**
     * For regular files only: whether this is an executable.
     */
    bool isExecutable = false;

    /**
     * For regular files only: the position of the contents of this
     * file in the NAR. Only returned by NAR accessors.
     */
    std::optional<uint64_t> narOffset;

    bool isNotNARSerialisable();
    std::string typeString();
  };

  virtual stat_t lstat(const canon_path_t& path);

  virtual std::optional<stat_t> maybeLstat(const canon_path_t& path) = 0;

  typedef std::optional<Type> dir_entry_t;

  typedef std::map<std::string, dir_entry_t> dir_entries_t;

  /**
   * @note Like `readFile`, this method should *not* follow symlinks.
   */
  virtual dir_entries_t readDirectory(const canon_path_t& path) = 0;

  virtual std::string readLink(const canon_path_t& path) = 0;

  virtual void dumpPath(const canon_path_t& path, Sink& sink, path_filter_t& filter = defaultPathFilter);

  Hash hashPath(const canon_path_t& path, path_filter_t& filter = defaultPathFilter,
                hash_algorithm_t ha = hash_algorithm_t::SHA256);

  /**
   * Return a corresponding path in the root filesystem, if
   * possible. This is only possible for filesystems that are
   * materialized in the root filesystem.
   */
  virtual std::optional<std::filesystem::path> getPhysicalPath(const canon_path_t& path) {
    return std::nullopt;
  }

  bool operator==(const SourceAccessor& x) const { return number == x.number; }

  auto operator<=>(const SourceAccessor& x) const { return number <=> x.number; }

  void setPathDisplay(std::string displayPrefix, std::string displaySuffix = "");

  virtual std::string showPath(const canon_path_t& path);

  /**
   * Resolve any symlinks in `path` according to the given
   * resolution mode.
   *
   * @param mode might only be a temporary solution for this.
   * See the discussion in https://github.com/NixOS/nix/pull/9985.
   */
  canon_path_t resolveSymlinks(const canon_path_t& path,
                            symlink_resolution_t mode = symlink_resolution_t::Full);

  /**
   * A string that uniquely represents the contents of this
   * accessor. This is used for caching lookups (see `fetchToStore()`).
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
   * `getFingerprint("/nix/store/foo/bar")` will return the path
   * `/bar` and the fingerprint of the `/nix/store/foo` accessor.
   */
  virtual std::pair<canon_path_t, std::optional<std::string>> getFingerprint(const canon_path_t& path) {
    return {path, fingerprint};
  }

  /**
   * Return the maximum last-modified time of the files in this
   * tree, if available.
   */
  virtual std::optional<time_t> getLastModified() { return std::nullopt; }

  /**
   * Invalidate any cached value the accessor may have for the specified path.
   */
  virtual void invalidateCache(const canon_path_t& path) {}
};

/**
 * Return a source accessor that contains only an empty root directory.
 */
ref<SourceAccessor> makeEmptySourceAccessor();

/**
 * Exception thrown when accessing a filtered path (see
 * `FilteringSourceAccessor`).
 */
MakeError(RestrictedPathError, Error);

/**
 * Return an accessor for the root filesystem.
 */
ref<SourceAccessor> getFSSourceAccessor();

/**
 * Construct an accessor for the filesystem rooted at `root`. Note
 * that it is not possible to escape `root` by appending `..` path
 * elements, and that absolute symlinks are resolved relative to
 * `root`.
 */
ref<SourceAccessor> makeFSSourceAccessor(std::filesystem::path root);

/**
 * Construct an accessor that presents a "union" view of a vector of
 * underlying accessors. Earlier accessors take precedence over later.
 */
ref<SourceAccessor> makeUnionSourceAccessor(std::vector<ref<SourceAccessor>>&& accessors);

} // namespace nix
