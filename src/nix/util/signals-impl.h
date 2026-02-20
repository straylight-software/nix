#pragma once
/**
 * @file
 *
 * Implementation of some inline definitions for Unix signals, and also
 * some extra Unix-only interfaces.
 *
 * (The only reason everything about signals isn't Unix-only is some
 * no-op definitions are provided on Windows to avoid excess CPP in
 * downstream code.)
 */

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

#include <boost/lexical_cast.hpp>

#include "nix/util/ansicolor.h"
#include "nix/util/error.h"
#include "nix/util/logging.h"
#include "nix/util/signals.h"
#include "nix/util/types.h"

namespace nix {

/* User interruption. */

namespace unix {

extern std::atomic<bool> is_interrupted;

extern thread_local std::function<bool()> interrupt_check;

void _interrupted();

/**
 * Start a thread that handles various signals. Also block those signals
 * on the current thread (and thus any threads created by it).
 * Saves the signal mask before changing the mask to block those signals.
 * See save_signal_mask().
 */
void start_signal_handler_thread();

/**
 * Saves the signal mask, which is the signal mask that nix will restore
 * before creating child processes.
 */
void save_signal_mask();

/**
 * To use in a process that already called `start_signal_handler_thread()`
 * or `save_signal_mask()` first.
 */
void restore_signals();

void trigger_interrupt();

} // namespace unix

static inline void set_interrupted(bool is_interrupted) {
  unix::is_interrupted = is_interrupted;
}

static inline auto get_interrupted() -> bool {
  return unix::is_interrupted;
}

static inline auto is_interrupted() -> bool {
  return unix::is_interrupted || (unix::interrupt_check && unix::interrupt_check());
}

/**
 * Throw `Interrupted` exception if the process has been interrupted.
 *
 * Call this in long-running loops and between slow operations to terminate
 * them as needed.
 */
inline void check_interrupt() {
  if (is_interrupted()) {
    unix::_interrupted();
  }
}

/**
 * A RAII class that causes the current thread to receive SIGUSR1 when
 * the signal handler thread receives SIGINT. That is, this allows
 * SIGINT to be multiplexed to multiple threads.
 */
struct receive_interrupts_t {
  pthread_t target{};
  std::unique_ptr<nix::interrupt_callback_t> callback{};

  receive_interrupts_t()
      : target(pthread_self()),
        callback(nix::create_interrupt_callback([&]() { pthread_kill(target, SIGUSR1); })) {}
};

} // namespace nix
