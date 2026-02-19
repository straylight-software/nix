#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "nix/util/canon-path.h"
#include "nix/util/file-system.h"
#include "nix/util/finally.h"
#include "nix/util/serialise.h"
#include "nix/util/signals.h"

#if defined(__linux__) && defined(__NR_openat2)
#  define HAVE_OPENAT2 1
#  include <linux/openat2.h>
#  include <sys/syscall.h>
#else
#  define HAVE_OPENAT2 0
#endif

#include "nix/util/util-config-private.h"
#include "nix/util/util-unix-config-private.h"

namespace nix {

namespace {

// This function is needed to handle non-blocking reads/writes. This is needed in the buildhook,
// because somehow the json logger file descriptor ends up being non-blocking and breaks
// remote-building.
// TODO: get rid of buildhook and remove this function again
// (https://github.com/NixOS/nix/issues/12688)
void poll_fd(int fd, int events) {
  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = events;
  int ret = poll(&pfd, 1, -1);
  if (ret == -1) {
    throw sys_error_t("poll on file descriptor failed");
  }
}
} // namespace

std::string read_file(int fd) {
  struct stat st;
  if (fstat(fd, &st) == -1)
    throw sys_error_t("statting file");

  return drain_fd(fd, true, st.st_size);
}

void read_full(int fd, char* buf, size_t count) {
  while (count) {
    check_interrupt();
    ssize_t res = read(fd, buf, count);
    if (res == -1) {
      switch (errno) {
        case EINTR:
          continue;
        case EAGAIN:
          poll_fd(fd, POLLIN);
          continue;
      }
      throw sys_error_t("reading from file");
    }
    if (res == 0)
      throw EndOfFile("unexpected end-of-file");
    count -= res;
    buf += res;
  }
}

void write_full(int fd, std::string_view s, bool allow_interrupts) {
  while (!s.empty()) {
    if (allow_interrupts)
      check_interrupt();
    ssize_t res = write(fd, s.data(), s.size());
    if (res == -1) {
      switch (errno) {
        case EINTR:
          continue;
        case EAGAIN:
          poll_fd(fd, POLLOUT);
          continue;
      }
      throw sys_error_t("writing to file");
    }
    if (res > 0)
      s.remove_prefix(res);
  }
}

std::string read_line(int fd, bool eof_ok) {
  std::string s;
  while (1) {
    check_interrupt();
    char ch;
    // FIXME: inefficient
    ssize_t rd = read(fd, &ch, 1);
    if (rd == -1) {
      switch (errno) {
        case EINTR:
          continue;
        case EAGAIN: {
          poll_fd(fd, POLLIN);
          continue;
        }
        default:
          throw sys_error_t("reading a line");
      }
    } else if (rd == 0) {
      if (eof_ok)
        return s;
      else
        throw EndOfFile("unexpected EOF reading a line");
    } else {
      if (ch == '\n')
        return s;
      s += ch;
    }
  }
}

void drain_fd(int fd, Sink& sink, bool block) {
  // silence GCC maybe-uninitialized warning in finally
  int saved = 0;

  if (!block) {
    saved = fcntl(fd, F_GETFL);
    if (fcntl(fd, F_SETFL, saved | O_NONBLOCK) == -1)
      throw sys_error_t("making file descriptor non-blocking");
  }

  finally_t finally([&]() {
    if (!block) {
      if (fcntl(fd, F_SETFL, saved) == -1)
        throw sys_error_t("making file descriptor blocking");
    }
  });

  std::vector<unsigned char> buf(64 * 1024);
  while (1) {
    check_interrupt();
    ssize_t rd = read(fd, buf.data(), buf.size());
    if (rd == -1) {
      if (!block && (errno == EAGAIN || errno == EWOULDBLOCK))
        break;
      if (errno != EINTR)
        throw sys_error_t("reading from file");
    } else if (rd == 0)
      break;
    else
      sink({reinterpret_cast<char*>(buf.data()), (size_t)rd});
  }
}

//////////////////////////////////////////////////////////////////////

void pipe_t::create() {
  int fds[2];
#if HAVE_PIPE2
  if (pipe2(fds, O_CLOEXEC) != 0)
    throw sys_error_t("creating pipe");
#else
  if (pipe(fds) != 0)
    throw sys_error_t("creating pipe");
  unix::close_on_exec(fds[0]);
  unix::close_on_exec(fds[1]);
#endif
  read_side = fds[0];
  write_side = fds[1];
}

//////////////////////////////////////////////////////////////////////

#if defined(__linux__) || defined(__FreeBSD__)
static int unix_close_range(unsigned int first, unsigned int last, int flags) {
#  if !HAVE_CLOSE_RANGE
  return syscall(SYS_close_range, first, last, (unsigned int)flags);
#  else
  return close_range(first, last, flags);
#  endif
}
#endif

void unix::close_extra_f_ds() {
  constexpr int MAX_KEPT_FD = 2;
  static_assert(std::max({STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) == MAX_KEPT_FD);

#if defined(__linux__) || defined(__FreeBSD__)
  // first try to close_range everything we don't care about. if this
  // returns an error with these parameters we're running on a kernel
  // that does not implement close_range (i.e. pre 5.9) and fall back
  // to the old method. we should remove that though, in some future.
  if (unix_close_range(MAX_KEPT_FD + 1, ~0U, 0) == 0) {
    return;
  }
#endif

#ifdef __linux__
  try {
    for (auto& s : directory_iterator_t{"/proc/self/fd"}) {
      check_interrupt();
      auto fd = std::stoi(s.path().filename());
      if (fd > MAX_KEPT_FD) {
        debug("closing leaked FD %d", fd);
        close(fd);
      }
    }
    return;
  } catch (sys_error_t&) {
  }
#endif

  int max_fd = 0;
#if HAVE_SYSCONF
  max_fd = sysconf(_SC_OPEN_MAX);
#endif
  for (int fd = MAX_KEPT_FD + 1; fd < max_fd; ++fd)
    close(fd); /* ignore result */
}

void unix::close_on_exec(int fd) {
  int prev;
  if ((prev = fcntl(fd, F_GETFD, 0)) == -1 || fcntl(fd, F_SETFD, prev | FD_CLOEXEC) == -1)
    throw sys_error_t("setting close-on-exec flag");
}

#ifdef __linux__

namespace linux {

std::optional<descriptor_t> openat2(descriptor_t dir_fd, const char* path, uint64_t flags, uint64_t mode,
                                  uint64_t resolve) {
#  if HAVE_OPENAT2
  /* cache_t the result of whether openat2 is not supported. */
  static std::atomic_flag unsupported{};

  if (!unsupported.test()) {
    /* No glibc wrapper yet, but there's a patch:
     * https://patchwork.sourceware.org/project/glibc/patch/20251029200519.3203914-1-adhemerval.zanella@linaro.org/
     */
    auto how = ::open_how{.flags = flags, .mode = mode, .resolve = resolve};
    auto res = ::syscall(__NR_openat2, dir_fd, path, &how, sizeof(how));
    /* cache_t that the syscall is not supported. */
    if (res < 0 && errno == ENOSYS) {
      unsupported.test_and_set();
      return std::nullopt;
    }

    return res;
  }
#  endif
  return std::nullopt;
}

} // namespace linux

#endif

static descriptor_t open_file_ensure_beneath_no_symlinks_iterative(descriptor_t dir_fd, const canon_path_t& path,
                                                           int flags, mode_t mode) {
  auto_close_fd_t parent_fd;
  auto nr_components = std::ranges::distance(path);
  assert(nr_components >= 1);
  auto components = std::views::take(path, nr_components - 1); /* Everything but last component */
  auto get_parent_fd = [&]() { return parent_fd ? parent_fd.get() : dir_fd; };

  /* This rather convoluted loop is necessary to avoid TOCTOU when validating that
     no inner path component is a symlink. */
  for (auto it = components.begin(); it != components.end(); ++it) {
    auto component = std::string(*it); /* Copy into a string to make NUL terminated. */
    assert(component != ".." &&
           !component.starts_with('/')); /* In case invariant is broken somehow.. */

    auto_close_fd_t parent_fd2 =
        ::openat(get_parent_fd(), /* First iteration uses dir_fd. */
                 component.c_str(),
                 O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC
#ifdef __linux__
                     | O_PATH /* Linux-specific optimization. Files are open only for path
                                 resolution purposes. */
#endif
#ifdef __FreeBSD__
                     | O_RESOLVE_BENEATH /* Further guard against any possible SNAFUs. */
#endif
        );

    if (!parent_fd2) {
      /* Construct the canon_path_t for error message. */
      auto path2 =
          std::ranges::fold_left(components.begin(), ++it, canon_path_t::root, [](auto lhs, auto rhs) {
            lhs.push(rhs);
            return lhs;
          });

      if (errno == ENOTDIR) /* Path component might be a symlink. */ {
        struct ::stat st;
        if (::fstatat(get_parent_fd(), component.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISLNK(st.st_mode))
          throw unix::symlink_not_allowed_t(path2);
        errno = ENOTDIR; /* Restore the errno. */
      } else if (errno == ELOOP) {
        throw unix::symlink_not_allowed_t(path2);
      }

      return INVALID_DESCRIPTOR;
    }

    parent_fd = std::move(parent_fd2);
  }

  auto res = ::openat(get_parent_fd(), std::string(path.base_name().value()).c_str(),
                      flags | O_NOFOLLOW, mode);
  if (res < 0 && errno == ELOOP)
    throw unix::symlink_not_allowed_t(path);
  return res;
}

descriptor_t unix::open_file_ensure_beneath_no_symlinks(descriptor_t dir_fd, const canon_path_t& path, int flags,
                                                 mode_t mode) {
  assert(!path.rel().starts_with('/')); /* Just in case the invariant is somehow broken. */
  assert(!path.is_root());
#ifdef __linux__
  auto maybe_fd = linux::openat2(dir_fd, path.rel_c_str(), flags, static_cast<uint64_t>(mode),
                                RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS);
  if (maybe_fd) {
    if (*maybe_fd < 0 && errno == ELOOP)
      throw unix::symlink_not_allowed_t(path);
    return *maybe_fd;
  }
#endif
  return open_file_ensure_beneath_no_symlinks_iterative(dir_fd, path, flags, mode);
}

} // namespace nix
