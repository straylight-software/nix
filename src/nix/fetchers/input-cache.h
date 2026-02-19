#include "nix/fetchers/fetchers.h"

namespace nix::fetchers {

enum class UseRegistries : int;
struct settings_t;

struct InputCache {
  struct CachedResult {
    ref<SourceAccessor> accessor;
    Input resolved_input;
    Input lockedInput;
    Attrs extra_attrs;
  };

  CachedResult get_accessor(const settings_t& settings, Store& store, const Input& original_input,
                           UseRegistries use_registries);

  struct CachedInput {
    Input lockedInput;
    ref<SourceAccessor> accessor;
    Attrs extra_attrs;
  };

  virtual std::optional<CachedInput> lookup(const Input& original_input) const = 0;

  virtual void upsert(Input key, CachedInput cached_input) = 0;

  virtual void clear() = 0;

  static ref<InputCache> create();

  virtual ~InputCache() = default;
};

} // namespace nix::fetchers
