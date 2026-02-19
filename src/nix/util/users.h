#pragma once
///@file

#include <filesystem>

#include "nix/util/types.h"

#ifndef _WIN32
#  include <sys/types.h>
#endif

namespace nix {

std::string get_user_name();

#ifndef _WIN32
/**
 * @return the given user's home directory from /etc/passwd.
 */
std::filesystem::path get_home_of(uid_t user_id);
#endif

/**
 * @return $HOME or the user's home directory from /etc/passwd.
 */
std::filesystem::path get_home();

/**
 * @return $NIX_CACHE_HOME or $XDG_CACHE_HOME/nix or $HOME/.cache/nix.
 */
std::filesystem::path get_cache_dir();

/**
 * @return $NIX_CONFIG_HOME or $XDG_CONFIG_HOME/nix or $HOME/.config/nix.
 */
std::filesystem::path get_config_dir();

/**
 * @return the directories to search for user configuration files
 */
std::vector<std::filesystem::path> get_config_dirs();

/**
 * @return $NIX_DATA_HOME or $XDG_DATA_HOME/nix or $HOME/.local/share/nix.
 */
std::filesystem::path get_data_dir();

/**
 * @return $NIX_STATE_HOME or $XDG_STATE_HOME/nix or $HOME/.local/state/nix.
 */
std::filesystem::path get_state_dir();

/**
 * Create the Nix state directory and return the path to it.
 */
std::filesystem::path create_nix_state_dir();

/**
 * Perform tilde expansion on a path, replacing tilde with the user's
 * home directory.
 */
std::string expand_tilde(std::string_view path);

/**
 * Is the current user UID 0 on Unix?
 *
 * Currently always false on Windows, but that may change.
 */
bool is_root_user();

} // namespace nix
