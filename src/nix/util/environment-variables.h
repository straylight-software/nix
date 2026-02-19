#pragma once
/**
 * @file
 *
 * Utilities for working with the current process's environment
 * variables.
 */

#include <optional>

#include "nix/util/file-path.h"
#include "nix/util/types.h"

namespace nix {

static constexpr auto environment_variables_category = "Options that change environment variables";

/**
 * @return an environment variable.
 */
std::optional<std::string> get_env(const std::string& key);

/**
 * Like `get_env`, but using `os_string_t` to avoid coercions.
 */
std::optional<os_string_t> get_env_os(const os_string_t& key);

/**
 * @return a non empty environment variable. Returns nullopt if the env
 * variable is set to ""
 */
std::optional<std::string> get_env_non_empty(const std::string& key);

/**
 * Get the entire environment.
 */
string_map_t get_env();

#ifdef _WIN32
/**
 * Implementation of missing POSIX function.
 */
int unsetenv(const char* name);
#endif

/**
 * Like POSIX `setenv`, but always overrides.
 *
 * We don't need the non-overriding version, and this is easier to
 * reimplement on Windows.
 */
int set_env(const char* name, const char* value);

/**
 * Like `set_env`, but using `os_string_t` to avoid coercions.
 */
int set_env_os(const os_string_t& name, const os_string_t& value);

/**
 * Clear the environment.
 */
void clear_env();

/**
 * Replace the entire environment with the given one.
 */
void replace_env(const string_map_t& new_env);

} // namespace nix
