#include "nix/util/signals.h"

#include <chrono>
#include <cstring>
#include <thread>

#include "nix/util/error.h"
#include "nix/util/logging.h"
#include "nix/util/sync.h"
#include "nix/util/terminal.h"
#include "nix/util/util.h"

namespace nix {

std::atomic<bool> unix::is_interrupted = false;

/* Issue #10559: Graceful shutdown support.
 * First Ctrl-C sets graceful_shutdown flag (finish current operation).
 * Second Ctrl-C within 2 seconds forces immediate termination.
 * Counter resets after 2 seconds of no interrupts.
 */
std::atomic<bool> unix::graceful_shutdown_requested = false;
static std::atomic<int> interrupt_count{0};
static std::atomic<std::chrono::steady_clock::time_point> last_interrupt_time{
    std::chrono::steady_clock::time_point{}};

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

/**
 * Issue #14300: Use function-local static to avoid initialization order issues
 * that can cause "mutex lock failed: Invalid argument" errors.
 * The mutex inside sync_t must be fully initialized before any thread tries
 * to lock it. By using a function-local static, we guarantee initialization
 * on first use (which is thread-safe in C++11+) rather than relying on
 * static initialization order.
 */
static sync_t<interrupt_callbacks_t>& get_interrupt_callbacks() {
  static sync_t<interrupt_callbacks_t> instance;
  return instance;
}

/* Suspend callbacks - invoked when SIGTSTP is received (before suspending) */
struct suspend_callbacks_t {
  typedef int64_t Token;
  Token next_token = 0;
  std::map<Token, std::function<void()>> callbacks;
};

/**
 * Issue #14300: Use function-local static for suspend callbacks too.
 */
static sync_t<suspend_callbacks_t>& get_suspend_callbacks() {
  static sync_t<suspend_callbacks_t> instance;
  return instance;
}

static void invoke_suspend_callbacks() {
  suspend_callbacks_t::Token i = 0;
  while (true) {
    std::function<void()> callback;
    {
      auto sc_lock(get_suspend_callbacks().lock());
      auto lb = sc_lock->callbacks.lower_bound(i);
      if (lb == sc_lock->callbacks.end()) {
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

/* Issue #10559: Timeout for double Ctrl-C detection (2 seconds) */
static constexpr auto kGracefulShutdownTimeout = std::chrono::seconds(2);

static void signal_handler_thread(sigset_t set) {
  while (true) {
    int signal = 0;
    sigwait(&set, &signal);

    if (signal == SIGINT || signal == SIGTERM || signal == SIGHUP) {
      auto now = std::chrono::steady_clock::now();
      auto last_time = last_interrupt_time.load(std::memory_order_relaxed);
      auto time_since_last = now - last_time;

      /* Reset counter if more than 2 seconds have passed since last interrupt */
      if (time_since_last > kGracefulShutdownTimeout) {
        interrupt_count.store(0, std::memory_order_relaxed);
      }

      int count = interrupt_count.fetch_add(1, std::memory_order_relaxed) + 1;
      last_interrupt_time.store(now, std::memory_order_relaxed);

      if (count == 1) {
        /* First interrupt: request graceful shutdown */
        unix::graceful_shutdown_requested.store(true, std::memory_order_release);
        /* Write directly to stderr to avoid heap allocation in signal context */
        const char* msg = "\nInterrupt received, finishing current operation... "
                          "(press Ctrl-C again to force quit)\n";
        [[maybe_unused]] auto _ = write(STDERR_FILENO, msg, strlen(msg));
      }

      /* Always trigger interrupt - this sets is_interrupted and calls callbacks */
      unix::trigger_interrupt();

    } else if (signal == SIGWINCH) {
      update_window_size();

    } else if (signal == SIGTSTP) {
      /* Invoke suspend callbacks to propagate SIGTSTP to child processes */
      invoke_suspend_callbacks();

      /* Restore default SIGTSTP handler and re-raise to actually suspend.
         After resuming, re-block SIGTSTP so we can catch it again. */
      struct sigaction sa_default{}, sa_old{};
      sa_default.sa_handler = SIG_DFL;
      sigemptyset(&sa_default.sa_mask);
      sigaction(SIGTSTP, &sa_default, &sa_old);

      sigset_t unblock_set;
      sigemptyset(&unblock_set);
      sigaddset(&unblock_set, SIGTSTP);
      pthread_sigmask(SIG_UNBLOCK, &unblock_set, nullptr);

      raise(SIGTSTP);

      /* After SIGCONT, re-block SIGTSTP */
      pthread_sigmask(SIG_BLOCK, &unblock_set, nullptr);
      sigaction(SIGTSTP, &sa_old, nullptr);
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
        auto ic_lock(get_interrupt_callbacks().lock());
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
  sigaddset(&set, SIGTSTP);
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
    auto ic_lock(get_interrupt_callbacks().lock());
    ic_lock->callbacks.erase(token);
  }
};

std::unique_ptr<interrupt_callback_t> create_interrupt_callback(std::function<void()> callback) {
  auto ic_lock(get_interrupt_callbacks().lock());
  auto token = ic_lock->next_token++;
  ic_lock->callbacks.emplace(token, callback);

  std::unique_ptr<interrupt_callback_impl_t> res{new interrupt_callback_impl_t{}};
  res->token = token;

  return std::unique_ptr<interrupt_callback_t>(res.release());
}

/* RAII helper to automatically deregister a suspend callback. */
struct suspend_callback_impl_t : suspend_callback_t {
  suspend_callbacks_t::Token token;

  ~suspend_callback_impl_t() override {
    auto sc_lock(get_suspend_callbacks().lock());
    sc_lock->callbacks.erase(token);
  }
};

std::unique_ptr<suspend_callback_t> create_suspend_callback(std::function<void()> callback) {
  auto sc_lock(get_suspend_callbacks().lock());
  auto token = sc_lock->next_token++;
  sc_lock->callbacks.emplace(token, callback);

  std::unique_ptr<suspend_callback_impl_t> res{new suspend_callback_impl_t{}};
  res->token = token;

  return std::unique_ptr<suspend_callback_t>(res.release());
}

void unix::reset_graceful_shutdown() {
  graceful_shutdown_requested.store(false, std::memory_order_release);
  interrupt_count.store(0, std::memory_order_relaxed);
  is_interrupted.store(false, std::memory_order_release);
}

} // namespace nix
