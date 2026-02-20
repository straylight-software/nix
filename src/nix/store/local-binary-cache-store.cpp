#include "nix/store/local-binary-cache-store.h"

#include <atomic>

#include "nix/store/globals.h"
#include "nix/store/nar-info-disk-cache.h"
#include "nix/store/store-registration.h"
#include "nix/util/signals.h"

namespace nix {

LocalBinaryCacheStoreConfig::LocalBinaryCacheStoreConfig(std::string_view scheme,
                                                         path_view_t binaryCacheDir,
                                                         const StoreReference::Params& params)
    : store_t::config_t{params}, binary_cache_store_config_t{params}, binaryCacheDir(binaryCacheDir) {}

std::string LocalBinaryCacheStoreConfig::doc() {
  return
#include "local-binary-cache-store.md"
      ;
}

StoreReference LocalBinaryCacheStoreConfig::getReference() const {
  return {
      .variant =
          StoreReference::Specified{
              .scheme = "file",
              .authority = binaryCacheDir,
          },
  };
}

struct local_binary_cache_store_t : virtual binary_cache_store {
  using config_t = LocalBinaryCacheStoreConfig;

  ref<config_t> config;

  local_binary_cache_store_t(ref<config_t> config)
      : store_t{*config}, binary_cache_store{*config}, config{config} {}

  void init() override;

protected:
  bool file_exists(const std::string& path) override;

  void upsert_file(const std::string& path, restartable_source_t& source,
                   const std::string& mime_type, uint64_t size_hint) override {
    auto path2 = config->binaryCacheDir + "/" + path;
    static std::atomic<int> counter{0};
    Path tmp = fmt("%s.tmp.%d.%d", path2, getpid(), ++counter);
    auto_delete_t del(tmp, false);
    write_file(tmp, source);
    std::filesystem::rename(tmp, path2);
    del.cancel();
  }

  void getFile(const std::string& path, sink_t& sink) override {
    try {
      read_file(config->binaryCacheDir + "/" + path, sink);
    } catch (sys_error_t& e) {
      if (e.err_no() == ENOENT)
        throw NoSuchBinaryCacheFile("file '%s' does not exist in binary cache", path);
      throw;
    }
  }

  store_path_set_t query_all_valid_paths() override {
    store_path_set_t paths;

    for (auto& entry : directory_iterator_t{config->binaryCacheDir}) {
      check_interrupt();
      auto name = entry.path().filename().string();
      if (name.size() != 40 || !has_suffix(name, ".narinfo"))
        continue;
      paths.insert(
          parseStorePath(store_dir + "/" + name.substr(0, name.size() - 8) + "-" + MissingName));
    }

    return paths;
  }

  std::optional<TrustedFlag> isTrustedClient() override { return Trusted; }
};

void local_binary_cache_store_t::init() {
  create_dirs(config->binaryCacheDir + "/nar");
  create_dirs(config->binaryCacheDir + "/" + realisationsPrefix);
  if (config->writeDebugInfo)
    create_dirs(config->binaryCacheDir + "/debuginfo");
  create_dirs(config->binaryCacheDir + "/log");
  binary_cache_store::init();
}

bool local_binary_cache_store_t::file_exists(const std::string& path) {
  return path_exists(config->binaryCacheDir + "/" + path);
}

string_set_t LocalBinaryCacheStoreConfig::uriSchemes() {
  if (get_env("_NIX_FORCE_HTTP") == "1")
    return {};
  else
    return {"file"};
}

ref<store_t> LocalBinaryCacheStoreConfig::open_store() const {
  auto store = make_ref<local_binary_cache_store_t>(
      ref{// FIXME we shouldn't actually need a mutable config
          std::const_pointer_cast<local_binary_cache_store_t::config_t>(shared_from_this())});
  store->init();
  return store;
}

static RegisterStoreImplementation<local_binary_cache_store_t::config_t>
    reg_local_binary_cache_store;

} // namespace nix
