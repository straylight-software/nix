// straylight::nix::build::embedded_guest (stub implementation)
//
// This is the stub implementation used when STRAYLIGHT_EMBED_GUEST is not enabled.
// It reports that no embedded guest data is available.

#include "embedded_guest.h"

namespace straylight::nix::build::embedded_guest {

bool is_available() {
  return false;
}

std::span<const uint8_t> kernel_data() {
  return {};
}

std::span<const uint8_t> initrd_data() {
  return {};
}

} // namespace straylight::nix::build::embedded_guest
