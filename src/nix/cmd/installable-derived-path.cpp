#include "nix/cmd/installable-derived-path.h"

#include "nix/store/derivations.h"

namespace nix {

std::string InstallableDerivedPath::what() const {
  return derived_path.to_string(*store);
}

DerivedPathsWithInfo InstallableDerivedPath::to_derived_paths() {
  return {{
      .path = derived_path,
      .info = make_ref<ExtraPathInfo>(),
  }};
}

std::optional<StorePath> InstallableDerivedPath::getStorePath() {
  return derived_path.getBaseStorePath();
}

InstallableDerivedPath InstallableDerivedPath::parse(ref<Store> store, std::string_view prefix,
                                                     ExtendedOutputsSpec extendedOutputsSpec) {
  auto derived_path =
      std::visit(overloaded{
                     // If the user did not use ^, we treat the output more
                     // liberally: we accept a symlink chain or an actual
                     // store path.
                     [&](const ExtendedOutputsSpec::Default&) -> DerivedPath {
                       auto store_path = store->followLinksToStorePath(prefix);
                       return DerivedPath::opaque_t{
                           .path = std::move(store_path),
                       };
                     },
                     // If the user did use ^, we just do exactly what is written.
                     [&](const ExtendedOutputsSpec::Explicit& outputSpec) -> DerivedPath {
                       auto drv =
                           make_ref<SingleDerivedPath>(SingleDerivedPath::parse(*store, prefix));
                       drv_require_experiment(*drv);
                       return DerivedPath::Built{
                           .drv_path = std::move(drv),
                           .outputs = outputSpec,
                       };
                     },
                 },
                 extendedOutputsSpec.raw);
  return InstallableDerivedPath{
      store,
      std::move(derived_path),
  };
}

} // namespace nix
