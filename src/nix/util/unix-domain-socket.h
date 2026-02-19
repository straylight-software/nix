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
auto_close_fd_t createUnixDomainSocket();

/**
 * Create a Unix domain socket in listen mode.
 */
auto_close_fd_t createUnixDomainSocket(const Path& path, mode_t mode);

/**
 * Bind a Unix domain socket to a path.
 */
void bind(socket_t fd, const std::string& path);

/**
 * Connect to a Unix domain socket.
 */
void connect(socket_t fd, const std::filesystem::path& path);

/**
 * Connect to a Unix domain socket.
 */
auto_close_fd_t connect(const std::filesystem::path& path);

} // namespace nix
