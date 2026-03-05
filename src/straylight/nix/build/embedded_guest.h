// straylight::nix::build::embedded_guest
//
// Header for accessing embedded Firecracker guest kernel and initrd.
// The actual data is linked in from external object files generated
// during the build process.
//
// When embedding is enabled (STRAYLIGHT_EMBED_GUEST=1), the kernel and
// initrd are compressed with zstd and linked into the binary.
//
// Usage:
//   if (embedded_guest::is_available()) {
//     auto kernel = embedded_guest::kernel_data();
//     auto initrd = embedded_guest::initrd_data();
//     // Create VM with vmm_create_from_embedded()
//   }

#ifndef STRAYLIGHT_NIX_BUILD_EMBEDDED_GUEST_H
#define STRAYLIGHT_NIX_BUILD_EMBEDDED_GUEST_H

#include <cstddef>
#include <cstdint>
#include <span>

namespace straylight::nix::build::embedded_guest {

/// Check if embedded guest data is available
bool is_available();

/// Get the compressed kernel data (zstd compressed vmlinux)
std::span<const uint8_t> kernel_data();

/// Get the compressed initrd data (zstd compressed, already gzip'd for kernel)
std::span<const uint8_t> initrd_data();

/// Compression format used (matches VMM_COMPRESS_* constants)
constexpr uint32_t compression_format() {
  return 2;
} // zstd

} // namespace straylight::nix::build::embedded_guest

#endif // STRAYLIGHT_NIX_BUILD_EMBEDDED_GUEST_H
