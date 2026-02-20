#pragma once
///@file

#include <limits>
#include <string>

#include "nix/util/file-descriptor.h"

namespace nix {

/**
 * Determine whether \param fd is a terminal.
 */
bool is_tty(descriptor_t fd);

/**
 * Determine whether ANSI escape sequences are appropriate for the
 * present output.
 */
bool is_tty();

/**
 * Truncate a string to 'width' printable characters. If 'filterAll'
 * is true, all ANSI escape sequences are filtered out. Otherwise,
 * some escape sequences (such as colour setting) are copied but not
 * included in the character count. Also, tabs are expanded to
 * spaces.
 */
std::string filter_ansi_escapes(std::string_view s, bool filter_all = false,
                                unsigned int width = std::numeric_limits<unsigned int>::max());

/**
 * Recalculate the window size, updating a global variable.
 *
 * Used in the `SIGWINCH` signal handler on Unix, for example.
 */
void update_window_size();

/**
 * @return the number of rows and columns of the terminal.
 *
 * The value is cached so this is quick. The cached result is computed
 * by `update_window_size()`.
 */
std::pair<unsigned short, unsigned short> get_window_size();

/**
 * @return The number of columns of the terminal, or std::numeric_limits<unsigned int>::max() if
 * unknown.
 */
unsigned int get_window_width();

/**
 * Get the slave name of a pseudoterminal in a thread-safe manner.
 *
 * @param fd The file descriptor of the pseudoterminal master
 * @return The slave device name as a string
 */
std::string get_pts_name(int fd);

} // namespace nix
