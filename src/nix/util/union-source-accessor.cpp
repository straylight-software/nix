#include "nix/util/source-accessor.h"

namespace nix {

struct union_source_accessor_t : SourceAccessor {
  std::vector<ref<SourceAccessor>> accessors;

  union_source_accessor_t(std::vector<ref<SourceAccessor>> _accessors)
      : accessors(std::move(_accessors)) {
    displayPrefix.clear();
  }

  std::string readFile(const canon_path_t& path) override {
    for (auto& accessor : accessors) {
      auto st = accessor->maybeLstat(path);
      if (st)
        return accessor->readFile(path);
    }
    throw FileNotFound("path '%s' does not exist", showPath(path));
  }

  std::optional<stat_t> maybeLstat(const canon_path_t& path) override {
    for (auto& accessor : accessors) {
      auto st = accessor->maybeLstat(path);
      if (st)
        return st;
    }
    return std::nullopt;
  }

  dir_entries_t readDirectory(const canon_path_t& path) override {
    dir_entries_t result;
    bool exists = false;
    for (auto& accessor : accessors) {
      auto st = accessor->maybeLstat(path);
      if (!st)
        continue;
      exists = true;
      for (auto& entry : accessor->readDirectory(path))
        // Don't override entries from previous accessors.
        result.insert(entry);
    }
    if (!exists)
      throw FileNotFound("path '%s' does not exist", showPath(path));
    return result;
  }

  std::string readLink(const canon_path_t& path) override {
    for (auto& accessor : accessors) {
      auto st = accessor->maybeLstat(path);
      if (st)
        return accessor->readLink(path);
    }
    throw FileNotFound("path '%s' does not exist", showPath(path));
  }

  std::string showPath(const canon_path_t& path) override {
    for (auto& accessor : accessors)
      return accessor->showPath(path);
    return SourceAccessor::showPath(path);
  }

  std::optional<std::filesystem::path> getPhysicalPath(const canon_path_t& path) override {
    for (auto& accessor : accessors) {
      auto p = accessor->getPhysicalPath(path);
      if (p)
        return p;
    }
    return std::nullopt;
  }

  std::pair<canon_path_t, std::optional<std::string>> getFingerprint(const canon_path_t& path) override {
    if (fingerprint)
      return {path, fingerprint};
    for (auto& accessor : accessors) {
      auto [subpath, fingerprint] = accessor->getFingerprint(path);
      if (fingerprint)
        return {subpath, fingerprint};
    }
    return {path, std::nullopt};
  }

  void invalidateCache(const canon_path_t& path) override {
    for (auto& accessor : accessors)
      accessor->invalidateCache(path);
  }
};

ref<SourceAccessor> makeUnionSourceAccessor(std::vector<ref<SourceAccessor>>&& accessors) {
  return make_ref<union_source_accessor_t>(std::move(accessors));
}

} // namespace nix
