#include "run.h"

#include <filesystem>

#include "nix/cmd/command-installable-value.h"
#include "nix/expr/eval.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/derivations.h"
#include "nix/store/globals.h"
#include "nix/store/local-fs-store.h"
#include "nix/store/store-api.h"
#include "nix/util/current-process.h"
#include "nix/util/finally.h"
#include "nix/util/signals.h"
#include "nix/util/source-accessor.h"
#include "nix/util/util.h"

#ifdef __linux__
#  include <sys/mount.h>

#  include "nix/store/personality.h"
#endif

#ifndef _WIN32
#  include <signal.h>
#  include <termios.h>
#  include <unistd.h>
#endif

#include <queue>

extern char** environ __attribute__((weak));


std::string chroot_helper_name = "__run_in_chroot";

namespace nix {

/* Convert `env` to a list of strings suitable for `execve`'s `envp` argument. */
strings_t to_envp(string_map_t env) {
  strings_t env_strs;
  for (auto& i : env) {
    env_strs.push_back(i.first + "=" + i.second);
  }

  return env_strs;
}

#ifndef _WIN32
/**
 * Set up process group and terminal control for an interactive shell.
 *
 * This fixes issue #2141: processes started by shellHook inherit the shell's
 * process group, causing signals (like SIGINT from Ctrl+C) to be delivered
 * to them as well as the shell. By putting the shell in its own process group
 * and giving it terminal control, background processes started by shellHook
 * can create their own process groups and won't receive terminal signals.
 *
 * The key insight is that when a shell starts a background process (e.g.,
 * `pg_ctl start` or `command &`), the shell puts it in a new process group.
 * But shellHook commands run during shell initialization, before the shell
 * has terminal control and before it can properly manage process groups.
 * By ensuring the shell is in its own process group with terminal control
 * from the start, background processes started by shellHook behave correctly.
 */
static void setup_interactive_process_group() {
  /* Check if we have a controlling terminal */
  int tty_fd = isatty(STDIN_FILENO)    ? STDIN_FILENO
               : isatty(STDOUT_FILENO) ? STDOUT_FILENO
               : isatty(STDERR_FILENO) ? STDERR_FILENO
                                       : -1;

  if (tty_fd < 0) {
    /* No controlling terminal, nothing to do */
    return;
  }

  pid_t shell_pgid = getpgrp();
  pid_t fg_pgid = tcgetpgrp(tty_fd);

  /* If we're not in the foreground process group, we shouldn't try to
     take terminal control - the parent process is managing things */
  if (fg_pgid != -1 && fg_pgid != shell_pgid) {
    return;
  }

  /* Ignore SIGTTOU while we manipulate terminal settings.
     SIGTTOU is sent to background processes that try to write to the terminal
     or change terminal settings. During the brief window where we're setting
     up our new process group, we might technically be "background" before
     we call tcsetpgrp. */
  struct sigaction sa_ignore{}, sa_old_ttou{}, sa_old_ttin{};
  sa_ignore.sa_handler = SIG_IGN;
  sigemptyset(&sa_ignore.sa_mask);

  sigaction(SIGTTOU, &sa_ignore, &sa_old_ttou);
  sigaction(SIGTTIN, &sa_ignore, &sa_old_ttin);

  /* Create a new process group with this process as the leader.
     This is the key fix: processes forked by shellHook will inherit
     this process group initially, but when bash starts them as background
     jobs, it will give them their own process groups. */
  pid_t pid = getpid();
  if (setpgid(pid, pid) == 0) {
    /* Take control of the terminal. This makes our new process group
       the foreground process group, so Ctrl+C sends SIGINT only to us
       (and our foreground children), not to background daemons started
       by shellHook that have their own process groups. */
    tcsetpgrp(tty_fd, pid);
  }

  /* Restore original signal handlers */
  sigaction(SIGTTOU, &sa_old_ttou, nullptr);
  sigaction(SIGTTIN, &sa_old_ttin, nullptr);
}
#endif

void exec_program_in_store(ref<store_t> store, use_lookup_path_t use_lookup_path,
                           const std::string& program, const strings_t& args,
                           std::optional<std::string_view> system,
                           std::optional<string_map_t> env) {
  logger->stop();

  char** envp;
  strings_t env_strs;
  std::vector<char*> envCharPtrs;
  if (env.has_value()) {
    env_strs = to_envp(env.value());
    envCharPtrs = strings_to_char_ptrs(env_strs);
    envp = envCharPtrs.data();
  } else {
    envp = environ;
  }

  restore_process_context();

#ifndef _WIN32
  /* Set up process group and terminal control for interactive shells.
     This ensures that background processes started by shellHook can
     properly detach from the shell's process group. See issue #2141. */
  setup_interactive_process_group();
#endif

  /* If this is a diverted store (i.e. its "logical" location
     (typically /nix/store) differs from its "physical" location
     (e.g. /home/eelco/nix/store), then run the command in a
     chroot. For non-root users, this requires running it in new
     mount and user namespaces. Unfortunately,
     unshare(CLONE_NEWUSER) doesn't work in a multithreaded program
     (which "nix" is), so we exec() a single-threaded helper program
     (chroot_helper() below) to do the work. */
  auto store2 = store.dynamic_pointer_cast<local_fs_store>();

  if (!store2) {
    throw Error("store '%s' is not a local store so it does not support command execution",
                store->config.getHumanReadableURI());
  }

  if (store->store_dir != store2->getRealStoreDir()) {
    strings_t helper_args = {chroot_helper_name, store->store_dir, store2->getRealStoreDir(),
                             std::string(system.value_or("")), program};
    for (auto& arg : args) {
      helper_args.push_back(arg);
    }

    execve(get_self_exe().value_or("nix").c_str(), strings_to_char_ptrs(helper_args).data(), envp);

    throw sys_error_t("could not execute chroot helper");
  }

#ifdef __linux__
  if (system) {
    linux::setPersonality(*system);
  }
#endif

  if (use_lookup_path == use_lookup_path_t::use) {
    // We have to set `environ` by hand because there is no `execvpe` on macOS.
    environ = envp;
    execvp(program.c_str(), strings_to_char_ptrs(args).data());
  } else {
    execve(program.c_str(), strings_to_char_ptrs(args).data(), envp);
  }

  throw sys_error_t("unable to execute '%s'", program);
}

} // namespace nix

