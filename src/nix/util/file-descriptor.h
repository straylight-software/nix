#pragma once
///@file

#include "nix/util/canon-path.h"
#include "nix/util/error.h"
#include "nix/util/types.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace nix {

struct Sink;
struct Source;

/**
 * Operating System capability
 */
using descriptor_t =
#ifdef _WIN32
    HANDLE
#else
    int
#endif
    ;

const descriptor_t INVALID_DESCRIPTOR =
#ifdef _WIN32
    INVALID_HANDLE_VALUE
#else
    -1
#endif
    ;

/**
 * Convert a native `descriptor_t` to a POSIX file descriptor
 *
 * This is a no-op except on Windows.
 */
static inline descriptor_t toDescriptor(int fd) {
#ifdef _WIN32
  return reinterpret_cast<HANDLE>(_get_osfhandle(fd));
#else
  return fd;
#endif
}

/**
 * Convert a POSIX file descriptor to a native `descriptor_t` in read-only
 * mode.
 *
 * This is a no-op except on Windows.
 */
static inline int fromDescriptorReadOnly(descriptor_t fd) {
#ifdef _WIN32
  return _open_osfhandle(reinterpret_cast<intptr_t>(fd), _O_RDONLY);
#else
  return fd;
#endif
}

/**
 * Read the contents of a resource into a string.
 */
std::string readFile(descriptor_t fd);

/**
 * Wrappers around read()/write() that read/write exactly the
 * requested number of bytes.
 */
void readFull(descriptor_t fd, char* buf, size_t count);

void writeFull(descriptor_t fd, std::string_view s, bool allowInterrupts = true);

/**
 * Read a line from a file descriptor.
 *
 * @param fd The file descriptor to read from
 * @param eofOk If true, return an unterminated line if EOF is reached. (e.g. the empty string)
 *
 * @return A line of text ending in `\n`, or a string without `\n` if `eofOk` is true and EOF is
 * reached.
 */
std::string readLine(descriptor_t fd, bool eofOk = false);

/**
 * Write a line to a file descriptor.
 */
void writeLine(descriptor_t fd, std::string s);

/**
 * Read a file descriptor until EOF occurs.
 */
std::string drainFD(descriptor_t fd, bool block = true, const size_t reserveSize = 0);

/**
 * The Windows version is always blocking.
 */
void drainFD(descriptor_t fd, Sink& sink
#ifndef _WIN32
             ,
             bool block = true
#endif
);

/**
 * Get [Standard Input](https://en.wikipedia.org/wiki/Standard_streams#Standard_input_(stdin))
 */
[[gnu::always_inline]]
inline descriptor_t getStandardInput() {
#ifndef _WIN32
  return STDIN_FILENO;
#else
  return GetStdHandle(STD_INPUT_HANDLE);
#endif
}

/**
 * Get [Standard Output](https://en.wikipedia.org/wiki/Standard_streams#Standard_output_(stdout))
 */
[[gnu::always_inline]]
inline descriptor_t getStandardOutput() {
#ifndef _WIN32
  return STDOUT_FILENO;
#else
  return GetStdHandle(STD_OUTPUT_HANDLE);
#endif
}

/**
 * Get [Standard Error](https://en.wikipedia.org/wiki/Standard_streams#Standard_error_(stderr))
 */
[[gnu::always_inline]]
inline descriptor_t getStandardError() {
#ifndef _WIN32
  return STDERR_FILENO;
#else
  return GetStdHandle(STD_ERROR_HANDLE);
#endif
}

/**
 * Automatic cleanup of resources.
 */
class auto_close_fd_t {
  descriptor_t fd;

public:
  auto_close_fd_t();
  auto_close_fd_t(descriptor_t fd);
  auto_close_fd_t(const auto_close_fd_t& fd) = delete;
  auto_close_fd_t(auto_close_fd_t&& fd) noexcept;
  ~auto_close_fd_t();
  auto_close_fd_t& operator=(const auto_close_fd_t& fd) = delete;
  auto_close_fd_t& operator=(auto_close_fd_t&& fd);
  descriptor_t get() const;
  explicit operator bool() const;
  descriptor_t release();
  void close();

  /**
   * Perform a blocking fsync operation.
   */
  void fsync() const;

  /**
   * Asynchronously flush to disk without blocking, if available on
   * the platform. This is just a performance optimization, and
   * fsync must be run later even if this is called.
   */
  void startFsync() const;
};

class pipe_t {
public:
  auto_close_fd_t readSide, writeSide;
  void create();
  void close();
};

#ifndef _WIN32 // Not needed on Windows, where we don't fork
namespace unix {

/**
 * Close all file descriptors except stdio fds (ie 0, 1, 2).
 * Good practice in child processes.
 */
void closeExtraFDs();

/**
 * Set the close-on-exec flag for the given file descriptor.
 */
void closeOnExec(descriptor_t fd);

} // namespace unix
#endif

#ifdef __linux__
namespace linux {

/**
 * Wrapper around Linux's openat2 syscall introduced in Linux 5.6.
 *
 * @see https://man7.org/linux/man-pages/man2/openat2.2.html
 * @see https://man7.org/linux/man-pages/man2/open_how.2type.html
v*
 * @param flags O_* flags
 * @param mode Mode for O_{CREAT,TMPFILE}
 * @param resolve RESOLVE_* flags
 *
 * @return nullopt if openat2 is not supported by the kernel.
 */
std::optional<descriptor_t> openat2(descriptor_t dirFd, const char* path, uint64_t flags, uint64_t mode,
                                  uint64_t resolve);

} // namespace linux
#endif

#if defined(_WIN32) && _WIN32_WINNT >= 0x0600
namespace windows {

Path handleToPath(descriptor_t handle);
std::wstring handleToFileName(descriptor_t handle);

} // namespace windows
#endif

#ifndef _WIN32
namespace unix {

struct symlink_not_allowed_t : public Error {
  canon_path_t path;

  symlink_not_allowed_t(canon_path_t path)
      /* Can't provide better error message, since the parent directory is only known to the caller.
       */
      : Error("relative path '%s' points to a symlink, which is not allowed", path.rel()),
        path(std::move(path)) {}
};

/**
 * Safe(r) function to open \param path file relative to \param dirFd, while
 * disallowing escaping from a directory and resolving any symlinks in the
 * process.
 *
 * @note When not on Linux or when openat2 is not available this is implemented
 * via openat single path component traversal. Uses RESOLVE_BENEATH with openat2
 * or O_RESOLVE_BENEATH.
 *
 * @note Since this is Unix-only path is specified as canon_path_t, which models
 * Unix-style paths and ensures that there are no .. or . components.
 *
 * @param flags O_* flags
 * @param mode Mode for O_{CREAT,TMPFILE}
 *
 * @pre path.isRoot() is false
 *
 * @throws symlink_not_allowed_t if any path components
 */
descriptor_t openFileEnsureBeneathNoSymlinks(descriptor_t dirFd, const canon_path_t& path, int flags,
                                           mode_t mode = 0);

} // namespace unix
#endif

MakeError(EndOfFile, Error);

} // namespace nix
