#pragma once

#include "nix/util/args.h"

namespace nix {

struct mix_long_listing_t : virtual Args {
  bool longListing = false;

  mix_long_listing_t() {
    addFlag({
        .longName = "long",
        .shortName = 'l',
        .description = "Show detailed file information.",
        .handler = {&longListing, true},
    });
  }
};

} // namespace nix
