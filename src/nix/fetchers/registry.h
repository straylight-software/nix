#pragma once
///@file

#include "nix/fetchers/fetchers.h"
#include "nix/util/source-path.h"
#include "nix/util/types.h"

namespace nix {
class Store;
}

namespace nix::fetchers {

struct Registry {
  enum RegistryType {
    flag_t = 0,
    User = 1,
    System = 2,
    Global = 3,
    Custom = 4,
  };

  RegistryType type;

  struct Entry {
    Input from, to;
    Attrs extraAttrs;
    bool exact = false;
  };

  std::vector<Entry> entries;

  Registry(RegistryType type) : type{type} {}

  static std::shared_ptr<Registry> read(const settings_t& settings, const source_path_t& path,
                                        RegistryType type);

  static std::shared_ptr<Registry> read(const settings_t& settings, std::string_view whence,
                                        std::string_view jsonStr, RegistryType type);

  void write(const std::filesystem::path& path);

  void add(const Input& from, const Input& to, const Attrs& extraAttrs);

  void remove(const Input& input);
};

typedef std::vector<std::shared_ptr<Registry>> Registries;

std::shared_ptr<Registry> getUserRegistry(const settings_t& settings);

std::shared_ptr<Registry> getCustomRegistry(const settings_t& settings,
                                            const std::filesystem::path& p);

std::filesystem::path getUserRegistryPath();

Registries getRegistries(const settings_t& settings, Store& store);

void overrideRegistry(const Input& from, const Input& to, const Attrs& extraAttrs);

enum class UseRegistries : int {
  No,
  All,
  Limited, // global and flag registry only
};

/**
 * Rewrite a flakeref using the registries. If `filter` is set, only
 * use the registries for which the filter function returns true.
 */
std::pair<Input, Attrs> lookupInRegistries(const settings_t& settings, Store& store,
                                           const Input& input, UseRegistries useRegistries);

} // namespace nix::fetchers
