#include "nix/util/signals.h"

#include <thread>

#include "nix/util/error.h"
#include "nix/util/sync.h"
#include "nix/util/terminal.h"
#include "nix/util/util.h"

namespace nix {

std::atomic<bool> unix::is_interrupted = false;

thread_local std::function<bool()> unix::interrupt_check;

void unix::_interrupted() {
  /* Block user interrupts while an exception is being handled.
     Throwing an exception while another exception is being handled
     kills the program! */
  if (!std::uncaught_exceptions()) {
    throw Interrupted("interrupted by the user");
  }
}

//////////////////////////////////////////////////////////////////////

/* We keep track of interrupt callbacks using integer tokens, so we can iterate
   safely without having to lock the data structure while executing arbitrary
   functions.
 */
struct interrupt_callbacks_t {
  typedef int64_t Token;

  /* We use unique tokens so that we can't accidentally delete the wrong
     handler because of an erroneous double delete. */
  Token next_token = 0;

  /* Used as a list, see interrupt_callbacks_t comment. */
  std::map<Token, std::function<void()>> callbacks;
};

static sync_t<interrupt_callbacks_t> interrupt_callbacks;

static void signal_handler_thread(sigset_t set) {
  while (true) {
    int signal = 0;
    sigwait(&set, &signal);

    if (signal == SIGINT || signal == SIGTERM || signal == SIGHUP) {
      unix::trigger_interrupt();

    } else if (signal == SIGWINCH) {
      update_window_size();
    }
  }
}

void unix::trigger_interrupt() {
  is_interrupted = true;

  {
    interrupt_callbacks_t::Token i = 0;
    while (true) {
      std::function<void()> callback;
      {
        auto ic_lock(interrupt_callbacks.lock());
        auto lb = ic_lock->callbacks.lower_bound(i);
        if (lb == ic_lock->callbacks.end()) {
          break;
        }

        callback = lb->second;
        i = lb->first + 1;
      }

      try {
        callback();
      } catch (...) {
        ignore_exception_in_destructor();
      }
    }
  }
}

static sigset_t saved_signal_mask;
static bool saved_signal_mask_is_set = false;

void unix::save_signal_mask() {
  if (sigprocmask(SIG_BLOCK, nullptr, &saved_signal_mask)) {
    throw sys_error_t("querying signal mask");
  }

  saved_signal_mask_is_set = true;
}

void unix::start_signal_handler_thread() {
  update_window_size();

  save_signal_mask();

  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGHUP);
  sigaddset(&set, SIGPIPE);
  sigaddset(&set, SIGWINCH);
  if (pthread_sigmask(SIG_BLOCK, &set, nullptr)) {
    throw sys_error_t("blocking signals");
  }

  std::thread(signal_handler_thread, set).detach();
}

void unix::restore_signals() {
  // If startSignalHandlerThread wasn't called, that means we're not running
  // in a proper libmain process, but a process that presumably manages its
  // own signal handlers. Such a process should call either
  //  - initNix(), to be a proper libmain process
  //  - startSignalHandlerThread(), to resemble libmain regarding signal
  //    handling only
  //  - saveSignalMask(), for processes that define their own signal handling
  //    thread
  // TODO: Warn about this? Have a default signal mask? The latter depends on
  //       whether we should generally inherit signal masks from the caller.
  //       I don't know what the larger unix ecosystem expects from us here.
  if (!saved_signal_mask_is_set) {
    return;
  }

  if (sigprocmask(SIG_SETMASK, &saved_signal_mask, nullptr)) {
    throw sys_error_t("restoring signals");
  }
}

/* RAII helper to automatically deregister a callback. */
struct interrupt_callback_impl_t : interrupt_callback_t {
  interrupt_callbacks_t::Token token;

  ~interrupt_callback_impl_t() override {
    auto ic_lock(interrupt_callbacks.lock());
    ic_lock->callbacks.erase(token);
  }
};

std::unique_ptr<interrupt_callback_t> create_interrupt_callback(std::function<void()> callback) {
  auto ic_lock(interrupt_callbacks.lock());
  auto token = ic_lock->next_token++;
  ic_lock->callbacks.emplace(token, callback);

  std::unique_ptr<interrupt_callback_impl_t> res{new interrupt_callback_impl_t{}};
  res->token = token;

  return std::unique_ptr<interrupt_callback_t>(res.release());
}

} // namespace nix
