#include "nix/store/local-fs-store.h"

#include "nix/store/derivations.h"
#include "nix/store/globals.h"
#include "nix/store/store-api.h"
#include "nix/util/archive.h"
#include "nix/util/compression.h"
#include "nix/util/posix-source-accessor.h"

namespace nix {

Path LocalFSStoreConfig::getDefaultStateDir() {
  return settings.nixStateDir;
}

Path LocalFSStoreConfig::getDefaultLogDir() {
  return settings.nixLogDir;
}

LocalFSStoreConfig::LocalFSStoreConfig(path_view_t rootDir, const Params& params)
    : StoreConfig(params)
      /* Default `?root` from `rootDir` if non set
       * NOTE: We would like to just do rootDir.set(...), which would take care of
       * all normalization and error checking for us. Unfortunately we cannot do
       * that because of the complicated initialization order of other fields with
       * the virtual class hierarchy of nix store configs, and the design of the
       * settings system. As such, we have no choice but to redefine the field and
       * manually repeat the same normalization logic.
       */
      ,
      rootDir{makeRootDirSetting(*this, !rootDir.empty() && params.count("root") == 0
                                            ? std::optional<Path>{canonPath(rootDir)}
                                            : std::nullopt)} {}

LocalFSStore::LocalFSStore(const Config& config)
    : Store{static_cast<const Store::Config&>(*this)}, config{config} {}

struct local_store_accessor_t : posix_source_accessor_t {
  ref<LocalFSStore> store;
  bool requireValidPath;

  local_store_accessor_t(ref<LocalFSStore> store, bool requireValidPath)
      : posix_source_accessor_t(std::filesystem::path{store->config.realStoreDir.get()}),
        store(store),
        requireValidPath(requireValidPath) {}

  void requireStoreObject(const canon_path_t& path) {
    auto [storePath, rest] = store->toStorePath(store->storeDir + path.abs());
    if (requireValidPath && !store->maybeQueryPathInfo(storePath))
      throw InvalidPath("path '%1%' is not a valid store path", store->printStorePath(storePath));
  }

  std::optional<stat_t> maybeLstat(const canon_path_t& path) override {
    /* Also allow `path` to point to the entire store, which is
       needed for resolving symlinks. */
    if (path.isRoot())
      return stat_t{.type = tDirectory};

    requireStoreObject(path);
    return posix_source_accessor_t::maybeLstat(path);
  }

  dir_entries_t readDirectory(const canon_path_t& path) override {
    requireStoreObject(path);
    return posix_source_accessor_t::readDirectory(path);
  }

  void readFile(const canon_path_t& path, Sink& sink,
                std::function<void(uint64_t)> sizeCallback) override {
    requireStoreObject(path);
    return posix_source_accessor_t::readFile(path, sink, sizeCallback);
  }

  std::string readLink(const canon_path_t& path) override {
    requireStoreObject(path);
    return posix_source_accessor_t::readLink(path);
  }
};

ref<SourceAccessor> LocalFSStore::getFSAccessor(bool requireValidPath) {
  return make_ref<local_store_accessor_t>(
      ref<LocalFSStore>(std::dynamic_pointer_cast<LocalFSStore>(shared_from_this())),
      requireValidPath);
}

std::shared_ptr<SourceAccessor> LocalFSStore::getFSAccessor(const StorePath& path,
                                                            bool requireValidPath) {
  auto absPath = std::filesystem::path{config.realStoreDir.get()} / path.to_string();
  if (requireValidPath) {
    /* Only return non-null if the store object is a fully-valid
       member of the store. */
    if (!isValidPath(path))
      return nullptr;
  } else {
    /* Return non-null as long as the some file system data exists,
       even if the store object is not fully registered. */
    if (!pathExists(absPath))
      return nullptr;
  }
  return std::make_shared<posix_source_accessor_t>(std::move(absPath));
}

const std::string LocalFSStore::drvsLogDir = "drvs";

std::optional<std::string> LocalFSStore::getBuildLogExact(const StorePath& path) {
  auto baseName = path.to_string();

  for (int j = 0; j < 2; j++) {
    Path logPath = j == 0 ? fmt("%s/%s/%s/%s", config.logDir.get(), drvsLogDir,
                                baseName.substr(0, 2), baseName.substr(2))
                          : fmt("%s/%s/%s", config.logDir.get(), drvsLogDir, baseName);
    Path logBz2Path = logPath + ".bz2";

    if (pathExists(logPath))
      return readFile(logPath);

    else if (pathExists(logBz2Path)) {
      try {
        return decompress("bzip2", readFile(logBz2Path));
      } catch (Error&) {
      }
    }
  }

  return std::nullopt;
}

} // namespace nix
