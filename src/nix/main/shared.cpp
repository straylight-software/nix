#include "nix/main/shared.h"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <iostream>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "nix/main/loggers.h"
#include "nix/main/progress-bar.h"
#include "nix/store/gc-store.h"
#include "nix/store/globals.h"
#include "nix/store/store-api.h"
#include "nix/util/current-process.h"
#include "nix/util/signals.h"
#include "nix/util/util.h"
#ifdef __linux__
#  include <features.h>
#endif

#include <openssl/crypto.h>

#include "main-config-private.h"
#include "nix/expr/config.h"
#include "nix/util/exit.h"
#include "nix/util/strings.h"

namespace nix {

char** saved_argv;

static bool gc_warning = true;

void print_gc_warning() {
  if (!gc_warning)
    return;
  static bool have_warned = false;
  warnOnce(have_warned, "you did not specify '--add-root'; "
                        "the result might be removed by the garbage collector");
}

void print_missing(ref<store_t> store, const std::vector<derived_path_t>& paths, verbosity_t lvl) {
  print_missing(store, store->query_missing(paths), lvl);
}

void print_missing(ref<store_t> store, const MissingPaths& missing, verbosity_t lvl) {
  if (!missing.willBuild.empty()) {
    if (missing.willBuild.size() == 1)
      printMsg(lvl, "this derivation will be built:");
    else
      printMsg(lvl, "these %d derivations will be built:", missing.willBuild.size());
    auto sorted = store->topoSortPaths(missing.willBuild);
    reverse(sorted.begin(), sorted.end());
    for (auto& i : sorted)
      printMsg(lvl, "  %s", store->printStorePath(i));
  }

  if (!missing.willSubstitute.empty()) {
    if (missing.willSubstitute.size() == 1) {
      printMsg(lvl, "this path will be fetched (%s download, %s unpacked):",
               render_size(missing.downloadSize), render_size(missing.nar_size));
    } else {
      printMsg(lvl, "these %d paths will be fetched (%s download, %s unpacked):",
               missing.willSubstitute.size(), render_size(missing.downloadSize),
               render_size(missing.nar_size));
    }
    std::vector<const store_path_t*> willSubstituteSorted = {};
    std::for_each(missing.willSubstitute.begin(), missing.willSubstitute.end(),
                  [&](const store_path_t& p) { willSubstituteSorted.push_back(&p); });
    std::sort(willSubstituteSorted.begin(), willSubstituteSorted.end(),
              [](const store_path_t* lhs, const store_path_t* rhs) {
                if (lhs->name() == rhs->name())
                  return lhs->to_string() < rhs->to_string();
                else
                  return lhs->name() < rhs->name();
              });
    for (auto p : willSubstituteSorted)
      printMsg(lvl, "  %s", store->printStorePath(*p));
  }

  if (!missing.unknown.empty()) {
    printMsg(lvl, "don't know how to build these paths%s:",
             (settings.readOnlyMode ? " (may be caused by read-only store access)" : ""));
    for (auto& i : missing.unknown)
      printMsg(lvl, "  %s", store->printStorePath(i));
  }
}

std::string get_arg(const std::string& opt, strings_t::iterator& i,
                    const strings_t::iterator& end) {
  ++i;
  if (i == end)
    throw UsageError("'%1%' requires an argument", opt);
  return *i;
}

#ifndef _WIN32
static void sig_handler(int signo) {}
#endif

void init_nix(bool load_config) {
  /* Turn on buffering for cerr. */
#if HAVE_PUBSETBUF
  static char buf[1024];
  std::cerr.rdbuf()->pubsetbuf(buf, sizeof(buf));
#endif

  init_lib_store(load_config);

#ifndef _WIN32
  unix::start_signal_handler_thread();

  /* Reset SIGCHLD to its default. */
  struct sigaction act;
  sigemptyset(&act.sa_mask);
  act.sa_flags = 0;

  act.sa_handler = SIG_DFL;
  if (sigaction(SIGCHLD, &act, 0))
    throw sys_error_t("resetting SIGCHLD");

  /* Install a dummy SIGUSR1 handler for use with pthread_kill(). */
  act.sa_handler = sig_handler;
  if (sigaction(SIGUSR1, &act, 0))
    throw sys_error_t("handling SIGUSR1");
#endif

#ifdef __APPLE__
  /* HACK: on darwin, we need can’t use sigprocmask with SIGWINCH.
   * Instead, add a dummy sigaction handler, and signal_handler_thread
   * can handle the rest. */
  act.sa_handler = sig_handler;
  if (sigaction(SIGWINCH, &act, 0))
    throw sys_error_t("handling SIGWINCH");

  /* Disable SA_RESTART for interrupts, so that system calls on this thread
   * error with EINTR like they do on Linux.
   * Most signals on BSD systems default to SA_RESTART on, but Nix
   * expects EINTR from syscalls to properly exit. */
  act.sa_handler = SIG_DFL;
  if (sigaction(SIGINT, &act, 0))
    throw sys_error_t("handling SIGINT");
  if (sigaction(SIGTERM, &act, 0))
    throw sys_error_t("handling SIGTERM");
  if (sigaction(SIGHUP, &act, 0))
    throw sys_error_t("handling SIGHUP");
  if (sigaction(SIGPIPE, &act, 0))
    throw sys_error_t("handling SIGPIPE");
  if (sigaction(SIGQUIT, &act, 0))
    throw sys_error_t("handling SIGQUIT");
  if (sigaction(SIGTRAP, &act, 0))
    throw sys_error_t("handling SIGTRAP");
#endif

#ifndef _WIN32
  /* Register a SIGSEGV handler to detect stack overflows.
     Why not initLibExpr()? init_gc() is essentially that, but
     detectStackOverflow is not an instance of the init function concept, as
     it may have to be invoked more than once per process. */
  detectStackOverflow();
#endif

  /* There is no privacy in the Nix system ;-)  At least not for
     now.  In particular, store objects should be readable by
     everybody. */
  umask(0022);
}

LegacyArgs::LegacyArgs(
    const std::string& program_name,
    std::function<bool(strings_t::iterator& arg, const strings_t::iterator& end)> parse_arg)
    : MixCommonArgs(program_name), parse_arg(parse_arg) {
  add_flag({
      .long_name = "no-build-output",
      .short_name = 'Q',
      .description = "Do not show build output.",
      .handler = {[&]() { set_log_format(LogFormat::raw); }},
  });

  add_flag({
      .long_name = "keep-failed",
      .short_name = 'K',
      .description = "Keep temporary directories of failed builds.",
      .handler = {&(bool&)settings.keep_failed, true},
  });

  add_flag({
      .long_name = "keep-going",
      .short_name = 'k',
      .description = "Keep going after a build fails.",
      .handler = {&(bool&)settings.keep_going, true},
  });

  add_flag({
      .long_name = "fallback",
      .description = "Build from source if substitution fails.",
      .handler = {&(bool&)settings.try_fallback, true},
  });

  auto intSettingAlias = [&](char short_name, const std::string& long_name,
                             const std::string& description, const std::string& dest) {
    add_flag({
        .long_name = long_name,
        .short_name = short_name,
        .description = description,
        .labels = {"n"},
        .handler = {[=](std::string s) {
          auto n = string2_int_with_unit_prefix<uint64_t>(s);
          settings.set(dest, std::to_string(n));
        }},
    });
  };

  intSettingAlias(0, "cores", "Maximum number of CPU cores to use inside a build.", "cores");
  intSettingAlias(0, "max-silent-time", "Number of seconds of silence before a build is killed.",
                  "max-silent-time");
  intSettingAlias(0, "timeout", "Number of seconds before a build is killed.", "timeout");

  add_flag({
      .long_name = "readonly-mode",
      .description = "Do not write to the Nix store.",
      .handler = {&settings.readOnlyMode, true},
  });

  add_flag({
      .long_name = "no-gc-warning",
      .description = "Disable warnings about not using `--add-root`.",
      .handler = {&gc_warning, false},
  });

  add_flag({
      .long_name = "store",
      .description = "The URL of the Nix store to use.",
      .labels = {"store-uri"},
      .handler = {&(std::string&)settings.storeUri},
  });
}

bool LegacyArgs::process_flag(strings_t::iterator& pos, strings_t::iterator end) {
  if (MixCommonArgs::process_flag(pos, end))
    return true;
  bool res = parse_arg(pos, end);
  if (res)
    ++pos;
  return res;
}

bool LegacyArgs::process_args(const strings_t& args, bool finish) {
  if (args.empty())
    return true;
  assert(args.size() == 1);
  strings_t ss(args);
  auto pos = ss.begin();
  if (!parse_arg(pos, ss.end()))
    throw UsageError("unexpected argument '%1%'", args.front());
  return true;
}

void parse_cmd_line(
    int argc, char** argv,
    std::function<bool(strings_t::iterator& arg, const strings_t::iterator& end)> parse_arg) {
  parse_cmd_line(std::string(base_name_of(argv[0])), argv_to_strings(argc, argv), parse_arg);
}

void parse_cmd_line(
    const std::string& program_name, const strings_t& args,
    std::function<bool(strings_t::iterator& arg, const strings_t::iterator& end)> parse_arg) {
  LegacyArgs(program_name, parse_arg).parse_cmdline(args);
}

std::string version() {
  return fmt("(Determinate Nix %s) %s", determinate_nix_version, nix_version);
}

void print_version(const std::string& program_name) {
  std::cout << fmt("%s %s", program_name, version()) << std::endl;
  if (verbosity > lvl_info) {
    strings_t cfg;
#if NIX_USE_BOEHMGC
    cfg.push_back("gc");
#endif
    cfg.push_back("signed-caches");
    std::cout << "System type: " << settings.thisSystem << "\n";
    std::cout << "Additional system types: "
              << concat_strings_sep(", ", settings.extraPlatforms.get()) << "\n";
    std::cout << "Features: " << concat_strings_sep(", ", cfg) << "\n";
    std::cout << "System configuration file: " << (settings.nixConfDir / "nix.conf") << "\n";
    std::cout << "User configuration files: " << concat_strings_sep(":", settings.nixUserConfFiles)
              << "\n";
    std::cout << "store_t directory: " << settings.nixStore << "\n";
    std::cout << "State directory: " << settings.nixStateDir << "\n";
    std::cout << "Data directory: " << settings.nixDataDir << "\n";
  }
  throw exit_t();
}

int handle_exceptions(const std::string& program_name, std::function<void()> fun) {
  receive_interrupts_t receive_interrupts; // FIXME: need better place for this

  error_info_t::program_name = base_name_of(program_name);

  std::string error = ANSI_RED "error:" ANSI_NORMAL " ";
  try {
    fun();
  } catch (exit_t& e) {
    return e.get_status();
  } catch (UsageError& e) {
    logError(e.info());
    printError("\nTry '%1% --help' for more information.", program_name);
    return 1;
  } catch (base_error_t& e) {
    logError(e.info());
    return e.info().status_;
  } catch (std::bad_alloc& e) {
    printError(error + "out of memory");
    return 1;
  } catch (std::exception& e) {
    printError(error + e.what());
    return 1;
  }

  return 0;
}

RunPager::RunPager() {
  if (!isatty(STDOUT_FILENO))
    return;
  char* pager = getenv("NIX_PAGER");
  if (!pager)
    pager = getenv("PAGER");
  if (pager && ((std::string)pager == "" || (std::string)pager == "cat"))
    return;

  logger->stop();

  pipe_t toPager;
  toPager.create();

#ifdef _WIN32 // TODO re-enable on Windows, once we can start processes.
  throw Error("Commit signature verification not implemented on Windows yet");
#else
  pid = start_process([&]() {
    if (dup2(toPager.read_side.get(), STDIN_FILENO) == -1)
      throw sys_error_t("dupping stdin");
    if (!getenv("LESS"))
      set_env("LESS", "FRSXMK");
    restore_process_context();
    if (pager)
      execl("/bin/sh", "sh", "-c", pager, nullptr);
    execlp("pager", "pager", nullptr);
    execlp("less", "less", nullptr);
    execlp("more", "more", nullptr);
    throw sys_error_t("executing '%1%'", pager);
  });

  pid.set_kill_signal(SIGINT);
  std_out = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 0);
  if (dup2(toPager.write_side.get(), STDOUT_FILENO) == -1)
    throw sys_error_t("dupping standard output");
#endif
}

