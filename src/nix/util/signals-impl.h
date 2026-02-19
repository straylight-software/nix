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

extern std::atomic<bool> _isInterrupted;

extern thread_local std::function<bool()> interruptCheck;

void _interrupted();

/**
 * Start a thread that handles various signals. Also block those signals
 * on the current thread (and thus any threads created by it).
 * Saves the signal mask before changing the mask to block those signals.
 * See saveSignalMask().
 */
void startSignalHandlerThread();

/**
 * Saves the signal mask, which is the signal mask that nix will restore
 * before creating child processes.
 */
void saveSignalMask();

/**
 * To use in a process that already called `startSignalHandlerThread()`
 * or `saveSignalMask()` first.
 */
void restoreSignals();

void triggerInterrupt();

} // namespace unix

static inline void setInterrupted(bool isInterrupted) {
  unix::_isInterrupted = isInterrupted;
}

static inline bool getInterrupted() {
  return unix::_isInterrupted;
}

static inline bool isInterrupted() {
  using namespace unix;
  return _isInterrupted || (interruptCheck && interruptCheck());
}

/**
 * Throw `Interrupted` exception if the process has been interrupted.
 *
 * Call this in long-running loops and between slow operations to terminate
 * them as needed.
 */
inline void checkInterrupt() {
  if (isInterrupted())
    unix::_interrupted();
}

/**
 * A RAII class that causes the current thread to receive SIGUSR1 when
 * the signal handler thread receives SIGINT. That is, this allows
 * SIGINT to be multiplexed to multiple threads.
 */
struct receive_interrupts_t {
  pthread_t target;
  std::unique_ptr<interrupt_callback_t> callback;

  receive_interrupts_t()
      : target(pthread_self()),
        callback(createInterruptCallback([&]() { pthread_kill(target, SIGUSR1); })) {}
};

} // namespace nix
