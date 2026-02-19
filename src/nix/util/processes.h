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
  bool separate_pg = false;
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
  void set_separate_pg(bool separate_pg);
  void set_kill_signal(int signal);
  pid_t release();
#endif
};

#ifndef _WIN32
/**
 * Kill all processes running under the specified uid by sending them
 * a SIGKILL.
 */
void kill_user(uid_t uid);
#endif

/**
 * Fork a process that runs the given function, and return the child
 * pid to the caller.
 */
struct process_options_t {
  std::string error_prefix = "";
  bool die_with_parent = true;
  bool run_exit_handlers = false;
  bool allow_vfork = false;
  /**
   * use clone() with the specified flags (Linux only)
   */
  int clone_flags = 0;
};

#ifndef _WIN32
pid_t start_process(std::function<void()> fun, const process_options_t& options = process_options_t());
#endif

/**
 * Run a program and return its stdout in a string (i.e., like the
 * shell backtick operator).
 */
std::string run_program(Path program, bool lookup_path = false, const strings_t& args = strings_t(),
                       const std::optional<std::string>& input = {}, bool is_interactive = false);

struct run_options_t {
  Path program;
  bool lookup_path = true;
  strings_t args;
#ifndef _WIN32
  std::optional<uid_t> uid;
  std::optional<uid_t> gid;
#endif
  std::optional<Path> chdir;
  std::optional<string_map_t> environment;
  std::optional<std::string> input;
  Source* standard_in = nullptr;
  Sink* standard_out = nullptr;
  bool merge_stderr_to_stdout = false;
  bool is_interactive = false;
};

std::pair<int, std::string> run_program(run_options_t&& options);

void run_program2(const run_options_t& options);

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
std::string status_to_string(int status);

bool status_ok(int status);

} // namespace nix
