#include "nix/fetchers/registry.h"

#include <nlohmann/json.hpp>

#include "nix/fetchers/fetch-settings.h"
#include "nix/fetchers/tarball.h"
#include "nix/store/globals.h"
#include "nix/store/local-fs-store.h"
#include "nix/store/store-api.h"
#include "nix/util/users.h"

namespace nix::fetchers {

std::shared_ptr<Registry> Registry::read(const settings_t& settings, const source_path_t& path,
                                         RegistryType type) {
  debug("reading registry '%s'", path);

  if (!path.path_exists())
    return std::make_shared<Registry>(type);

  try {
    return read(settings, path.to_string(), path.read_file(), type);
  } catch (Error& e) {
    warn("cannot read flake registry '%s': %s", path, e.what());
    return std::make_shared<Registry>(type);
  }
}

std::shared_ptr<Registry> Registry::read(const settings_t& settings, std::string_view whence,
                                         std::string_view jsonStr, RegistryType type) {
  auto registry = std::make_shared<Registry>(type);

  try {
    auto json = nlohmann::json::parse(jsonStr);

    auto version = json.value("version", 0);

    if (version == 2) {
      for (auto& i : json["flakes"]) {
        auto toAttrs = json_to_attrs(i["to"]);
        Attrs extra_attrs;
        auto j = toAttrs.find("dir");
        if (j != toAttrs.end()) {
          extra_attrs.insert(*j);
          toAttrs.erase(j);
        }
        auto exact = i.find("exact");
        registry->entries.push_back(
            Entry{.from = input_t::fromAttrs(settings, json_to_attrs(i["from"])),
                  .to = input_t::fromAttrs(settings, std::move(toAttrs)),
                  .extra_attrs = extra_attrs,
                  .exact = exact != i.end() && exact.value()});
      }
    }

    else
      warn("flake registry '%s' has unsupported version %d", whence, version);

  } catch (nlohmann::json::exception& e) {
    warn("cannot parse flake registry '%s': %s", whence, e.what());
  }

  return registry;
}

void Registry::write(const std::filesystem::path& path) {
  nlohmann::json arr;
  for (auto& entry : entries) {
    nlohmann::json obj;
    obj["from"] = attrs_to_json(entry.from.toAttrs());
    obj["to"] = attrs_to_json(entry.to.toAttrs());
    if (!entry.extra_attrs.empty())
      obj["to"].update(attrs_to_json(entry.extra_attrs));
    if (entry.exact)
      obj["exact"] = true;
    arr.emplace_back(std::move(obj));
  }

  nlohmann::json json;
  json["version"] = 2;
  json["flakes"] = std::move(arr);

  create_dirs(path.parent_path());
  write_file(path, json.dump(2));
}

void Registry::add(const input_t& from, const input_t& to, const Attrs& extra_attrs) {
  entries.emplace_back(Entry{.from = from, .to = to, .extra_attrs = extra_attrs});
}

void Registry::remove(const input_t& input) {
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [&](const Entry& entry) { return entry.from == input; }),
                entries.end());
}

static std::filesystem::path get_system_registry_path() {
  return settings.nixConfDir / "registry.json";
}

static std::shared_ptr<Registry> get_system_registry(const settings_t& settings) {
  static auto system_registry =
      Registry::read(settings,
                     source_path_t{get_fs_source_accessor(), canon_path_t{get_system_registry_path().string()}}
                         .resolve_symlinks(),
                     Registry::System);
  return system_registry;
}

std::filesystem::path get_user_registry_path() {
  return get_config_dir() / "registry.json";
}

std::shared_ptr<Registry> get_user_registry(const settings_t& settings) {
  static auto user_registry =
      Registry::read(settings,
                     source_path_t{get_fs_source_accessor(), canon_path_t{get_user_registry_path().string()}}
                         .resolve_symlinks(),
                     Registry::User);
  return user_registry;
}

