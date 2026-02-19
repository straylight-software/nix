#include "nix/util/mounted-source-accessor.h"

#include <boost/unordered/concurrent_flat_map.hpp>

namespace nix {

struct mounted_source_accessor_impl_t : mounted_source_accessor_t {
  boost::concurrent_flat_map<canon_path_t, ref<SourceAccessor>> mounts;

  mounted_source_accessor_impl_t(std::map<canon_path_t, ref<SourceAccessor>> _mounts) {
    displayPrefix.clear();

    // Currently we require a root filesystem. This could be relaxed.
    assert(_mounts.contains(canon_path_t::root));

    for (auto& [path, accessor] : _mounts)
      mount(path, accessor);

    // FIXME: return dummy parent directories automatically?
  }

  std::string readFile(const canon_path_t& path) override {
    auto [accessor, subpath] = resolve(path);
    return accessor->readFile(subpath);
  }

  stat_t lstat(const canon_path_t& path) override {
    auto [accessor, subpath] = resolve(path);
    return accessor->lstat(subpath);
  }

  std::optional<stat_t> maybeLstat(const canon_path_t& path) override {
    auto [accessor, subpath] = resolve(path);
    return accessor->maybeLstat(subpath);
  }

  dir_entries_t readDirectory(const canon_path_t& path) override {
    auto [accessor, subpath] = resolve(path);
    return accessor->readDirectory(subpath);
  }

  std::string readLink(const canon_path_t& path) override {
    auto [accessor, subpath] = resolve(path);
    return accessor->readLink(subpath);
  }

  std::string showPath(const canon_path_t& path) override {
    auto [accessor, subpath] = resolve(path);
    return displayPrefix + accessor->showPath(subpath) + displaySuffix;
  }

  std::pair<ref<SourceAccessor>, canon_path_t> resolve(canon_path_t path) {
    // Find the nearest parent of `path` that is a mount point.
    std::vector<std::string> subpath;
    while (true) {
      if (auto mount = getMount(path)) {
        std::reverse(subpath.begin(), subpath.end());
        return {ref(mount), canon_path_t(subpath)};
      }

      assert(!path.isRoot());
      subpath.push_back(std::string(*path.baseName()));
      path.pop();
    }
  }

  std::optional<std::filesystem::path> getPhysicalPath(const canon_path_t& path) override {
    auto [accessor, subpath] = resolve(path);
    return accessor->getPhysicalPath(subpath);
  }

  void mount(canon_path_t mountPoint, ref<SourceAccessor> accessor) override {
    mounts.emplace(std::move(mountPoint), std::move(accessor));
  }

  std::shared_ptr<SourceAccessor> getMount(canon_path_t mountPoint) override {
    if (auto res = getConcurrent(mounts, mountPoint))
      return *res;
    else
      return nullptr;
  }

  std::pair<canon_path_t, std::optional<std::string>> getFingerprint(const canon_path_t& path) override {
    if (fingerprint)
      return {path, fingerprint};
    auto [accessor, subpath] = resolve(path);
    return accessor->getFingerprint(subpath);
  }

  void invalidateCache(const canon_path_t& path) override {
    auto [accessor, subpath] = resolve(path);
    accessor->invalidateCache(subpath);
  }
};

ref<mounted_source_accessor_t>
makeMountedSourceAccessor(std::map<canon_path_t, ref<SourceAccessor>> mounts) {
  return make_ref<mounted_source_accessor_impl_t>(std::move(mounts));
}

} // namespace nix
