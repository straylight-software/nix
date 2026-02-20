#include "nix/fetchers/filtering-source-accessor.h"

#include <boost/unordered/unordered_flat_set.hpp>

#include "nix/util/sync.h"

namespace nix {

std::optional<std::filesystem::path>
FilteringSourceAccessor::get_physical_path(const canon_path_t& path) {
  checkAccess(path);
  return next->get_physical_path(prefix / path);
}

std::string FilteringSourceAccessor::read_file(const canon_path_t& path) {
  checkAccess(path);
  return next->read_file(prefix / path);
}

void FilteringSourceAccessor::read_file(const canon_path_t& path, sink_t& sink,
                                       std::function<void(uint64_t)> size_callback) {
  checkAccess(path);
  return next->read_file(prefix / path, sink, size_callback);
}

bool FilteringSourceAccessor::path_exists(const canon_path_t& path) {
  return is_allowed(path) && next->path_exists(prefix / path);
}

std::optional<source_accessor_t::stat_t> FilteringSourceAccessor::maybe_lstat(const canon_path_t& path) {
  return is_allowed(path) ? next->maybe_lstat(prefix / path) : std::nullopt;
}

source_accessor_t::stat_t FilteringSourceAccessor::lstat(const canon_path_t& path) {
  checkAccess(path);
  return next->lstat(prefix / path);
}

source_accessor_t::dir_entries_t FilteringSourceAccessor::read_directory(const canon_path_t& path) {
  checkAccess(path);
  dir_entries_t entries;
  for (auto& entry : next->read_directory(prefix / path)) {
    if (is_allowed(path / entry.first))
      entries.insert(std::move(entry));
  }
  return entries;
}

std::string FilteringSourceAccessor::read_link(const canon_path_t& path) {
  checkAccess(path);
  return next->read_link(prefix / path);
}

std::string FilteringSourceAccessor::show_path(const canon_path_t& path) {
  return display_prefix + next->show_path(prefix / path) + display_suffix;
}

std::pair<canon_path_t, std::optional<std::string>>
FilteringSourceAccessor::get_fingerprint(const canon_path_t& path) {
  if (fingerprint)
    return {path, fingerprint};
  return next->get_fingerprint(prefix / path);
}

void FilteringSourceAccessor::invalidate_cache(const canon_path_t& path) {
  next->invalidate_cache(prefix / path);
}

void FilteringSourceAccessor::checkAccess(const canon_path_t& path) {
  if (!is_allowed(path))
    throw make_not_allowed_error
        ? make_not_allowed_error(path)
        : RestrictedPathError("access to path '%s' is forbidden", show_path(path));
}

struct allow_list_source_accessor_impl_t : AllowListSourceAccessor {
  shared_sync_t<std::set<canon_path_t>> allowed_prefixes;
  shared_sync_t<boost::unordered_flat_set<canon_path_t>> allowed_paths;

  allow_list_source_accessor_impl_t(ref<source_accessor_t> next, std::set<canon_path_t>&& allowed_prefixes,
                              boost::unordered_flat_set<canon_path_t>&& allowed_paths,
                              MakeNotAllowedError&& make_not_allowed_error)
      : AllowListSourceAccessor(source_path_t(next), std::move(make_not_allowed_error)),
        allowed_prefixes(std::move(allowed_prefixes)),
        allowed_paths(std::move(allowed_paths)) {}

  bool is_allowed(const canon_path_t& path) override {
    return allowed_paths.read_lock()->contains(path) || path.is_allowed(*allowed_prefixes.read_lock());
  }

  void allowPrefix(canon_path_t prefix) override { allowed_prefixes.lock()->insert(std::move(prefix)); }
};

ref<AllowListSourceAccessor>
AllowListSourceAccessor::create(ref<source_accessor_t> next, std::set<canon_path_t>&& allowed_prefixes,
                                boost::unordered_flat_set<canon_path_t>&& allowed_paths,
                                MakeNotAllowedError&& make_not_allowed_error) {
  return make_ref<allow_list_source_accessor_impl_t>(
      next, std::move(allowed_prefixes), std::move(allowed_paths), std::move(make_not_allowed_error));
}

bool CachingFilteringSourceAccessor::is_allowed(const canon_path_t& path) {
  auto i = cache.find(path);
  if (i != cache.end())
    return i->second;
  auto res = isAllowedUncached(path);
  cache.emplace(path, res);
  return res;
}

} // namespace nix
