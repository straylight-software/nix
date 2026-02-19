#include "nix/store/binary-cache-store.h"

namespace nix {

struct LocalBinaryCacheStoreConfig : std::enable_shared_from_this<LocalBinaryCacheStoreConfig>,
                                     virtual Store::Config,
                                     BinaryCacheStoreConfig {
  using BinaryCacheStoreConfig::BinaryCacheStoreConfig;

  /**
   * @param binaryCacheDir `file://` is a short-hand for `file:///`
   * for now.
   */
  LocalBinaryCacheStoreConfig(std::string_view scheme, path_view_t binaryCacheDir,
                              const Params& params);

  Path binaryCacheDir;

  static const std::string name() { return "Local Binary Cache Store"; }

  static string_set_t uriSchemes();

  static std::string doc();

  ref<Store> openStore() const override;

  StoreReference getReference() const override;
};

} // namespace nix
