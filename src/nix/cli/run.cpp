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

#include <queue>

extern char** environ __attribute__((weak));

namespace nix::fs {
using namespace std::filesystem;
}

using namespace nix;

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

  /* If this is a diverted store (i.e. its "logical" location
     (typically /nix/store) differs from its "physical" location
     (e.g. /home/eelco/nix/store), then run the command in a
     chroot. For non-root users, this requires running it in new
     mount and user namespaces. Unfortunately,
     unshare(CLONE_NEWUSER) doesn't work in a multithreaded program
     (which "nix" is), so we exec() a single-threaded helper program
     (chroot_helper() below) to do the work. */
  auto store2 = store.dynamic_pointer_cast<local_fs_store>();

  if (!store2)
    throw Error("store '%s' is not a local store so it does not support command execution",
                store->config.getHumanReadableURI());

  if (store->store_dir != store2->getRealStoreDir()) {
    strings_t helper_args = {chroot_helper_name, store->store_dir, store2->getRealStoreDir(),
                             std::string(system.value_or("")), program};
    for (auto& arg : args)
      helper_args.push_back(arg);

    execve(get_self_exe().value_or("nix").c_str(), strings_to_char_ptrs(helper_args).data(), envp);

    throw sys_error_t("could not execute chroot helper");
  }

#ifdef __linux__
  if (system)
    linux::setPersonality(*system);
#endif

  if (use_lookup_path == use_lookup_path_t::use) {
    // We have to set `environ` by hand because there is no `execvpe` on macOS.
    environ = envp;
    execvp(program.c_str(), strings_to_char_ptrs(args).data());
  } else
    execve(program.c_str(), strings_to_char_ptrs(args).data(), envp);

  throw sys_error_t("unable to execute '%s'", program);
}

} // namespace nix

struct cmd_run_t : InstallableValueCommand, MixEnvironment {
  using InstallableCommand::run;

  std::vector<std::string> args;

  cmd_run_t() { expect_args({.label = "args", .handler = {&args}, .completer = complete_path}); }

  std::string description() override { return "run a Nix application"; }

  std::string doc() override {
    return
#include "run.md"
        ;
  }

  strings_t getDefaultFlakeAttrPaths() override {
    strings_t res{
        "apps." + settings.thisSystem.get() + ".default",
        "defaultApp." + settings.thisSystem.get(),
    };
    for (auto& s : SourceExprCommand::getDefaultFlakeAttrPaths())
      res.push_back(s);
    return res;
  }

  strings_t getDefaultFlakeAttrPathPrefixes() override {
    strings_t res{"apps." + settings.thisSystem.get() + "."};
    for (auto& s : SourceExprCommand::getDefaultFlakeAttrPathPrefixes())
      res.push_back(s);
    return res;
  }

  void run(ref<store_t> store, ref<InstallableValue> installable) override {
    auto state = getEvalState();

    lock_flags.applyNixConfig = true;
    auto app = installable->toApp(*state).resolve(getEvalStore(), store);

    strings_t all_args{app.program.string()};
    for (auto& i : args)
      all_args.push_back(i);

    // Release our references to eval caches to ensure they are persisted to disk, because
    // we are about to exec out of this process without running C++ destructors.
    state->evalCaches.clear();

    setEnviron();

    exec_program_in_store(store, use_lookup_path_t::dont_use, app.program.string(), all_args);
  }
};

static auto r_cmd_run = registerCommand<cmd_run_t>("run");

void chroot_helper(int argc, char** argv) {
  int p = 1;
  std::string store_dir = argv[p++];
  std::string real_store_dir = argv[p++];
  std::string system = argv[p++];
  std::string cmd = argv[p++];
  strings_t args;
  while (p < argc)
    args.push_back(argv[p++]);

#ifdef __linux__
  uid_t uid = getuid();
  uid_t gid = getgid();

  if (unshare(CLONE_NEWUSER | CLONE_NEWNS) == -1)
    /* Try with just CLONE_NEWNS in case user namespaces are
       specifically disabled. */
    if (unshare(CLONE_NEWNS) == -1)
      throw sys_error_t("setting up a private mount namespace");

  /* Bind-mount real_store_dir on /nix/store. If the latter mount
     point doesn't already exists, we have to create a chroot
     environment containing the mount point and bind mounts for the
     children of /.
     Overlayfs for user namespaces is fixed in Linux since ac519625ed
     (v5.11, 14 February 2021) */
  if (!path_exists(store_dir)) {
    // FIXME: Use overlayfs?

    std::filesystem::path tmp_dir = create_temp_dir();

    create_dirs(tmp_dir + store_dir);

    if (mount(real_store_dir.c_str(), (tmp_dir + store_dir).c_str(), "", MS_BIND, 0) == -1)
      throw sys_error_t("mounting '%s' on '%s'", real_store_dir, store_dir);

    for (const auto& entry : directory_iterator_t{"/"}) {
      check_interrupt();
      const auto& src = entry.path();
      std::filesystem::path dst = tmp_dir / entry.path().filename();
      if (path_exists(dst))
        continue;
      auto st = entry.symlink_status();
      if (std::filesystem::is_directory(st)) {
        if (mkdir(dst.c_str(), 0700) == -1)
          throw sys_error_t("creating directory '%s'", dst);
        if (mount(src.c_str(), dst.c_str(), "", MS_BIND | MS_REC, 0) == -1)
          throw sys_error_t("mounting '%s' on '%s'", src, dst);
      } else if (std::filesystem::is_symlink(st))
        create_symlink(read_link(src), dst);
    }

    char* cwd = getcwd(0, 0);
    if (!cwd)
      throw sys_error_t("getting current directory");
    finally_t free_cwd([&]() { free(cwd); });

    if (chroot(tmp_dir.c_str()) == -1)
      throw sys_error_t("chrooting into '%s'", tmp_dir);

    if (chdir(cwd) == -1)
      throw sys_error_t("chdir to '%s' in chroot", cwd);
  } else if (mount("overlay", store_dir.c_str(), "overlay", MS_MGC_VAL,
                   fmt("lowerdir=%s:%s", store_dir, real_store_dir).c_str()) == -1)
    if (mount(real_store_dir.c_str(), store_dir.c_str(), "", MS_BIND, 0) == -1)
      throw sys_error_t("mounting '%s' on '%s'", real_store_dir, store_dir);

  write_file(std::filesystem::path{"/proc/self/setgroups"}, "deny");
  write_file(std::filesystem::path{"/proc/self/uid_map"}, fmt("%d %d %d", uid, uid, 1));
  write_file(std::filesystem::path{"/proc/self/gid_map"}, fmt("%d %d %d", gid, gid, 1));

#  ifdef __linux__
  if (system != "")
    linux::setPersonality(system);
#  endif

  execvp(cmd.c_str(), strings_to_char_ptrs(args).data());

  throw sys_error_t("unable to exec '%s'", cmd);

#else
  throw Error("mounting the Nix store on '%s' is not supported on this platform", store_dir);
#endif
}
