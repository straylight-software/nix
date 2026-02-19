#pragma once

#include "nix/cmd/command.h"
#include "nix/cmd/installable-flake.h"
#include "nix/flake/flake.h"

namespace nix {

using namespace nix::flake;

class flake_command_t : virtual Args, public MixFlakeOptions {
protected:
  std::string flakeUrl = ".";

public:
  flake_command_t();

  FlakeRef get_flake_ref();

  LockedFlake lock_flake();

  std::vector<FlakeRef> get_flake_refs_for_completion() override;
};

} // namespace nix
