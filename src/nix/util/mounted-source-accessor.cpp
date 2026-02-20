#include "nix/util/mounted-source-accessor.h"

#include <boost/unordered/concurrent_flat_map.hpp>

namespace nix {

struct mounted_source_accessor_impl_t : mounted_source_accessor_t {
  boost::concurrent_flat_map<canon_path_t, ref<source_accessor_t>> mounts;

  mounted_source_accessor_impl_t(std::map<canon_path_t, ref<source_accessor_t>> _mounts) {
    display_prefix.clear();

    // Currently we require a root filesystem. This could be relaxed.
    assert(_mounts.contains(canon_path_t::root));

    for (auto& [path, accessor] : _mounts)
      mount(path, accessor);

    // FIXME: return dummy parent directories automatically?
  }

  std::string read_file(const canon_path_t& path) override {
    auto [accessor, subpath] = resolve(path);
    return accessor->read_file(subpath);
  }

  stat_t lstat(const canon_path_t& path) override {
    auto [accessor, subpath] = resolve(path);
    return accessor->lstat(subpath);
  }

  std::optional<stat_t> maybe_lstat(const canon_path_t& path) override {
    auto [accessor, subpath] = resolve(path);
    return accessor->maybe_lstat(subpath);
  }

  dir_entries_t read_directory(const canon_path_t& path) override {
    auto [accessor, subpath] = resolve(path);
    return accessor->read_directory(subpath);
  }

  std::string read_link(const canon_path_t& path) override {
    auto [accessor, subpath] = resolve(path);
    return accessor->read_link(subpath);
  }

  std::string show_path(const canon_path_t& path) override {
    auto [accessor, subpath] = resolve(path);
    return display_prefix + accessor->show_path(subpath) + display_suffix;
  }

  std::pair<ref<source_accessor_t>, canon_path_t> resolve(canon_path_t path) {
    // Find the nearest parent of `path` that is a mount point.
    std::vector<std::string> subpath;
    while (true) {
      if (auto mount = get_mount(path)) {
        std::reverse(subpath.begin(), subpath.end());
        return {ref(mount), canon_path_t(subpath)};
      }

      assert(!path.is_root());
      subpath.push_back(std::string(*path.base_name()));
      path.pop();
    }
  }

  std::optional<std::filesystem::path> get_physical_path(const canon_path_t& path) override {
    auto [accessor, subpath] = resolve(path);
    return accessor->get_physical_path(subpath);
  }

  void mount(canon_path_t mount_point, ref<source_accessor_t> accessor) override {
    mounts.emplace(std::move(mount_point), std::move(accessor));
  }

  std::shared_ptr<source_accessor_t> get_mount(canon_path_t mount_point) override {
    if (auto res = get_concurrent(mounts, mount_point))
      return *res;
    else
      return nullptr;
  }

  std::pair<canon_path_t, std::optional<std::string>>
  get_fingerprint(const canon_path_t& path) override {
    if (fingerprint)
      return {path, fingerprint};
    auto [accessor, subpath] = resolve(path);
    return accessor->get_fingerprint(subpath);
  }

  void invalidate_cache(const canon_path_t& path) override {
    auto [accessor, subpath] = resolve(path);
    accessor->invalidate_cache(subpath);
  }
};

ref<mounted_source_accessor_t>
make_mounted_source_accessor(std::map<canon_path_t, ref<source_accessor_t>> mounts) {
  return make_ref<mounted_source_accessor_impl_t>(std::move(mounts));
}

} // namespace nix
