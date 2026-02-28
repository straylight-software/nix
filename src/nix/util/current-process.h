#pragma once
///@file

#include <chrono>
#include <optional>

#ifndef _WIN32
#  include <sys/resource.h>
#endif

#include "nix/util/types.h"

namespace nix {

/**
 * Get the current process's user space CPU time.
 */
std::chrono::microseconds get_cpu_user_time();

/**
 * If cgroups are active, attempt to calculate the number of CPUs available.
 * If cgroups are unavailable or if cpu.max is set to "max", return 0.
 */
unsigned int get_max_cpu();

// It does not seem possible to dynamically change stack size on Windows.
#ifndef _WIN32
/**
 * Change the stack size.
 */
void set_stack_size(size_t stack_size);

/**
 * Save the current umask for later restoration.
 * Should be called early in main() before any umask changes.
 */
void save_umask();

/**
 * Restore the saved umask. Called by restore_process_context().
 */
void restore_umask();
#endif

/**
 * Restore the original inherited Unix process context (such as signal
 * masks, stack size).

 * See unix::start_signal_handler_thread(), unix::save_signal_mask().
 */
void restore_process_context(bool restore_mounts = true);

/**
 * @return the path of the current executable.
 */
std::optional<Path> get_self_exe();

} // namespace nix
