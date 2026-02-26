#include "nix/util/error.h"

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <compare>
#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nix/util/environment-variables.h"
#include "nix/util/fmt.h"
#include "nix/util/logging.h"
#include "nix/util/position.h"
#include "nix/util/terminal.h"

namespace nix {

void base_error_t::add_trace(std::shared_ptr<const pos_t>&& pos, const hint_fmt_t& hint,
                             trace_print_t print) {
  err_.traces_.push_front(trace_t{.pos_ = std::move(pos), .hint_ = hint, .print_ = print});
}

void throw_exception_self_check() {
  // This is meant to be caught in initLibUtil()
  throw Error("C++ exception handling is broken. This would appear to be a problem with the way "
              "Nix was compiled and/or linked and/or loaded.");
}

// c++ std::exception descendants must have a 'const char* what()' function.
// This stringifies the error and caches it for use by what(), or similarly by msg().
auto base_error_t::calc_what() const -> const std::string& {
  if (what_.has_value()) {
    return *what_;
  }
  string_sink_t oss;
  show_error_info(oss, err_, logger_settings.show_trace);
  what_ = oss.str();
  return *what_;
}

std::optional<std::string> error_info_t::program_name = std::nullopt;

auto operator<<(std::ostream& out, const hint_fmt_t& hint) -> std::ostream& {
  return out << hint.str();
}

/**
 * An arbitrarily defined value comparison for the purpose of using traces in the key of a sorted
 * container.
 */
inline auto operator<=>(const trace_t& lhs, const trace_t& rhs) -> std::strong_ordering {
  // `std::shared_ptr` does not have value semantics for its comparison
  // functions, so we need to check for nulls and compare the dereferenced
  // values here.
  if (lhs.pos_ != rhs.pos_) {
    // Compare null status first
    const int lhs_has_pos = lhs.pos_ ? 1 : 0;
    const int rhs_has_pos = rhs.pos_ ? 1 : 0;
    if (auto cmp = lhs_has_pos <=> rhs_has_pos; cmp != 0) {
      return cmp;
    }
    if (auto cmp = *lhs.pos_ <=> *rhs.pos_; cmp != 0) {
      return cmp;
    }
  }
  // This formats a freshly formatted hint string and then throws it away, which
  // shouldn't be much of a problem because it only runs when pos is equal, and this function is
  // used for trace printing, which is infrequent.
  return lhs.hint_.str() <=> rhs.hint_.str();
}

// print lines of code to the ostream, indicating the error column.
void print_code_lines(std::ostream& out, const std::string& prefix, const pos_t& err_pos,
                      const lines_of_code_t& loc) {
  // previous line of code.
  if (loc.prev_line_of_code_.has_value()) {
    out << '\n' << fmt("%1% %|2$5d|| %3%", prefix, (err_pos.line - 1), *loc.prev_line_of_code_);
  }

  if (loc.err_line_of_code_.has_value()) {
    // line of code containing the error.
    out << '\n' << fmt("%1% %|2$5d|| %3%", prefix, (err_pos.line), *loc.err_line_of_code_);
    // error arrows for the column range.
    if (err_pos.column > 0) {
      const auto start = static_cast<std::size_t>(err_pos.column);
      const std::string spaces_str(start, ' ');
      constexpr const char* arrow_str = "^";

      out << '\n' << fmt("%1%      |%2%" ANSI_RED "%3%" ANSI_NORMAL, prefix, spaces_str, arrow_str);
    }
  }

  // next line of code.
  if (loc.next_line_of_code_.has_value()) {
    out << '\n' << fmt("%1% %|2$5d|| %3%", prefix, (err_pos.line + 1), *loc.next_line_of_code_);
  }
}

namespace {

auto indent(std::string_view indent_first, std::string_view indent_rest, std::string_view str)
    -> std::string {
  std::string res;
  bool first = true;

  while (!str.empty()) {
    auto end = str.find('\n');
    if (!first) {
      res += "\n";
    }
    res += chomp(std::string(first ? indent_first : indent_rest) + std::string(str.substr(0, end)));
    first = false;
    if (end == std::string_view::npos) {
      break;
    }
    str = str.substr(end + 1);
  }

  return res;
}

/**
 * A development aid for finding missing positions, to improve error messages. Example use:
 *
 *     _NIX_EVAL_SHOW_UNKNOWN_LOCATIONS=1 _NIX_TEST_ACCEPT=1 make tests/lang.sh.test
 *     git diff -U20 tests
 *
 */
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,cert-err58-cpp)
bool print_unknown_locations = get_env("_NIX_EVAL_SHOW_UNKNOWN_LOCATIONS").has_value();

/**
 * Print a position, if it is known.
 *
 * @return true if a position was printed.
 */
auto print_pos_maybe(std::ostream& oss, std::string_view indent_str,
                     const std::shared_ptr<const pos_t>& pos) -> bool {
  const bool has_pos = pos && *pos;
  if (has_pos) {
    oss << indent_str << ANSI_BLUE << "at " ANSI_WARNING << *pos << ANSI_NORMAL << ":";

    if (auto loc = pos->get_code_lines()) {
      print_code_lines(oss, "", *pos, *loc);
      oss << "\n";
    }
  } else if (print_unknown_locations) {
    oss << "\n"
        << indent_str << ANSI_BLUE << "at " ANSI_RED << "UNKNOWN LOCATION" << ANSI_NORMAL << "\n";
  }
  return has_pos;
}

void print_trace(std::ostream& output, std::string_view indent_str, std::size_t& count,
                 const trace_t& trace) {
  output << "\n" << "… " << trace.hint_.str() << "\n";

  if (print_pos_maybe(output, indent_str, trace.pos_)) {
    count++;
  }
}

void print_skipped_traces_maybe(std::ostream& output, std::string_view indent_str,
                                std::size_t& count, std::vector<trace_t>& skipped_traces,
                                std::set<trace_t> traces_seen) {
  if (!skipped_traces.empty()) {
    // If we only skipped a few frames, print them out normally;
    // messages like "1 duplicate frames omitted" aren't helpful.
    constexpr std::size_t max_skipped_traces_to_print = 5U;
    if (skipped_traces.size() <= max_skipped_traces_to_print) {
      for (auto& trace : skipped_traces) {
        print_trace(output, indent_str, count, trace);
      }
    } else {
      output << "\n"
             << ANSI_WARNING "(" << skipped_traces.size()
             << " duplicate frames omitted)" ANSI_NORMAL << "\n";
      // Clear the set of "seen" traces after printing a chunk of
      // `duplicate frames omitted`.
      //
      // Consider a mutually recursive stack trace with:
      // - 10 entries of A
      // - 10 entries of B
      // - 10 entries of A
      //
      // If we don't clear `tracesSeen` here, we would print output like this:
      // - 1 entry of A
      // - (9 duplicate frames omitted)
      // - 1 entry of B
      // - (19 duplicate frames omitted)
      //
      // This would obscure the control flow, which went from A,
      // to B, and back to A again.
      //
      // In contrast, if we do clear `tracesSeen`, the output looks like this:
      // - 1 entry of A
      // - (9 duplicate frames omitted)
      // - 1 entry of B
      // - (9 duplicate frames omitted)
      // - 1 entry of A
      // - (9 duplicate frames omitted)
      //
      // See: `tests/functional/lang/eval-fail-mutual-recursion.nix`
      traces_seen.clear();
    }
  }
  // We've either printed each trace in `skippedTraces` normally, or
  // printed a chunk of `duplicate frames omitted`. Either way, we've
  // processed these traces and can clear them.
  skipped_traces.clear();
}

} // namespace

