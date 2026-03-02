// src/continuity/narinfo_parser.h
//
// Legacy compatibility wrapper for narinfo parsing.
// Wraps the Continuity-generated types in a familiar interface.

#pragma once

#include "continuity/nix/nix_formats.h"

namespace kaitai {

// Re-export Continuity types with legacy names
using narinfo_t = continuity::nix::narinfo_t;
using compression_t = continuity::nix::compression_t;
using sig_t = continuity::nix::sig_t;

// Legacy-compatible parse function
inline auto parse_narinfo(std::string_view text) {
  return continuity::nix::parse_narinfo(text);
}

// Legacy-compatible serialize function
inline auto serialize_narinfo(const narinfo_t& ni) {
  return continuity::nix::serialize_narinfo(ni);
}

} // namespace kaitai
