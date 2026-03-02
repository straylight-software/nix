#include "nix/store/build/derivation-builder.h"

#include <functional>
#include <queue>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "nix/store/active-builds.h"
#include "nix/store/build/child.h"
#include "nix/store/build/derivation-env-desugar.h"
#include "nix/store/builtins.h"
#include "nix/store/daemon.h"
#include "nix/store/globals.h"
#include "nix/store/local-store.h"
#include "nix/store/path-references.h"
#include "nix/store/posix-fs-canonicalise.h"
#include "nix/store/restricted-store.h"
#include "nix/store/store-config-private.h"
#include "nix/store/user-lock.h"
#include "nix/util/archive.h"
#include "nix/util/file-system.h"
#include "nix/util/finally.h"
#include "nix/util/git.h"
#include "nix/util/posix-source-accessor.h"
#include "nix/util/processes.h"
#include "nix/util/terminal.h"
#include "nix/util/topo-sort.h"
#include "nix/util/unix-domain-socket.h"
#include "nix/util/util.h"

#if HAVE_STATVFS
#  include <sys/statvfs.h>
#endif

#include <iostream>

#include <grp.h>
#include <pwd.h>

#include "nix/store/build/derivation-check.h"
#include "nix/store/store-config-private.h"
#include "nix/util/signals.h"
#include "nix/util/strings.h"

#if NIX_WITH_AWS_AUTH
#  include "nix/store/aws-creds.h"
#  include "nix/store/s3-url.h"
#  include "nix/util/url.h"
#endif

namespace nix {

struct not_deterministic_t : build_error_t {
  not_deterministic_t(auto&&... args)
      : build_error_t(build_result_t::Failure::not_deterministic_t, args...) {}
};

/**
 * This class represents the state for building locally.
 *
 * @todo Ideally, it would not be a class, but a single function.
 * However, besides the main entry point, there are a few more methods
 * which are externally called, and need to be gotten rid of. There are
 * also some virtual methods (either directly here or inherited from
 * `DerivationBuilderCallbacks`, a stop-gap) that represent outgoing
 * rather than incoming call edges that either should be removed, or
 * become (higher order) function parameters.
 */
// FIXME: rename this to UnixDerivationBuilder or something like that.
struct derivation_builder_impl_t : public DerivationBuilder, public DerivationBuilderParams {
  /**
   * The process ID of the builder.
   */
  process_handle_t pid;

  /**
   * Handles to track active builds for `nix ps`.
   */
  std::optional<TrackActiveBuildsStore::BuildHandle> activeBuildHandle;

  LocalStore& store;

  std::unique_ptr<DerivationBuilderCallbacks> misc_methods;

  derivation_builder_impl_t(LocalStore& store,
                            std::unique_ptr<DerivationBuilderCallbacks> misc_methods,
                            DerivationBuilderParams params)
      : DerivationBuilderParams{std::move(params)},
        store{store},
        misc_methods{std::move(misc_methods)},
        derivation_type{drv.type()} {}

  ~derivation_builder_impl_t() {
    /* Careful: we should never ever throw an exception from a
       destructor. */
    try {
      kill_child();
    } catch (...) {
      ignore_exception_in_destructor();
    }
    try {
      stop_daemon();
    } catch (...) {
      ignore_exception_in_destructor();
    }
    try {
      cleanup_build(false);
    } catch (...) {
      ignore_exception_in_destructor();
    }
  }

  /**
   * User selected for running the builder.
   */
  std::unique_ptr<UserLock> buildUser;

  /**
   * The temporary directory used for the build.
   */
  Path tmp_dir;

  /**
   * The top-level temporary directory. `tmp_dir` is either equal to
   * or a child of this directory.
   */
  Path topTmpDir;

  /**
   * The file descriptor of the temporary directory.
   */
  auto_close_fd_t tmpDirFd;

  /**
   * The sort of derivation we are building.
   *
   * Just a cached value, computed from `drv`.
   */
  const DerivationType derivation_type;

  typedef string_map_t environment_t;
  environment_t env;

  /**
   * Hash rewriting.
   */
  string_map_t input_rewrites, outputRewrites;
  typedef std::map<store_path_t, store_path_t> redirected_outputs_t;
  redirected_outputs_t redirectedOutputs;

  /**
   * The output paths used during the build.
   *
   * - input_t-addressed derivations or fixed content-addressed outputs are
   *   sometimes built when some of their outputs already exist, and can not
   *   be hidden via sandboxing. We use temporary locations instead and
   *   rewrite after the build. Otherwise the regular predetermined paths are
   *   put here.
   *
   * - Floating content-addressing derivations do not know their final build
   *   output paths until the outputs are hashed, so random locations are
   *   used, and then renamed. The randomness helps guard against hidden
   *   self-references.
   */
  OutputPathMap scratchOutputs;

  const static Path home_dir;

  /**
   * The recursive Nix daemon socket.
   */
  auto_close_fd_t daemonSocket;

  /**
   * The daemon main thread.
   */
  std::thread daemonThread;

  /**
   * The daemon worker threads.
   */
  std::vector<std::thread> daemonWorkerThreads;

  /**
   * Callback to propagate SIGTSTP/SIGCONT to child build processes.
   * This allows suspending builds with Ctrl+Z.
   */
  std::unique_ptr<suspend_callback_t> suspendCallback;

  const store_path_set_t& originalPaths() override { return inputPaths; }

  bool is_allowed(const store_path_t& path) override {
    return inputPaths.count(path) || addedPaths.count(path);
  }

  bool is_allowed(const DrvOutput& id) override { return addedDrvOutputs.count(id); }

  bool is_allowed(const derived_path_t& req);

  friend struct restricted_store_t;

  /**
   * Whether we need to perform hash rewriting if there are valid output paths.
   */
  virtual bool needs_hash_rewrite() { return true; }

  std::optional<descriptor_t> start_build() override;

  SingleDrvOutputs unprepare_build() override;

  /**
   * Acquire a build user lock. Return nullptr if no lock is available.
   */
  virtual std::unique_ptr<UserLock> get_build_user() { return acquire_user_lock(1, false); }

  /**
   * Construct the `ActiveBuild` object for `ActiveBuildsTracker`.
   */
  virtual ActiveBuild get_active_build();

  /**
   * Return the paths that should be made available in the sandbox.
   * This includes:
   *
   * * The paths specified by the `sandbox-paths` setting, and their closure in the Nix store.
   * * The contents of the `__impureHostDeps` derivation attribute, if the sandbox is in relaxed
   * mode.
   * * The paths returned by the `pre-build-hook`.
   * * The paths in the input closure of the derivation.
   */
  PathsInChroot get_paths_in_sandbox();

  virtual void set_build_tmp_dir() { tmp_dir = topTmpDir; }

  /**
   * Return the path of the temporary directory in the sandbox.
   */
  virtual Path tmp_dir_in_sandbox() {
    assert(!topTmpDir.empty());
    return topTmpDir;
  }

  /**
   * Ensure that there are no processes running that conflict with
   * `buildUser`.
   */
  virtual void prepare_user() { kill_sandbox(false); }

  /**
   * Called by prepareBuild() to do any setup in the parent to
   * prepare for a sandboxed build.
   */
  virtual void prepare_sandbox();

  virtual strings_t get_pre_build_hook_args() {
    return strings_t({store.printStorePath(drv_path)});
  }

  virtual Path real_path_in_host(const Path& p) { return store.toRealPath(p); }

  /**
   * Open the slave side of the pseudoterminal and use it as stderr.
   */
  void open_slave();

  /**
   * Called by prepareBuild() to start the child process for the
   * build. Must set `pid`. The child must call run_child().
   */
  virtual void start_child();

#if NIX_WITH_AWS_AUTH
  /**
   * Pre-resolve AWS credentials for S3 URLs in builtin:fetchurl.
   * This should be called before forking to ensure credentials are available in child.
   * Returns the credentials if successfully resolved, or std::nullopt otherwise.
   */
  std::optional<AwsCredentials> preResolveAwsCredentials();
#endif

  /**
   * Fill in the environment for the builder.
   */
  void init_env();

  /**
   * Process messages send by the sandbox initialization.
   */
  void process_sandbox_setup_messages();

  /**
   * Start an in-process nix daemon thread for recursive-nix.
   */
  void start_daemon();

  /**
   * Stop the in-process nix daemon thread.
   * @see start_daemon
   */
  void stop_daemon();

  void add_dependency_impl(const store_path_t& path) override;

  /**
   * Make a file owned by the builder.
   *
   * SAFETY: this function is prone to TOCTOU as it receives a path and not a descriptor.
   * It's only safe to call in a child of a directory only visible to the owner.
   */
  void chown_to_builder(const Path& path);

  /**
   * Make a file owned by the builder addressed by its file descriptor.
   */
  void chown_to_builder(int fd, const Path& path);

  /**
   * Create a file in `tmp_dir` owned by the builder.
   */
  void write_builder_file(const std::string& name, std::string_view contents);

  /**
   * Arguments passed to run_child().
   */
  struct run_child_args_t {
#if NIX_WITH_AWS_AUTH
    std::optional<AwsCredentials> awsCredentials;
#endif
  };

