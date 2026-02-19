#pragma once

#include "nix/expr/eval.h"

namespace nix {
namespace flake {
struct settings_t;
} // namespace flake
} // namespace nix

namespace nix::flake::primops {

/**
 * Returns a `builtins.getFlake` primop with the given nix::flake::settings_t.
 */
nix::PrimOp getFlake(const settings_t& settings);

extern nix::PrimOp parseFlakeRef;
extern nix::PrimOp flakeRefToString;

} // namespace nix::flake::primops
