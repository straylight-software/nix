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

auto_close_fd_t create_unix_domain_socket() {
  auto_close_fd_t fd_socket = to_descriptor(socket(PF_UNIX,
                                                   SOCK_STREAM
#ifdef SOCK_CLOEXEC
                                                       | SOCK_CLOEXEC
#endif
                                                   ,
                                                   0));
  if (!fd_socket) {
    throw sys_error_t("cannot create Unix domain socket");
  }
#ifndef _WIN32
  unix::close_on_exec(fd_socket.get());
#endif
  return fd_socket;
}

auto_close_fd_t create_unix_domain_socket(const Path& path, mode_t mode) {
  auto fd_socket = nix::create_unix_domain_socket();

  bind(fd_socket.get(), path);

  if (chmod(path.c_str(), mode) == -1) {
    throw sys_error_t("changing permissions on '%1%'", path);
  }

  if (listen(to_socket(fd_socket.get()), 100) == -1) {
    throw sys_error_t("cannot listen on socket '%1%'", path);
  }

  return fd_socket;
}

static void bind_connect_proc_helper(std::string_view operation_name, auto&& operation, socket_t fd,
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
    throw Error("cannot %s to socket at '%s': path is too long", operation_name, path);
#else
    pipe_t pipe;
    pipe.create();
    process_handle_t pid = start_process([&] {
      try {
        pipe.read_side.close();
        Path dir = dir_of(path);
        if (chdir(dir.c_str()) == -1) {
          throw sys_error_t("chdir to '%s' failed", dir);
        }
        std::string base(base_name_of(path));
        if (base.size() + 1 >= sizeof(addr.sun_path)) {
          throw Error("socket path '%s' is too long", base);
        }
        memcpy(addr.sun_path, base.c_str(), base.size() + 1);
        if (operation(fd, psaddr, sizeof(addr)) == -1) {
          throw sys_error_t("cannot %s to socket at '%s'", operation_name, path);
        }
        write_full(pipe.write_side.get(), "0\n");
      } catch (sys_error_t& e) {
        write_full(pipe.write_side.get(), fmt("%d\n", e.err_no()));
      } catch (...) {
        write_full(pipe.write_side.get(), "-1\n");
      }
    });
    pipe.write_side.close();
    auto err_no = string2_int<int>(chomp(drain_fd(pipe.read_side.get())));
    if (!err_no || *err_no == -1) {
      throw Error("cannot %s to socket at '%s'", operation_name, path);
    } else if (*err_no > 0) {
      errno = *err_no;
      throw sys_error_t("cannot %s to socket at '%s'", operation_name, path);
    }
#endif
  } else {
    memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (operation(fd, psaddr, sizeof(addr)) == -1) {
      throw sys_error_t("cannot %s to socket at '%s'", operation_name, path);
    }
  }
}

void bind(socket_t fd, const std::string& path) {
  unlink(path.c_str());

  bind_connect_proc_helper("bind", ::bind, fd, path);
}

void connect(socket_t fd, const std::filesystem::path& path) {
  bind_connect_proc_helper("connect", ::connect, fd, path.string());
}

auto_close_fd_t connect(const std::filesystem::path& path) {
  auto fd = create_unix_domain_socket();
  nix::connect(to_socket(fd.get()), path);
  return fd;
}

} // namespace nix
