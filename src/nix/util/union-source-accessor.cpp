#include "nix/util/source-accessor.h"

namespace nix {

struct union_source_accessor_t : source_accessor_t {
  std::vector<ref<source_accessor_t>> accessors;

  union_source_accessor_t(std::vector<ref<source_accessor_t>> _accessors)
      : accessors(std::move(_accessors)) {
    display_prefix.clear();
  }

  std::string read_file(const canon_path_t& path) override {
    for (auto& accessor : accessors) {
      auto st = accessor->maybe_lstat(path);
      if (st) {
        return accessor->read_file(path);
      }
    }
    throw FileNotFound("path '%s' does not exist", show_path(path));
  }

  std::optional<stat_t> maybe_lstat(const canon_path_t& path) override {
    for (auto& accessor : accessors) {
      auto st = accessor->maybe_lstat(path);
      if (st) {
        return st;
      }
    }
    return std::nullopt;
  }

  dir_entries_t read_directory(const canon_path_t& path) override {
    dir_entries_t result;
    bool exists = false;
    for (auto& accessor : accessors) {
      auto st = accessor->maybe_lstat(path);
      if (!st) {
        continue;
      }
      exists = true;
      for (auto& entry : accessor->read_directory(path)) {
        // Don't override entries from previous accessors.
        result.insert(entry);
      }
    }
    if (!exists) {
      throw FileNotFound("path '%s' does not exist", show_path(path));
    }
    return result;
  }

  std::string read_link(const canon_path_t& path) override {
    for (auto& accessor : accessors) {
      auto st = accessor->maybe_lstat(path);
      if (st) {
        return accessor->read_link(path);
      }
    }
    throw FileNotFound("path '%s' does not exist", show_path(path));
  }

  std::string show_path(const canon_path_t& path) override {
    for (auto& accessor : accessors) {
      return accessor->show_path(path);
    }
    return source_accessor_t::show_path(path);
  }

  std::optional<std::filesystem::path> get_physical_path(const canon_path_t& path) override {
    for (auto& accessor : accessors) {
      auto p = accessor->get_physical_path(path);
      if (p) {
        return p;
      }
    }
    return std::nullopt;
  }

  std::pair<canon_path_t, std::optional<std::string>>
  get_fingerprint(const canon_path_t& path) override {
    if (fingerprint) {
      return {path, fingerprint};
    }
    for (auto& accessor : accessors) {
      auto [subpath, fingerprint] = accessor->get_fingerprint(path);
      if (fingerprint) {
        return {subpath, fingerprint};
      }
    }
    return {path, std::nullopt};
  }

  void invalidate_cache(const canon_path_t& path) override {
    for (auto& accessor : accessors) {
      accessor->invalidate_cache(path);
    }
  }
};

ref<source_accessor_t> make_union_source_accessor(std::vector<ref<source_accessor_t>>&& accessors) {
  return make_ref<union_source_accessor_t>(std::move(accessors));
}

} // namespace nix
