// kaitai/narinfo_parser.h
//
// Legacy compatibility wrapper for narinfo parsing.
// Wraps the Cornell-generated types in a familiar interface.

#pragma once

#include "cornell/nix/nix_formats.h"

namespace kaitai {

// Re-export Cornell types with legacy names
using narinfo_t = cornell::nix::narinfo_t;
using compression_t = cornell::nix::compression_t;
using sig_t = cornell::nix::sig_t;

// Legacy-compatible parse function
inline auto parse_narinfo(std::string_view text) {
  return cornell::nix::parse_narinfo(text);
}

// Legacy-compatible serialize function
inline auto serialize_narinfo(const narinfo_t& ni) {
  return cornell::nix::serialize_narinfo(ni);
}

} // namespace kaitai
