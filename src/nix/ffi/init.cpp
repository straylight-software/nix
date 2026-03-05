/**
 * @file init.cpp
 * @brief Initialization and version functions for Nix FFI.
 */

#include <atomic>

#include <nix/util/util.h>

#include "internal.h"

namespace {
std::atomic<bool> g_initialized{false};
}

extern "C" {

NixError nix_init(void) {
  if (g_initialized.exchange(true)) {
    return NIX_OK; // Already initialized
  }

  return nix::ffi::catch_errors([&] {
    // Initialize Nix GC runtime
    nix::init_gc();
    return NIX_OK;
  });
}

const char* nix_version(void) {
  // TODO: Return actual version from nix::version
  return "3.0.0-straylight";
}

} // extern "C"
