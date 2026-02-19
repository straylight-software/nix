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

Pid::Pid() {}

Pid::Pid(pid_t pid) : pid(pid) {}

Pid::~Pid() {
  if (pid != -1)
    kill();
}

void Pid::operator=(pid_t pid) {
  if (this->pid != -1 && this->pid != pid)
    kill();
  this->pid = pid;
  killSignal = SIGKILL; // reset signal to default
}

Pid::operator pid_t() {
  return pid;
}

int Pid::kill() {
  assert(pid != -1);

  debug("killing process %1%", pid);

  /* Send the requested signal to the child.  If it has its own
     process group, send the signal to every process in the child
     process group (which hopefully includes *all* its children). */
  if (::kill(separatePG ? -pid : pid, killSignal) != 0) {
    /* On BSDs, killing a process group will return EPERM if all
       processes in the group are zombies (or something like
       that). So try to detect and ignore that situation. */
#if defined(__FreeBSD__) || defined(__APPLE__)
    if (errno != EPERM || ::kill(pid, 0) != 0)
#endif
      logError(sys_error_t("killing process %d", pid).info());
  }

  return wait();
}

int Pid::wait() {
  assert(pid != -1);
  while (1) {
    int status;
    int res = waitpid(pid, &status, 0);
    if (res == pid) {
      pid = -1;
      return status;
    }
    if (errno != EINTR)
      throw sys_error_t("cannot get exit status of PID %d", pid);
    checkInterrupt();
  }
}

void Pid::setSeparatePG(bool separatePG) {
  this->separatePG = separatePG;
}

void Pid::setKillSignal(int signal) {
  this->killSignal = signal;
}

pid_t Pid::release() {
  pid_t p = pid;
  pid = -1;
  return p;
}

void killUser(uid_t uid) {
  debug("killing all processes running under uid '%1%'", uid);

  assert(uid != 0); /* just to be safe... */

  /* The system call kill(-1, sig) sends the signal `sig' to all
     users to which the current process can send signals.  So we
     fork a process, switch to uid, and send a mass kill. */

  Pid pid = startProcess([&] {
    if (setuid(uid) == -1)
      throw sys_error_t("setting uid");

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
      if (kill(-1, SIGKILL) == 0)
        break;
#endif
      if (errno == ESRCH || errno == EPERM)
        break; /* no more processes */
      if (errno != EINTR)
        throw sys_error_t("cannot kill processes for uid '%1%'", uid);
    }

    _exit(0);
  });

  int status = pid.wait();
  if (status != 0)
    throw Error("cannot kill processes for uid '%1%': %2%", uid, statusToString(status));

  /* !!! We should really do some check to make sure that there are
     no processes left running under `uid', but there is no portable
     way to do so (I think).  The most reliable way may be `ps -eo
     uid | grep -q $uid'. */
}

//////////////////////////////////////////////////////////////////////

using child_wrapper_function_t = std::function<void()>;

/* Wrapper around vfork to prevent the child process from clobbering
   the caller's stack frame in the parent. */
static pid_t doFork(bool allowVfork, child_wrapper_function_t& fun) __attribute__((noinline));

static pid_t doFork(bool allowVfork, child_wrapper_function_t& fun) {
#ifdef __linux__
  pid_t pid = allowVfork ? vfork() : fork();
#else
  pid_t pid = fork();
#endif
  if (pid != 0)
    return pid;
  fun();
  unreachable();
}

#ifdef __linux__
static int childEntry(void* arg) {
  auto& fun = *reinterpret_cast<child_wrapper_function_t*>(arg);
  fun();
  return 1;
}
#endif

pid_t startProcess(std::function<void()> fun, const process_options_t& options) {
  auto newLogger = makeSimpleLogger();
  child_wrapper_function_t wrapper = [&] {
    if (!options.allowVfork) {
      /* Set a simple logger, while releasing (not destroying)
         the parent logger. We don't want to run the parent
         logger's destructor since that will crash (e.g. when
         ~progress_bar_t() tries to join a thread that doesn't
         exist. */
      logger.release();
      logger = std::move(newLogger);
    }
    try {
#ifdef __linux__
      if (options.dieWithParent && prctl(PR_SET_PDEATHSIG, SIGKILL) == -1)
        throw sys_error_t("setting death signal");
#endif
      fun();
    } catch (std::exception& e) {
      try {
        std::cerr << options.errorPrefix << e.what() << "\n";
      } catch (...) {
      }
    } catch (...) {
    }
    if (options.runExitHandlers)
      exit(1);
    else
      _exit(1);
  };

  pid_t pid = -1;

  if (options.cloneFlags) {
#ifdef __linux__
    // Not supported, since then we don't know when to free the stack.
    assert(!(options.cloneFlags & CLONE_VM));

    size_t stackSize = 1 * 1024 * 1024;
    auto stack = static_cast<char*>(
        mmap(0, stackSize, PROT_WRITE | PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0));
    if (stack == MAP_FAILED)
      throw sys_error_t("allocating stack");

    finally_t freeStack([&] { munmap(stack, stackSize); });

    pid = clone(childEntry, stack + stackSize, options.cloneFlags | SIGCHLD, &wrapper);
#else
    throw Error("clone flags are only supported on Linux");
#endif
  } else
    pid = doFork(options.allowVfork, wrapper);

  if (pid == -1)
    throw sys_error_t("unable to fork");

  return pid;
}

