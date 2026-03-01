#pragma once
///@file

#include "nix/util/file-system.h"
#include "nix/util/processes.h"
#include "nix/util/ref.h"
#include "nix/util/sync.h"
#include "nix/util/url.h"

namespace nix {

strings_t get_nix_ssh_opts();

/**
 * Get an environment map with SSH_AUTH_SOCK set if possible.
 * This handles the common case of `sudo nix ...` where the environment
 * is sanitized but we want to use the user's SSH agent.
 *
 * @return An environment map with SSH_AUTH_SOCK set if found, or empty if
 *         SSH_AUTH_SOCK is already set or no agent socket was found.
 */
std::optional<string_map_t> get_ssh_agent_env();

class SSHMaster {
private:
  parsed_url_t::authority_t authority;
  std::string hostname_and_user;
  bool fakeSSH;
  const std::string keyFile;
  /**
   * raw_t bytes, not base64 encoding.
   */
  const std::string ssh_public_host_key;
  const bool useMaster;
  const bool compress;
  const descriptor_t logFD;

  const ref<const auto_delete_t> tmp_dir;

  struct State {
#ifndef _WIN32 // TODO re-enable on Windows, once we can start processes.
    process_handle_t sshMaster;
#endif
    Path socket_path;
    bool starting = false; // True while SSH master is being started
  };

  sync_t<State> state_;
  std::condition_variable state_cv_;

  void addCommonSSHOpts(strings_t& args);
  bool isMasterRunning();

#ifndef _WIN32 // TODO re-enable on Windows, once we can start processes.
  Path startMaster();
#endif

public:
  SSHMaster(const parsed_url_t::authority_t& authority, std::string_view keyFile,
            std::string_view ssh_public_host_key, bool useMaster, bool compress,
            descriptor_t logFD = INVALID_DESCRIPTOR);

  struct Connection {
#ifndef _WIN32 // TODO re-enable on Windows, once we can start processes.
    process_handle_t sshPid;
#endif
    auto_close_fd_t out, in;

    /**
     * Try to set the buffer size in both directions to the
     * designated amount, if possible. If not possible, does
     * nothing.
     *
     * Current implementation is to use `fcntl` with `F_SETPIPE_SZ`,
     * which is Linux-only. For this implementation, `size` must
     * convertible to an `int`. In other words, it must be within
     * `[0, INT_MAX]`.
     */
    void trySetBufferSize(size_t size);
  };

  /**
   * Eagerly start the SSH master connection if not already running.
   *
   * This should be called before creating connections from a pool to avoid
   * deadlocks where multiple pool threads block waiting for the master to start
   * while holding pool slots. (issue #14615)
   *
   * If useMaster is false or the master is already running, this is a no-op.
   */
  void ensureMaster();

  /**
   * @param command The command (arg vector) to execute.
   *
   * @param extraSshArgs Extra arguments to pass to SSH (not the command to
   * execute). Will not be used when "fake SSHing" to the local
   * machine.
   */
  std::unique_ptr<Connection> startCommand(strings_t&& command, strings_t&& extraSshArgs = {});
};

} // namespace nix
