#pragma once

// straylight/nix/testing/temp_dir.h - Test utilities for temporary directories
//
// Provides a robust temp_directory_path() that falls back to /tmp when TMPDIR
// points to a non-existent directory (common in sandboxed build environments).

#include <cstdlib>
#include <filesystem>

namespace straylight::nix::testing {

/// Returns a valid temporary directory path, falling back to /tmp if TMPDIR
/// points to a non-existent directory.
[[nodiscard]] inline auto temp_directory_path() -> std::filesystem::path {
  std::error_code ec;
  auto path = std::filesystem::temp_directory_path(ec);
  if (!ec && std::filesystem::exists(path, ec)) {
    return path;
  }
  // Fallback to /tmp
  return "/tmp";
}

} // namespace straylight::nix::testing
