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

void write_line(descriptor_t fd, std::string s) {
  s += '\n';
  write_full(fd, s);
}

std::string drain_fd(descriptor_t fd, bool block, const size_t reserve_size) {
  // the parser needs two extra bytes to append terminating characters, other users will
  // not care very much about the extra memory.
  string_sink_t sink(reserve_size + 2);
#ifdef _WIN32
  // non-blocking is not supported this way on Windows
  assert(block);
  drain_fd(fd, sink);
#else
  drain_fd(fd, sink, block);
#endif
  return std::move(sink.str());
}

//////////////////////////////////////////////////////////////////////

auto_close_fd_t::auto_close_fd_t() : fd_{INVALID_DESCRIPTOR} {}

auto_close_fd_t::auto_close_fd_t(descriptor_t fd) : fd_{fd} {}

// NOTE: This can be noexcept since we are just copying a value and resetting
// the file descriptor in the rhs.
auto_close_fd_t::auto_close_fd_t(auto_close_fd_t&& that) noexcept : fd_{that.fd_} {
  that.fd_ = INVALID_DESCRIPTOR;
}

auto auto_close_fd_t::operator=(auto_close_fd_t&& that) noexcept -> auto_close_fd_t& {
  close();
  fd_ = that.fd_;
  that.fd_ = INVALID_DESCRIPTOR;
  return *this;
}

auto_close_fd_t::~auto_close_fd_t() {
  try {
    close();
  } catch (...) {
    ignore_exception_in_destructor();
  }
}

auto auto_close_fd_t::get() const -> descriptor_t {
  return fd_;
}

void auto_close_fd_t::close() {
  if (fd_ != INVALID_DESCRIPTOR) {
    if (
#ifdef _WIN32
        ::CloseHandle(fd_)
#else
        ::close(fd_)
#endif
        == -1) {
      /* This should never happen. */
      throw native_sys_error_t("closing file descriptor %1%", fd_);
    }
    fd_ = INVALID_DESCRIPTOR;
  }
}

void auto_close_fd_t::fsync() const {
  if (fd_ != INVALID_DESCRIPTOR) {
    int result;
    result =
#ifdef _WIN32
        ::FlushFileBuffers(fd_)
#elif defined(__APPLE__)
        ::fcntl(fd_, F_FULLFSYNC)
#else
        ::fsync(fd_)
#endif
        ;
    if (result == -1) {
      throw native_sys_error_t("fsync file descriptor %1%", fd_);
    }
  }
}

void auto_close_fd_t::start_fsync() const {
#ifdef __linux__
  if (fd_ != -1) {
    /* Ignore failure, since fsync must be run later anyway. This is just a performance
     * optimization. */
    ::sync_file_range(fd_, 0, 0, SYNC_FILE_RANGE_WRITE);
  }
#endif
}

auto_close_fd_t::operator bool() const {
  return fd_ != INVALID_DESCRIPTOR;
}

auto auto_close_fd_t::release() -> descriptor_t {
  descriptor_t old_fd = fd_;
  fd_ = INVALID_DESCRIPTOR;
  return old_fd;
}

//////////////////////////////////////////////////////////////////////

void pipe_t::close() {
  read_side.close();
  write_side.close();
}

} // namespace nix
