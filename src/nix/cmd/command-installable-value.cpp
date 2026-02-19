#include "nix/cmd/command-installable-value.h"

namespace nix {

void InstallableValueCommand::run(ref<Store> store, ref<Installable> installable) {
  auto installableValue = InstallableValue::require(installable);
  run(store, installableValue);
}

} // namespace nix
