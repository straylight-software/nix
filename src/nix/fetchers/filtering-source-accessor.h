#pragma once

#include <boost/unordered/unordered_flat_set_fwd.hpp>

#include "nix/util/source-path.h"

namespace nix {

/**
 * A function that returns an exception of type
 * `RestrictedPathError` explaining that access to `path` is
 * forbidden.
 */
typedef std::function<RestrictedPathError(const canon_path_t& path)> MakeNotAllowedError;

/**
 * An abstract wrapping `SourceAccessor` that performs access
 * control. Subclasses should override `isAllowed()` to implement an
 * access control policy. The error message is customized at construction.
 */
struct FilteringSourceAccessor : SourceAccessor {
  ref<SourceAccessor> next;
  canon_path_t prefix;
  MakeNotAllowedError makeNotAllowedError;

  FilteringSourceAccessor(const source_path_t& src, MakeNotAllowedError&& makeNotAllowedError)
      : next(src.accessor), prefix(src.path), makeNotAllowedError(std::move(makeNotAllowedError)) {
    displayPrefix.clear();
  }

  std::optional<std::filesystem::path> getPhysicalPath(const canon_path_t& path) override;

  std::string readFile(const canon_path_t& path) override;

  void readFile(const canon_path_t& path, Sink& sink,
                std::function<void(uint64_t)> sizeCallback) override;

  bool pathExists(const canon_path_t& path) override;

  stat_t lstat(const canon_path_t& path) override;

  std::optional<stat_t> maybeLstat(const canon_path_t& path) override;

  dir_entries_t readDirectory(const canon_path_t& path) override;

  std::string readLink(const canon_path_t& path) override;

  std::string showPath(const canon_path_t& path) override;

  std::pair<canon_path_t, std::optional<std::string>> getFingerprint(const canon_path_t& path) override;

  void invalidateCache(const canon_path_t& path) override;

  /**
   * Call `makeNotAllowedError` to throw a `RestrictedPathError`
   * exception if `isAllowed()` returns `false` for `path`.
   */
  void checkAccess(const canon_path_t& path);

  /**
   * Return `true` iff access to path is allowed.
   */
  virtual bool isAllowed(const canon_path_t& path) = 0;
};

/**
 * A wrapping `SourceAccessor` that checks paths against a set of
 * allowed prefixes.
 */
struct AllowListSourceAccessor : public FilteringSourceAccessor {
  /**
   * Grant access to the specified prefix.
   */
  virtual void allowPrefix(canon_path_t prefix) = 0;

  static ref<AllowListSourceAccessor> create(ref<SourceAccessor> next,
                                             std::set<canon_path_t>&& allowedPrefixes,
                                             boost::unordered_flat_set<canon_path_t>&& allowedPaths,
                                             MakeNotAllowedError&& makeNotAllowedError);

  using FilteringSourceAccessor::FilteringSourceAccessor;
};

/**
 * A wrapping `SourceAccessor` mix-in where `isAllowed()` caches the result of virtual
 * `isAllowedUncached()`.
 */
struct CachingFilteringSourceAccessor : FilteringSourceAccessor {
  std::map<canon_path_t, bool> cache;

  using FilteringSourceAccessor::FilteringSourceAccessor;

  bool isAllowed(const canon_path_t& path) override;

  virtual bool isAllowedUncached(const canon_path_t& path) = 0;
};

} // namespace nix
