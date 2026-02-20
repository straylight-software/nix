#include "nix/util/exit.h"

// All special member functions are defaulted in the header.
// This file exists for ABI stability and to ensure the vtable is emitted.

namespace nix {

// Explicit instantiation point for vtable
// (empty - all methods are inline/defaulted in header)

} // namespace nix
