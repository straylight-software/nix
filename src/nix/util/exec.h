#pragma once

#include "nix/util/os-string.h"

namespace nix {

/**
 * `execvpe` is a GNU extension, so we need to implement it for other POSIX
 * platforms.
 *
 * We use our own implementation unconditionally for consistency.
 */
int execvpe(const os_char_t* file0, const os_char_t* const argv[], const os_char_t* const envp[]);

} // namespace nix
