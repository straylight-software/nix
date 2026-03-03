// src/continuity/nar_parser.h
//
// Legacy compatibility wrapper for NAR parsing.
// Wraps the Continuity-generated types in a familiar interface.

#pragma once

#include "continuity/nix/nix_formats.h"

namespace kaitai {

// Re-export Continuity types with legacy names
using nar_t = continuity::nix::nar_t;
using nar_node_t = continuity::nix::nar_node_t;
using nar_entry_t = continuity::nix::nar_entry_t;

// Legacy-compatible parse function
inline auto parse_nar(std::span<const std::uint8_t> data) {
  return continuity::nix::parse_nar(data);
}

// Legacy-compatible serialize function
inline auto serialize_nar(const nar_t& nar) {
  std::vector<std::uint8_t> out;
  continuity::nix::serialize_nar(nar, out);
  return out;
}

} // namespace kaitai
