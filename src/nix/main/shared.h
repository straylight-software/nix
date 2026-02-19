#pragma once
///@file

#include <signal.h>

#include "nix/main/common-args.h"
#include "nix/store/derived-path.h"
#include "nix/store/path.h"
#include "nix/util/args.h"
#include "nix/util/args/root.h"
#include "nix/util/file-descriptor.h"
#include "nix/util/processes.h"

namespace nix {

int handle_exceptions(const std::string& program_name, std::function<void()> fun);

/**
 * Don't forget to call init_plugins() after settings are initialized!
 * @param load_config Whether to load configuration from `nix.conf`, `NIX_CONFIG`, etc. May be
 * disabled for unit tests.
 */
void init_nix(bool load_config = true);

void parse_cmd_line(
    int argc, char** argv,
    std::function<bool(strings_t::iterator& arg, const strings_t::iterator& end)> parse_arg);

void parse_cmd_line(
    const std::string& program_name, const strings_t& args,
    std::function<bool(strings_t::iterator& arg, const strings_t::iterator& end)> parse_arg);

std::string version();

void print_version(const std::string& program_name);

/**
 * Ugh.  No better place to put this.
 */
void print_gc_warning();

class Store;
struct MissingPaths;

void print_missing(ref<Store> store, const std::vector<DerivedPath>& paths, verbosity_t lvl = lvl_info);

void print_missing(ref<Store> store, const MissingPaths& missing, verbosity_t lvl = lvl_info);

std::string get_arg(const std::string& opt, strings_t::iterator& i, const strings_t::iterator& end);

template <class N>
N getIntArg(const std::string& opt, strings_t::iterator& i, const strings_t::iterator& end,
            bool allowUnit) {
  ++i;
  if (i == end)
    throw UsageError("'%1%' requires an argument", opt);
  return string2_int_with_unit_prefix<N>(*i);
}

struct LegacyArgs : public MixCommonArgs, public root_args_t {
  std::function<bool(strings_t::iterator& arg, const strings_t::iterator& end)> parse_arg;

  LegacyArgs(const std::string& program_name,
             std::function<bool(strings_t::iterator& arg, const strings_t::iterator& end)> parse_arg);

  bool process_flag(strings_t::iterator& pos, strings_t::iterator end) override;

  bool process_args(const strings_t& args, bool finish) override;
};

/**
 * The constructor of this class starts a pager if standard output is a
 * terminal and $PAGER is set. Standard output is redirected to the
 * pager.
 */
class RunPager {
public:
  RunPager();
  ~RunPager();

private:
#ifndef _WIN32 // TODO re-enable on Windows, once we can start processes.
  Pid pid;
#endif
  descriptor_t std_out;
};

extern volatile ::sig_atomic_t blockInt;

/* GC helpers. */

struct GCResults;

struct PrintFreed {
  bool show;
  const GCResults& results;

  PrintFreed(bool show, const GCResults& results) : show(show), results(results) {}

  ~PrintFreed();
};

#ifndef _WIN32
/**
 * Install a SIGSEGV handler to detect stack overflows.
 */
void detectStackOverflow();

/**
 * Pluggable behavior to run in case of a stack overflow.
 *
 * Default value: defaultStackOverflowHandler.
 *
 * This is called by the handler installed by detectStackOverflow().
 *
 * This gives Nix library consumers a limit opportunity to report the error
 * condition. The handler should exit the process.
 * See defaultStackOverflowHandler() for a reference implementation.
 *
 * NOTE: use with diligence, because this runs in the signal handler, with very
 * limited stack space and a potentially a corrupted heap, all while the failed
 * thread is blocked indefinitely. All functions called must be reentrant.
 */
extern std::function<void(siginfo_t* info, void* ctx)> stackOverflowHandler;

/**
 * The default, robust implementation of stackOverflowHandler.
 *
 * Prints an error message directly to stderr using a syscall instead of the
 * logger. Exits the process immediately after.
 */
void defaultStackOverflowHandler(siginfo_t* info, void* ctx);
#endif

} // namespace nix
