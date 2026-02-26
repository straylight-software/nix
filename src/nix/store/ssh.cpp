#include "nix/store/ssh.h"

#include "nix/util/base-n.h"
#include "nix/util/current-process.h"
#include "nix/util/environment-variables.h"
#include "nix/util/exec.h"
#include "nix/util/finally.h"
#include "nix/util/util.h"

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

  pipe_t in, out;
  in.create();
  out.create();

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

        if (dup2(in.read_side.get(), STDIN_FILENO) == -1)
          throw sys_error_t("duping over stdin");
        if (dup2(out.write_side.get(), STDOUT_FILENO) == -1)
          throw sys_error_t("duping over stdout");
        if (logFD != -1 && dup2(logFD, STDERR_FILENO) == -1)
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

  // Wait for the SSH connection to be established,
  // So that we don't overwrite the password prompt with our progress bar.
  if (!fakeSSH && !useMaster && !isMasterRunning()) {
    std::string reply;
    try {
      reply = read_line(out.read_side.get());
    } catch (EndOfFile& e) {
    }

    if (reply != "started") {
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

  auto state(state_.lock());

  if (state->sshMaster != INVALID_DESCRIPTOR)
    return state->socket_path;

  state->socket_path = (Path)*tmp_dir + "/ssh.sock";

  pipe_t out;
  out.create();

  process_options_t options;
  options.die_with_parent = false;

  auto suspension = logger->suspend();

  if (isMasterRunning())
    return state->socket_path;

  state->sshMaster = start_process(
      [&]() {
        restore_process_context();

        close(out.read_side.get());

        if (dup2(out.write_side.get(), STDOUT_FILENO) == -1)
          throw sys_error_t("duping over stdout");

        strings_t args = {"ssh", hostname_and_user.c_str(), "-M", "-N", "-S", state->socket_path};
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

  std::string reply;
  try {
    reply = read_line(out.read_side.get());
  } catch (EndOfFile& e) {
  }

  if (reply != "started") {
    printTalkative("SSH master stdout first line: %s", reply);
    throw Error("failed to start SSH master connection to '%s'", authority.host());
  }

  return state->socket_path;
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
