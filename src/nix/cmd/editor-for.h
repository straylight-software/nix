#pragma once
///@file

#include "nix/util/source-path.h"
#include "nix/util/types.h"

namespace nix {

/**
 * Helper function to generate args that invoke $EDITOR on
 * filename:lineno.
 */
strings_t editorFor(const source_path_t& file, uint32_t line);

} // namespace nix
