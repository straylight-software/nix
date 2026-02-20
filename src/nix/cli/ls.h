#pragma once

#include "nix/util/args.h"

namespace nix {

struct mix_long_listing_t : virtual args_t {
  bool long_listing = false;

  mix_long_listing_t() {
    add_flag({
        .long_name = "long",
        .short_name = 'l',
        .description = "Show detailed file information.",
        .handler = {&long_listing, true},
    });
  }
};

} // namespace nix