  /**
   * Run the builder's process.
   */
  void run_child(run_child_args_t args);

  /**
   * Move the current process into the chroot, if any. Called early
   * by run_child().
   */
  virtual void enter_chroot() {}

  /**
   * Change the current process's uid/gid to the build user, if
   * any. Called by run_child().
   */
  virtual void set_user();

  /**
   * Execute the derivation builder process. Called by run_child() as
   * its final step. Should not return unless there is an error.
   */
  virtual void exec_builder(const strings_t& args, const strings_t& env_strs);

  /**
   * Check that the derivation outputs all exist and register them
   * as valid.
   */
  SingleDrvOutputs register_outputs();

  /**
   * Delete the temporary directory, if we have one.
   *
   * @param force We know the build suceeded, so don't attempt to
   * preseve anything for debugging.
   */
  virtual void cleanup_build(bool force);

  /**
   * Kill any processes running under the build user UID or in the
   * cgroup of the build.
   */
  virtual void kill_sandbox(bool get_stats);

  bool kill_child() override;

  bool decide_whether_disk_full();

  /**
   * Create alternative path calculated from but distinct from the
   * input, so we can avoid overwriting outputs (or other store paths)
   * that already exist.
   */
  store_path_t make_fallback_path(const store_path_t& path);

  /**
   * Make a path to another based on the output name along with the
   * derivation hash.
   *
   * @todo Add option to randomize, so we can audit whether our
   * rewrites caught everything
   */
  store_path_t make_fallback_path(OutputNameView output_name);
};

void handle_diff_hook(uid_t uid, uid_t gid, const Path& try_a, const Path& try_b,
                      const Path& drv_path, const Path& tmp_dir) {
  auto& diff_hook_opt = settings.diff_hook.get();
  if (diff_hook_opt && settings.runDiffHook) {
    auto& diff_hook = *diff_hook_opt;
    try {
      auto diff_res = run_program(run_options_t{.program = diff_hook,
                                                .lookup_path = true,
                                                .args = {try_a, try_b, drv_path, tmp_dir},
                                                .uid = uid,
                                                .gid = gid,
                                                .chdir = "/"});
      if (!status_ok(diff_res.first))
        throw exec_error_t(diff_res.first, "diff-hook program '%1%' %2%", diff_hook,
                           status_to_string(diff_res.first));

      if (diff_res.second != "")
        printError(chomp(diff_res.second));
    } catch (Error& error) {
      error_info_t ei = error.info();
      // FIXME: wrap errors.
      ei.msg_ = hint_fmt_t("diff hook execution failed: %s", ei.msg_.str());
      logError(ei);
    }
  }
}

const Path derivation_builder_impl_t::home_dir = "/homeless-shelter";

void derivation_builder_impl_t::kill_sandbox(bool get_stats) {
  if (buildUser) {
    auto uid = buildUser->getUID();
    assert(uid != 0);
    kill_user(uid);
  }
}

bool derivation_builder_impl_t::kill_child() {
  bool ret = pid != -1;
  if (ret) {
    /* Deregister suspend callback since we're killing the child */
    suspendCallback.reset();

    /* If we're using a build user, then there is a tricky race
       condition: if we kill the build user before the child has
       done its setuid() to the build user uid, then it won't be
       killed, and we'll potentially lock up in pid.wait().  So
       also send a conventional kill to the child. First try SIGTERM
       for graceful shutdown, then SIGKILL if it doesn't respond. */
    ::kill(-pid, SIGTERM); /* ignore the result */

    /* Give the process a chance to exit gracefully (up to 5 seconds) */
    for (int i = 0; i < 50; i++) {
      int status;
      if (waitpid(pid, &status, WNOHANG) > 0) {
        pid.release();
        kill_sandbox(true);
        activeBuildHandle.reset();
        return ret;
      }
      usleep(100000); /* 100ms */
    }

    /* Process didn't exit gracefully, force kill */
    ::kill(-pid, SIGKILL); /* ignore the result */

    kill_sandbox(true);

    pid.wait();

    activeBuildHandle.reset();
  }
  return ret;
}

SingleDrvOutputs derivation_builder_impl_t::unprepare_build() {
  /* Since we got an EOF on the logger pipe, the builder is presumed
     to have terminated.  In fact, the builder could also have
     simply have closed its end of the pipe --- Loss of the pipe
     doesn't mean the process has exited yet, so first check if it
     has already exited and wait for it if still running. Only kill
     as a last resort. */
  int status;
  pid_t ret = waitpid(pid, &status, WNOHANG);
  if (ret == 0) {
    /* Process is still running, wait for it to finish naturally. */
    status = pid.wait();
  } else if (ret == pid) {
    /* Process already exited, release the handle without killing. */
    pid.release();
  } else {
    /* Error or unexpected result, fall back to kill. */
    status = pid.kill();
  }

  debug("builder process for '%s' finished", store.printStorePath(drv_path));

  buildResult.timesBuilt++;
  buildResult.stopTime = time(0);

  /* So the child is gone now. */
  misc_methods->childTerminated();

  /* Deregister suspend callback since child is gone */
  suspendCallback.reset();

  /* Close the read side of the logger pipe. */
  builder_out.close();

  /* Close the log file. */
  misc_methods->closeLogFile();

  /* When running under a build user, make sure that all processes
     running under that uid are gone.  This is to prevent a
     malicious user from leaving behind a process that keeps files
     open and modifies them after they have been chown'ed to
     root. */
  kill_sandbox(true);

  activeBuildHandle.reset();

  /* Terminate the recursive Nix daemon. */
  stop_daemon();

  if (buildResult.cpu_user && buildResult.cpu_system) {
    debug("builder for '%s' terminated with status %d, user CPU %.3fs, system CPU %.3fs",
          store.printStorePath(drv_path), status, ((double)buildResult.cpu_user->count()) / 1000000,
          ((double)buildResult.cpu_system->count()) / 1000000);
  }

  /* Check the exit status. */
  if (!status_ok(status)) {
    /* Check *before* cleaning up. */
    bool disk_full = decide_whether_disk_full();

    cleanup_build(false);

    throw BuilderFailureError{
        !derivation_type.isSandboxed() || disk_full ? build_result_t::Failure::TransientFailure
                                                    : build_result_t::Failure::PermanentFailure,
        status,
        disk_full ? "\nnote: build failure may have been caused by lack of free disk space" : "",
    };
  }

  /* Compute the FS closure of the outputs and register them as
     being valid. */
  auto built_outputs = register_outputs();

  cleanup_build(true);

  return built_outputs;
}

static void chmod_(const Path& path, mode_t mode) {
  if (chmod(path.c_str(), mode) == -1)
    throw sys_error_t("setting permissions on '%s'", path);
}

/* Move/rename path 'src' to 'dst'. Temporarily make 'src' writable if
   it's a directory and we're not root (to be able to update the
   directory's parent link ".."). */
static void move_path(const Path& src, const Path& dst) {
  auto st = lstat(src);

  bool change_perm = (geteuid() && S_ISDIR(st.st_mode) && !(st.st_mode & S_IWUSR));

  if (change_perm)
    chmod_(src, st.st_mode | S_IWUSR);

  std::filesystem::rename(src, dst);

  if (change_perm)
    chmod_(dst, st.st_mode);
}

static void replace_valid_path(const Path& store_path, const Path& tmp_path) {
  /* We can't atomically replace store_path (the original) with
     tmp_path (the replacement), so we have to move it out of the
     way first.  We'd better not be interrupted here, because if
     we're repairing (say) Glibc, we end up with a broken system. */
  Path old_path;

  if (path_exists(store_path)) {
    // why do we loop here?
    // although makeTempPath should be unique, we can't
    // guarantee that.
    do {
      old_path = make_temp_path(store_path, ".old");
      // store paths are often directories so we can't just unlink() it
      // let's make sure the path doesn't exist before we try to use it
    } while (path_exists(old_path));
    move_path(store_path, old_path);
  }
  try {
    move_path(tmp_path, store_path);
  } catch (...) {
    try {
      // attempt to recover
      if (!old_path.empty())
        move_path(old_path, store_path);
    } catch (...) {
      ignore_exception_except_interrupt();
    }
    throw;
  }
  if (!old_path.empty())
    delete_path(old_path);
}

bool derivation_builder_impl_t::decide_whether_disk_full() {
  bool disk_full = false;

  /* Heuristically check whether the build failure may have
     been caused by a disk full condition.  We have no way
     of knowing whether the build actually got an ENOSPC.
     So instead, check if the disk is (nearly) full now.  If
     so, we don't mark this build as a permanent failure. */
#if HAVE_STATVFS
  {
    uint64_t required = 8ULL * 1024 * 1024; // FIXME: make configurable
    struct statvfs st;
    if (statvfs(store.config->real_store_dir.get().c_str(), &st) == 0 &&
        (uint64_t)st.f_bavail * st.f_bsize < required)
      disk_full = true;
    if (statvfs(tmp_dir.c_str(), &st) == 0 && (uint64_t)st.f_bavail * st.f_bsize < required)
      disk_full = true;
  }
#endif

  return disk_full;
}

/**
 * Rethrow the current exception as a subclass of `Error`.
 */
static void rethrow_exception_as_error() {
  try {
    throw;
  } catch (Error&) {
    throw;
  } catch (std::exception& e) {
    throw Error(e.what());
  } catch (...) {
    throw Error("unknown exception");
  }
}

/**
 * Send the current exception to the parent in the format expected by
 * `derivation_builder_impl_t::process_sandbox_setup_messages()`.
 */
static void handle_child_exception(bool send_exception) {
  try {
    rethrow_exception_as_error();
  } catch (Error& e) {
    if (send_exception) {
      write_full(STDERR_FILENO, "\1\n");
      fd_sink_t sink(STDERR_FILENO);
      sink << e;
      sink.flush();
    } else
      std::cerr << e.msg();
  }
}

static void check_not_world_writable(std::filesystem::path path) {
  while (true) {
    auto st = lstat(path);
    if (st.st_mode & S_IWOTH)
      throw Error("Path %s is world-writable or a symlink. That's not allowed for security.", path);
    if (path == path.parent_path())
      break;
    path = path.parent_path();
  }
  return;
}

std::optional<descriptor_t> derivation_builder_impl_t::start_build() {
  if (use_build_users()) {
    if (!buildUser)
      buildUser = get_build_user();

    if (!buildUser)
      return std::nullopt;
  }

  /* Make sure that no other processes are executing under the
     sandbox uids. This must be done before any chown_to_builder()
     calls. */
  prepare_user();

  auto build_dir = store.config->getBuildDir();

  create_dirs(build_dir);

  if (buildUser)
    check_not_world_writable(build_dir);

  /* Create a temporary directory where the build will take
     place. */
  topTmpDir = create_temp_dir(build_dir, "nix", 0700);
  set_build_tmp_dir();
  assert(!tmp_dir.empty());

  /* The TOCTOU between the previous mkdir call and this open call is unavoidable due to
     POSIX semantics.*/
  tmpDirFd = auto_close_fd_t{open(tmp_dir.c_str(), O_RDONLY | O_NOFOLLOW | O_DIRECTORY)};
  if (!tmpDirFd)
    throw sys_error_t("failed to open the build temporary directory descriptor '%1%'", tmp_dir);

  chown_to_builder(tmpDirFd.get(), tmp_dir);

  for (auto& [output_name, status] : initialOutputs) {
    /* Set scratch path we'll actually use during the build.

       If we're not doing a chroot build, but we have some valid
       output paths.  Since we can't just overwrite or delete
       them, we have to do hash rewriting: i.e. in the
       environment/arguments passed to the build, we replace the
       hashes of the valid outputs with unique dummy strings;
       after the build, we discard the redirected outputs
       corresponding to the valid outputs, and rewrite the
       contents of the new outputs to replace the dummy strings
       with the actual hashes. */
    auto scratchPath = !status.known ? make_fallback_path(output_name)
                       : !needs_hash_rewrite()
                           /* Can always use original path in sandbox */
                           ? status.known->path
                           : !status.known->isPresent()
                                 /* If path doesn't yet exist can just use it */
                                 ? status.known->path
                                 : build_mode != bmRepair && !status.known->isValid()
                                       /* If we aren't repairing we'll delete a corrupted path, so
                                          we can use original path */
                                       ? status.known->path
                                       : /* If we are repairing or the path is totally valid, we'll
                                            need to use a temporary path */
                                       make_fallback_path(status.known->path);
    scratchOutputs.insert_or_assign(output_name, scratchPath);

    /* Substitute output placeholders with the scratch output paths.
       We'll use during the build. */
    input_rewrites[hash_placeholder(output_name)] = store.printStorePath(scratchPath);

    /* Additional tasks if we know the final path a priori. */
    if (!status.known)
      continue;
    auto fixedFinalPath = status.known->path;

    /* Additional tasks if the final and scratch are both known and
       differ. */
    if (fixedFinalPath == scratchPath)
      continue;

    /* Ensure scratch path is ours to use. */
    delete_path(store.printStorePath(scratchPath));

    /* Rewrite and unrewrite paths */
    {
      std::string h1{fixedFinalPath.hash_part()};
      std::string h2{scratchPath.hash_part()};
      input_rewrites[h1] = h2;
    }

    redirectedOutputs.insert_or_assign(std::move(fixedFinalPath), std::move(scratchPath));
  }

  /* Construct the environment passed to the builder. */
  init_env();

  prepare_sandbox();

  if (needs_hash_rewrite() && path_exists(home_dir))
    throw Error("home directory '%1%' exists; please remove it to assure purity of builds without "
                "sandboxing",
                home_dir);

  /* Fire up a Nix daemon to process recursive Nix calls from the
     builder. */
  if (drv_options.getRequiredSystemFeatures(drv).count("recursive-nix"))
    start_daemon();

  /* Run the builder. */
  printMsg(lvl_chatty, "executing builder '%1%'", drv.builder);
  printMsg(lvl_chatty, "using builder args '%1%'", concat_strings_sep(" ", drv.args));
  for (auto& i : drv.env)
    printMsg(lvl_vomit, "setting builder env variable '%1%'='%2%'", i.first, i.second);

  /* Create the log file. */
  misc_methods->openLogFile();

  /* Create a pseudoterminal to get the output of the builder. */
  builder_out = posix_openpt(O_RDWR | O_NOCTTY);
  if (!builder_out)
    throw sys_error_t("opening pseudoterminal master");

  std::string slave_name = get_pts_name(builder_out.get());

  if (buildUser) {
    if (chmod(slave_name.c_str(), 0600))
      throw sys_error_t("changing mode of pseudoterminal slave");

    if (chown(slave_name.c_str(), buildUser->getUID(), 0))
      throw sys_error_t("changing owner of pseudoterminal slave");
  }
#ifdef __APPLE__
  else {
    if (grantpt(builder_out.get()))
      throw sys_error_t("granting access to pseudoterminal slave");
  }
#endif

  if (unlockpt(builder_out.get()))
    throw sys_error_t("unlocking pseudoterminal");

  buildResult.start_time = time(0);

  /* Start a child process to build the derivation. */
  start_child();

  pid.set_separate_pg(true);

  /* Register a callback to propagate SIGTSTP to the child process group.
     This allows suspending builds with Ctrl+Z. When the parent receives
     SIGTSTP (handled in the signal handler thread), this callback sends
     SIGTSTP to the child's process group. The child will receive SIGCONT
     automatically when the parent is resumed. */
  suspendCallback = create_suspend_callback([this]() {
    if (pid != -1) {
      /* Send SIGTSTP to the child's process group */
      ::kill(-pid, SIGTSTP);
    }
  });

  /* Make the build visible to `nix ps`. */
  if (auto tracker = dynamic_cast<TrackActiveBuildsStore*>(&store))
    activeBuildHandle.emplace(tracker->buildStarted(get_active_build()));

  process_sandbox_setup_messages();

  return builder_out.get();
}

ActiveBuild derivation_builder_impl_t::get_active_build() {
  return {
      .nix_pid = getpid(),
      .client_pid = std::nullopt, // FIXME
      .clientUid = std::nullopt,  // FIXME
      .main_pid = pid,
      .mainUser = UserInfo::fromUid(buildUser ? buildUser->getUID() : getuid()),
      .start_time = buildResult.start_time,
      .derivation = drv_path,
  };
}

PathsInChroot derivation_builder_impl_t::get_paths_in_sandbox() {
  /* Allow a user-configurable set of directories from the
     host file system. */
  PathsInChroot paths_in_chroot = defaultPathsInChroot;

  for (auto& p : paths_in_chroot)
    if (!p.second.optional && !maybe_lstat(p.second.source))
      throw sys_error_t(
          "path '%s' is configured as part of the `sandbox-paths` option, but is inaccessible",
          p.second.source);

  if (has_prefix(store.store_dir, tmp_dir_in_sandbox())) {
    throw Error("`sandbox-build-dir` must not contain the storeDir");
  }
  paths_in_chroot[tmp_dir_in_sandbox()] = {.source = tmp_dir};

  path_set_t allowed_paths = settings.allowedImpureHostPrefixes;

  /* This works like the above, except on a per-derivation level */
  auto impure_paths = drv_options.impureHostDeps;

  for (auto& i : impure_paths) {
    bool found = false;
    /* Note: we're not resolving symlinks here to prevent
       giving a non-root user info about inaccessible
       files. */
    Path canonI = canon_path(i);
    /* If only we had a trie to do this more efficiently :) luckily, these are generally going to be
     * pretty small */
    for (auto& a : allowed_paths) {
      Path canonA = canon_path(a);
      if (is_dir_or_in_dir(canonI, canonA)) {
        found = true;
        break;
      }
    }
    if (!found)
      throw Error(
          "derivation '%s' requested impure path '%s', but it was not in allowed-impure-host-deps",
          store.printStorePath(drv_path), i);

    /* Allow files in drv_options.impureHostDeps to be missing; e.g.
       macOS 11+ has no /usr/lib/libSystem*.dylib */
    paths_in_chroot[i] = {i, true};
  }

  if (settings.preBuildHook != "") {
    printMsg(lvl_chatty, "executing pre-build hook '%1%'", settings.preBuildHook);

    enum build_hook_state_t { st_begin, st_extra_chroot_dirs };

    auto state = st_begin;
    auto lines = run_program(settings.preBuildHook, false, get_pre_build_hook_args());
    auto last_pos = std::string::size_type{0};
    for (auto nl_pos = lines.find('\n'); nl_pos != std::string::npos;
         nl_pos = lines.find('\n', last_pos)) {
      auto line = lines.substr(last_pos, nl_pos - last_pos);
      last_pos = nl_pos + 1;
      if (state == st_begin) {
        if (line == "extra-sandbox-paths" || line == "extra-chroot-dirs") {
          state = st_extra_chroot_dirs;
        } else {
          throw Error("unknown pre-build hook command '%1%'", line);
        }
      } else if (state == st_extra_chroot_dirs) {
        if (line == "") {
          state = st_begin;
        } else {
          auto p = line.find('=');
          if (p == std::string::npos)
            paths_in_chroot[line] = {.source = line};
          else
            paths_in_chroot[line.substr(0, p)] = {.source = line.substr(p + 1)};
        }
      }
    }
  }

  return paths_in_chroot;
}

void derivation_builder_impl_t::prepare_sandbox() {
  if (drv_options.useUidRange(drv))
    throw Error("feature 'uid-range' is not supported on this platform");
}

void derivation_builder_impl_t::open_slave() {
  std::string slave_name = get_pts_name(builder_out.get());

  auto_close_fd_t builder_out = open(slave_name.c_str(), O_RDWR | O_NOCTTY);
  if (!builder_out)
    throw sys_error_t("opening pseudoterminal slave");

  // Put the pt into raw mode to prevent \n -> \r\n translation.
  struct termios term;
  if (tcgetattr(builder_out.get(), &term))
    throw sys_error_t("getting pseudoterminal attributes");

  cfmakeraw(&term);

  if (tcsetattr(builder_out.get(), TCSANOW, &term))
    throw sys_error_t("putting pseudoterminal into raw mode");

  if (dup2(builder_out.get(), STDERR_FILENO) == -1)
    throw sys_error_t("cannot pipe standard error into log file");
}

#if NIX_WITH_AWS_AUTH
std::optional<AwsCredentials> derivation_builder_impl_t::preResolveAwsCredentials() {
  if (drv.isBuiltin() && drv.builder == "builtin:fetchurl") {
    auto url = drv.env.find("url");
    if (url != drv.env.end()) {
      try {
        auto parsed_url = parse_url(url->second);
        if (parsed_url.scheme() == "s3") {
          debug("Pre-resolving AWS credentials for S3 URL in builtin:fetchurl");
          auto s3Url = ParsedS3URL::parse(parsed_url);

          // Use the preResolveAwsCredentials from aws-creds
          auto credentials = getAwsCredentialsProvider()->getCredentials(s3Url);
          debug("Successfully pre-resolved AWS credentials in parent process");
          return credentials;
        }
      } catch (const std::exception& e) {
        debug("Error pre-resolving S3 credentials: %s", e.what());
      }
    }
  }
  return std::nullopt;
}
#endif

void derivation_builder_impl_t::start_child() {
  run_child_args_t args{
#if NIX_WITH_AWS_AUTH
      .awsCredentials = preResolveAwsCredentials(),
#endif
  };

  pid = start_process([this, args = std::move(args)]() {
    open_slave();
    run_child(std::move(args));
  });
}

void derivation_builder_impl_t::process_sandbox_setup_messages() {
  std::vector<std::string> msgs;
  while (true) {
    std::string msg = [&]() {
      try {
        return read_line(builder_out.get());
      } catch (Error& e) {
        auto status = pid.wait();
        e.add_trace({},
                    "while waiting for the build environment for '%s' to initialize (%s, previous "
                    "messages: %s)",
                    store.printStorePath(drv_path), status_to_string(status),
                    concat_strings_sep("\n", msgs));
        throw;
      }
    }();
    if (msg.substr(0, 1) == "\2")
      break;
    if (msg.substr(0, 1) == "\1") {
      fd_source_t source(builder_out.get());
      auto ex = read_error(source);
      ex.add_trace({}, "while setting up the build environment");
      throw ex;
    }
    debug("sandbox setup: " + msg);
    msgs.push_back(std::move(msg));
  }
}

void derivation_builder_impl_t::init_env() {
  env.clear();

  /* Most shells initialise PATH to some default (/bin:/usr/bin:...) when
     PATH is not set.  We don't want this, so we fill it in with some dummy
     value. */
  env["PATH"] = "/path-not-set";

  /* Set HOME to a non-existing path to prevent certain programs from using
     /etc/passwd (or NIS, or whatever) to locate the home directory (for
     example, wget looks for ~/.wgetrc).  I.e., these tools use /etc/passwd
     if HOME is not set, but they will just assume that the settings file
     they are looking for does not exist if HOME is set but points to some
     non-existing path. */
  env["HOME"] = home_dir;

  /* Tell the builder where the Nix store is.  Usually they
     shouldn't care, but this is useful for purity checking (e.g.,
     the compiler or linker might only want to accept paths to files
     in the store or in the build directory). */
  env["NIX_STORE"] = store.store_dir;

  /* The maximum number of cores to utilize for parallel building. */
  env["NIX_BUILD_CORES"] =
      fmt("%d", settings.build_cores ? settings.build_cores : settings.getDefaultCores());

  /* Write the final environment. Note that this is intentionally
     *not* `drv.env`, because we've desugared things like like
     "passAFile", "expandReferencesGraph", structured attrs, etc. */
  for (const auto& [name, info] : desugaredEnv.variables) {
    env[name] = info.prependBuildDirectory ? tmp_dir_in_sandbox() + "/" + info.value : info.value;
  }

  /* Add extra files, similar to `finalEnv` */
  for (const auto& [file_name, value] : desugaredEnv.extraFiles) {
    write_builder_file(file_name, rewrite_strings(value, input_rewrites));
  }

  /* For convenience, set an environment pointing to the top build
     directory. */
  env["NIX_BUILD_TOP"] = tmp_dir_in_sandbox();

  /* Also set TMPDIR and variants to point to this directory. */
  env["TMPDIR"] = env["TEMPDIR"] = env["TMP"] = env["TEMP"] = tmp_dir_in_sandbox();

  /* Explicitly set PWD to prevent problems with chroot builds.  In
     particular, dietlibc cannot figure out the cwd because the
     inode of the current directory doesn't appear in .. (because
     getdents returns the inode of the mount point). */
  env["PWD"] = tmp_dir_in_sandbox();

  /* Compatibility hack with Nix <= 0.7: if this is a fixed-output
     derivation, tell the builder, so that for instance `fetchurl'
     can skip checking the output.  On older Nixes, this environment
     variable won't be set, so `fetchurl' will do the check. */
  if (derivation_type.isFixed())
    env["NIX_OUTPUT_CHECKED"] = "1";

  /* *Only* if this is a fixed-output derivation, propagate the
     values of the environment variables specified in the
     `impureEnvVars' attribute to the builder.  This allows for
     instance environment variables for proxy configuration such as
     `http_proxy' to be easily passed to downloaders like
     `fetchurl'.  Passing such environment variables from the caller
     to the builder is generally impure, but the output of
     fixed-output derivations is by definition pure (since we
     already know the cryptographic hash of the output). */
  if (!derivation_type.isSandboxed()) {
    auto& impure_env = settings.impure_env.get();
    if (!impure_env.empty())
      experimental_feature_settings.require(xp_t::configurable_impure_env);

    for (auto& i : drv_options.impureEnvVars) {
      auto env_var = impure_env.find(i);
      if (env_var != impure_env.end()) {
        env[i] = env_var->second;
      } else {
        env[i] = get_env(i).value_or("");
      }
    }
  }

  /* Currently structured log messages piggyback on stderr, but we
     may change that in the future. So tell the builder which file
     descriptor to use for that. */
  env["NIX_LOG_FD"] = "2";

  /* Trigger colored output in various tools. */
  env["TERM"] = "xterm-256color";
}

void derivation_builder_impl_t::start_daemon() {
  experimental_feature_settings.require(xp_t::recursive_nix);

  auto store = make_restricted_store(
      [&] {
        auto config = make_ref<LocalStore::config_t>(*this->store.config);
        config->pathInfoCacheSize = 0;
        config->stateDir = "/no-such-path";
        config->logDir = "/no-such-path";
        return config;
      }(),
      ref<LocalStore>(std::dynamic_pointer_cast<LocalStore>(this->store.shared_from_this())),
      *this);

  addedPaths.clear();

  auto socket_name = ".nix-socket";
  Path socket_path = tmp_dir + "/" + socket_name;
  env["NIX_REMOTE"] = "unix://" + tmp_dir_in_sandbox() + "/" + socket_name;

  daemonSocket = create_unix_domain_socket(socket_path, 0600);

  chown_to_builder(socket_path);

  daemonThread = std::thread([this, store]() {
    while (true) {
      /* Accept a connection. */
      struct sockaddr_un remoteAddr;
      socklen_t remoteAddrLen = sizeof(remoteAddr);

      auto_close_fd_t remote =
          accept(daemonSocket.get(), (struct sockaddr*)&remoteAddr, &remoteAddrLen);
      if (!remote) {
        if (errno == EINTR || errno == EAGAIN)
          continue;
        if (errno == EINVAL || errno == ECONNABORTED)
          break;
        throw sys_error_t("accepting connection");
      }

      unix::close_on_exec(remote.get());

      debug("received daemon connection");

      auto worker_thread = std::thread([store, remote{std::move(remote)}]() {
        try {
          daemon::process_connection(store, fd_source_t(remote.get()), fd_sink_t(remote.get()),
                                     NotTrusted, daemon::Recursive);
          debug("terminated daemon connection");
        } catch (const Interrupted&) {
          debug("interrupted daemon connection");
        } catch (SystemError&) {
          ignore_exception_except_interrupt();
        }
      });

      daemonWorkerThreads.push_back(std::move(worker_thread));
    }

    debug("daemon shutting down");
  });
}

void derivation_builder_impl_t::stop_daemon() {
  if (daemonSocket && shutdown(daemonSocket.get(), SHUT_RDWR) == -1) {
    // According to the POSIX standard, the 'shutdown' function should
    // return an ENOTCONN error when attempting to shut down a socket that
    // hasn't been connected yet. This situation occurs when the 'accept'
    // function is called on a socket without any accepted connections,
    // leaving the socket unconnected. While Linux doesn't seem to produce
    // an error for sockets that have only been accepted, more
    // POSIX-compliant operating systems like OpenBSD, macOS, and others do
    // return the ENOTCONN error. Therefore, we handle this error here to
    // avoid raising an exception for compliant behaviour.
    if (errno == ENOTCONN) {
      daemonSocket.close();
    } else {
      throw sys_error_t("shutting down daemon socket");
    }
  }

  if (daemonThread.joinable())
    daemonThread.join();

  // FIXME: should prune worker threads more quickly.
  // FIXME: shutdown the client socket to speed up worker termination.
  for (auto& thread : daemonWorkerThreads)
    thread.join();
  daemonWorkerThreads.clear();

  // release the socket.
  daemonSocket.close();
}

void derivation_builder_impl_t::add_dependency_impl(const store_path_t& path) {
  addedPaths.insert(path);
}

void derivation_builder_impl_t::chown_to_builder(const Path& path) {
  if (!buildUser)
    return;
  if (chown(path.c_str(), buildUser->getUID(), buildUser->getGID()) == -1)
    throw sys_error_t("cannot change ownership of '%1%'", path);
}

void derivation_builder_impl_t::chown_to_builder(int fd, const Path& path) {
  if (!buildUser)
    return;
  if (fchown(fd, buildUser->getUID(), buildUser->getGID()) == -1)
    throw sys_error_t("cannot change ownership of file '%1%'", path);
}

void derivation_builder_impl_t::write_builder_file(const std::string& name,
                                                   std::string_view contents) {
  auto path = std::filesystem::path(tmp_dir) / name;
  auto_close_fd_t fd{openat(tmpDirFd.get(), name.c_str(),
                            O_WRONLY | O_TRUNC | O_CREAT | O_CLOEXEC | O_EXCL | O_NOFOLLOW, 0666)};
  if (!fd)
    throw sys_error_t("creating file %s", path);
  write_file(fd, path, contents);
  chown_to_builder(fd.get(), path);
}

void derivation_builder_impl_t::run_child(run_child_args_t args) {
  /* Warning: in the child we should absolutely not make any SQLite
     calls! */

  bool send_exception = true;

  try { /* child */

    common_child_init();

    /* Make the contents of netrc and the CA certificate bundle
       available to builtin:fetchurl (which may run under a
       different uid and/or in a sandbox). */
    BuiltinBuilderContext ctx{
        .drv = drv,
        .tmp_dir_in_sandbox = tmp_dir_in_sandbox(),
#if NIX_WITH_AWS_AUTH
        .awsCredentials = args.awsCredentials,
#endif
    };

    if (drv.isBuiltin() && drv.builder == "builtin:fetchurl") {
      try {
        ctx.netrcData = read_file(settings.netrcFile);
      } catch (SystemError&) {
      }

      try {
        ctx.caFileData = read_file(settings.ca_file);
      } catch (SystemError&) {
      }
    }

    enter_chroot();

    if (chdir(tmp_dir_in_sandbox().c_str()) == -1)
      throw sys_error_t("changing into '%1%'", tmp_dir);

    /* Close all other file descriptors. */
    unix::close_extra_f_ds();

    /* Disable core dumps by default. */
    struct rlimit limit = {0, RLIM_INFINITY};
    setrlimit(RLIMIT_CORE, &limit);

    // FIXME: set other limits to deterministic values?

    set_user();

    /* Indicate that we managed to set up the build environment. */
    write_full(STDERR_FILENO, std::string("\2\n"));

    send_exception = false;

    /* If this is a builtin builder, call it now. This should not return. */
    if (drv.isBuiltin()) {
      try {
        logger = make_json_logger(get_standard_error());

        for (auto& e : drv.outputs)
          ctx.outputs.insert_or_assign(e.first, store.printStorePath(scratchOutputs.at(e.first)));

        std::string builtin_name = drv.builder.substr(8);
        assert(RegisterBuiltinBuilder::builtinBuilders);
        if (auto builtin = get(RegisterBuiltinBuilder::builtinBuilders(), builtin_name))
          (*builtin)(ctx);
        else
          throw Error("unsupported builtin builder '%1%'", builtin_name);
        _exit(0);
      } catch (std::exception& e) {
        write_full(STDERR_FILENO, e.what() + std::string("\n"));
        _exit(1);
      }
    }

    /* It's not a builtin builder, so execute the program. */

    strings_t args;
    args.push_back(std::string(base_name_of(drv.builder)));

    for (auto& i : drv.args)
      args.push_back(rewrite_strings(i, input_rewrites));

    strings_t env_strs;
    for (auto& i : env)
      env_strs.push_back(rewrite_strings(i.first + "=" + i.second, input_rewrites));

    exec_builder(args, env_strs);

    throw sys_error_t("executing '%1%'", drv.builder);

  } catch (...) {
    handle_child_exception(send_exception);
    _exit(1);
  }
}

void derivation_builder_impl_t::set_user() {
  /* If we are running in `build-users' mode, then switch to the
     user we allocated above.  Make sure that we drop all root
     privileges.  Note that above we have closed all file
     descriptors except std*, so that's safe.  Also note that
     setuid() when run as root sets the real, effective and
     saved UIDs. */
  if (buildUser) {
    /* Preserve supplementary groups of the build user, to allow
       admins to specify groups such as "kvm".  */
    auto gids = buildUser->getSupplementaryGIDs();
    if (setgroups(gids.size(), gids.data()) == -1)
      throw sys_error_t("cannot set supplementary groups of build user");

    if (setgid(buildUser->getGID()) == -1 || getgid() != buildUser->getGID() ||
        getegid() != buildUser->getGID())
      throw sys_error_t("setgid failed");

    if (setuid(buildUser->getUID()) == -1 || getuid() != buildUser->getUID() ||
        geteuid() != buildUser->getUID())
      throw sys_error_t("setuid failed");
  }
}

void derivation_builder_impl_t::exec_builder(const strings_t& args, const strings_t& env_strs) {
  execve(drv.builder.c_str(), strings_to_char_ptrs(args).data(),
         strings_to_char_ptrs(env_strs).data());
}

SingleDrvOutputs derivation_builder_impl_t::register_outputs() {
  std::map<std::string, valid_path_info_t> infos;

  /* Set of inodes seen during calls to canonicalise_path_meta_data()
     for this build's outputs.  This needs to be shared between
     outputs to allow hard links between outputs. */
  InodesSeen inodes_seen;

  /* The paths that can be referenced are the input closures, the
     output paths, and any paths that have been built via recursive
     Nix calls. */
  store_path_set_t referenceable_paths;
  for (auto& p : inputPaths)
    referenceable_paths.insert(p);
  for (auto& i : scratchOutputs)
    referenceable_paths.insert(i.second);
  for (auto& p : addedPaths)
    referenceable_paths.insert(p);

  /* Check whether the output paths were created, and make all
     output paths read-only.  Then get the references of each output (that we
     might need to register), so we can topologically sort them. For the ones
     that are most definitely already installed, we just store their final
     name so we can also use it in rewrites. */
  string_set_t outputs_to_sort;

  struct already_registered_t {
    store_path_t path;
  };

  struct perhaps_need_to_register_t {
    store_path_set_t refs;
    /**
     * References to other outputs. Built by looking up in
     * `scratchOutputsInverse`.
     */
    string_set_t other_outputs;
  };

  /* inverse map of scratchOutputs for efficient lookup */
  std::map<store_path_t, std::string> scratchOutputsInverse;
  for (auto& [output_name, path] : scratchOutputs)
    scratchOutputsInverse.insert_or_assign(path, output_name);

  std::map<std::string, std::variant<already_registered_t, perhaps_need_to_register_t>>
      outputReferencesIfUnregistered;
  std::map<std::string, struct stat> outputStats;
  for (auto& [output_name, _] : drv.outputs) {
    auto scratchOutput = get(scratchOutputs, output_name);
    assert(scratchOutput);
    auto actualPath = real_path_in_host(store.printStorePath(*scratchOutput));

    outputs_to_sort.insert(output_name);

    /* Updated wanted info to remove the outputs we definitely don't need to register */
    auto initialOutput = get(initialOutputs, output_name);
    assert(initialOutput);
    auto& initialInfo = *initialOutput;

    /* Don't register if already valid, and not checking */
    bool wanted = build_mode == bmCheck || !(initialInfo.known && initialInfo.known->isValid());
    if (!wanted) {
      outputReferencesIfUnregistered.insert_or_assign(
          output_name, already_registered_t{.path = initialInfo.known->path});
      continue;
    }

    auto optSt = maybe_lstat(actualPath.c_str());
    if (!optSt)
      throw build_error_t(build_result_t::Failure::OutputRejected,
                          "builder for '%s' failed to produce output path for output '%s' at '%s'",
                          store.printStorePath(drv_path), output_name, actualPath);
    struct stat& st = *optSt;

#ifndef __CYGWIN__
    /* Check that the output is not group or world writable, as
       that means that someone else can have interfered with the
       build.  Also, the output should be owned by the build
       user. */
    if ((!S_ISLNK(st.st_mode) && (st.st_mode & (S_IWGRP | S_IWOTH))) ||
        (buildUser && st.st_uid != buildUser->getUID()))
      throw build_error_t(
          build_result_t::Failure::OutputRejected,
          "suspicious ownership or permission on '%s' for output '%s'; rejecting this build output",
          actualPath, output_name);
#endif

    /* Canonicalise first.  This ensures that the path we're
       rewriting doesn't contain a hard link to /etc/shadow or
       something like that. */
    canonicalise_path_meta_data(actualPath,
                                buildUser ? std::optional(buildUser->getUIDRange()) : std::nullopt,
                                inodes_seen);

    bool discardReferences = false;
    if (auto udr = get(drv_options.unsafeDiscardReferences, output_name)) {
      discardReferences = *udr;
    }

    store_path_set_t references;
    if (discardReferences)
      debug("discarding references of output '%s'", output_name);
    else {
      debug("scanning for references for output '%s' in temp location '%s'", output_name,
            actualPath);

      /* Pass blank sink_t as we are not ready to hash data at this stage. */
      null_sink_t blank;
      references = scan_for_references(blank, actualPath, referenceable_paths);
    }

    string_set_t referencedOutputs;
    for (auto& r : references)
      if (auto* o = get(scratchOutputsInverse, r))
        referencedOutputs.insert(*o);

    outputReferencesIfUnregistered.insert_or_assign(output_name,
                                                    perhaps_need_to_register_t{
                                                        .refs = references,
                                                        .other_outputs = referencedOutputs,
                                                    });
    outputStats.insert_or_assign(output_name, std::move(st));
  }

  string_set_t empty_set;

  auto topo_sort_result =
      topoSort(outputs_to_sort, [&](const std::string& name) -> const string_set_t& {
        auto* orifu = get(outputReferencesIfUnregistered, name);
        if (!orifu)
          throw build_error_t(build_result_t::Failure::OutputRejected,
                              "no output reference for '%s' in build of '%s'", name,
                              store.printStorePath(drv_path));
        return std::visit(
            overloaded{
                /* Since we'll use the already installed versions of these, we
                   can treat them as leaves and ignore any references they
                   have. */
                [&](const already_registered_t&) -> const string_set_t& { return empty_set; },
                [&](const perhaps_need_to_register_t& refs) -> const string_set_t& {
                  return refs.other_outputs;
                },
            },
            *orifu);
      });

  auto sorted_output_names = std::visit(
      overloaded{
          [&](cycle_t<std::string>& cycle) -> std::vector<std::string> {
            // TODO with more -vvvv also show the temporary paths for manual inspection.
            throw build_error_t(
                build_result_t::Failure::OutputRejected,
                "cycle detected in build of '%s' in the references of output '%s' from output '%s'",
                store.printStorePath(drv_path), cycle.path, cycle.parent);
          },
          [](auto& sorted) { return sorted; }},
      topo_sort_result);

  std::reverse(sorted_output_names.begin(), sorted_output_names.end());

  OutputPathMap final_outputs;

  for (auto& output_name : sorted_output_names) {
    auto output = get(drv.outputs, output_name);
    auto scratchPath = get(scratchOutputs, output_name);
    assert(output && scratchPath);
    auto actualPath = real_path_in_host(store.printStorePath(*scratchPath));

    auto finish = [&](store_path_t finalStorePath) {
      /* store_t the final path */
      final_outputs.insert_or_assign(output_name, finalStorePath);
      /* The rewrite rule will be used in downstream outputs that refer to
         use. This is why the topological sort is essential to do first
         before this for loop. */
      if (*scratchPath != finalStorePath)
        outputRewrites[std::string{scratchPath->hash_part()}] =
            std::string{finalStorePath.hash_part()};
    };

    auto orifu = get(outputReferencesIfUnregistered, output_name);
    assert(orifu);

    std::optional<store_path_set_t> referencesOpt = std::visit(
        overloaded{
            [&](const already_registered_t& skippedFinalPath) -> std::optional<store_path_set_t> {
              finish(skippedFinalPath.path);
              return std::nullopt;
            },
            [&](const perhaps_need_to_register_t& r) -> std::optional<store_path_set_t> {
              return r.refs;
            },
        },
        *orifu);

    if (!referencesOpt)
      continue;
    auto references = *referencesOpt;

    auto rewriteOutput = [&](const string_map_t& rewrites) {
      /* Apply hash rewriting if necessary. */
      if (!rewrites.empty()) {
        debug("rewriting hashes in '%1%'; cross fingers", actualPath);

        /* #8113: Hash rewriting can break NAR lexical order.
         *
         * NAR format requires directory entries to be sorted lexically.
         * When we rewrite hashes in filenames, the sort order may change.
         * For example: "aaa-abc123" < "bbb-xyz789" but after rewriting
         * "aaa-xyz789" > "bbb-abc123".
         *
         * To fix this, we use a two-pass approach:
         * 1. First pass: rewrite hashes in the NAR stream
         * 2. Second pass: re-dump/restore to re-sort directory entries
         *
         * This guarantees correct sorting since dump_path always sorts entries.
         */
        auto source = sink_to_source([&](sink_t& next_sink) {
          RewritingSink rsink(rewrites, next_sink);
          dump_path(actualPath, rsink);
          rsink.flush();
        });
        Path tmp_path = actualPath + ".tmp";
        restore_path(tmp_path, *source);
        delete_path(actualPath);
        move_path(tmp_path, actualPath);

        /* Re-dump and restore to ensure NAR entries are properly sorted.
         * This fixes #8113 where hash rewriting can break lexical order
         * of directory entries, causing "NAR directory is not sorted" errors. */
        auto resort_source =
            sink_to_source([&](sink_t& next_sink) { dump_path(actualPath, next_sink); });
        Path resort_tmp_path = actualPath + ".resort.tmp";
        restore_path(resort_tmp_path, *resort_source);
        delete_path(actualPath);
        move_path(resort_tmp_path, actualPath);

#ifdef __APPLE__
        /* #6065: On aarch64-darwin, re-codesign binaries after hash rewriting.
         *
         * Apple Silicon (aarch64-darwin) requires all executables to be
         * properly code-signed. When we rewrite hashes in Mach-O binaries,
         * we invalidate any existing code signatures. The kernel will refuse
         * to execute binaries with invalid signatures, causing "Killed: 9"
         * or similar errors.
         *
         * We use `codesign -f -s -` to ad-hoc sign all Mach-O executables
         * after rewriting. The `-s -` means "ad-hoc signing" (no identity).
         * This is sufficient for local execution. */
        if (drv.platform == "aarch64-darwin" || drv.platform == "x86_64-darwin") {
          std::function<void(const Path&)> resignMachO = [&](const Path& path) {
            auto st = lstat(path);

            if (S_ISDIR(st.st_mode)) {
              for (auto& entry : read_directory(path)) {
                resignMachO(path + "/" + entry.name);
              }
            } else if (S_ISREG(st.st_mode) && (st.st_mode & S_IXUSR)) {
              /* Check if this looks like a Mach-O binary by reading magic */
              int fd = ::open(path.c_str(), O_RDONLY);
              if (fd >= 0) {
                uint32_t magic = 0;
                if (::read(fd, &magic, sizeof(magic)) == sizeof(magic)) {
                  /* Mach-O magic numbers (both endiannesses) */
                  constexpr uint32_t MH_MAGIC_64 = 0xfeedfacf;
                  constexpr uint32_t MH_CIGAM_64 = 0xcffaedfe;
                  constexpr uint32_t MH_MAGIC = 0xfeedface;
                  constexpr uint32_t MH_CIGAM = 0xcefaedfe;
                  constexpr uint32_t FAT_MAGIC = 0xcafebabe;
                  constexpr uint32_t FAT_CIGAM = 0xbebafeca;

                  if (magic == MH_MAGIC_64 || magic == MH_CIGAM_64 || magic == MH_MAGIC ||
                      magic == MH_CIGAM || magic == FAT_MAGIC || magic == FAT_CIGAM) {
                    ::close(fd);
                    debug("re-signing Mach-O binary after hash rewrite: %s", path);
                    /* Use ad-hoc signing. Ignore failures for non-signable files. */
                    run_program("codesign", true, {"-f", "-s", "-", path});
                    return;
                  }
                }
                ::close(fd);
              }
            }
          };

          try {
            resignMachO(actualPath);
          } catch (ExecError& e) {
            /* codesign failures are not fatal - some files may not be signable */
            debug("codesign warning: %s", e.what());
          }
        }
#endif

        /* FIXME: set proper permissions in restore_path() so
           we don't have to do another traversal. */
        canonicalise_path_meta_data(actualPath, {}, inodes_seen);
      }
    };

    auto rewriteRefs = [&]() -> store_references_t {
      /* In the CA case, we need the rewritten refs to calculate the
         final path, therefore we look for a *non-rewritten
         self-reference, and use a bool rather try to solve the
         computationally intractable fixed point. */
      store_references_t res{
          .self = false,
      };
      for (auto& r : references) {
        auto name = r.name();
        auto origHash = std::string{r.hash_part()};
        if (r == *scratchPath) {
          res.self = true;
        } else if (auto outputRewrite = get(outputRewrites, origHash)) {
          std::string new_ref = *outputRewrite;
          new_ref += '-';
          new_ref += name;
          res.others.insert(store_path_t{new_ref});
        } else {
          res.others.insert(r);
        }
      }
      return res;
    };

    auto newInfoFromCA =
        [&](const derivation_output_t::CAFloating outputHash) -> valid_path_info_t {
      auto st = get(outputStats, output_name);
      if (!st)
        throw build_error_t(build_result_t::Failure::OutputRejected,
                            "output path %1% without valid stats info", actualPath);
      if (outputHash.method.getFileIngestionMethod() == file_ingestion_method_t::flat) {
        /* The output path should be a regular file without execute permission. */
        if (!S_ISREG(st->st_mode) || (st->st_mode & S_IXUSR) != 0)
          throw build_error_t(
              build_result_t::Failure::OutputRejected,
              "output path '%1%' should be a non-executable regular file "
              "since recursive hashing is not enabled (one of outputHashMode={flat,text} is true)",
              actualPath);
      }
      rewriteOutput(outputRewrites);
      /* FIXME optimize and deduplicate with add_to_store */
      std::string oldHashPart{scratchPath->hash_part()};
      auto got = [&] {
        auto fim = outputHash.method.getFileIngestionMethod();
        switch (fim) {
          case file_ingestion_method_t::flat:
          case file_ingestion_method_t::nix_archive: {
            HashModuloSink caSink{outputHash.hash_algo, oldHashPart};
            auto fim = outputHash.method.getFileIngestionMethod();
            dump_path({get_fs_source_accessor(), canon_path_t(actualPath)}, caSink,
                      (file_serialisation_method_t)fim);
            return caSink.finish().hash;
          }
          case file_ingestion_method_t::git: {
            return git::dump_hash(outputHash.hash_algo,
                                  {get_fs_source_accessor(), canon_path_t(actualPath)})
                .hash;
          }
        }
        assert(false);
      }();

      auto newInfo0 = valid_path_info_t::makeFromCA(
          store, output_path_name(drv.name, output_name),
          ContentAddressWithReferences::fromParts(outputHash.method, std::move(got), rewriteRefs()),
          Hash::dummy);
      if (*scratchPath != newInfo0.path) {
        // If the path has some self-references, we need to rewrite
        // them.
        // (note that this doesn't invalidate the ca hash we calculated
        // above because it's computed *modulo the self-references*, so
        // it already takes this rewrite into account).
        rewriteOutput(string_map_t{{oldHashPart, std::string(newInfo0.path.hash_part())}});
      }

      {
        hash_result_t narHashAndSize =
            hash_path({get_fs_source_accessor(), canon_path_t(actualPath)},
                      file_serialisation_method_t::nix_archive, hash_algorithm_t::SHA256);
        newInfo0.nar_hash = narHashAndSize.hash;
        newInfo0.nar_size = narHashAndSize.num_bytes_digested;
      }

      assert(newInfo0.ca);
      return newInfo0;
    };

    valid_path_info_t newInfo = std::visit(
        overloaded{

            [&](const derivation_output_t::InputAddressed& output) {
              /* input-addressed case */
              auto requiredFinalPath = output.path;
              /* Preemptively add rewrite rule for final hash, as that is
                 what the NAR hash will use rather than normalized-self references */
              if (*scratchPath != requiredFinalPath)
                outputRewrites.insert_or_assign(std::string{scratchPath->hash_part()},
                                                std::string{requiredFinalPath.hash_part()});
              rewriteOutput(outputRewrites);
              hash_result_t narHashAndSize =
                  hash_path({get_fs_source_accessor(), canon_path_t(actualPath)},
                            file_serialisation_method_t::nix_archive, hash_algorithm_t::SHA256);
              valid_path_info_t newInfo0{requiredFinalPath, {store, narHashAndSize.hash}};
              newInfo0.nar_size = narHashAndSize.num_bytes_digested;
              auto refs = rewriteRefs();
              newInfo0.references = std::move(refs.others);
              if (refs.self)
                newInfo0.references.insert(newInfo0.path);
              return newInfo0;
            },

            [&](const derivation_output_t::CAFixed& dof) {
              auto& wanted = dof.ca.hash;

              // Replace the output by a fresh copy of itself to make sure
              // that there's no stale file descriptor pointing to it
              Path tmpOutput = actualPath + ".tmp";
              copy_file(std::filesystem::path(actualPath), std::filesystem::path(tmpOutput), true);

              std::filesystem::rename(tmpOutput, actualPath);

              return newInfoFromCA(derivation_output_t::CAFloating{
                  .method = dof.ca.method,
                  .hash_algo = wanted.algo(),
              });
            },

            [&](const derivation_output_t::CAFloating& dof) { return newInfoFromCA(dof); },

            [&](const derivation_output_t::Deferred&) -> valid_path_info_t {
              // No derivation should reach that point without having been
              // rewritten first
              assert(false);
            },

            [&](const derivation_output_t::Impure& doi) {
              return newInfoFromCA(derivation_output_t::CAFloating{
                  .method = doi.method,
                  .hash_algo = doi.hash_algo,
              });
            },

        },
        output->raw);

    /* FIXME: set proper permissions in restore_path() so
        we don't have to do another traversal. */
    canonicalise_path_meta_data(actualPath, {}, inodes_seen);

    /* Calculate where we'll move the output files. In the checking case we
       will leave leave them where they are, for now, rather than move to
       their usual "final destination" */
    auto finalDestPath = store.printStorePath(newInfo.path);

    /* Add a temp root for the output path to prevent GC from deleting
       it between when we move/create it and when we register it as
       valid. This fixes a race condition where GC could delete the
       path during the registration window. */
    store.addTempRoot(newInfo.path);

    /* lock_t final output path, if not already locked. This happens with
       floating CA derivations and hash-mismatching fixed-output
       derivations. */
    PathLocks dynamicOutputLock;
    dynamicOutputLock.setDeletion(true);
    auto optFixedPath = output->path(store, drv.name, output_name);
    if (!optFixedPath || store.printStorePath(*optFixedPath) != finalDestPath) {
      assert(newInfo.ca);
      dynamicOutputLock.lockPaths({store.toRealPath(finalDestPath)});
    }

    /* Move files, if needed */
    if (store.toRealPath(finalDestPath) != actualPath) {
      if (build_mode == bmRepair) {
        /* Path already exists, need to replace it */
        replace_valid_path(store.toRealPath(finalDestPath), actualPath);
      } else if (build_mode == bmCheck) {
        /* Path already exists, and we want to compare, so we leave out
           new path in place. */
      } else if (store.isValidPath(newInfo.path)) {
        /* Path already exists because CA path produced by something
           else. No moving needed. */
        assert(newInfo.ca);
        /* Can delete our scratch copy now. */
        delete_path(actualPath);
      } else {
        auto dest_path = store.toRealPath(finalDestPath);
        delete_path(dest_path);
        move_path(actualPath, dest_path);
      }
    }

    if (build_mode == bmCheck) {
      /* Check against already registered outputs */

      if (store.isValidPath(newInfo.path)) {
        valid_path_info_t oldInfo(*store.queryPathInfo(newInfo.path));
        if (newInfo.nar_hash != oldInfo.nar_hash) {
          if (settings.runDiffHook || settings.keep_failed) {
            auto dst = store.toRealPath(finalDestPath + ".check");
            delete_path(dst);
            move_path(actualPath, dst);

            handle_diff_hook(buildUser ? buildUser->getUID() : getuid(),
                             buildUser ? buildUser->getGID() : getgid(), finalDestPath, dst,
                             store.printStorePath(drv_path), tmp_dir);

            throw not_deterministic_t(
                "derivation '%s' may not be deterministic: output '%s' differs from '%s'",
                store.printStorePath(drv_path), store.toRealPath(finalDestPath), dst);
          } else
            throw not_deterministic_t(
                "derivation '%s' may not be deterministic: output '%s' differs",
                store.printStorePath(drv_path), store.toRealPath(finalDestPath));
        }

        /* Since we verified the build, it's now ultimately trusted. */
        if (!oldInfo.ultimate) {
          oldInfo.ultimate = true;
          store.signPathInfo(oldInfo);
          store.registerValidPaths({{oldInfo.path, oldInfo}});
        }
      }
    } else {
      /* do tasks relating to registering these outputs */

      /* For debugging, print out the referenced and unreferenced paths. */
      for (auto& i : inputPaths) {
        if (references.count(i))
          debug("referenced input: '%1%'", store.printStorePath(i));
        else
          debug("unreferenced input: '%1%'", store.printStorePath(i));
      }

      if (!store.isValidPath(newInfo.path))
        store.optimisePath(store.toRealPath(finalDestPath),
                           NoRepair); // FIXME: combine with scanForReferences()

      newInfo.deriver = drv_path;
      newInfo.ultimate = true;
      store.signPathInfo(newInfo);

      finish(newInfo.path);

      /* If it's a CA path, register it right away. This is necessary if it
         isn't statically known so that we can safely unlock the path before
         the next iteration

         This is also good so that if a fixed-output produces the
         wrong path, we still store the result (just don't consider
         the derivation sucessful, so if someone fixes the problem by
         just changing the wanted hash, the redownload (or whateer
         possibly quite slow thing it was) doesn't have to be done
         again. */
      if (newInfo.ca)
        store.registerValidPaths({{newInfo.path, newInfo}});
    }

    /* Do this in both the check and non-check cases, because we
       want `check_outputs` below to work, which needs these path
       infos. */
    infos.emplace(output_name, std::move(newInfo));
  }

  /* Apply output checks. This includes checking of the wanted vs got
     hash of fixed-outputs. */
  check_outputs(store, drv_path, drv.outputs, drv_options.output_checks, infos, *act);

  if (build_mode == bmCheck) {
    return {};
  }

  /* Register each output path as valid, and register the sets of
     paths referenced by each of them.  If there are cycles in the
     outputs, this will fail. */
  {
    ValidPathInfos infos2;
    for (auto& [output_name, newInfo] : infos) {
      infos2.insert_or_assign(newInfo.path, newInfo);
    }
    store.registerValidPaths(infos2);
  }

  /* If we made it this far, we are sure the output matches the
     derivation That means it's safe to link the derivation to the
     output hash. We must do that for floating CA derivations, which
     otherwise couldn't be cached, but it's fine to do in all cases.
     */
  SingleDrvOutputs built_outputs;

  for (auto& [output_name, newInfo] : infos) {
    auto oldinfo = get(initialOutputs, output_name);
    assert(oldinfo);
    auto thisRealisation = realisation_t{
        {
            .out_path = newInfo.path,
        },
        DrvOutput{oldinfo->outputHash, output_name},
    };
    if (experimental_feature_settings.is_enabled(xp_t::ca_derivations) && !drv.type().is_impure()) {
      store.signRealisation(thisRealisation);
      store.register_drv_output(thisRealisation);
    }
    built_outputs.emplace(output_name, thisRealisation);
  }

  return built_outputs;
}

void derivation_builder_impl_t::cleanup_build(bool force) {
  if (force) {
    /* Delete unused redirected outputs (when doing hash rewriting). */
    for (auto& i : redirectedOutputs)
      delete_path(store.toRealPath(i.second));
  }

  if (topTmpDir != "") {
    /* As an extra precaution, even in the event of `delete_path` failing to
     * clean up, the `tmp_dir` will be chowned as if we were to move
     * it inside the Nix store.
     *
     * This hardens against an attack which smuggles a file descriptor
     * to make use of the temporary directory.
     */
    chmod(topTmpDir.c_str(), 0000);

    /* Don't keep temporary directories for builtins because they
       might have privileged stuff (like a copy of netrc). */
    if (settings.keep_failed && !force && !drv.isBuiltin()) {
      printError("note: keeping build directory '%s'", tmp_dir);
      chmod(topTmpDir.c_str(), 0755);
      chmod(tmp_dir.c_str(), 0755);
    } else
      delete_path(topTmpDir);
    topTmpDir = "";
    tmp_dir = "";
  }
}

store_path_t derivation_builder_impl_t::make_fallback_path(OutputNameView output_name) {
  // This is a bogus path type, constructed this way to ensure that it doesn't collide with any
  // other store path See doc/manual/source/protocols/store-path.md for details
  // TODO: We may want to separate the responsibilities of constructing the path fingerprint and of
  // actually doing the hashing
  auto path_type =
      "rewrite:" + std::string(drv_path.to_string()) + ":name:" + std::string(output_name);
  return store.makeStorePath(path_type,
                             // pass an all-zeroes hash
                             Hash(hash_algorithm_t::SHA256),
                             output_path_name(drv.name, output_name));
}

store_path_t derivation_builder_impl_t::make_fallback_path(const store_path_t& path) {
  // This is a bogus path type, constructed this way to ensure that it doesn't collide with any
  // other store path See doc/manual/source/protocols/store-path.md for details
  auto path_type =
      "rewrite:" + std::string(drv_path.to_string()) + ":" + std::string(path.to_string());
  return store.makeStorePath(path_type,
                             // pass an all-zeroes hash
                             Hash(hash_algorithm_t::SHA256), path.name());
}

} // namespace nix

