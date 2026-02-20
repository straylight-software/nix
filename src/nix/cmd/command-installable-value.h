#pragma once
///@file

#include "nix/cmd/command.h"
#include "nix/cmd/installable-value.h"

namespace nix {

/**
 * An InstallableCommand where the single positional argument must be an
 * InstallableValue in particular.
 */
struct InstallableValueCommand : InstallableCommand {
  /**
   * Entry point to this command
   */
  virtual void run(ref<store_t> store, ref<InstallableValue> installable) = 0;

  void run(ref<store_t> store, ref<Installable> installable) override;
};

} // namespace nix
