#pragma once
///@file

#include "nix/util/file-system.h"
#include "nix/util/processes.h"
#include "nix/util/ref.h"
#include "nix/util/sync.h"
#include "nix/util/url.h"

namespace nix {

strings_t getNixSshOpts();

class SSHMaster {
private:
  parsed_url_t::authority_t authority;
  std::string hostnameAndUser;
  bool fakeSSH;
  const std::string keyFile;
  /**
   * raw_t bytes, not Base64 encoding.
   */
  const std::string sshPublicHostKey;
  const bool useMaster;
  const bool compress;
  const descriptor_t logFD;

  const ref<const auto_delete_t> tmpDir;

  struct State {
#ifndef _WIN32 // TODO re-enable on Windows, once we can start processes.
    Pid sshMaster;
#endif
    Path socketPath;
  };

  sync_t<State> state_;

  void addCommonSSHOpts(strings_t& args);
  bool isMasterRunning();

#ifndef _WIN32 // TODO re-enable on Windows, once we can start processes.
  Path startMaster();
#endif

public:
  SSHMaster(const parsed_url_t::authority_t& authority, std::string_view keyFile,
            std::string_view sshPublicHostKey, bool useMaster, bool compress,
            descriptor_t logFD = INVALID_DESCRIPTOR);

  struct Connection {
#ifndef _WIN32 // TODO re-enable on Windows, once we can start processes.
    Pid sshPid;
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
   * @param command The command (arg vector) to execute.
   *
   * @param extraSshArgs Extra arguments to pass to SSH (not the command to
   * execute). Will not be used when "fake SSHing" to the local
   * machine.
   */
  std::unique_ptr<Connection> startCommand(strings_t&& command, strings_t&& extraSshArgs = {});
};

} // namespace nix
