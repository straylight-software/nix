#pragma once

#include "nix/expr/eval.h"

namespace nix {
namespace flake {
struct settings_t;
} // namespace flake
} // namespace nix

namespace nix::flake::primops {

/**
 * Returns a `builtins.get_flake` primop with the given nix::flake::settings_t.
 */
nix::PrimOp get_flake(const settings_t& settings);

extern nix::PrimOp parse_flake_ref;
extern nix::PrimOp flake_ref_to_string;

} // namespace nix::flake::primops
