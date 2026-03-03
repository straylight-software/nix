#include "nix/store/store-registration.h"

#include "nix/store/globals.h"
#include "nix/store/local-store.h"
#include "nix/store/store-open.h"
#include "nix/store/uds-remote-store.h"

namespace nix {

ref<store_t> open_store() {
  return open_store(settings.storeUri.get());
}

ref<store_t> open_store(const std::string& uri, const store_t::config_t::Params& extra_params) {
  return open_store(StoreReference::parse(uri, extra_params));
}

ref<store_t> open_store(StoreReference&& store_uri) {
  auto store = resolve_store_config(std::move(store_uri))->open_store();
  store->init();
  return store;
}

ref<store_config_t> resolve_store_config(StoreReference&& store_uri) {
  auto& params = store_uri.params;

  auto store_config =
      std::visit(overloaded{
                     [&](const StoreReference::Auto&) -> ref<store_config_t> {
                       auto stateDir = get_or(params, "state", settings.nixStateDir);
                       if (access(stateDir.c_str(), R_OK | W_OK) == 0) {
                         // Root has direct access to /nix/var/nix - use LocalStore
                         return make_ref<LocalStore::config_t>(params);
                       }
                       // Non-root: use straylight daemonless store if registered
                       // User-space store at ~/.local/share/nix with /nix/store fallback
                       for (const auto& [name, implem] : Implementations::registered()) {
                         if (implem.uriSchemes.count("straylight")) {
                           return implem.parseConfig("straylight", "", params);
                         }
                       }
                       // Fallback to daemon if straylight not available
                       if (path_exists(settings.nixDaemonSocketFile)) {
                         return make_ref<UDSRemoteStore::config_t>(params);
                       }
                       // Last resort: LocalStore (will fail if not writable)
                       return make_ref<LocalStore::config_t>(params);
                     },
                     [&](const StoreReference::Specified& g) {
                       for (const auto& [storeName, implem] : Implementations::registered()) {
                         if (implem.uriSchemes.count(g.scheme)) {
                           return implem.parseConfig(g.scheme, g.authority, params);
                         }
                       }

                       throw Error("don't know how to open Nix store with scheme '%s'", g.scheme);
                     },
                 },
                 store_uri.variant);

  experimental_feature_settings.require(store_config->experimental_feature());
  store_config->warn_unknown_settings();

  return store_config;
}

std::list<ref<store_t>> get_default_substituters() {
  static auto stores([]() {
    std::list<ref<store_t>> stores;

    string_set_t done;

    auto add_store = [&](const std::string& uri) {
      if (!done.insert(uri).second) {
        return;
      }
      try {
        stores.push_back(open_store(uri));
      } catch (Error& e) {
        logWarning(e.info());
      }
    };

    for (const auto& uri : settings.substituters.get()) {
      add_store(uri);
    }

    stores.sort(
        [](ref<store_t>& a, ref<store_t>& b) { return a->config.priority < b->config.priority; });

    return stores;
  }());

  return stores;
}

Implementations::Map& Implementations::registered() {
  static Map registered;
  return registered;
}

} // namespace nix