struct cmd_run_t : nix::InstallableValueCommand, nix::MixEnvironment {
  using InstallableCommand::run;

  std::vector<std::string> args;

  cmd_run_t() { expect_args({.label = "args", .handler = {&args}, .completer = complete_path}); }

  std::string description() override { return "run a Nix application"; }

  std::string doc() override {
    return
#include "run.md"
        ;
  }

  nix::strings_t getDefaultFlakeAttrPaths() override {
    nix::strings_t res{
        "apps." + nix::settings.thisSystem.get() + ".default",
        "defaultApp." + nix::settings.thisSystem.get(),
    };
    for (auto& s : SourceExprCommand::getDefaultFlakeAttrPaths()) {
      res.push_back(s);
    }
    return res;
  }

  nix::strings_t getDefaultFlakeAttrPathPrefixes() override {
    nix::strings_t res{"apps." + nix::settings.thisSystem.get() + "."};
    for (auto& s : SourceExprCommand::getDefaultFlakeAttrPathPrefixes()) {
      res.push_back(s);
    }
    return res;
  }

  void run(nix::ref<nix::store_t> store, nix::ref<nix::InstallableValue> installable) override {
    auto state = getEvalState();

    lock_flags.applyNixConfig = true;
    auto app = installable->toApp(*state).resolve(getEvalStore(), store);

    nix::strings_t all_args{app.program.string()};
    for (auto& i : args) {
      all_args.push_back(i);
    }

    // Release our references to eval caches to ensure they are persisted to disk, because
    // we are about to exec out of this process without running C++ destructors.
    state->evalCaches.clear();

    setEnviron();

    nix::exec_program_in_store(store, nix::use_lookup_path_t::dont_use, app.program.string(),
                               all_args);
  }
};

