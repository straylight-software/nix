#pragma once

#include <boost/unordered/unordered_flat_set_fwd.hpp>

#include "nix/util/source-path.h"

namespace nix {

/**
 * A function that returns an exception of type
 * `RestrictedPathError` explaining that access to `path` is
 * forbidden.
 */
using MakeNotAllowedError = std::function<RestrictedPathError(const canon_path_t& path)>;

/**
 * An abstract wrapping `source_accessor_t` that performs access
 * control. Subclasses should override `is_allowed()` to implement an
 * access control policy. The error message is customized at construction.
 */
struct FilteringSourceAccessor : source_accessor_t {
  ref<source_accessor_t> next;
  canon_path_t prefix;
  MakeNotAllowedError make_not_allowed_error;

  FilteringSourceAccessor(const source_path_t& src, MakeNotAllowedError&& make_not_allowed_error)
      : next(src.accessor),
        prefix(src.path),
        make_not_allowed_error(std::move(make_not_allowed_error)) {
    display_prefix.clear();
  }

  std::optional<std::filesystem::path> get_physical_path(const canon_path_t& path) override;

  std::string read_file(const canon_path_t& path) override;

  void read_file(const canon_path_t& path, sink_t& sink,
                 std::function<void(uint64_t)> size_callback) override;

  bool path_exists(const canon_path_t& path) override;

  stat_t lstat(const canon_path_t& path) override;

  std::optional<stat_t> maybe_lstat(const canon_path_t& path) override;

  dir_entries_t read_directory(const canon_path_t& path) override;

  std::string read_link(const canon_path_t& path) override;

  std::string show_path(const canon_path_t& path) override;

  std::pair<canon_path_t, std::optional<std::string>>
  get_fingerprint(const canon_path_t& path) override;

  void invalidate_cache(const canon_path_t& path) override;

  /**
   * Call `make_not_allowed_error` to throw a `RestrictedPathError`
   * exception if `is_allowed()` returns `false` for `path`.
   */
  void checkAccess(const canon_path_t& path);

  /**
   * Return `true` iff access to path is allowed.
   */
  virtual bool is_allowed(const canon_path_t& path) = 0;
};

/**
 * A wrapping `source_accessor_t` that checks paths against a set of
 * allowed prefixes.
 */
struct AllowListSourceAccessor : public FilteringSourceAccessor {
  /**
   * Grant access to the specified prefix.
   */
  virtual void allowPrefix(canon_path_t prefix) = 0;

  static ref<AllowListSourceAccessor>
  create(ref<source_accessor_t> next, std::set<canon_path_t>&& allowed_prefixes,
         boost::unordered_flat_set<canon_path_t>&& allowed_paths,
         MakeNotAllowedError&& make_not_allowed_error);

  using FilteringSourceAccessor::FilteringSourceAccessor;
};

/**
 * A wrapping `source_accessor_t` mix-in where `is_allowed()` caches the result of virtual
 * `isAllowedUncached()`.
 */
struct CachingFilteringSourceAccessor : FilteringSourceAccessor {
  std::map<canon_path_t, bool> cache;

  using FilteringSourceAccessor::FilteringSourceAccessor;

  bool is_allowed(const canon_path_t& path) override;

  virtual bool isAllowedUncached(const canon_path_t& path) = 0;
};

} // namespace nix
