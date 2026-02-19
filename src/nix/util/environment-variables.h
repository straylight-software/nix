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

static constexpr auto environmentVariablesCategory = "Options that change environment variables";

/**
 * @return an environment variable.
 */
std::optional<std::string> getEnv(const std::string& key);

/**
 * Like `getEnv`, but using `os_string_t` to avoid coercions.
 */
std::optional<os_string_t> getEnvOs(const os_string_t& key);

/**
 * @return a non empty environment variable. Returns nullopt if the env
 * variable is set to ""
 */
std::optional<std::string> getEnvNonEmpty(const std::string& key);

/**
 * Get the entire environment.
 */
string_map_t getEnv();

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
int setEnv(const char* name, const char* value);

/**
 * Like `setEnv`, but using `os_string_t` to avoid coercions.
 */
int setEnvOs(const os_string_t& name, const os_string_t& value);

/**
 * Clear the environment.
 */
void clearEnv();

/**
 * Replace the entire environment with the given one.
 */
void replaceEnv(const string_map_t& newEnv);

} // namespace nix
