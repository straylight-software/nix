#pragma once
///@file

#include <optional>

#include "nix/util/types.h"

namespace nix {

/**
 * Save the current mount namespace. Ignored if called more than
 * once.
 */
void save_mount_namespace();

/**
 * Restore the mount namespace saved by save_mount_namespace(). Ignored
 * if save_mount_namespace() was never called.
 */
void restore_mount_namespace();

/**
 * Cause this thread to try to not share any FS attributes with the main
 * thread, because this causes setns() in restore_mount_namespace() to
 * fail.
 *
 * This is best effort -- EPERM and ENOSYS failures are just ignored.
 */
void try_unshare_filesystem();

bool user_namespaces_supported();

bool mount_and_pid_namespaces_supported();

} // namespace nix