auto show_error_info(std::ostream& out, const error_info_t& einfo, bool show_trace)
    -> std::ostream& {
  std::string prefix;
  switch (einfo.level_) {
    case verbosity_t::lvl_error: {
      prefix = ANSI_RED "error";
      break;
    }
    case verbosity_t::lvl_notice: {
      prefix = ANSI_RED "note";
      break;
    }
    case verbosity_t::lvl_warn: {
      if (einfo.is_from_expr_) {
        prefix = ANSI_WARNING "evaluation warning";
      } else {
        prefix = ANSI_WARNING "warning";
      }
      break;
    }
    case verbosity_t::lvl_info: {
      prefix = ANSI_GREEN "info";
      break;
    }
    case verbosity_t::lvl_talkative: {
      prefix = ANSI_GREEN "talk";
      break;
    }
    case verbosity_t::lvl_chatty: {
      prefix = ANSI_GREEN "chat";
      break;
    }
    case verbosity_t::lvl_vomit: {
      prefix = ANSI_GREEN "vomit";
      break;
    }
    case verbosity_t::lvl_debug: {
      prefix = ANSI_WARNING "debug";
      break;
    }
    default:
      assert(false);
  }

  // FIXME: show the program name as part of the trace?
  if (einfo.program_name && einfo.program_name != error_info_t::program_name) {
    prefix += fmt(" [%s]:" ANSI_NORMAL " ", einfo.program_name.value_or(""));
  } else {
    prefix += ":" ANSI_NORMAL " ";
  }

  string_sink_t oss;

  /*
   * Traces
   * ------
   *
   *  The semantics of traces is a bit weird. We have only one option to
   *  print them and to make them verbose (--show-trace). In the code they
   *  are always collected, but they are not printed by default. The code
   *  also collects more traces when the option is on. This means that there
   *  is no way to print the simplified traces at all.
   *
   *  I (layus) designed the code to attach positions to a restricted set of
   *  messages. This means that we have  a lot of traces with no position at
   *  all, including most of the base error messages. For example "type
   *  error: found a string while a set was expected" has no position, but
   *  will come with several traces detailing it's precise relation to the
   *  closest know position. This makes erroring without printing traces
   *  quite useless.
   *
   *  This is why I introduced the idea to always print a few traces on
   *  error. The number 3 is quite arbitrary, and was selected so as not to
   *  clutter the console on error. For the same reason, a trace with an
   *  error position takes more space, and counts as two traces towards the
   *  limit.
   *
   *  The rest is truncated, unless --show-trace is passed. This preserves
   *  the same bad semantics of --show-trace to both show the trace and
   *  augment it with new data. Not too sure what is the best course of
   *  action.
   *
   *  The issue is that it is fundamentally hard to provide a trace for a
   *  lazy language. The trace will only cover the current spine of the
   *  evaluation, missing things that have been evaluated before. For
   *  example, most type errors are hard to inspect because there is not
   *  trace for the faulty value. These errors should really print the faulty
   *  value itself.
   *
   *  In function calls, the --show-trace flag triggers extra traces for each
   *  function invocation. These work as scopes, allowing to follow the
   *  current spine of the evaluation graph. Without that flag, the error
   *  trace should restrict itself to a restricted prefix of that trace,
   *  until the first scope. If we ever get to such a precise error
   *  reporting, there would be no need to add an arbitrary limit here. We
   *  could always print the full trace, and it would just be small without
   *  the flag.
   *
   *  One idea I had is for XxxError.add_trace() to perform nothing if one
   *  scope has already been traced. Alternatively, we could stop here when
   *  we encounter such a scope instead of after an arbitrary number of
   *  traces. This however requires to augment traces with the notion of
   *  "scope".
   *
   *  This is particularly visible in code like evalAttrs(...) where we have
   *  to make a decision between the two following options.
   *
   *  ``` long traces
   *  inline void eval_state_t::evalAttrs(Env & env, expr_t * e, value_t & v, const pos_t & pos,
   * std::string_view error_ctx)
   *  {
   *      try {
   *          e->eval(*this, env, v);
   *          if (v.type() != nAttrs)
   *              error<TypeError>("expected a set but found %1%", v);
   *      } catch (Error & e) {
   *          e.add_trace(pos, error_ctx);
   *          throw;
   *      }
   *  }
   *  ```
   *
   *  ``` short traces
   *  inline void eval_state_t::evalAttrs(Env & env, expr_t * e, value_t & v, const pos_t & pos,
   * std::string_view error_ctx)
   *  {
   *      e->eval(*this, env, v);
   *      try {
   *          if (v.type() != nAttrs)
   *              error<TypeError>("expected a set but found %1%", v);
   *      } catch (Error & e) {
   *          e.add_trace(pos, error_ctx);
   *          throw;
   *      }
   *  }
   *  ```
   *
   *  The second example can be rewritten more concisely, but kept in this
   *  form to highlight the symmetry. The first option adds more information,
   *  because whatever caused an error down the line, in the generic eval
   *  function, will get annotated with the code location that uses and
   *  required it. The second option is less verbose, but does not provide
   *  any context at all as to where and why a failing value was required.
   *
   *  Scopes would fix that, by adding context only when --show-trace is
   *  passed, and keeping the trace terse otherwise.
   *
   */

  // Enough indent to align with with the `... `
  // prepended to each element of the trace
  const auto* ellipsis_indent = "  ";

  if (!einfo.traces_.empty()) {
    // Stack traces seen since we last printed a chunk of `duplicate frames
    // omitted`.
    std::set<trace_t> traces_seen;
    // A consecutive sequence of stack traces that are all in `tracesSeen`.
    std::vector<trace_t> skipped_traces;
    size_t count = 0;
    bool truncate = false;

    for (const auto& trace : einfo.traces_) {
      if (trace.hint_.str().empty()) {
        continue;
      }

      if (!show_trace && count > 3) {
        truncate = true;
      }

      if (!truncate || trace.print_ == trace_print_t::always) {
        if (traces_seen.count(trace)) {
          skipped_traces.push_back(trace);
          continue;
        }

        traces_seen.insert(trace);

        print_skipped_traces_maybe(oss, ellipsis_indent, count, skipped_traces, traces_seen);

        count++;

        print_trace(oss, ellipsis_indent, count, trace);
      }
    }

    print_skipped_traces_maybe(oss, ellipsis_indent, count, skipped_traces, traces_seen);

    if (truncate) {
      oss << "\n"
          << ANSI_WARNING
          "(stack trace truncated; use '--show-trace' to show the full, detailed trace)" ANSI_NORMAL
          << "\n";
    }

    oss << "\n" << prefix;
  }

  oss << einfo.msg_ << "\n";

  print_pos_maybe(oss, "", einfo.pos_);

  auto suggestions = einfo.suggestions_.trim();
  if (!suggestions.suggestions.empty()) {
    oss << "Did you mean " << suggestions.trim() << "?" << std::endl;
  }

  out << indent(prefix, std::string(filter_ansi_escapes(prefix, true).size(), ' '),
                chomp(oss.str()));

  return out;
}

/** Write to stderr in a robust and minimal way, considering that the process
 * may be in a bad state.
 */
static void write_err(std::string_view buf) {
  while (!buf.empty()) {
    auto n = write(STDERR_FILENO, buf.data(), buf.size());
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      abort();
    }
    buf = buf.substr(n);
  }
}

void panic(std::string_view msg) {
  write_err("\n\n" ANSI_RED
            "terminating due to unexpected unrecoverable internal error: " ANSI_NORMAL);
  write_err(msg);
  write_err("\n");
  std::terminate();
}

void unreachable(std::source_location loc) {
  char buf[512];
  int n = snprintf(buf, sizeof(buf), "Unexpected condition in %s at %s:%" PRIuLEAST32,
                   loc.function_name(), loc.file_name(), loc.line());
  if (n < 0) {
    panic("Unexpected condition and could not format error message");
  }
  panic(std::string_view(buf, std::min(static_cast<int>(sizeof(buf)), n)));
}

} // namespace nix
