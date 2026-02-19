#pragma once
///@file

#include <filesystem>
#include <string>

namespace nix {

/**
 * @brief Get path to the nix manual dir.
 *
 * Nix relies on the man pages being available at a NIX_MAN_DIR for
 * displaying help messaged for legacy cli.
 *
 * NIX_MAN_DIR is a compile-time parameter, so man pages are unlikely to work
 * for cases when the nix executable is installed out-of-store or as a static binary.
 *
 */
std::filesystem::path get_nix_man_dir();

/**
 * Show the manual page for the specified program.
 *
 * @param name Name of the man item.
 */
void show_man_page(const std::string& name);

} // namespace nix
