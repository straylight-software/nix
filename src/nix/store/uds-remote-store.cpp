#include "nix/store/uds-remote-store.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "nix/store/globals.h"
#include "nix/store/store-registration.h"
#include "nix/store/worker-protocol.h"
#include "nix/util/unix-domain-socket.h"

#ifdef _WIN32
#  include <afunix.h>
#  include <winsock2.h>
#else
#  include <sys/socket.h>
#  include <sys/un.h>
#endif

namespace nix {

UDSRemoteStoreConfig::UDSRemoteStoreConfig(std::string_view scheme, std::string_view authority,
                                           const StoreReference::Params& params)
    : store_t::config_t{params},
      local_fs_store::config_t{params},
      remote_store::config_t{params},
      path{authority.empty() ? settings.nixDaemonSocketFile : authority} {
  if (uriSchemes().count(scheme) == 0) {
    throw UsageError("Scheme must be 'unix'");
  }
}

std::string UDSRemoteStoreConfig::doc() {
  return
#include "uds-remote-store.md"
      ;
}

// A bit gross that we now pass empty string but this is knowing that
// empty string will later default to the same nixDaemonSocketFile. Why
// don't we just wire it all through? I believe there are cases where it
// will live reload so we want to continue to account for that.
UDSRemoteStoreConfig::UDSRemoteStoreConfig(const Params& params)
    : UDSRemoteStoreConfig(*uriSchemes().begin(), "", params) {}

UDSRemoteStore::UDSRemoteStore(ref<const config_t> config)
    : store_t{*config}, local_fs_store{*config}, remote_store{*config}, config{config} {}

StoreReference UDSRemoteStoreConfig::getReference() const {
  /* We specifically return "daemon" here instead of "unix://" or "unix://${path}"
   * to be more compatible with older versions of nix. Some tooling out there
   * tries hard to parse store references and it might not be able to handle "unix://". */
  if (path == settings.nixDaemonSocketFile) {
    return {
        .variant = StoreReference::Daemon{},
        .params = getQueryParams(),
    };
  }
  return {
      .variant =
          StoreReference::Specified{
              .scheme = *uriSchemes().begin(),
              .authority = path,
          },
      .params = getQueryParams(),
  };
}

void UDSRemoteStore::Connection::closeWrite() {
  shutdown(to_socket(fd.get()), SHUT_WR);
}

ref<remote_store::Connection> UDSRemoteStore::open_connection() {
  auto conn = make_ref<Connection>();

  /* Connect to a daemon that does the privileged work for us. */
  conn->fd = nix::connect(config->path);

  conn->from.set_fd(conn->fd.get());
  conn->to.set_fd(conn->fd.get());

  conn->start_time = std::chrono::steady_clock::now();

  return conn;
}

void UDSRemoteStore::addIndirectRoot(const Path& path) {
  auto conn(getConnection());
  conn->to << WorkerProto::Op::AddIndirectRoot << path;
  conn.processStderr();
  read_int(conn->from);
}

ref<store_t> UDSRemoteStore::config_t::open_store() const {
  return make_ref<UDSRemoteStore>(ref{shared_from_this()});
}

static RegisterStoreImplementation<UDSRemoteStore::config_t> reg_uds_remote_store;

} // namespace nix
