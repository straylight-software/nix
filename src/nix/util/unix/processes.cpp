#include "nix/util/processes.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <sstream>
#include <thread>

#include <grp.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nix/util/current-process.h"
#include "nix/util/environment-variables.h"
#include "nix/util/executable-path.h"
#include "nix/util/finally.h"
#include "nix/util/serialise.h"
#include "nix/util/signals.h"

#ifdef __APPLE__
#  include <sys/syscall.h>
#endif

#ifdef __linux__
#  include <sys/mman.h>
#  include <sys/prctl.h>
#endif

#include "nix/util/util-config-private.h"
#include "nix/util/util-unix-config-private.h"

namespace nix {

process_handle_t::process_handle_t() {}

process_handle_t::process_handle_t(::pid_t pid) : pid_(pid) {}

process_handle_t::~process_handle_t() {
  if (pid_ != -1) {
    try {
      kill();
    } catch (...) {
      // Destructors must not throw. The process may have already exited
      // or been reaped elsewhere.
    }
  }
}

void process_handle_t::operator=(::pid_t pid) {
  if (this->pid_ != -1 && this->pid_ != pid) {
    kill();
  }
  this->pid_ = pid;
  kill_signal_ = SIGKILL; // reset signal to default
}

process_handle_t::operator ::pid_t() {
  return pid_;
}

int process_handle_t::kill() {
  assert(pid_ != -1);

  debug("killing process %1%", pid_);

  /* Send the requested signal to the child.  If it has its own
     process group, send the signal to every process in the child
     process group (which hopefully includes *all* its children). */
  if (::kill(separate_pg_ ? -pid_ : pid_, kill_signal_) != 0) {
    /* On BSDs, killing a process group will return EPERM if all
       processes in the group are zombies (or something like
       that). So try to detect and ignore that situation. */
#if defined(__FreeBSD__) || defined(__APPLE__)
    if (errno != EPERM || ::kill(pid_, 0) != 0)
#endif
      logError(sys_error_t("killing process %d", pid_).info());
  }

  return wait();
}

int process_handle_t::wait() {
  assert(pid_ != -1);
  while (1) {
    int status;
    int res = waitpid(pid_, &status, 0);
    if (res == pid_) {
      pid_ = -1;
      return status;
    }
    if (errno != EINTR) {
      throw sys_error_t("cannot get exit status of PID %d", pid_);
    }
    check_interrupt();
  }
}

void process_handle_t::set_separate_pg(bool separate_pg) {
  this->separate_pg_ = separate_pg;
}

void process_handle_t::set_kill_signal(int signal) {
  this->kill_signal_ = signal;
}

::pid_t process_handle_t::release() {
  ::pid_t p = pid_;
  pid_ = -1;
  return p;
}

void kill_user(uid_t uid) {
  debug("killing all processes running under uid '%1%'", uid);

  assert(uid != 0); /* just to be safe... */

  /* The system call kill(-1, sig) sends the signal `sig' to all
     users to which the current process can send signals.  So we
     fork a process, switch to uid, and send a mass kill. */

  process_handle_t pid = start_process([&] {
    if (setuid(uid) == -1) {
      throw sys_error_t("setting uid");
    }

    while (true) {
#ifdef __APPLE__
      /* OSX's kill syscall takes a third parameter that, among
         other things, determines if kill(-1, signo) affects the
         calling process. In the OSX libc, it's set to true,
         which means "follow POSIX", which we don't want here
           */
      if (syscall(SYS_kill, -1, SIGKILL, false) == 0)
        break;
#else
      if (kill(-1, SIGKILL) == 0) {
        break;
      }
#endif
      if (errno == ESRCH || errno == EPERM) {
        break; /* no more processes */
      }
      if (errno != EINTR) {
        throw sys_error_t("cannot kill processes for uid '%1%'", uid);
      }
    }

    _exit(0);
  });

  int status = pid.wait();
  if (status != 0) {
    throw Error("cannot kill processes for uid '%1%': %2%", uid, status_to_string(status));
  }

  /* !!! We should really do some check to make sure that there are
     no processes left running under `uid', but there is no portable
     way to do so (I think).  The most reliable way may be `ps -eo
     uid | grep -q $uid'. */
}

//////////////////////////////////////////////////////////////////////

using child_wrapper_function_t = std::function<void()>;

/* Wrapper around vfork to prevent the child process from clobbering
   the caller's stack frame in the parent. */
static process_handle_t do_fork(bool allow_vfork, child_wrapper_function_t& fun)
    __attribute__((noinline));

static process_handle_t do_fork(bool allow_vfork, child_wrapper_function_t& fun) {
#ifdef __linux__
  process_handle_t pid = allow_vfork ? vfork() : fork();
#else
  process_handle_t pid = fork();
#endif
  if (pid != 0) {
    return pid;
  }
  fun();
  unreachable();
}

#ifdef __linux__
static int child_entry(void* arg) {
  auto& fun = *reinterpret_cast<child_wrapper_function_t*>(arg);
  fun();
  return 1;
}
#endif

process_handle_t start_process(std::function<void()> fun, const process_options_t& options) {
  auto new_logger = make_simple_logger();
  child_wrapper_function_t wrapper = [&] {
    if (!options.allow_vfork) {
      /* Set a simple logger, while releasing (not destroying)
         the parent logger. We don't want to run the parent
         logger's destructor since that will crash (e.g. when
         ~progress_bar_t() tries to join a thread that doesn't
         exist. */
      logger.release();
      logger = std::move(new_logger);
    }
    try {
#ifdef __linux__
      if (options.die_with_parent && prctl(PR_SET_PDEATHSIG, SIGKILL) == -1) {
        throw sys_error_t("setting death signal");
      }
#endif
      fun();
    } catch (std::exception& e) {
      try {
        std::cerr << options.error_prefix << e.what() << "\n";
      } catch (...) {
      }
    } catch (...) {
    }
    if (options.run_exit_handlers) {
      exit(1);
    } else {
      _exit(1);
    }
  };

  process_handle_t pid = -1;

  if (options.clone_flags) {
#ifdef __linux__
    // Not supported, since then we don't know when to free the stack.
    assert(!(options.clone_flags & CLONE_VM));

    size_t stack_size = 1 * 1024 * 1024;
    auto stack = static_cast<char*>(mmap(0, stack_size, PROT_WRITE | PROT_READ,
                                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0));
    if (stack == MAP_FAILED) {
      throw sys_error_t("allocating stack");
    }

    finally_t free_stack([&] { munmap(stack, stack_size); });

    pid = clone(child_entry, stack + stack_size, options.clone_flags | SIGCHLD, &wrapper);
#else
    throw Error("clone flags are only supported on Linux");
#endif
  } else {
    pid = do_fork(options.allow_vfork, wrapper);
  }

  if (pid == -1) {
    throw sys_error_t("unable to fork");
  }

  return pid;
}

std::string run_program(Path program, bool lookup_path, const strings_t& args,
                        const std::optional<std::string>& input, bool is_interactive) {
  auto res = run_program(run_options_t{.program = program,
                                       .lookup_path = lookup_path,
                                       .args = args,
                                       .input = input,
                                       .is_interactive = is_interactive});

  if (!status_ok(res.first)) {
    throw exec_error_t(res.first, "program '%1%' %2%", program, status_to_string(res.first));
  }

  return res.second;
}

// Output = error code + "standard out" output stream
std::pair<int, std::string> run_program(run_options_t&& options) {
  string_sink_t sink;
  options.standard_out = &sink;

  int status = 0;

  try {
    run_program2(options);
  } catch (exec_error_t& e) {
    status = e.status;
  }

  return {status, std::move(sink.str())};
}

void run_program2(const run_options_t& options) {
  check_interrupt();

  assert(!(options.standard_in && options.input));

  std::unique_ptr<source_t> source_;
  source_t* source = options.standard_in;

  if (options.input) {
    source_ = std::make_unique<string_source_t>(*options.input);
    source = source_.get();
  }

  /* Create a pipe. */
  pipe_t out, in;
  if (options.standard_out) {
    out.create();
  }
  if (source) {
    in.create();
  }

  process_options_t process_options;
  // vfork implies that the environment of the main process and the fork will
  // be shared (technically this is undefined, but in practice that's the
  // case), so we can't use it if we alter the environment
  process_options.allow_vfork = !options.environment;

  auto suspension = logger->suspend_if(options.is_interactive);

  /* Fork. */
  process_handle_t pid = start_process(
      [&] {
        if (options.environment) {
          replace_env(*options.environment);
        }
        if (options.standard_out && dup2(out.write_side.get(), STDOUT_FILENO) == -1) {
          throw sys_error_t("dupping stdout");
        }
        if (options.merge_stderr_to_stdout) {
          if (dup2(STDOUT_FILENO, STDERR_FILENO) == -1) {
            throw sys_error_t("cannot dup stdout into stderr");
          }
        }
        if (source && dup2(in.read_side.get(), STDIN_FILENO) == -1) {
          throw sys_error_t("dupping stdin");
        }

        if (options.chdir && chdir((*options.chdir).c_str()) == -1) {
          throw sys_error_t("chdir failed");
        }
        if (options.gid && setgid(*options.gid) == -1) {
          throw sys_error_t("setgid failed");
        }
        /* Drop all other groups if we're setgid. */
        if (options.gid && setgroups(0, 0) == -1) {
          throw sys_error_t("setgroups failed");
        }
        if (options.uid && setuid(*options.uid) == -1) {
          throw sys_error_t("setuid failed");
        }

        strings_t args_(options.args);
        args_.push_front(options.program);

        restore_process_context();

        if (options.lookup_path) {
          execvp(options.program.c_str(), strings_to_char_ptrs(args_).data());
          // This allows you to refer to a program with a pathname relative
          // to the PATH variable.
        } else {
          execv(options.program.c_str(), strings_to_char_ptrs(args_).data());
        }

        throw sys_error_t("executing '%1%'", options.program);
      },
      process_options);

  out.write_side.close();

  std::thread writer_thread;

  std::promise<void> promise;

  finally_t do_join([&] {
    if (writer_thread.joinable()) {
      writer_thread.join();
    }
  });

  if (source) {
    in.read_side.close();
    writer_thread = std::thread([&] {
      try {
        std::vector<char> buf(8 * 1024);
        while (true) {
          size_t n;
          try {
            n = source->read(buf.data(), buf.size());
          } catch (EndOfFile&) {
            break;
          }
          write_full(in.write_side.get(), {buf.data(), n});
        }
        promise.set_value();
      } catch (...) {
        promise.set_exception(std::current_exception());
      }
      in.write_side.close();
    });
  }

  if (options.standard_out) {
    drain_fd(out.read_side.get(), *options.standard_out);
  }

  /* Wait for the child to finish. */
  int status = pid.wait();

  /* Wait for the writer thread to finish. */
  if (source) {
    promise.get_future().get();
  }

  if (status) {
    throw exec_error_t(status, "program '%1%' %2%", options.program, status_to_string(status));
  }
}

//////////////////////////////////////////////////////////////////////

std::string status_to_string(int status) {
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    if (WIFEXITED(status)) {
      return fmt("failed with exit code %1%", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
      int sig = WTERMSIG(status);
#if HAVE_STRSIGNAL
      const char* description = strsignal(sig);
      return fmt("failed due to signal %1% (%2%)", sig, description);
#else
      return fmt("failed due to signal %1%", sig);
#endif
    } else {
      return "died abnormally";
    }
  } else {
    return "succeeded";
  }
}

bool status_ok(int status) {
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int execvpe(const char* file0, const char* const argv[], const char* const envp[]) {
  auto file = executable_path_t::load().find_path(file0);
  // `const_cast` is safe. See the note in
  // https://pubs.opengroup.org/onlinepubs/9799919799/functions/exec.html
  return execve(file.c_str(), const_cast<char* const*>(argv), const_cast<char* const*>(envp));
}

} // namespace nix
