#pragma once

#include "nix/util/args.h"

namespace nix {

struct MixLongListing : virtual Args {
  bool longListing = false;

  MixLongListing() {
    addFlag({
        .longName = "long",
        .shortName = 'l',
        .description = "Show detailed file information.",
        .handler = {&longListing, true},
    });
  }
};

} // namespace nix
