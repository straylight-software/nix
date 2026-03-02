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

LocalFSStoreConfig::LocalFSStoreConfig(path_view_t root_dir, const Params& params)
    : store_config_t(params)
      /* Default `?root` from `root_dir` if non set
       * NOTE: We would like to just do root_dir.set(...), which would take care of
       * all normalization and error checking for us. Unfortunately we cannot do
       * that because of the complicated initialization order of other fields with
       * the virtual class hierarchy of nix store configs, and the design of the
       * settings system. As such, we have no choice but to redefine the field and
       * manually repeat the same normalization logic.
       */
      ,
      root_dir{makeRootDirSetting(*this, !root_dir.empty() && params.count("root") == 0
                                             ? std::optional<Path>{canon_path(root_dir)}
                                             : std::nullopt)} {}

local_fs_store::local_fs_store(const config_t& config)
    : store_t{static_cast<const store_t::config_t&>(*this)}, config{config} {}

struct local_store_accessor_t : posix_source_accessor_t {
  ref<local_fs_store> store;
  bool require_valid_path;

  local_store_accessor_t(ref<local_fs_store> store, bool require_valid_path)
      : posix_source_accessor_t(std::filesystem::path{store->config.real_store_dir.get()}),
        store(store),
        require_valid_path(require_valid_path) {}

  void requireStoreObject(const canon_path_t& path) {
    auto [store_path, rest] = store->toStorePath(store->store_dir + path.abs());
    if (require_valid_path && !store->maybeQueryPathInfo(store_path)) {
      throw InvalidPath("path '%1%' is not a valid store path", store->printStorePath(store_path));
    }
  }

  std::optional<stat_t> maybe_lstat(const canon_path_t& path) override {
    /* Also allow `path` to point to the entire store, which is
       needed for resolving symlinks. */
    if (path.is_root()) {
      return stat_t{.type = t_directory};
    }

    requireStoreObject(path);
    return posix_source_accessor_t::maybe_lstat(path);
  }

  dir_entries_t read_directory(const canon_path_t& path) override {
    requireStoreObject(path);
    return posix_source_accessor_t::read_directory(path);
  }

  void read_file(const canon_path_t& path, sink_t& sink,
                 std::function<void(uint64_t)> size_callback) override {
    requireStoreObject(path);
    return posix_source_accessor_t::read_file(path, sink, size_callback);
  }

  std::string read_link(const canon_path_t& path) override {
    requireStoreObject(path);
    return posix_source_accessor_t::read_link(path);
  }
};

ref<source_accessor_t> local_fs_store::getFSAccessor(bool require_valid_path) {
  return make_ref<local_store_accessor_t>(
      ref<local_fs_store>(std::dynamic_pointer_cast<local_fs_store>(shared_from_this())),
      require_valid_path);
}

std::shared_ptr<source_accessor_t> local_fs_store::getFSAccessor(const store_path_t& path,
                                                                 bool require_valid_path) {
  auto abs_path = std::filesystem::path{config.real_store_dir.get()} / path.to_string();
  if (require_valid_path) {
    /* Only return non-null if the store object is a fully-valid
       member of the store. */
    if (!isValidPath(path)) {
      return nullptr;
    }
  } else {
    /* Return non-null as long as the some file system data exists,
       even if the store object is not fully registered. */
    if (!path_exists(abs_path)) {
      return nullptr;
    }
  }
  return std::make_shared<posix_source_accessor_t>(std::move(abs_path));
}

const std::string local_fs_store::drvsLogDir = "drvs";

std::optional<std::string> local_fs_store::getBuildLogExact(const store_path_t& path) {
  auto base_name = path.to_string();

  for (int j = 0; j < 2; j++) {
    Path logPath = j == 0 ? fmt("%s/%s/%s/%s", config.logDir.get(), drvsLogDir,
                                base_name.substr(0, 2), base_name.substr(2))
                          : fmt("%s/%s/%s", config.logDir.get(), drvsLogDir, base_name);
    Path logBz2Path = logPath + ".bz2";

    if (path_exists(logPath)) {
      return read_file(logPath);
    }

    else if (path_exists(logBz2Path)) {
      try {
        return decompress("bzip2", read_file(logBz2Path));
      } catch (Error&) {
      }
    }
  }

  return std::nullopt;
}

} // namespace nix