std::string runProgram(Path program, bool lookupPath, const strings_t& args,
                       const std::optional<std::string>& input, bool isInteractive) {
  auto res = runProgram(run_options_t{.program = program,
                                   .lookupPath = lookupPath,
                                   .args = args,
                                   .input = input,
                                   .isInteractive = isInteractive});

  if (!statusOk(res.first))
    throw exec_error_t(res.first, "program '%1%' %2%", program, statusToString(res.first));

  return res.second;
}

// Output = error code + "standard out" output stream
std::pair<int, std::string> runProgram(run_options_t&& options) {
  string_sink_t sink;
  options.standardOut = &sink;

  int status = 0;

  try {
    runProgram2(options);
  } catch (exec_error_t& e) {
    status = e.status;
  }

  return {status, std::move(sink.s)};
}

void runProgram2(const run_options_t& options) {
  checkInterrupt();

  assert(!(options.standardIn && options.input));

  std::unique_ptr<Source> source_;
  Source* source = options.standardIn;

  if (options.input) {
    source_ = std::make_unique<string_source_t>(*options.input);
    source = source_.get();
  }

  /* Create a pipe. */
  pipe_t out, in;
  if (options.standardOut)
    out.create();
  if (source)
    in.create();

  process_options_t processOptions;
  // vfork implies that the environment of the main process and the fork will
  // be shared (technically this is undefined, but in practice that's the
  // case), so we can't use it if we alter the environment
  processOptions.allowVfork = !options.environment;

  auto suspension = logger->suspendIf(options.isInteractive);

  /* Fork. */
  Pid pid = startProcess(
      [&] {
        if (options.environment)
          replaceEnv(*options.environment);
        if (options.standardOut && dup2(out.writeSide.get(), STDOUT_FILENO) == -1)
          throw sys_error_t("dupping stdout");
        if (options.mergeStderrToStdout)
          if (dup2(STDOUT_FILENO, STDERR_FILENO) == -1)
            throw sys_error_t("cannot dup stdout into stderr");
        if (source && dup2(in.readSide.get(), STDIN_FILENO) == -1)
          throw sys_error_t("dupping stdin");

        if (options.chdir && chdir((*options.chdir).c_str()) == -1)
          throw sys_error_t("chdir failed");
        if (options.gid && setgid(*options.gid) == -1)
          throw sys_error_t("setgid failed");
        /* Drop all other groups if we're setgid. */
        if (options.gid && setgroups(0, 0) == -1)
          throw sys_error_t("setgroups failed");
        if (options.uid && setuid(*options.uid) == -1)
          throw sys_error_t("setuid failed");

        strings_t args_(options.args);
        args_.push_front(options.program);

        restoreProcessContext();

        if (options.lookupPath)
          execvp(options.program.c_str(), stringsToCharPtrs(args_).data());
        // This allows you to refer to a program with a pathname relative
        // to the PATH variable.
        else
          execv(options.program.c_str(), stringsToCharPtrs(args_).data());

        throw sys_error_t("executing '%1%'", options.program);
      },
      processOptions);

  out.writeSide.close();

  std::thread writerThread;

  std::promise<void> promise;

  finally_t doJoin([&] {
    if (writerThread.joinable())
      writerThread.join();
  });

  if (source) {
    in.readSide.close();
    writerThread = std::thread([&] {
      try {
        std::vector<char> buf(8 * 1024);
        while (true) {
          size_t n;
          try {
            n = source->read(buf.data(), buf.size());
          } catch (EndOfFile&) {
            break;
          }
          writeFull(in.writeSide.get(), {buf.data(), n});
        }
        promise.set_value();
      } catch (...) {
        promise.set_exception(std::current_exception());
      }
      in.writeSide.close();
    });
  }

  if (options.standardOut)
    drainFD(out.readSide.get(), *options.standardOut);

  /* Wait for the child to finish. */
  int status = pid.wait();

  /* Wait for the writer thread to finish. */
  if (source)
    promise.get_future().get();

  if (status)
    throw exec_error_t(status, "program '%1%' %2%", options.program, statusToString(status));
}

//////////////////////////////////////////////////////////////////////

std::string statusToString(int status) {
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    if (WIFEXITED(status))
      return fmt("failed with exit code %1%", WEXITSTATUS(status));
    else if (WIFSIGNALED(status)) {
      int sig = WTERMSIG(status);
#if HAVE_STRSIGNAL
      const char* description = strsignal(sig);
      return fmt("failed due to signal %1% (%2%)", sig, description);
#else
      return fmt("failed due to signal %1%", sig);
#endif
    } else
      return "died abnormally";
  } else
    return "succeeded";
}

bool statusOk(int status) {
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int execvpe(const char* file0, const char* const argv[], const char* const envp[]) {
  auto file = executable_path_t::load().findPath(file0);
  // `const_cast` is safe. See the note in
  // https://pubs.opengroup.org/onlinepubs/9799919799/functions/exec.html
  return execve(file.c_str(), const_cast<char* const*>(argv), const_cast<char* const*>(envp));
}

} // namespace nix
