#include "nix/store/binary-cache-store.h"

namespace nix {

struct LocalBinaryCacheStoreConfig : std::enable_shared_from_this<LocalBinaryCacheStoreConfig>,
                                     virtual store_t::config_t,
                                     binary_cache_store_config_t {
  using binary_cache_store_config_t::binary_cache_store_config_t;

  /**
   * @param binaryCacheDir `file://` is a short-hand for `file:///`
   * for now.
   */
  LocalBinaryCacheStoreConfig(std::string_view scheme, path_view_t binaryCacheDir,
                              const Params& params);

  Path binaryCacheDir;

  static const std::string name() { return "Local Binary Cache store_t"; }

  static string_set_t uriSchemes();

  static std::string doc();

  ref<store_t> open_store() const override;

  StoreReference getReference() const override;
};

} // namespace nix
