#include <fcntl.h>
#include <unistd.h>

#include "nix/util/serialise.h"
#include "nix/util/util.h"
#ifdef _WIN32
#  include <fileapi.h>
#  include <winnt.h>

#  include "nix/util/windows-error.h"
#endif

namespace nix {

void writeLine(descriptor_t fd, std::string s) {
  s += '\n';
  writeFull(fd, s);
}

std::string drainFD(descriptor_t fd, bool block, const size_t reserveSize) {
  // the parser needs two extra bytes to append terminating characters, other users will
  // not care very much about the extra memory.
  string_sink_t sink(reserveSize + 2);
#ifdef _WIN32
  // non-blocking is not supported this way on Windows
  assert(block);
  drainFD(fd, sink);
#else
  drainFD(fd, sink, block);
#endif
  return std::move(sink.s);
}

//////////////////////////////////////////////////////////////////////

auto_close_fd_t::auto_close_fd_t() : fd{INVALID_DESCRIPTOR} {}

auto_close_fd_t::auto_close_fd_t(descriptor_t fd) : fd{fd} {}

// NOTE: This can be noexcept since we are just copying a value and resetting
// the file descriptor in the rhs.
auto_close_fd_t::auto_close_fd_t(auto_close_fd_t&& that) noexcept : fd{that.fd} {
  that.fd = INVALID_DESCRIPTOR;
}

auto_close_fd_t& auto_close_fd_t::operator=(auto_close_fd_t&& that) {
  close();
  fd = that.fd;
  that.fd = INVALID_DESCRIPTOR;
  return *this;
}

auto_close_fd_t::~auto_close_fd_t() {
  try {
    close();
  } catch (...) {
    ignoreExceptionInDestructor();
  }
}

descriptor_t auto_close_fd_t::get() const {
  return fd;
}

void auto_close_fd_t::close() {
  if (fd != INVALID_DESCRIPTOR) {
    if (
#ifdef _WIN32
        ::CloseHandle(fd)
#else
        ::close(fd)
#endif
        == -1)
      /* This should never happen. */
      throw native_sys_error_t("closing file descriptor %1%", fd);
    fd = INVALID_DESCRIPTOR;
  }
}

void auto_close_fd_t::fsync() const {
  if (fd != INVALID_DESCRIPTOR) {
    int result;
    result =
#ifdef _WIN32
        ::FlushFileBuffers(fd)
#elif defined(__APPLE__)
        ::fcntl(fd, F_FULLFSYNC)
#else
        ::fsync(fd)
#endif
        ;
    if (result == -1)
      throw native_sys_error_t("fsync file descriptor %1%", fd);
  }
}

void auto_close_fd_t::startFsync() const {
#ifdef __linux__
  if (fd != -1) {
    /* Ignore failure, since fsync must be run later anyway. This is just a performance
     * optimization. */
    ::sync_file_range(fd, 0, 0, SYNC_FILE_RANGE_WRITE);
  }
#endif
}

auto_close_fd_t::operator bool() const {
  return fd != INVALID_DESCRIPTOR;
}

descriptor_t auto_close_fd_t::release() {
  descriptor_t oldFD = fd;
  fd = INVALID_DESCRIPTOR;
  return oldFD;
}

//////////////////////////////////////////////////////////////////////

void pipe_t::close() {
  readSide.close();
  writeSide.close();
}

} // namespace nix
