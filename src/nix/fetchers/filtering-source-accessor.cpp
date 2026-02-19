#include "nix/fetchers/filtering-source-accessor.h"

#include <boost/unordered/unordered_flat_set.hpp>

#include "nix/util/sync.h"

namespace nix {

std::optional<std::filesystem::path>
FilteringSourceAccessor::getPhysicalPath(const canon_path_t& path) {
  checkAccess(path);
  return next->getPhysicalPath(prefix / path);
}

std::string FilteringSourceAccessor::readFile(const canon_path_t& path) {
  checkAccess(path);
  return next->readFile(prefix / path);
}

void FilteringSourceAccessor::readFile(const canon_path_t& path, Sink& sink,
                                       std::function<void(uint64_t)> sizeCallback) {
  checkAccess(path);
  return next->readFile(prefix / path, sink, sizeCallback);
}

bool FilteringSourceAccessor::pathExists(const canon_path_t& path) {
  return isAllowed(path) && next->pathExists(prefix / path);
}

std::optional<SourceAccessor::stat_t> FilteringSourceAccessor::maybeLstat(const canon_path_t& path) {
  return isAllowed(path) ? next->maybeLstat(prefix / path) : std::nullopt;
}

SourceAccessor::stat_t FilteringSourceAccessor::lstat(const canon_path_t& path) {
  checkAccess(path);
  return next->lstat(prefix / path);
}

SourceAccessor::dir_entries_t FilteringSourceAccessor::readDirectory(const canon_path_t& path) {
  checkAccess(path);
  dir_entries_t entries;
  for (auto& entry : next->readDirectory(prefix / path)) {
    if (isAllowed(path / entry.first))
      entries.insert(std::move(entry));
  }
  return entries;
}

std::string FilteringSourceAccessor::readLink(const canon_path_t& path) {
  checkAccess(path);
  return next->readLink(prefix / path);
}

std::string FilteringSourceAccessor::showPath(const canon_path_t& path) {
  return displayPrefix + next->showPath(prefix / path) + displaySuffix;
}

std::pair<canon_path_t, std::optional<std::string>>
FilteringSourceAccessor::getFingerprint(const canon_path_t& path) {
  if (fingerprint)
    return {path, fingerprint};
  return next->getFingerprint(prefix / path);
}

void FilteringSourceAccessor::invalidateCache(const canon_path_t& path) {
  next->invalidateCache(prefix / path);
}

void FilteringSourceAccessor::checkAccess(const canon_path_t& path) {
  if (!isAllowed(path))
    throw makeNotAllowedError
        ? makeNotAllowedError(path)
        : RestrictedPathError("access to path '%s' is forbidden", showPath(path));
}

struct allow_list_source_accessor_impl_t : AllowListSourceAccessor {
  shared_sync_t<std::set<canon_path_t>> allowedPrefixes;
  shared_sync_t<boost::unordered_flat_set<canon_path_t>> allowedPaths;

  allow_list_source_accessor_impl_t(ref<SourceAccessor> next, std::set<canon_path_t>&& allowedPrefixes,
                              boost::unordered_flat_set<canon_path_t>&& allowedPaths,
                              MakeNotAllowedError&& makeNotAllowedError)
      : AllowListSourceAccessor(source_path_t(next), std::move(makeNotAllowedError)),
        allowedPrefixes(std::move(allowedPrefixes)),
        allowedPaths(std::move(allowedPaths)) {}

  bool isAllowed(const canon_path_t& path) override {
    return allowedPaths.readLock()->contains(path) || path.isAllowed(*allowedPrefixes.readLock());
  }

  void allowPrefix(canon_path_t prefix) override { allowedPrefixes.lock()->insert(std::move(prefix)); }
};

ref<AllowListSourceAccessor>
AllowListSourceAccessor::create(ref<SourceAccessor> next, std::set<canon_path_t>&& allowedPrefixes,
                                boost::unordered_flat_set<canon_path_t>&& allowedPaths,
                                MakeNotAllowedError&& makeNotAllowedError) {
  return make_ref<allow_list_source_accessor_impl_t>(
      next, std::move(allowedPrefixes), std::move(allowedPaths), std::move(makeNotAllowedError));
}

bool CachingFilteringSourceAccessor::isAllowed(const canon_path_t& path) {
  auto i = cache.find(path);
  if (i != cache.end())
    return i->second;
  auto res = isAllowedUncached(path);
  cache.emplace(path, res);
  return res;
}

} // namespace nix
