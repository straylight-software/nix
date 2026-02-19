#pragma once
///@file

#include "nix/util/file-descriptor.h"

#ifdef _WIN32
#  include <winsock2.h>
#endif

namespace nix {

/**
 * Often we want to use `descriptor_t`, but Windows makes a slightly
 * stronger file descriptor vs socket distinction, at least at the level
 * of C types.
 */
using socket_t =
#ifdef _WIN32
    SOCKET
#else
    int
#endif
    ;

#ifdef _WIN32
/**
 * Windows gives this a different name
 */
#  define SHUT_WR SD_SEND
#  define SHUT_RDWR SD_BOTH
#endif

/**
 * Convert a `descriptor_t` to a `socket_t`
 *
 * This is a no-op except on Windows.
 */
static inline socket_t to_socket(descriptor_t fd) {
#ifdef _WIN32
  return reinterpret_cast<socket_t>(fd);
#else
  return fd;
#endif
}

/**
 * Convert a `socket_t` to a `descriptor_t`
 *
 * This is a no-op except on Windows.
 */
static inline descriptor_t from_socket(socket_t fd) {
#ifdef _WIN32
  return reinterpret_cast<descriptor_t>(fd);
#else
  return fd;
#endif
}

} // namespace nix