std::shared_ptr<Registry> get_custom_registry(const settings_t& settings,
                                            const std::filesystem::path& p) {
  static auto custom_registry = Registry::read(
      settings, source_path_t{get_fs_source_accessor(), canon_path_t{p.string()}}.resolve_symlinks(),
      Registry::Custom);
  return custom_registry;
}

std::shared_ptr<Registry> get_flag_registry() {
  static auto flag_registry = std::make_shared<Registry>(Registry::flag_t);
  return flag_registry;
}

void override_registry(const input_t& from, const input_t& to, const Attrs& extra_attrs) {
  get_flag_registry()->add(from, to, extra_attrs);
}

static std::shared_ptr<Registry> get_global_registry(const settings_t& settings, store_t& store) {
  static auto reg = [&]() {
    try {
      auto path = settings.flakeRegistry.get();
      if (path == "") {
        return std::make_shared<Registry>(Registry::Global); // empty registry
      }

      return Registry::read(
          settings,
          [&] -> source_path_t {
            if (!is_absolute(path)) {
              auto store_path = download_file(store, settings, path, "flake-registry.json").store_path;
              if (auto store2 = dynamic_cast<local_fs_store*>(&store))
                store2->addPermRoot(store_path, (get_cache_dir() / "flake-registry.json").string());
              return {store.requireStoreObjectAccessor(store_path)};
            } else {
              return source_path_t{get_fs_source_accessor(), canon_path_t{path}}.resolve_symlinks();
            }
          }(),
          Registry::Global);
    } catch (Error& e) {
      warn("cannot fetch global flake registry '%s', will use builtin fallback registry: %s",
           settings.flakeRegistry.get(), e.info().msg);
      // Use builtin registry as fallback
      return Registry::read(settings, "builtin flake registry",
#include "builtin-flake-registry.json.gen.h"
                            , Registry::Global);
    }
  }();

  return reg;
}

Registries get_registries(const settings_t& settings, store_t& store) {
  Registries registries;
  registries.push_back(get_flag_registry());
  registries.push_back(get_user_registry(settings));
  registries.push_back(get_system_registry(settings));
  registries.push_back(get_global_registry(settings, store));
  return registries;
}

std::pair<input_t, Attrs> lookup_in_registries(const settings_t& settings, store_t& store,
                                           const input_t& _input, UseRegistries use_registries) {
  Attrs extra_attrs;
  int n = 0;
  input_t input(_input);

  if (use_registries == UseRegistries::No)
    return {input, extra_attrs};

restart:

  n++;
  if (n > 100)
    throw Error("cycle detected in flake registry for '%s'", input.to_string());

  for (auto& registry : get_registries(settings, store)) {
    if (use_registries == UseRegistries::Limited && !(registry->type == fetchers::Registry::flag_t ||
                                                     registry->type == fetchers::Registry::Global))
      continue;
    // FIXME: O(n)
    for (auto& entry : registry->entries) {
      if (entry.exact) {
        if (entry.from == input) {
          debug("resolved flakeref '%s' against registry %d exactly", input.to_string(),
                registry->type);
          input = entry.to;
          extra_attrs = entry.extra_attrs;
          goto restart;
        }
      } else {
        if (entry.from.contains(input)) {
          debug("resolved flakeref '%s' against registry %d", input.to_string(), registry->type);
          input = entry.to.applyOverrides(
              !entry.from.getRef() && input.getRef() ? input.getRef()
                                                     : std::optional<std::string>(),
              !entry.from.getRev() && input.getRev() ? input.getRev() : std::optional<Hash>());
          extra_attrs = entry.extra_attrs;
          goto restart;
        }
      }
    }
  }

  if (!input.isDirect())
    throw Error("cannot find flake '%s' in the flake registries", input.to_string());

  debug("looked up '%s' -> '%s'", _input.to_string(), input.to_string());

  return {input, extra_attrs};
}

} // namespace nix::fetchers
