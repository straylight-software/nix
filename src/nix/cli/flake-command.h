#pragma once

#include "nix/cmd/command.h"
#include "nix/cmd/installable-flake.h"
#include "nix/flake/flake.h"

namespace nix {

using namespace nix::flake;

class FlakeCommand : virtual Args, public MixFlakeOptions {
protected:
  std::string flakeUrl = ".";

public:
  FlakeCommand();

  FlakeRef getFlakeRef();

  LockedFlake lockFlake();

  std::vector<FlakeRef> getFlakeRefsForCompletion() override;
};

} // namespace nix
