#include <string>

#include "nix/fetchers/fetchers.h"

namespace nix::fetchers {

enum class UseRegistries : int;
struct settings_t;

struct InputCache {
  struct CachedResult {
    ref<source_accessor_t> accessor;
    input_t resolved_input;
    input_t lockedInput;
    Attrs extra_attrs;
  };

  CachedResult get_accessor(const settings_t& settings, store_t& store,
                            const input_t& original_input, UseRegistries use_registries);

  struct CachedInput {
    input_t lockedInput;
    ref<source_accessor_t> accessor;
    Attrs extra_attrs;
  };

  virtual std::optional<CachedInput> lookup(const input_t& original_input) const = 0;

  virtual void upsert(input_t key, CachedInput cached_input) = 0;

  virtual void clear() = 0;

  /**
   * Cache for lock file content to avoid re-reading and re-parsing lock files
   * for sub-flakes during a single evaluation session. This addresses issue #9339.
   */
  struct CachedLockFile {
    std::string content;
    std::string path;
  };

  virtual std::optional<CachedLockFile> lookupLockFile(const std::string& path) const = 0;
  virtual void upsertLockFile(std::string path, CachedLockFile lock_file) = 0;

  /**
   * Cache for registry lookup results to avoid repeated registry resolution
   * during evaluation. Also helps with issue #9339.
   */
  struct CachedRegistryLookup {
    input_t resolved_input;
    Attrs extra_attrs;
  };

  virtual std::optional<CachedRegistryLookup>
  lookupRegistryResolution(const input_t& input) const = 0;
  virtual void upsertRegistryResolution(input_t key, CachedRegistryLookup result) = 0;

  static ref<InputCache> create();

  virtual ~InputCache() = default;
};

} // namespace nix::fetchers