static auto r_cmd_run = nix::registerCommand<cmd_run_t>("run");

void chroot_helper(int argc, char** argv) {
  int p = 1;
  std::string store_dir = argv[p++];
  std::string real_store_dir = argv[p++];
  std::string system = argv[p++];
  std::string cmd = argv[p++];
  nix::strings_t args;
  while (p < argc) {
    args.push_back(argv[p++]);
  }

#ifdef __linux__
  uid_t uid = getuid();
  uid_t gid = getgid();

  if (unshare(CLONE_NEWUSER | CLONE_NEWNS) == -1) {
    /* Try with just CLONE_NEWNS in case user namespaces are
       specifically disabled. */
    if (unshare(CLONE_NEWNS) == -1) {
      throw nix::sys_error_t("setting up a private mount namespace");
    }
  }

  /* Bind-mount real_store_dir on /nix/store. If the latter mount
     point doesn't already exists, we have to create a chroot
     environment containing the mount point and bind mounts for the
     children of /.
     Overlayfs for user namespaces is fixed in Linux since ac519625ed
     (v5.11, 14 February 2021) */
  if (!nix::path_exists(store_dir)) {
    // FIXME: Use overlayfs?

    std::filesystem::path tmp_dir = nix::create_temp_dir();

    nix::create_dirs(tmp_dir / store_dir);

    if (mount(real_store_dir.c_str(), (tmp_dir / store_dir).c_str(), "", MS_BIND, 0) == -1) {
      throw nix::sys_error_t("mounting '%s' on '%s'", real_store_dir, store_dir);
    }

    for (const auto& entry : nix::directory_iterator_t{"/"}) {
      nix::check_interrupt();
      const auto& src = entry.path();
      std::filesystem::path dst = tmp_dir / entry.path().filename();
      if (nix::path_exists(dst)) {
        continue;
      }
      auto st = entry.symlink_status();
      if (std::filesystem::is_directory(st)) {
        if (mkdir(dst.c_str(), 0700) == -1) {
          throw nix::sys_error_t("creating directory '%s'", dst);
        }
        if (mount(src.c_str(), dst.c_str(), "", MS_BIND | MS_REC, 0) == -1) {
          throw nix::sys_error_t("mounting '%s' on '%s'", src, dst);
        }
      } else if (std::filesystem::is_symlink(st)) {
        nix::create_symlink(nix::read_link(src), dst);
      }
    }

    char* cwd = getcwd(0, 0);
    if (!cwd) {
      throw nix::sys_error_t("getting current directory");
    }
    auto free_cwd = finally_t([&]() { free(cwd); });

    if (chroot(tmp_dir.c_str()) == -1) {
      throw nix::sys_error_t("chrooting into '%s'", tmp_dir);
    }

    if (chdir(cwd) == -1) {
      throw nix::sys_error_t("chdir to '%s' in chroot", cwd);
    }
  } else if (mount("overlay", store_dir.c_str(), "overlay", MS_MGC_VAL,
                   nix::fmt("lowerdir=%s:%s", store_dir, real_store_dir).c_str()) == -1) {
    if (mount(real_store_dir.c_str(), store_dir.c_str(), "", MS_BIND, 0) == -1) {
      throw nix::sys_error_t("mounting '%s' on '%s'", real_store_dir, store_dir);
    }
  }

  nix::write_file(std::filesystem::path{"/proc/self/setgroups"}, "deny");
  nix::write_file(std::filesystem::path{"/proc/self/uid_map"}, nix::fmt("%d %d %d", uid, uid, 1));
  nix::write_file(std::filesystem::path{"/proc/self/gid_map"}, nix::fmt("%d %d %d", gid, gid, 1));

#  ifdef __linux__
  if (system != "") {
    nix::linux::setPersonality(system);
  }
#  endif

  execvp(cmd.c_str(), nix::strings_to_char_ptrs(args).data());

  throw nix::sys_error_t("unable to exec '%s'", cmd);

#else
  throw nix::Error("mounting the Nix store on '%s' is not supported on this platform", store_dir);
#endif
}
