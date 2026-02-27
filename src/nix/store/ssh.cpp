#include "nix/store/ssh.h"

#include "nix/store/globals.h"
#include "nix/util/base-n.h"
#include "nix/util/current-process.h"
#include "nix/util/environment-variables.h"
#include "nix/util/exec.h"
#include "nix/util/finally.h"
#include "nix/util/util.h"

#ifndef _WIN32
#  include <poll.h>
#  include <signal.h>
#endif

namespace nix {

static std::string parse_public_host_key(std::string_view host,
                                         std::string_view ssh_public_host_key) {
  try {
    return base64::decode(ssh_public_host_key);
  } catch (Error& e) {
    e.add_trace({}, "while decoding ssh public host key for host '%s'", host);
    throw;
  }
}

struct invalid_ssh_authority_t : public Error {
  invalid_ssh_authority_t(const parsed_url_t::authority_t& authority, std::string_view reason)
      : Error("invalid SSH authority: '%s': %s", authority.to_string(), reason) {}
};

/**
 * Checks if the hostname/username are valid for use with ssh.
 *
 * @todo Enforce this better. Probably this needs to reimplement the same logic as in
 * https://github.com/openssh/openssh-portable/blob/6ebd472c391a73574abe02771712d407c48e130d/ssh.c#L648-L681
 */
static void check_valid_authority(const parsed_url_t::authority_t& authority) {
  if (const auto& user = authority.user()) {
    if (user->empty())
      throw invalid_ssh_authority_t(authority, "user name must not be empty");
    if (user->starts_with("-"))
      throw invalid_ssh_authority_t(authority,
                                    fmt("user name '%s' must not start with '-'", *user));
  }

  {
    std::string_view host = authority.host();
    if (host.empty())
      throw invalid_ssh_authority_t(authority, "host name must not be empty");
    if (host.starts_with("-"))
      throw invalid_ssh_authority_t(authority, fmt("host name '%s' must not start with '-'", host));
  }
}

strings_t get_nix_ssh_opts() {
  std::string ssh_opts = get_env("NIX_SSHOPTS").value_or("");

  try {
    return shell_split_string(ssh_opts);
  } catch (Error& e) {
    e.add_trace({}, "while splitting NIX_SSHOPTS '%s'", ssh_opts);
    throw;
  }
}

SSHMaster::SSHMaster(const parsed_url_t::authority_t& authority, std::string_view keyFile,
                     std::string_view ssh_public_host_key, bool useMaster, bool compress,
                     descriptor_t logFD)
    : authority(authority),
      hostname_and_user([authority]() {
        std::string result;
        if (authority.user())
          result = *authority.user() + "@";
        result += authority.host();
        return result;
      }()),
      fakeSSH(authority.to_string() == "localhost"),
      keyFile(keyFile),
      ssh_public_host_key(parse_public_host_key(authority.host(), ssh_public_host_key)),
      useMaster(useMaster && !fakeSSH),
      compress(compress),
      logFD(logFD),
      tmp_dir(make_ref<auto_delete_t>(create_temp_dir("", "nix", 0700))) {
  check_valid_authority(authority);
}

void SSHMaster::addCommonSSHOpts(strings_t& args) {
  auto sshArgs = get_nix_ssh_opts();
  args.insert(args.end(), sshArgs.begin(), sshArgs.end());

  if (!keyFile.empty())
    args.insert(args.end(), {"-i", keyFile});
  if (!ssh_public_host_key.empty()) {
    std::filesystem::path file_name = tmp_dir->path() / "host-key";
    write_file(file_name.string(), authority.host() + " " + ssh_public_host_key + "\n");
    args.insert(args.end(), {"-oUserKnownHostsFile=" + file_name.string()});
  }
  if (compress)
    args.push_back("-C");

  if (authority.port())
    args.push_back(fmt("-p%d", *authority.port()));

  // We use this to make ssh signal back to us that the connection is established.
  // It really does run locally; see createSSHEnv which sets up SHELL to make
  // it launch more reliably. The local command runs synchronously, so presumably
  // the remote session won't be garbled if the local command is slow.
  args.push_back("-oPermitLocalCommand=yes");
  args.push_back("-oLocalCommand=echo started");
}

bool SSHMaster::isMasterRunning() {
  strings_t args = {"-O", "check", hostname_and_user};
  addCommonSSHOpts(args);

  auto res =
      run_program(run_options_t{.program = "ssh", .args = args, .merge_stderr_to_stdout = true});
  return res.first == 0;
}

#ifndef _WIN32
/**
 * Wait for data to be available on a file descriptor with a timeout.
 *
 * @param fd The file descriptor to wait on.
 * @param timeout_seconds The timeout in seconds. 0 means no timeout.
 * @return true if data is available, false if timeout occurred.
 * @throws sys_error_t if poll() fails.
 */
static bool wait_for_data(int fd, unsigned int timeout_seconds) {
  if (timeout_seconds == 0)
    return true; // No timeout, assume data will be available

  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = POLLIN;

  int timeout_ms = timeout_seconds * 1000;
  int ret = poll(&pfd, 1, timeout_ms);

  if (ret == -1) {
    if (errno == EINTR)
      return wait_for_data(fd, timeout_seconds); // Retry on interrupt
    throw sys_error_t("poll() failed while waiting for SSH connection");
  }

  return ret > 0; // true if data available, false if timeout
}
#endif

strings_t create_ssh_env() {
  // Copy the environment and set SHELL=/bin/sh
  string_map_t env = get_env();

  // SSH will invoke the "user" shell for -oLocalCommand, but that means
  // $SHELL. To keep things simple and avoid potential issues with other
  // shells, we set it to /bin/sh.
  // Technically, we don't need that, and we could reinvoke ourselves to print
  // "started". Self-reinvocation is tricky with library consumers, but mostly
  // solved; refer to the development history of nixExePath in libstore/globals.cc.
  env.insert_or_assign("SHELL", "/bin/sh");

  strings_t r;
  for (auto& [k, v] : env) {
    r.push_back(k + "=" + v);
  }

  return r;
}

std::unique_ptr<SSHMaster::Connection> SSHMaster::startCommand(strings_t&& command,
                                                               strings_t&& extraSshArgs) {
#ifdef _WIN32 // TODO re-enable on Windows, once we can start processes.
  throw UnimplementedError(
      "cannot yet SSH on windows because spawning processes is not yet implemented");
#else
  Path socket_path = startMaster();

  pipe_t in, out, err;
  in.create();
  out.create();
  err.create();

  auto conn = std::make_unique<Connection>();
  process_options_t options;
  options.die_with_parent = false;

  std::unique_ptr<logger_t::suspension_t> loggerSuspension;
  if (!fakeSSH && !useMaster) {
    loggerSuspension = std::make_unique<logger_t::suspension_t>(logger->suspend());
  }

  conn->sshPid = start_process(
      [&]() {
        restore_process_context();

        close(in.write_side.get());
        close(out.read_side.get());
        close(err.read_side.get());

        if (dup2(in.read_side.get(), STDIN_FILENO) == -1)
          throw sys_error_t("duping over stdin");
        if (dup2(out.write_side.get(), STDOUT_FILENO) == -1)
          throw sys_error_t("duping over stdout");
        if (logFD != INVALID_DESCRIPTOR && dup2(logFD, STDERR_FILENO) == -1)
          throw sys_error_t("duping over stderr");
        else if (logFD == INVALID_DESCRIPTOR && dup2(err.write_side.get(), STDERR_FILENO) == -1)
          throw sys_error_t("duping over stderr");

        strings_t args;

        if (!fakeSSH) {
          args = {"ssh", hostname_and_user.c_str(), "-x"};
          addCommonSSHOpts(args);
          if (socket_path != "")
            args.insert(args.end(), {"-S", socket_path});
          if (verbosity >= lvl_chatty)
            args.push_back("-v");
          args.splice(args.end(), std::move(extraSshArgs));
          args.push_back("--");
        }

        args.splice(args.end(), std::move(command));
        auto env = create_ssh_env();
        nix::execvpe(args.begin()->c_str(), strings_to_char_ptrs(args).data(),
                     strings_to_char_ptrs(env).data());

        // could not exec ssh/bash
        throw sys_error_t("unable to execute '%s'", args.front());
      },
      options);

  in.read_side = INVALID_DESCRIPTOR;
  out.write_side = INVALID_DESCRIPTOR;
  err.write_side = INVALID_DESCRIPTOR;

  // Wait for the SSH connection to be established,
  // So that we don't overwrite the password prompt with our progress bar.
  if (!fakeSSH && !useMaster && !isMasterRunning()) {
    // Helper to clean up SSH process on error
    auto killSsh = [&]() {
      if (conn->sshPid != INVALID_DESCRIPTOR) {
        kill(conn->sshPid, SIGTERM);
        conn->sshPid = INVALID_DESCRIPTOR;
      }
    };

    // Wait for data with timeout to prevent hanging forever (issue #10645)
    unsigned int timeout = settings.sshTimeout;
    if (!wait_for_data(out.read_side.get(), timeout)) {
      killSsh();
      throw Error("SSH connection to '%s' timed out after %d seconds. "
                  "Check network connectivity and SSH configuration. "
                  "You can adjust the timeout with the 'ssh-timeout' setting.",
                  authority.host(), timeout);
    }

    std::string reply;
    try {
      reply = read_line(out.read_side.get());
    } catch (EndOfFile& e) {
      killSsh();
      std::string childStderr;
      try {
        childStderr = drain_fd(err.read_side.get(), false);
      } catch (...) {
      }
      if (!childStderr.empty())
        throw Error("failed to start SSH connection to '%s': %s", authority.host(),
                    chomp(childStderr));
      throw Error("failed to start SSH connection to '%s'", authority.host());
    }

    if (reply != "started") {
      killSsh();
      printTalkative("SSH stdout first line: %s", reply);
      throw Error("failed to start SSH connection to '%s'", authority.host());
    }
  }

  conn->out = std::move(out.read_side);
  conn->in = std::move(in.write_side);

  return conn;
#endif
}

#ifndef _WIN32 // TODO re-enable on Windows, once we can start processes.

Path SSHMaster::startMaster() {
  if (!useMaster)
    return "";

  Path socket_path;
  process_handle_t sshMaster = INVALID_DESCRIPTOR;

  {
    auto state(state_.lock());

    // If master is already running, return immediately
    if (state->sshMaster != INVALID_DESCRIPTOR)
      return state->socket_path;

    // If another thread is starting the master, wait for it to complete.
    // This prevents the deadlock in issue #14615 where multiple threads
    // would block on the lock while one thread holds it during blocking I/O.
    while (state->starting) {
      state.wait(state_cv_);
      // After waking up, check if master is now running
      if (state->sshMaster != INVALID_DESCRIPTOR)
        return state->socket_path;
    }

    // Mark that we're starting the master (prevents other threads from also trying)
    state->starting = true;
    state->socket_path = (Path)*tmp_dir + "/ssh.sock";
    socket_path = state->socket_path;
  }
  // Lock is now released - other threads can check state->starting and wait

  // Ensure we clear the 'starting' flag even on exceptions
  auto cleanup = finally_t([&]() {
    auto state(state_.lock());
    state->starting = false;
    state_cv_.notify_all();
  });

  auto suspension = logger->suspend();

  // Check if a master is already running (e.g., from a previous process).
  // This is done outside the lock since it runs an external command.
  if (isMasterRunning()) {
    auto state(state_.lock());
    // Set sshMaster to a sentinel value to indicate external master is running.
    // We use -1 (::pid_t) as a sentinel since we don't own the process.
    state->sshMaster = (::pid_t)-1;
    return socket_path;
  }

  pipe_t out, err;
  out.create();
  err.create();

  process_options_t options;
  options.die_with_parent = false;

  sshMaster = start_process(
      [&]() {
        restore_process_context();

        close(out.read_side.get());
        close(err.read_side.get());

        if (dup2(out.write_side.get(), STDOUT_FILENO) == -1)
          throw sys_error_t("duping over stdout");
        if (dup2(err.write_side.get(), STDERR_FILENO) == -1)
          throw sys_error_t("duping over stderr");

        strings_t args = {"ssh", hostname_and_user.c_str(), "-M", "-N", "-S", socket_path};
        if (verbosity >= lvl_chatty)
          args.push_back("-v");
        addCommonSSHOpts(args);
        auto env = create_ssh_env();
        nix::execvpe(args.begin()->c_str(), strings_to_char_ptrs(args).data(),
                     strings_to_char_ptrs(env).data());

        throw sys_error_t("unable to execute '%s'", args.front());
      },
      options);

  out.write_side = INVALID_DESCRIPTOR;
  err.write_side = INVALID_DESCRIPTOR;

  // Helper to clean up SSH master process on error.
  // Use process_handle_t's kill method which sends SIGKILL by default,
  // but we set it to SIGTERM for graceful shutdown.
  sshMaster.set_kill_signal(SIGTERM);
  auto killMaster = [&]() {
    if (sshMaster != INVALID_DESCRIPTOR) {
      sshMaster.kill();
      // Release ownership so destructor doesn't wait for the killed process
      sshMaster.release();
    }
  };

  // Wait for data with timeout to prevent hanging forever (issue #10645)
  unsigned int timeout = settings.sshTimeout;
  if (!wait_for_data(out.read_side.get(), timeout)) {
    killMaster();
    throw Error("SSH master connection to '%s' timed out after %d seconds. "
                "Check network connectivity and SSH configuration. "
                "You can adjust the timeout with the 'ssh-timeout' setting.",
                authority.host(), timeout);
  }

  std::string reply;
  try {
    reply = read_line(out.read_side.get());
  } catch (EndOfFile& e) {
    killMaster();
    std::string childStderr;
    try {
      childStderr = drain_fd(err.read_side.get(), false);
    } catch (...) {
    }
    if (!childStderr.empty())
      throw Error("failed to start SSH master connection to '%s': %s", authority.host(),
                  chomp(childStderr));
    throw Error("failed to start SSH master connection to '%s'", authority.host());
  }

  if (reply != "started") {
    killMaster();
    printTalkative("SSH master stdout first line: %s", reply);
    throw Error("failed to start SSH master connection to '%s'", authority.host());
  }

  // Successfully started - store the master process handle
  {
    auto state(state_.lock());
    state->sshMaster = std::move(sshMaster);
  }

  return socket_path;
}

#endif

void SSHMaster::Connection::trySetBufferSize(size_t size) {
#ifdef F_SETPIPE_SZ
  /* This `fcntl` method of doing this takes a positive `int`. Check
     and convert accordingly.

     The function overall still takes `size_t` because this is more
     portable for a platform-agnostic interface. */
  assert(size <= INT_MAX);
  int pipesize = size;
  fcntl(in.get(), F_SETPIPE_SZ, pipesize);
  fcntl(out.get(), F_SETPIPE_SZ, pipesize);
#endif
}

} // namespace nix
