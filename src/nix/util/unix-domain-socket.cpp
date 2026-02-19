#include "nix/util/unix-domain-socket.h"

#include "nix/util/file-system.h"
#include "nix/util/util.h"

#ifdef _WIN32
#  include <afunix.h>
#  include <winsock2.h>
#else
#  include <sys/socket.h>
#  include <sys/un.h>

#  include "nix/util/processes.h"
#endif
#include <unistd.h>

namespace nix {

auto_close_fd_t createUnixDomainSocket() {
  auto_close_fd_t fdSocket = toDescriptor(socket(PF_UNIX,
                                             SOCK_STREAM
#ifdef SOCK_CLOEXEC
                                                 | SOCK_CLOEXEC
#endif
                                             ,
                                             0));
  if (!fdSocket)
    throw sys_error_t("cannot create Unix domain socket");
#ifndef _WIN32
  unix::closeOnExec(fdSocket.get());
#endif
  return fdSocket;
}

auto_close_fd_t createUnixDomainSocket(const Path& path, mode_t mode) {
  auto fdSocket = nix::createUnixDomainSocket();

  bind(fdSocket.get(), path);

  if (chmod(path.c_str(), mode) == -1)
    throw sys_error_t("changing permissions on '%1%'", path);

  if (listen(toSocket(fdSocket.get()), 100) == -1)
    throw sys_error_t("cannot listen on socket '%1%'", path);

  return fdSocket;
}

static void bindConnectProcHelper(std::string_view operationName, auto&& operation, socket_t fd,
                                  const std::string& path) {
  struct sockaddr_un addr;
  addr.sun_family = AF_UNIX;

  // Casting between types like these legacy C library interfaces
  // require is forbidden in C++. To maintain backwards
  // compatibility, the implementation of the bind/connect functions
  // contains some hints to the compiler that allow for this
  // special case.
  auto* psaddr = reinterpret_cast<struct sockaddr*>(&addr);

  if (path.size() + 1 >= sizeof(addr.sun_path)) {
#ifdef _WIN32
    throw Error("cannot %s to socket at '%s': path is too long", operationName, path);
#else
    pipe_t pipe;
    pipe.create();
    Pid pid = startProcess([&] {
      try {
        pipe.readSide.close();
        Path dir = dirOf(path);
        if (chdir(dir.c_str()) == -1)
          throw sys_error_t("chdir to '%s' failed", dir);
        std::string base(baseNameOf(path));
        if (base.size() + 1 >= sizeof(addr.sun_path))
          throw Error("socket path '%s' is too long", base);
        memcpy(addr.sun_path, base.c_str(), base.size() + 1);
        if (operation(fd, psaddr, sizeof(addr)) == -1)
          throw sys_error_t("cannot %s to socket at '%s'", operationName, path);
        writeFull(pipe.writeSide.get(), "0\n");
      } catch (sys_error_t& e) {
        writeFull(pipe.writeSide.get(), fmt("%d\n", e.errNo));
      } catch (...) {
        writeFull(pipe.writeSide.get(), "-1\n");
      }
    });
    pipe.writeSide.close();
    auto errNo = string2Int<int>(chomp(drainFD(pipe.readSide.get())));
    if (!errNo || *errNo == -1)
      throw Error("cannot %s to socket at '%s'", operationName, path);
    else if (*errNo > 0) {
      errno = *errNo;
      throw sys_error_t("cannot %s to socket at '%s'", operationName, path);
    }
#endif
  } else {
    memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (operation(fd, psaddr, sizeof(addr)) == -1)
      throw sys_error_t("cannot %s to socket at '%s'", operationName, path);
  }
}

void bind(socket_t fd, const std::string& path) {
  unlink(path.c_str());

  bindConnectProcHelper("bind", ::bind, fd, path);
}

void connect(socket_t fd, const std::filesystem::path& path) {
  bindConnectProcHelper("connect", ::connect, fd, path.string());
}

auto_close_fd_t connect(const std::filesystem::path& path) {
  auto fd = createUnixDomainSocket();
  nix::connect(toSocket(fd.get()), path);
  return fd;
}

} // namespace nix
