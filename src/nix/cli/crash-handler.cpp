#include "crash-handler.h"

#include <exception>
#include <sstream>

#include <boost/core/demangle.hpp>

#include "nix/util/fmt.h"
#include "nix/util/logging.h"

// Darwin and FreeBSD stdenv do not define _GNU_SOURCE but do have _Unwind_Backtrace.
#if defined(__APPLE__) || defined(__FreeBSD__)
#  define BOOST_STACKTRACE_GNU_SOURCE_NOT_REQUIRED
#endif

#include <boost/stacktrace/stacktrace.hpp>

#ifndef _WIN32
#  include <syslog.h>
#endif

namespace nix {

namespace {

void log_fatal(std::string const& s) {
  write_to_stderr(s + "\n");
  // std::string for guaranteed null termination
#ifndef _WIN32
  syslog(LOG_CRIT, "%s", s.c_str());
#endif
}

void on_terminate() {
  log_fatal("Determinate Nix crashed. This is a bug. Please report this at "
            "https://github.com/DeterminateSystems/nix-src/issues with the following information "
            "included:\n");
  try {
    std::exception_ptr eptr = std::current_exception();
    if (eptr) {
      std::rethrow_exception(eptr);
    } else {
      log_fatal("std::terminate() called without exception");
    }
  } catch (const std::exception& ex) {
    log_fatal(fmt("Exception: %s: %s", boost::core::demangle(typeid(ex).name()), ex.what()));
  } catch (...) {
    log_fatal("Unknown exception!");
  }

  log_fatal("Stack trace:");
  std::ostringstream ss;
  ss << boost::stacktrace::stacktrace();
  log_fatal(ss.str());

  std::abort();
}
} // namespace

void register_crash_handler() {
  // DO NOT use this for signals. Boost stacktrace is very much not
  // async-signal-safe, and in a world with ASLR, addr2line is pointless.
  //
  // If you want signals, set up a minidump system and do it out-of-process.
  std::set_terminate(on_terminate);
}
} // namespace nix