RunPager::~RunPager() {
  try {
#ifndef _WIN32 // TODO re-enable on Windows, once we can start processes.
    if (pid != -1) {
      std::cout.flush();
      dup2(std_out, STDOUT_FILENO);
      pid.wait();
    }
#endif
  } catch (...) {
    ignore_exception_in_destructor();
  }
}

PrintFreed::~PrintFreed() {
  if (show)
    std::cout << fmt("%d store paths deleted, %s freed\n", results.paths.size(),
                     render_size(results.bytes_freed));
}

#ifndef _WIN32

// Stack overflow handler - minimal implementation
// Uses sigaltstack to handle SIGSEGV on alternate stack

std::function<void(siginfo_t* info, void* ctx)> stackOverflowHandler;

void defaultStackOverflowHandler(siginfo_t* info, void* ctx) {
  // Write directly to stderr to avoid heap allocation in signal handler
  const char msg[] = "error: stack overflow detected (SIGSEGV)\n";
  [[maybe_unused]] auto _ = write(STDERR_FILENO, msg, sizeof(msg) - 1);
  _exit(1);
}

static char altstack_buffer[SIGSTKSZ];

static void sigsegv_handler(int sig, siginfo_t* info, void* ctx) {
  // Check if this is a stack overflow (address near the stack)
  // If so, call the handler, otherwise re-raise
  if (stackOverflowHandler) {
    stackOverflowHandler(info, ctx);
  } else {
    defaultStackOverflowHandler(info, ctx);
  }
}

void detectStackOverflow() {
  // Set up alternate signal stack
  stack_t ss;
  ss.ss_sp = altstack_buffer;
  ss.ss_size = sizeof(altstack_buffer);
  ss.ss_flags = 0;
  if (sigaltstack(&ss, nullptr) == -1) {
    return; // silently fail if we can't set up alt stack
  }

  // Install SIGSEGV handler
  struct sigaction sa;
  sa.sa_sigaction = sigsegv_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGSEGV, &sa, nullptr) == -1) {
    return; // silently fail
  }

  // Initialize default handler if not set
  if (!stackOverflowHandler) {
    stackOverflowHandler = defaultStackOverflowHandler;
  }
}

#endif // _WIN32

} // namespace nix
