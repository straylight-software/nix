#pragma once
///@file

#include <filesystem>

#include <unistd.h>

#include "nix/util/file-descriptor.h"
#include "nix/util/socket.h"
#include "nix/util/types.h"

namespace nix {

/**
 * Create a Unix domain socket.
 */
AutoCloseFD createUnixDomainSocket();

/**
 * Create a Unix domain socket in listen mode.
 */
AutoCloseFD createUnixDomainSocket(const Path& path, mode_t mode);

/**
 * Bind a Unix domain socket to a path.
 */
void bind(Socket fd, const std::string& path);

/**
 * Connect to a Unix domain socket.
 */
void connect(Socket fd, const std::filesystem::path& path);

/**
 * Connect to a Unix domain socket.
 */
AutoCloseFD connect(const std::filesystem::path& path);

} // namespace nix