// FIXME: do this properly
#include "chroot-derivation-builder.inc"
#include "darwin-derivation-builder.inc"
#include "external-derivation-builder.inc"
#include "linux-derivation-builder.inc"
#include "wasi-derivation-builder.inc"

namespace nix {

std::unique_ptr<DerivationBuilder>
make_derivation_builder(LocalStore& store, std::unique_ptr<DerivationBuilderCallbacks> misc_methods,
                        DerivationBuilderParams params) {
  bool useSandbox = false;

  /* Are we doing a sandboxed build? */
  {
    if (settings.sandboxMode == smEnabled) {
      if (params.drv_options.noChroot)
        throw Error("derivation '%s' has '__noChroot' set, "
                    "but that's not allowed when 'sandbox' is 'true'",
                    store.printStorePath(params.drv_path));
#ifdef __APPLE__
      if (params.drv_options.additionalSandboxProfile != "")
        throw Error("derivation '%s' specifies a sandbox profile, "
                    "but this is only allowed when 'sandbox' is 'relaxed'",
                    store.printStorePath(params.drv_path));
#endif
      useSandbox = true;
    } else if (settings.sandboxMode == smDisabled)
      useSandbox = false;
    else if (settings.sandboxMode == smRelaxed)
      // FIXME: cache derivationType
      useSandbox = params.drv.type().isSandboxed() && !params.drv_options.noChroot;
  }

  if (params.drv.platform == "wasm32-wasip1")
    return std::make_unique<wasi_derivation_builder_t>(store, std::move(misc_methods),
                                                       std::move(params));

  if (store.store_dir != store.config->real_store_dir.get()) {
#ifdef __linux__
    useSandbox = true;
#else
    throw Error("building using a diverted store is not supported on this platform");
#endif
  }

#ifdef __linux__
  if (useSandbox && !mount_and_pid_namespaces_supported()) {
    if (!settings.sandboxFallback)
      throw Error("this system does not support the kernel namespaces that are required for "
                  "sandboxing; use '--no-sandbox' to disable sandboxing");
    debug("auto-disabling sandboxing because the prerequisite namespaces are not available");
    useSandbox = false;
  }

#endif

  if (!useSandbox && params.drv_options.useUidRange(params.drv))
    throw Error("feature 'uid-range' is only supported in sandboxed builds");

#ifdef __APPLE__
  return std::make_unique<DarwinDerivationBuilder>(store, std::move(misc_methods),
                                                   std::move(params), useSandbox);
#elif defined(__linux__)
  if (useSandbox)
    return std::make_unique<chroot_linux_derivation_builder_t>(store, std::move(misc_methods),
                                                               std::move(params));

  return std::make_unique<linux_derivation_builder_t>(store, std::move(misc_methods),
                                                      std::move(params));
#else
  if (useSandbox)
    throw Error("sandboxing builds is not supported on this platform");

  return std::make_unique<derivation_builder_impl_t>(store, std::move(misc_methods),
                                                     std::move(params));
#endif
}

} // namespace nix
