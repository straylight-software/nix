#include "nix/store/local-binary-cache-store.h"

#include <atomic>

#include <sys/file.h>

#include "nix/store/globals.h"
#include "nix/store/nar-info-disk-cache.h"
#include "nix/store/store-registration.h"
#include "nix/util/signals.h"

namespace nix {

/**
 * RAII wrapper for flock-based file locking.
 * Used to coordinate concurrent writes to the local binary cache.
 */
class CacheLock {
  int fd = -1;

public:
  CacheLock(const std::filesystem::path& lockPath) {
    fd = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
      throw sys_error_t("opening lock file '%s'", lockPath);
    }

    // Block until we acquire exclusive lock
    while (::flock(fd, LOCK_EX) != 0) {
      if (errno != EINTR) {
        throw sys_error_t("acquiring lock on '%s'", lockPath);
      }
    }
  }

  ~CacheLock() {
    if (fd >= 0) {
      ::flock(fd, LOCK_UN);
      ::close(fd);
    }
  }

  CacheLock(const CacheLock&) = delete;
  CacheLock& operator=(const CacheLock&) = delete;
};

LocalBinaryCacheStoreConfig::LocalBinaryCacheStoreConfig(std::string_view scheme,
                                                         path_view_t binaryCacheDir,
                                                         const StoreReference::Params& params)
    : store_t::config_t{params},
      binary_cache_store_config_t{params},
      binaryCacheDir(binaryCacheDir) {}

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

private:
  /**
   * Get the path to the lock file for a given cache file.
   * Uses a single lock file per cache to serialize all writes.
   * This is simpler and avoids issues with per-file lock cleanup.
   */
  std::filesystem::path getLockPath() const { return config->binaryCacheDir + "/.cache.lock"; }

  /**
   * Check if a path represents a narinfo file (hash-addressed).
   * These files are content-addressed and idempotent to write.
   */
  static bool isNarInfoPath(const std::string& path) {
    return has_suffix(path, ".narinfo") && path.find('/') == std::string::npos;
  }

  /**
   * Check if a path represents a NAR file (content-addressed).
   * NAR files are named by their content hash, so concurrent writes
   * of the same path will have identical content.
   */
  static bool isNarPath(const std::string& path) {
    return has_prefix(path, "nar/") &&
           (has_suffix(path, ".nar") || has_suffix(path, ".nar.xz") ||
            has_suffix(path, ".nar.bz2") || has_suffix(path, ".nar.zst") ||
            has_suffix(path, ".nar.lzip") || has_suffix(path, ".nar.lz4") ||
            has_suffix(path, ".nar.br"));
  }

protected:
  bool file_exists(const std::string& path) override;

  /**
   * Atomically write a file to the cache with proper locking.
   *
   * Concurrency safety:
   * 1. For content-addressed files (NAR, narinfo): If the file already exists,
   *    we skip the write since content-addressed files are immutable and
   *    another process must have written the identical content.
   *
   * 2. For other files: We use flock to serialize writes, then atomic rename
   *    to ensure readers never see partial content.
   *
   * 3. The atomic rename pattern (write to tmp, rename to final) ensures that
   *    readers either see the complete old content or complete new content.
   */
  void upsert_file(const std::string& path, restartable_source_t& source,
                   const std::string& mime_type, uint64_t size_hint) override {
    auto path2 = config->binaryCacheDir + "/" + path;

    // For content-addressed files (NAR and narinfo), check if already exists.
    // These are idempotent: if present, the content is guaranteed identical.
    bool isContentAddressed = isNarInfoPath(path) || isNarPath(path);
    if (isContentAddressed && path_exists(path2)) {
      // Another process already wrote this file. Since it's content-addressed,
      // the content is identical, so we can safely skip.
      return;
    }

    // Acquire cache-wide lock to serialize writes.
    // This prevents races where two processes both pass the exists check above.
    CacheLock lock(getLockPath());

    // Double-check after acquiring lock (another process may have written it
    // while we were waiting for the lock).
    if (isContentAddressed && path_exists(path2)) {
      return;
    }

    // Generate unique temporary filename using pid and atomic counter
    static std::atomic<int> counter{0};
    Path tmp = fmt("%s.tmp.%d.%d", path2, getpid(), ++counter);
    auto_delete_t del(tmp, false);

    // Write to temporary file
    write_file(tmp, source);

    // Atomic rename to final location.
    // This is atomic on POSIX: either the old file or new file is visible,
    // never partial content.
    std::filesystem::rename(tmp, path2);
    del.cancel();
  }

  void getFile(const std::string& path, sink_t& sink) override {
    try {
      read_file(config->binaryCacheDir + "/" + path, sink);
    } catch (sys_error_t& e) {
      if (e.err_no() == ENOENT) {
        throw NoSuchBinaryCacheFile("file '%s' does not exist in binary cache", path);
      }
      throw;
    }
  }

  store_path_set_t query_all_valid_paths() override {
    store_path_set_t paths;

    for (auto& entry : directory_iterator_t{config->binaryCacheDir}) {
      check_interrupt();
      auto name = entry.path().filename().string();
      if (name.size() != 40 || !has_suffix(name, ".narinfo")) {
        continue;
      }
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
  if (config->writeDebugInfo) {
    create_dirs(config->binaryCacheDir + "/debuginfo");
  }
  create_dirs(config->binaryCacheDir + "/log");
  binary_cache_store::init();
}

bool local_binary_cache_store_t::file_exists(const std::string& path) {
  return path_exists(config->binaryCacheDir + "/" + path);
}

string_set_t LocalBinaryCacheStoreConfig::uriSchemes() {
  if (get_env("_NIX_FORCE_HTTP") == "1") {
    return {};
  } else {
    return {"file"};
  }
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
