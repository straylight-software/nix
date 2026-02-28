#include "nix/store/ssh-store.h"

#include "nix/store/local-fs-store.h"
#include "nix/store/remote-store-connection.h"
#include "nix/store/ssh.h"
#include "nix/store/store-registration.h"
#include "nix/store/worker-protocol-impl.h"
#include "nix/store/worker-protocol.h"
#include "nix/util/archive.h"
#include "nix/util/pool.h"
#include "nix/util/source-accessor.h"

namespace nix {

SSHStoreConfig::SSHStoreConfig(std::string_view scheme, std::string_view authority,
                               const Params& params)
    : store_t::config_t{params},
      remote_store::config_t{params},
      CommonSSHStoreConfig{scheme, authority, params} {}

std::string SSHStoreConfig::doc() {
  return
#include "ssh-store.md"
      ;
}

StoreReference SSHStoreConfig::getReference() const {
  return {
      .variant =
          StoreReference::Specified{
              .scheme = *uriSchemes().begin(),
              .authority = authority.to_string(),
          },
      .params = getQueryParams(),
  };
}

struct alignas(8) /* Work around ASAN failures on i686-linux. */
    ssh_store : virtual remote_store {
  using config_t = SSHStoreConfig;

  ref<const config_t> config;

  ssh_store(ref<const config_t> config)
      : store_t{*config},
        remote_store{*config},
        config{config},
        master(config->createSSHMaster(
            // Use SSH master only if using more than 1 connection.
            connections->capacity() > 1)) {
    // Eagerly start the SSH master connection before any pool connections are
    // created. This prevents deadlocks where multiple threads from the pool
    // block inside startMaster() while holding pool slots. (issue #14615)
    master.ensureMaster();
  }

  // FIXME extend daemon protocol, move implementation to RemoteStore
  std::optional<std::string> getBuildLogExact(const store_path_t& path) override {
    unsupported("getBuildLogExact");
  }

protected:
  struct Connection : remote_store::Connection {
    std::unique_ptr<SSHMaster::Connection> sshConn;

    void closeWrite() override { sshConn->in.close(); }
  };

  ref<remote_store::Connection> open_connection() override;

  std::vector<std::string> extra_remote_program_args;

  SSHMaster master;

  void setOptions(remote_store::Connection& conn) override {
    /* TODO Add a way to explicitly ask for some options to be
       forwarded. One option: A way to query the daemon for its
       settings, and then a series of params to ssh_store like
       forward-cores or forward-overridden-cores that only
       override the requested settings.
    */
  };
};

MountedSSHStoreConfig::MountedSSHStoreConfig(string_map_t params)
    : store_config_t(params),
      remote_store_config_t(params),
      CommonSSHStoreConfig(params),
      SSHStoreConfig(params),
      LocalFSStoreConfig(params) {}

MountedSSHStoreConfig::MountedSSHStoreConfig(std::string_view scheme, std::string_view host,
                                             string_map_t params)
    : store_config_t(params),
      remote_store_config_t(params),
      CommonSSHStoreConfig(scheme, host, params),
      SSHStoreConfig(scheme, host, params),
      LocalFSStoreConfig(params) {}

std::string MountedSSHStoreConfig::doc() {
  return
#include "mounted-ssh-store.md"
      ;
}

/**
 * The mounted ssh store assumes that filesystems on the remote host are
 * shared with the local host. This means that the remote nix store is
 * available locally and is therefore treated as a local filesystem
 * store.
 *
 * mounted_ssh_store_t is very similar to UDSRemoteStore --- ignoring the
 * superficial difference of SSH vs Unix domain sockets, they both are
 * accessing remote stores, and they both assume the store will be
 * mounted in the local filesystem.
 *
 * The difference lies in how they manage GC roots. See addPermRoot
 * below for details.
 */
struct mounted_ssh_store_t : virtual ssh_store, virtual local_fs_store {
  using config_t = MountedSSHStoreConfig;

  mounted_ssh_store_t(ref<const config_t> config)
      : store_t{*config}, remote_store{*config}, ssh_store{config}, local_fs_store{*config} {
    extra_remote_program_args = {
        "--process-ops",
    };
  }

  void nar_from_path(const store_path_t& path, sink_t& sink) override {
    return store_t::nar_from_path(path, sink);
  }

  ref<source_accessor_t> getFSAccessor(bool require_valid_path) override {
    return local_fs_store::getFSAccessor(require_valid_path);
  }

  std::shared_ptr<source_accessor_t> getFSAccessor(const store_path_t& path,
                                                   bool require_valid_path) override {
    return local_fs_store::getFSAccessor(path, require_valid_path);
  }

  std::optional<std::string> getBuildLogExact(const store_path_t& path) override {
    return local_fs_store::getBuildLogExact(path);
  }

  /**
   * This is the key difference from UDSRemoteStore: UDSRemote store
   * has the client create the direct root, and the remote side create
   * the indirect root.
   *
   * We could also do that, but the race conditions (will the remote
   * side see the direct root the client made?) seems bigger.
   *
   * In addition, the remote-side will have a process associated with
   * the authenticating user handling the connection (even if there
   * is a system-wide daemon or similar). This process can safely make
   * the direct and indirect roots without there being such a risk of
   * privilege escalation / symlinks in directories owned by the
   * originating requester that they cannot delete.
   */
  Path addPermRoot(const store_path_t& path, const Path& gc_root) override {
    auto conn(getConnection());
    conn->to << WorkerProto::Op::AddPermRoot;
    WorkerProto::write(*this, *conn, path);
    WorkerProto::write(*this, *conn, gc_root);
    conn.processStderr();
    return read_string(conn->from);
  }
};

ref<store_t> ssh_store::config_t::open_store() const {
  return make_ref<ssh_store>(ref{shared_from_this()});
}

ref<store_t> mounted_ssh_store_t::config_t::open_store() const {
  return make_ref<mounted_ssh_store_t>(
      ref{std::dynamic_pointer_cast<const mounted_ssh_store_t::config_t>(shared_from_this())});
}

ref<remote_store::Connection> ssh_store::open_connection() {
  auto conn = make_ref<Connection>();
  strings_t command = config->remoteProgram.get();
  command.push_back("--stdio");
  if (config->remoteStore.get() != "") {
    command.push_back("--store");
    command.push_back(config->remoteStore.get());
  }
  command.insert(command.end(), extra_remote_program_args.begin(), extra_remote_program_args.end());
  conn->sshConn = master.startCommand(std::move(command));
  conn->to = fd_sink_t(conn->sshConn->in.get());
  conn->from = fd_source_t(conn->sshConn->out.get());
  return conn;
}

static RegisterStoreImplementation<ssh_store::config_t> reg_ssh_store;
static RegisterStoreImplementation<mounted_ssh_store_t::config_t> reg_mounted_ssh_store;

} // namespace nix
