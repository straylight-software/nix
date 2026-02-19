#pragma once
///@file

#include "nix/store/gc-store.h"
#include "nix/store/log-store.h"
#include "nix/store/store-api.h"

namespace nix {

struct LocalFSStoreConfig : virtual StoreConfig {
private:
  static optional_path_setting_t makeRootDirSetting(LocalFSStoreConfig& self,
                                                std::optional<Path> default_value) {
    return {
        &self,
        std::move(default_value),
        "root",
        "Directory prefixed to all other paths.",
    };
  }

public:
  using StoreConfig::StoreConfig;

  /**
   * Used to override the `root` settings. Can't be done via modifying
   * `params` reliably because this parameter is unused except for
   * passing to base class constructors.
   *
   * @todo Make this less error-prone with new store settings system.
   */
  LocalFSStoreConfig(path_view_t path, const Params& params);

  optional_path_setting_t root_dir = makeRootDirSetting(*this, std::nullopt);

private:
  /**
   * An indirection so that we don't need to refer to global settings
   * in headers.
   */
  static Path getDefaultStateDir();

  /**
   * An indirection so that we don't need to refer to global settings
   * in headers.
   */
  static Path getDefaultLogDir();

public:
  path_setting_t stateDir{this, root_dir.get() ? *root_dir.get() + "/nix/var/nix" : getDefaultStateDir(),
                       "state", "Directory where Nix stores state."};

  path_setting_t logDir{this, root_dir.get() ? *root_dir.get() + "/nix/var/log/nix" : getDefaultLogDir(),
                     "log", "directory where Nix stores log files."};

  path_setting_t real_store_dir{this, root_dir.get() ? *root_dir.get() + "/nix/store" : store_dir, "real",
                           "Physical path of the Nix store."};
};

struct alignas(8) /* Work around ASAN failures on i686-linux. */
    local_fs_store : virtual Store,
                   virtual GcStore,
                   virtual LogStore {
  using config_t = LocalFSStoreConfig;

  const config_t& config;

  inline static std::string operation_name = "Local Filesystem Store";

  const static std::string drvsLogDir;

  local_fs_store(const config_t& params);

  ref<SourceAccessor> getFSAccessor(bool require_valid_path = true) override;
  std::shared_ptr<SourceAccessor> getFSAccessor(const StorePath& path,
                                                bool require_valid_path = true) override;

  /**
   * Creates symlink from the `gc_root` to the `store_path` and
   * registers the `gc_root` as a permanent GC root. The `gc_root`
   * symlink lives outside the store and is created and owned by the
   * user.
   *
   * @param gc_root The location of the symlink.
   *
   * @param store_path The store object being rooted. The symlink will
   * point to `toRealPath(store.printStorePath(store_path))`.
   *
   * How the permanent GC root corresponding to this symlink is
   * managed is implementation-specific.
   */
  virtual Path addPermRoot(const StorePath& store_path, const Path& gc_root) = 0;

  virtual Path getRealStoreDir() { return config.real_store_dir; }

  Path toRealPath(const StorePath& store_path) { return toRealPath(printStorePath(store_path)); }

  Path toRealPath(const Path& store_path) {
    assert(isInStore(store_path));
    return getRealStoreDir() + "/" + std::string(store_path, store_dir.size() + 1);
  }

  std::optional<std::string> getBuildLogExact(const StorePath& path) override;
};

} // namespace nix
