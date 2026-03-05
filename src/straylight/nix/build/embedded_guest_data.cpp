// straylight::nix::build::embedded_guest (full implementation)
//
// This file is generated during the build process.
// It contains zstd-compressed kernel and initrd data.
//
// To regenerate:
//   1. Build firecracker-guest: nix build .#firecracker-guest
//   2. Compress: zstd -19 vmlinux -o vmlinux.zst && zstd -19 initrd.img -o initrd.zst
//   3. Convert to C array: xxd -i vmlinux.zst > embedded_guest_data.cpp
//
// For now, this is a placeholder that will be replaced by the actual
// genrule output during the build.

#include "embedded_guest.h"

// When embedding is enabled, these symbols come from linked object files
// The symbols are created by objcopy from binary blobs

#if defined(STRAYLIGHT_EMBED_GUEST_DATA)
// These symbols are defined by objcopy from the binary blobs
// Symbol names are derived from filenames: vmlinux.zst -> _binary_vmlinux_zst_*
//                                          initrd.img -> _binary_initrd_img_*
extern "C" {
extern const uint8_t _binary_vmlinux_zst_start[];
extern const uint8_t _binary_vmlinux_zst_end[];
extern const uint8_t _binary_initrd_img_start[];
extern const uint8_t _binary_initrd_img_end[];
}
#endif

namespace straylight::nix::build::embedded_guest {

#if defined(STRAYLIGHT_EMBED_GUEST_DATA)

bool is_available() {
  return true;
}

std::span<const uint8_t> kernel_data() {
  return std::span{_binary_vmlinux_zst_start,
                   static_cast<size_t>(_binary_vmlinux_zst_end - _binary_vmlinux_zst_start)};
}

std::span<const uint8_t> initrd_data() {
  return std::span{_binary_initrd_img_start,
                   static_cast<size_t>(_binary_initrd_img_end - _binary_initrd_img_start)};
}

#else

// Stub implementation when embedding is disabled
bool is_available() {
  return false;
}

std::span<const uint8_t> kernel_data() {
  return {};
}

std::span<const uint8_t> initrd_data() {
  return {};
}

#endif

} // namespace straylight::nix::build::embedded_guest
