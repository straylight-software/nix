#include <cstdio>
#include <fstream>

#include <signal.h>

#include "cmd-config-private.h"

#if USE_READLINE
#  include <readline/history.h>
#  include <readline/readline.h>
#else
// editline < 1.15.2 don't wrap their API for C++ usage
// (added in https://github.com/troglobit/editline/commit/91398ceb3427b730995357e9d120539fb9bb7461).
// This results in linker errors due to to name-mangling of editline C symbols.
// For compatibility with these versions, we wrap the API here
// (wrapping multiple times on newer versions is no problem).
extern "C" {
#  include <editline.h>
}
#endif

#include "nix/cmd/repl-interacter.h"
#include "nix/cmd/repl.h"
#include "nix/util/environment-variables.h"
#include "nix/util/file-system.h"
#include "nix/util/finally.h"
#include "nix/util/signals.h"

namespace nix {

namespace {
// Used to communicate to NixRepl::getLine whether a signal occurred in ::readline.
volatile sig_atomic_t g_signal_received = 0;

void sigint_handler(int signo) {
  g_signal_received = signo;
}
}; // namespace

static detail::ReplCompleterMixin* cur_repl; // ugly

#if !USE_READLINE
static char* completion_callback(char* s, int* match) {
  auto possible = cur_repl->complete_prefix(s);
  if (possible.size() == 1) {
    *match = 1;
    auto* res = strdup(possible.begin()->c_str() + strlen(s));
    if (!res)
      throw Error("allocation failure");
    return res;
  } else if (possible.size() > 1) {
    auto check_all_have_same_at = [&](size_t pos) {
      auto& first = *possible.begin();
      for (auto& p : possible) {
        if (p.size() <= pos || p[pos] != first[pos])
          return false;
      }
      return true;
    };
    size_t start = strlen(s);
    size_t len = 0;
    while (check_all_have_same_at(start + len))
      ++len;
    if (len > 0) {
      *match = 1;
      auto* res = strdup(std::string(*possible.begin(), start, len).c_str());
      if (!res)
        throw Error("allocation failure");
      return res;
    }
  }

  *match = 0;
  return nullptr;
}

static int list_possible_callback(char* s, char*** avp) {
  auto possible = cur_repl->complete_prefix(s);

  if (possible.size() > (std::numeric_limits<int>::max() / sizeof(char*)))
    throw Error("too many completions");

  int ac = 0;
  char** vp = nullptr;

  auto check = [&](auto* p) {
    if (!p) {
      if (vp) {
        while (--ac >= 0)
          free(vp[ac]);
        free(vp);
      }
      throw Error("allocation failure");
    }
    return p;
  };

  vp = check((char**)malloc(possible.size() * sizeof(char*)));

  for (auto& p : possible)
    vp[ac++] = check(strdup(p.c_str()));

  *avp = vp;

  return ac;
}
#endif

// Workaround for editline's read_history() which uses a fixed 256-byte buffer,
// causing lines longer than 255 chars to be split. (NixOS/nix#15162)
// We implement our own history read that handles arbitrary line lengths.
static void read_history_unlimited(const std::string& path) {
  std::ifstream file(path);
  if (!file)
    return;

  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty())
      add_history(line.c_str());
  }
}

ReadlineLikeInteracter::Guard ReadlineLikeInteracter::init(detail::ReplCompleterMixin* repl) {
  // Allow nix-repl specific settings in .inputrc
  rl_readline_name = "nix-repl";
  try {
    create_dirs(dir_of(historyFile));
  } catch (SystemError& e) {
    logWarning(e.info());
  }
#if !USE_READLINE
  el_hist_size = 1000;
#endif
#if USE_READLINE
  read_history(historyFile.c_str());
#else
  // Use our own implementation to avoid editline's 256-byte line limit
  read_history_unlimited(historyFile);
#endif
  auto oldRepl = cur_repl;
  cur_repl = repl;
  Guard restoreRepl([oldRepl] { cur_repl = oldRepl; });
#if !USE_READLINE
  rl_set_complete_func(completion_callback);
  rl_set_list_possib_func(list_possible_callback);
#endif
  return restoreRepl;
}

static constexpr const char* prompt_for_type(ReplPromptType prompt_type) {
  switch (prompt_type) {
    case ReplPromptType::ReplPrompt:
      return "nix-repl> ";
    case ReplPromptType::ContinuationPrompt:
      return "        > "; // 9 spaces + >
  }
  assert(false);
}

bool ReadlineLikeInteracter::get_line(std::string& input, ReplPromptType prompt_type) {
#ifndef _WIN32 // TODO use more signals.hh for this
  struct sigaction act, old;
  sigset_t saved_signal_mask, set;

  auto setupSignals = [&]() {
    act.sa_handler = sigint_handler;
    sigfillset(&act.sa_mask);
    act.sa_flags = 0;
    if (sigaction(SIGINT, &act, &old))
      throw sys_error_t("installing handler for SIGINT");

    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    if (sigprocmask(SIG_UNBLOCK, &set, &saved_signal_mask))
      throw sys_error_t("unblocking SIGINT");
  };
  auto restore_signals = [&]() {
    if (sigprocmask(SIG_SETMASK, &saved_signal_mask, nullptr))
      throw sys_error_t("restoring signals");

    if (sigaction(SIGINT, &old, 0))
      throw sys_error_t("restoring handler for SIGINT");
  };

  setupSignals();
#endif
  char* s = readline(prompt_for_type(prompt_type));
  finally_t doFree([&]() { free(s); });
#ifndef _WIN32 // TODO use more signals.hh for this
  restore_signals();
#endif

  if (g_signal_received) {
    g_signal_received = 0;
    input.clear();
    return true;
  }

  // editline doesn't echo the input to the output when non-interactive, unlike readline
  // this results in a different behavior when running tests. The echoing is
  // quite useful for reading the test output, so we add it here.
  if (auto e = get_env("_NIX_TEST_REPL_ECHO"); s && e && *e == "1") {
#if !USE_READLINE
    // This is probably not right for multi-line input, but we don't use that
    // in the characterisation tests, so it's fine.
    std::cout << prompt_for_type(prompt_type) << s << std::endl;
#endif
  }

  if (!s)
    return false;
  input += s;
  input += '\n';

  return true;
}

ReadlineLikeInteracter::~ReadlineLikeInteracter() {
  write_history(historyFile.c_str());
}

}; // namespace nix
