#pragma once
/**
 * @file
 *
 * @brief source_path_t
 */

#include "nix/util/canon-path.h"
#include "nix/util/ref.h"
#include "nix/util/source-accessor.h"
#include "nix/util/std-hash.h"

namespace nix {

/**
 * An abstraction for accessing source files during
 * evaluation. Currently, it's just a wrapper around `canon_path_t` that
 * accesses files in the regular filesystem, but in the future it will
 * support fetching files in other ways.
 */
struct source_path_t {
  ref<SourceAccessor> accessor;
  canon_path_t path;

  source_path_t(ref<SourceAccessor> accessor, canon_path_t path = canon_path_t::root)
      : accessor(std::move(accessor)), path(std::move(path)) {}

  std::string_view base_name() const;

  /**
   * Construct the parent of this `source_path_t`. Aborts if `this`
   * denotes the root.
   */
  source_path_t parent() const;

  /**
   * If this `source_path_t` denotes a regular file (not a symlink),
   * return its contents; otherwise throw an error.
   */
  std::string read_file() const;

  void
  read_file(Sink& sink, std::function<void(uint64_t)> size_callback = [](uint64_t size) {}) const {
    return accessor->read_file(path, sink, size_callback);
  }

  /**
   * Return whether this `source_path_t` denotes a file (of any type)
   * that exists
   */
  bool path_exists() const;

  /**
   * Return stats about this `source_path_t`, or throw an exception if
   * it doesn't exist.
   */
  SourceAccessor::stat_t lstat() const;

  /**
   * Return stats about this `source_path_t`, or std::nullopt if it
   * doesn't exist.
   */
  std::optional<SourceAccessor::stat_t> maybe_lstat() const;

  /**
   * If this `source_path_t` denotes a directory (not a symlink),
   * return its directory entries; otherwise throw an error.
   */
  SourceAccessor::dir_entries_t read_directory() const;

  /**
   * If this `source_path_t` denotes a symlink, return its target;
   * otherwise throw an error.
   */
  std::string read_link() const;

  /**
   * Dump this `source_path_t` to `sink` as a NAR archive.
   */
  void dump_path(Sink& sink, path_filter_t& filter = default_path_filter) const;

  /**
   * Return the location of this path in the "real" filesystem, if
   * it has a physical location.
   */
  std::optional<std::filesystem::path> get_physical_path() const;

  std::string to_string() const;

  /**
   * Append a `canon_path_t` to this path.
   */
  source_path_t operator/(const canon_path_t& x) const;

  /**
   * Append a single component `c` to this path. `c` must not
   * contain a slash. A slash is implicitly added between this path
   * and `c`.
   */
  source_path_t operator/(std::string_view c) const;

  bool operator==(const source_path_t& x) const noexcept;
  std::strong_ordering operator<=>(const source_path_t& x) const noexcept;

  /**
   * Convenience wrapper around `SourceAccessor::resolve_symlinks()`.
   */
  source_path_t resolve_symlinks(symlink_resolution_t mode = symlink_resolution_t::full) const {
    return {accessor, accessor->resolve_symlinks(path, mode)};
  }

  void invalidate_cache() const { accessor->invalidate_cache(path); }

  friend class std::hash<nix::source_path_t>;
};

std::ostream& operator<<(std::ostream& str, const source_path_t& path);

inline std::size_t hash_value(const source_path_t& path) {
  std::size_t hash = 0;
  boost::hash_combine(hash, path.accessor->number);
  boost::hash_combine(hash, path.path);
  return hash;
}

} // namespace nix

template <>
struct std::hash<nix::source_path_t> {
  using is_avalanching = std::true_type;

  std::size_t operator()(const nix::source_path_t& s) const noexcept { return nix::hash_value(s); }
};
