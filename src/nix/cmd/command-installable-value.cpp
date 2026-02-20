#include "nix/cmd/command-installable-value.h"

namespace nix {

void InstallableValueCommand::run(ref<store_t> store, ref<Installable> installable) {
  auto installableValue = InstallableValue::require(installable);
  run(store, installableValue);
}

} // namespace nix
