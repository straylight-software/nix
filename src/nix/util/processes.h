#pragma once
///@file

#include <atomic>
#include <functional>
#include <map>
#include <optional>
#include <sstream>

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "nix/util/ansicolor.h"
#include "nix/util/error.h"
#include "nix/util/file-descriptor.h"
#include "nix/util/logging.h"
#include "nix/util/types.h"

namespace nix {

struct Sink;
struct Source;

class Pid {
#ifndef _WIN32
  pid_t pid = -1;
  bool separatePG = false;
  int killSignal = SIGKILL;
#else
  auto_close_fd_t pid = INVALID_DESCRIPTOR;
#endif
public:
  Pid();
#ifndef _WIN32
  Pid(pid_t pid);
  void operator=(pid_t pid);
  operator pid_t();
#else
  Pid(auto_close_fd_t pid);
  void operator=(auto_close_fd_t pid);
#endif
  ~Pid();
  int kill();
  int wait();

  // TODO: Implement for Windows
#ifndef _WIN32
  void setSeparatePG(bool separatePG);
  void setKillSignal(int signal);
  pid_t release();
#endif
};

#ifndef _WIN32
/**
 * Kill all processes running under the specified uid by sending them
 * a SIGKILL.
 */
void killUser(uid_t uid);
#endif

/**
 * Fork a process that runs the given function, and return the child
 * pid to the caller.
 */
struct process_options_t {
  std::string errorPrefix = "";
  bool dieWithParent = true;
  bool runExitHandlers = false;
  bool allowVfork = false;
  /**
   * use clone() with the specified flags (Linux only)
   */
  int cloneFlags = 0;
};

#ifndef _WIN32
pid_t startProcess(std::function<void()> fun, const process_options_t& options = process_options_t());
#endif

/**
 * Run a program and return its stdout in a string (i.e., like the
 * shell backtick operator).
 */
std::string runProgram(Path program, bool lookupPath = false, const strings_t& args = strings_t(),
                       const std::optional<std::string>& input = {}, bool isInteractive = false);

struct run_options_t {
  Path program;
  bool lookupPath = true;
  strings_t args;
#ifndef _WIN32
  std::optional<uid_t> uid;
  std::optional<uid_t> gid;
#endif
  std::optional<Path> chdir;
  std::optional<string_map_t> environment;
  std::optional<std::string> input;
  Source* standardIn = nullptr;
  Sink* standardOut = nullptr;
  bool mergeStderrToStdout = false;
  bool isInteractive = false;
};

std::pair<int, std::string> runProgram(run_options_t&& options);

void runProgram2(const run_options_t& options);

class exec_error_t : public Error {
public:
  int status;

  template <typename... Args>
  exec_error_t(int status, const Args&... args) : Error(args...), status(status) {}
};

/**
 * Convert the exit status of a child as returned by wait() into an
 * error string.
 */
std::string statusToString(int status);

bool statusOk(int status);

} // namespace nix
