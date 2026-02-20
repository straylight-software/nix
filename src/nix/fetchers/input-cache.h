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

  CachedResult get_accessor(const settings_t& settings, store_t& store, const input_t& original_input,
                           UseRegistries use_registries);

  struct CachedInput {
    input_t lockedInput;
    ref<source_accessor_t> accessor;
    Attrs extra_attrs;
  };

  virtual std::optional<CachedInput> lookup(const input_t& original_input) const = 0;

  virtual void upsert(input_t key, CachedInput cached_input) = 0;

  virtual void clear() = 0;

  static ref<InputCache> create();

  virtual ~InputCache() = default;
};

} // namespace nix::fetchers
