#pragma once
///@file

#include "nix/fetchers/fetchers.h"
#include "nix/util/source-path.h"
#include "nix/util/types.h"

namespace nix {
class store_t;
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
    input_t from, to;
    Attrs extra_attrs;
    bool exact = false;
  };

  std::vector<Entry> entries;

  Registry(RegistryType type) : type{type} {}

  static std::shared_ptr<Registry> read(const settings_t& settings, const source_path_t& path,
                                        RegistryType type);

  static std::shared_ptr<Registry> read(const settings_t& settings, std::string_view whence,
                                        std::string_view jsonStr, RegistryType type);

  void write(const std::filesystem::path& path);

  void add(const input_t& from, const input_t& to, const Attrs& extra_attrs);

  void remove(const input_t& input);
};

using Registries = std::vector<std::shared_ptr<Registry>>;

std::shared_ptr<Registry> get_user_registry(const settings_t& settings);

std::shared_ptr<Registry> get_custom_registry(const settings_t& settings,
                                            const std::filesystem::path& p);

std::filesystem::path get_user_registry_path();

Registries get_registries(const settings_t& settings, store_t& store);

void override_registry(const input_t& from, const input_t& to, const Attrs& extra_attrs);

enum class UseRegistries : int {
  No,
  All,
  Limited, // global and flag registry only
};

/**
 * Rewrite a flakeref using the registries. If `filter` is set, only
 * use the registries for which the filter function returns true.
 */
std::pair<input_t, Attrs> lookup_in_registries(const settings_t& settings, store_t& store,
                                           const input_t& input, UseRegistries use_registries);

} // namespace nix::fetchers
