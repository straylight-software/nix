#include "nix/store/local-overlay-store.h"

#include <regex>

#include "nix/store/realisation.h"
#include "nix/store/store-open.h"
#include "nix/store/store-registration.h"
#include "nix/util/callback.h"
#include "nix/util/processes.h"
#include "nix/util/url.h"

namespace nix {

std::string LocalOverlayStoreConfig::doc() {
  return
#include "local-overlay-store.md"
      ;
}

ref<store_t> LocalOverlayStoreConfig::open_store() const {
  return make_ref<local_overlay_store>(
      ref{std::dynamic_pointer_cast<const LocalOverlayStoreConfig>(shared_from_this())});
}

StoreReference LocalOverlayStoreConfig::getReference() const {
  return {
      .variant =
          StoreReference::Specified{
              .scheme = *uriSchemes().begin(),
          },
  };
}

Path LocalOverlayStoreConfig::toUpperPath(const store_path_t& path) const {
  return upperLayer + "/" + path.to_string();
}

local_overlay_store::local_overlay_store(ref<const config_t> config)
    : store_t{*config},
      local_fs_store{*config},
      LocalStore{static_cast<ref<const LocalStore::config_t>>(config)},
      config{config},
      lowerStore(open_store(percent_decode(config->lowerStoreUri.get()))
                     .dynamic_pointer_cast<local_fs_store>()) {
  if (config->checkMount.get()) {
    std::smatch match;
    std::string mountInfo;
    auto mounts = read_file(std::filesystem::path{"/proc/self/mounts"});
    auto regex = std::regex(R"((^|\n)overlay )" + config->real_store_dir.get() + R"( .*(\n|$))");

    // Mount points can be stacked, so there might be multiple matching entries.
    // Loop until the last match, which will be the current state of the mount point.
    while (std::regex_search(mounts, match, regex)) {
      mountInfo = match.str();
      mounts = match.suffix();
    }

    auto checkOption = [&](std::string option, std::string value) {
      return std::regex_search(mountInfo, std::regex("\\b" + option + "=" + value + "( |,)"));
    };

    auto expectedLowerDir = lowerStore->config.real_store_dir.get();
    if (!checkOption("lowerdir", expectedLowerDir) ||
        !checkOption("upperdir", config->upperLayer)) {
      debug("expected lowerdir: %s", expectedLowerDir);
      debug("expected upperdir: %s", config->upperLayer);
      debug("actual mount: %s", mountInfo);
      throw Error("overlay filesystem '%s' mounted incorrectly", config->real_store_dir.get());
    }
  }
}

void local_overlay_store::register_drv_output(const realisation_t& info) {
  // First do queryRealisation on lower layer to populate DB
  auto res = lowerStore->query_realisation(info.id);
  if (res) {
    LocalStore::register_drv_output({*res, info.id});
  }

  LocalStore::register_drv_output(info);
}

void local_overlay_store::query_path_info_uncached(
    const store_path_t& path,
    Callback<std::shared_ptr<const valid_path_info_t>> callback) noexcept {
  auto callbackPtr = std::make_shared<decltype(callback)>(std::move(callback));

  LocalStore::query_path_info_uncached(
      path, {[this, path, callbackPtr](std::future<std::shared_ptr<const valid_path_info_t>> fut) {
        try {
          auto info = fut.get();
          if (info) {
            return (*callbackPtr)(std::move(info));
          }
        } catch (...) {
          return callbackPtr->rethrow();
        }
        // If we don't have it, check lower store
        lowerStore->queryPathInfo(
            path, {[path, callbackPtr](std::future<ref<const valid_path_info_t>> fut) {
              try {
                (*callbackPtr)(fut.get().get_ptr());
              } catch (...) {
                return callbackPtr->rethrow();
              }
            }});
      }});
}

void local_overlay_store::query_realisation_uncached(
    const DrvOutput& drvOutput,
    Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept {
  auto callbackPtr = std::make_shared<decltype(callback)>(std::move(callback));

  LocalStore::query_realisation_uncached(
      drvOutput,
      {[this, drvOutput, callbackPtr](std::future<std::shared_ptr<const UnkeyedRealisation>> fut) {
        try {
          auto info = fut.get();
          if (info) {
            return (*callbackPtr)(std::move(info));
          }
        } catch (...) {
          return callbackPtr->rethrow();
        }
        // If we don't have it, check lower store
        lowerStore->query_realisation(
            drvOutput, {[callbackPtr](std::future<std::shared_ptr<const UnkeyedRealisation>> fut) {
              try {
                (*callbackPtr)(fut.get());
              } catch (...) {
                return callbackPtr->rethrow();
              }
            }});
      }});
}

bool local_overlay_store::isValidPathUncached(const store_path_t& path) {
  auto res = LocalStore::isValidPathUncached(path);
  if (res) {
    return res;
  }
  res = lowerStore->isValidPath(path);
  if (res) {
    // Get path info from lower store so upper DB genuinely has it.
    auto p = lowerStore->queryPathInfo(path);
    // recur on references, syncing entire closure.
    for (auto& r : p->references) {
      if (r != path) {
        isValidPath(r);
      }
    }
    LocalStore::registerValidPath(*p);
  }
  return res;
}

void local_overlay_store::query_referrers(const store_path_t& path, store_path_set_t& referrers) {
  LocalStore::query_referrers(path, referrers);
  lowerStore->query_referrers(path, referrers);
}

void local_overlay_store::queryGCReferrers(const store_path_t& path, store_path_set_t& referrers) {
  LocalStore::query_referrers(path, referrers);
}

store_path_set_t local_overlay_store::queryValidDerivers(const store_path_t& path) {
  auto res = LocalStore::queryValidDerivers(path);
  for (const auto& p : lowerStore->queryValidDerivers(path)) {
    res.insert(p);
  }
  return res;
}

std::optional<store_path_t>
local_overlay_store::queryPathFromHashPart(const std::string& hash_part) {
  auto res = LocalStore::queryPathFromHashPart(hash_part);
  if (res) {
    return res;
  } else {
    return lowerStore->queryPathFromHashPart(hash_part);
  }
}

void local_overlay_store::registerValidPaths(const ValidPathInfos& infos) {
  // First, get any from lower store so we merge
  {
    store_path_set_t notInUpper;
    for (auto& [p, _] : infos) {
      if (!LocalStore::isValidPathUncached(p)) { // avoid divergence
        notInUpper.insert(p);
      }
    }
    auto pathsInLower = lowerStore->queryValidPaths(notInUpper);
    ValidPathInfos inLower;
    for (auto& p : pathsInLower) {
      inLower.insert_or_assign(p, *lowerStore->queryPathInfo(p));
    }
    LocalStore::registerValidPaths(inLower);
  }
  // Then do original request
  LocalStore::registerValidPaths(infos);
}

void local_overlay_store::collectGarbage(const GCOptions& options, GCResults& results) {
  LocalStore::collectGarbage(options, results);

  remountIfNecessary();
}

void local_overlay_store::deleteStorePath(const Path& path, uint64_t& bytes_freed) {
  auto mergedDir = config->real_store_dir.get() + "/";
  if (path.substr(0, mergedDir.length()) != mergedDir) {
    warn("local-overlay: unexpected gc path '%s' ", path);
    return;
  }

  store_path_t store_path = {path.substr(mergedDir.length())};
  auto upperPath = config->toUpperPath(store_path);

  if (path_exists(upperPath)) {
    debug("upper exists: %s", path);
    if (lowerStore->isValidPath(store_path)) {
      debug("lower exists: %s", store_path.to_string());
      // Path also exists in lower store.
      // We must delete via upper layer to avoid creating a whiteout.
      delete_path(upperPath, bytes_freed);
      _remountRequired = true;
    } else {
      // Path does not exist in lower store.
      // So we can delete via overlayfs and not need to remount.
      LocalStore::deleteStorePath(path, bytes_freed);
    }
  }
}

void local_overlay_store::optimiseStore() {
  activity_t act(*logger, act_optimise_store);

  // Note for LocalOverlayStore, queryAllValidPaths only returns paths in upper layer
  auto paths = query_all_valid_paths();

  act.progress(0, paths.size());

  uint64_t done = 0;

  for (auto& path : paths) {
    if (lowerStore->isValidPath(path)) {
      uint64_t bytes_freed = 0;
      // Deduplicate store path
      deleteStorePath(toRealPath(path), bytes_freed);
    }
    done++;
    act.progress(done, paths.size());
  }

  remountIfNecessary();
}

LocalStore::VerificationResult local_overlay_store::verifyAllValidPaths(RepairFlag repair) {
  store_path_set_t done;

  auto existsInStoreDir = [&](const store_path_t& store_path) {
    return path_exists(config->real_store_dir.get() + "/" + store_path.to_string());
  };

  bool errors = false;
  store_path_set_t validPaths;

  for (auto& i : query_all_valid_paths()) {
    verifyPath(i, existsInStoreDir, done, validPaths, repair, errors);
  }

  return {
      .errors = errors,
      .validPaths = validPaths,
  };
}

void local_overlay_store::remountIfNecessary() {
  if (!_remountRequired) {
    return;
  }

  if (config->remountHook.get().empty()) {
    warn("'%s' needs remounting, set remount-hook to do this automatically",
         config->real_store_dir.get());
  } else {
    run_program(config->remountHook, false, {config->real_store_dir});
  }

  _remountRequired = false;
}

static RegisterStoreImplementation<local_overlay_store::config_t> reg_local_overlay_store;

} // namespace nix
