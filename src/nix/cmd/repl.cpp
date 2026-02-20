#include "nix/cmd/repl.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

#include "nix/cmd/common-eval-args.h"
#include "nix/cmd/editor-for.h"
#include "nix/cmd/markdown.h"
#include "nix/cmd/repl-interacter.h"
#include "nix/expr/attr-path.h"
#include "nix/expr/eval-settings.h"
#include "nix/expr/eval.h"
#include "nix/expr/get-drvs.h"
#include "nix/expr/print.h"
#include "nix/expr/value.h"
#include "nix/flake/flake.h"
#include "nix/flake/lockfile.h"
#include "nix/main/shared.h"
#include "nix/store/derivations.h"
#include "nix/store/globals.h"
#include "nix/store/local-fs-store.h"
#include "nix/store/log-store.h"
#include "nix/store/store-open.h"
#include "nix/util/ansicolor.h"
#include "nix/util/error.h"
#include "nix/util/finally.h"
#include "nix/util/ref.h"
#include "nix/util/signals.h"
#include "nix/util/strings.h"
#include "nix/util/users.h"

namespace nix {

/**
 * Returned by `nix_repl_t::process_line`.
 */
enum class process_line_result_t {
  /**
   * The user exited with `:quit`. The REPL should exit. The surrounding
   * program or evaluation (e.g., if the REPL was acting as the debugger)
   * should also exit.
   */
  quit,
  /**
   * The user exited with `:continue`. The REPL should exit, but the program
   * should continue running.
   */
  Continue,
  /**
   * The user did not exit. The REPL should request another line of input.
   */
  prompt_again,
};

struct nix_repl_t : AbstractNixRepl, detail::ReplCompleterMixin, gc {
  size_t debug_trace_index;

  std::list<std::filesystem::path> loaded_files;
  // Arguments passed to :load-flake, saved so they can be reloaded with :reload
  strings_t loaded_flakes;
  std::function<AnnotatedValues()> get_values;

  const static int env_size = 32768;
  std::shared_ptr<StaticEnv> static_env;
  value_t last_loaded;
  Env* env;
  int displ;
  string_set_t var_names;

  RunNix* run_nix_ptr;

  void run_nix(const std::string& program, const strings_t& args,
              const std::optional<std::string>& input = {});

  std::unique_ptr<ReplInteracter> interacter;

  nix_repl_t(const LookupPath& lookup_path, nix::ref<store_t> store, ref<eval_state_t> state,
          std::function<AnnotatedValues()> get_values, RunNix* run_nix);
  virtual ~nix_repl_t() = default;

  ReplExitStatus main_loop() override;
  void init_env() override;

  virtual string_set_t complete_prefix(const std::string& prefix) override;
  store_path_t get_derivation_path(value_t& v);
  process_line_result_t process_line(std::string line);

  void load_file(const std::filesystem::path& path);
  void load_flake(const std::string& flake_ref);
  void load_files();
  void load_flakes();
  void reload_files_and_flakes();
  void show_last_loaded();
  void add_attrs_to_scope(value_t& attrs);
  void add_var_to_scope(const symbol_t name, value_t& v);
  expr_t* parse_string(std::string s);
  void eval_string(std::string s, value_t& v);
  void load_debug_trace_env(DebugTrace& dt);

  void print_value(std::ostream& str, value_t& v,
                  unsigned int max_depth = std::numeric_limits<unsigned int>::max()) {
    // Hide the progress bar during printing because it might interfere
    auto suspension = logger->suspend();
    ::nix::print_value(*state, str, v,
                      PrintOptions{
                          .ansi_colors = true,
                          .force = true,
                          .derivationPaths = true,
                          .max_depth = max_depth,
                          .prettyIndent = 2,
                          .errors = ErrorPrintBehavior::ThrowTopLevel,
                      });
  }
};

std::string remove_whitespace(std::string s) {
  s = chomp(s);
  size_t n = s.find_first_not_of(" \n\r\t");
  if (n != std::string::npos)
    s = std::string(s, n);
  return s;
}

nix_repl_t::nix_repl_t(const LookupPath& lookup_path, nix::ref<store_t> store, ref<eval_state_t> state,
                 std::function<nix_repl_t::AnnotatedValues()> get_values, RunNix* run_nix)
    : AbstractNixRepl(state),
      debug_trace_index(0),
      get_values(get_values),
      static_env(new StaticEnv(nullptr, state->staticBaseEnv)),
      run_nix_ptr{run_nix},
      interacter(make_unique<ReadlineLikeInteracter>((get_data_dir() / "repl-history").string())) {}

static std::ostream& show_debug_trace(std::ostream& out, const pos_table_t& positions,
                                    const DebugTrace& dt) {
  if (dt.isError)
    out << ANSI_RED "error: " << ANSI_NORMAL;
  out << dt.hint.str() << "\n";

  auto pos = dt.getPos(positions);

  if (pos) {
    out << pos;
    if (auto loc = pos.get_code_lines()) {
      out << "\n";
      print_code_lines(out, "", pos, *loc);
      out << "\n";
    }
  }

  return out;
}

make_error(IncompleteReplExpr, ParseError);

static bool is_first_repl = true;

ReplExitStatus nix_repl_t::main_loop() {
  if (is_first_repl) {
    std::string_view debugger_notice = "";
    if (state->debugRepl) {
      debugger_notice = " debugger";
    }
    notice("Nix %s\nType :? for help.", version(), debugger_notice);
  }

  is_first_repl = false;

  load_files();

  auto _guard = interacter->init(static_cast<detail::ReplCompleterMixin*>(this));

  std::string input;

  while (true) {
    // Hide the progress bar while waiting for user input, so that it won't interfere.
    {
      auto suspension = logger->suspend();
      // When continuing input from previous lines, don't print a prompt, just align to the same
      // number of chars as the prompt.
      if (!interacter->get_line(input, input.empty() ? ReplPromptType::ReplPrompt
                                                    : ReplPromptType::ContinuationPrompt)) {
        // Ctrl-D should exit the debugger.
        state->debugStop = false;
        logger->cout("");
        // TODO: Should Ctrl-D exit just the current debugger session or
        // the entire program?
        return ReplExitStatus::QuitAll;
      }
      // `suspension` resumes the logger
    }
    try {
      switch (process_line(input)) {
        case process_line_result_t::quit:
          return ReplExitStatus::QuitAll;
        case process_line_result_t::Continue:
          return ReplExitStatus::Continue;
        case process_line_result_t::prompt_again:
          break;
        default:
          unreachable();
      }
    } catch (IncompleteReplExpr&) {
      continue;
    } catch (Error& e) {
      printMsg(lvl_error, e.msg());
    } catch (Interrupted& e) {
      printMsg(lvl_error, e.msg());
    }

    // We handled the current input fully, so we should clear it
    // and read brand new input.
    input.clear();
    std::cout << std::endl;
  }
}

string_set_t nix_repl_t::complete_prefix(const std::string& prefix) {
  string_set_t completions;

  size_t start = prefix.find_last_of(" \n\r\t(){}[]");
  std::string prev, cur;
  if (start == std::string::npos) {
    prev = "";
    cur = prefix;
  } else {
    prev = std::string(prefix, 0, start + 1);
    cur = std::string(prefix, start + 1);
  }

  size_t slash, dot;

  if ((slash = cur.rfind('/')) != std::string::npos) {
    try {
      auto dir = std::string(cur, 0, slash);
      auto prefix2 = std::string(cur, slash + 1);
      for (auto& entry : directory_iterator_t{dir == "" ? "/" : dir}) {
        check_interrupt();
        auto name = entry.path().filename().string();
        if (name[0] != '.' && has_prefix(name, prefix2))
          completions.insert(prev + entry.path().string());
      }
    } catch (Error&) {
    }
  } else if ((dot = cur.rfind('.')) == std::string::npos) {
    /* This is a variable name; look it up in the current scope. */
    string_set_t::iterator i = var_names.lower_bound(cur);
    while (i != var_names.end()) {
      if (i->substr(0, cur.size()) != cur)
        break;
      completions.insert(prev + *i);
      i++;
    }
  } else {
    /* Temporarily disable the debugger, to avoid re-entering readline. */
    auto debug_repl = state->debugRepl;
    state->debugRepl = nullptr;
    finally_t restore_debug([&]() { state->debugRepl = debug_repl; });
    try {
      /* This is an expression that should evaluate to an
         attribute set.  Evaluate it to get the names of the
         attributes. */
      auto expr = cur.substr(0, dot);
      auto cur2 = cur.substr(dot + 1);

      expr_t* e = parse_string(expr);
      value_t v;
      e->eval(*state, *env, v);
      state->forceAttrs(v, no_pos,
                        "while evaluating an attrset for the purpose of completion (this error "
                        "should not be displayed; file an issue?)");

      for (auto& i : *v.attrs()) {
        std::string_view name = state->symbols[i.name];
        if (name.substr(0, cur2.size()) != cur2)
          continue;
        completions.insert(concat_strings(prev, expr, ".", name));
      }

    } catch (ParseError& e) {
      // Quietly ignore parse errors.
    } catch (EvalError& e) {
      // Quietly ignore evaluation errors.
    } catch (BadURL& e) {
      // Quietly ignore BadURL flake-related errors.
    } catch (FileNotFound& e) {
      // Quietly ignore non-existent file being `import`-ed.
    }
  }

  return completions;
}

// FIXME: DRY and match or use the parser
static bool is_var_name(std::string_view s) {
  if (s.size() == 0)
    return false;
  char c = s[0];
  if ((c >= '0' && c <= '9') || c == '-' || c == '\'')
    return false;
  for (auto& i : s)
    if (!((i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z') || (i >= '0' && i <= '9') || i == '_' ||
          i == '-' || i == '\''))
      return false;
  return true;
}

store_path_t nix_repl_t::get_derivation_path(value_t& v) {
  auto package_info = get_derivation(*state, v, false);
  if (!package_info)
    throw Error("expression does not evaluate to a derivation, so I can't build it");
  auto drv_path = package_info->queryDrvPath();
  if (!drv_path)
    throw Error("expression did not evaluate to a valid derivation (no 'drvPath' attribute)");
  state->waitForPath(*drv_path);
  if (!state->store->isValidPath(*drv_path))
    throw Error("expression evaluated to invalid derivation '%s'",
                state->store->printStorePath(*drv_path));
  return *drv_path;
}

void nix_repl_t::load_debug_trace_env(DebugTrace& dt) {
  init_env();

  auto se = state->getStaticEnv(dt.expr);
  if (se) {
    auto vm = map_static_env_bindings(state->symbols, *se.get(), dt.env);

    // add staticenv vars.
    for (auto& [name, value] : *(vm.get()))
      add_var_to_scope(state->symbols.create(name), *value);
  }
}

process_line_result_t nix_repl_t::process_line(std::string line) {
  line = trim(line);
  if (line.empty())
    return process_line_result_t::prompt_again;

  set_interrupted(false);

  std::string command, arg;

  if (line[0] == ':') {
    size_t p = line.find_first_of(" \n\r\t");
    command = line.substr(0, p);
    if (p != std::string::npos)
      arg = remove_whitespace(line.substr(p));
  } else {
    arg = line;
  }

  if (command == ":?" || command == ":help") {
    // FIXME: convert to Markdown, include in the 'nix repl' manpage.
    std::cout
        << "The following commands are available:\n"
        << "\n"
        << "  <expr>                       Evaluate and print expression\n"
        << "  <x> = <expr>                 Bind expression to variable\n"
        << "  :a, :add <expr>              Add attributes from resulting set to scope\n"
        << "  :b <expr>                    Build a derivation\n"
        << "  :bl <expr>                   Build a derivation, creating GC roots in the\n"
        << "                               working directory\n"
        << "  :e, :edit <expr>             Open package or function in $EDITOR\n"
        << "  :i <expr>                    Build derivation, then install result into\n"
        << "                               current profile\n"
        << "  :l, :load <path>             Load Nix expression and add it to scope\n"
        << "  :lf, :load-flake <ref>       Load Nix flake and add it to scope\n"
        << "  :ll, :last-loaded            Show most recently loaded variables added to scope\n"
        << "  :p, :print <expr>            Evaluate and print expression recursively\n"
        << "                               Strings are printed directly, without escaping.\n"
        << "  :q, :quit                    Exit nix-repl\n"
        << "  :r, :reload                  Reload all files\n"
        << "  :sh <expr>                   Build dependencies of derivation, then start\n"
        << "                               nix-shell\n"
        << "  :t <expr>                    Describe result of evaluation\n"
        << "  :u <expr>                    Build derivation, then start nix-shell\n"
        << "  :doc <expr>                  Show documentation of a builtin function\n"
        << "  :log <expr>                  Show logs for a derivation\n"
        << "  :te, :trace-enable [bool]    Enable, disable or toggle showing traces for\n"
        << "                               errors\n"
        << "  :?, :help                    Brings up this help menu\n";
    if (state->debugRepl) {
      std::cout << "\n"
                << "        Debug mode commands\n"
                << "  :env             Show env stack\n"
                << "  :bt, :backtrace  Show trace stack\n"
                << "  :st              Show current trace\n"
                << "  :st <idx>        Change to another trace in the stack\n"
                << "  :c, :continue    Go until end of program, exception, or builtins.break\n"
                << "  :s, :step        Go one step\n";
    }

  }

  else if (state->debugRepl && (command == ":bt" || command == ":backtrace")) {
    for (const auto& [idx, i] : enumerate(state->debugTraces)) {
      std::cout << "\n" << ANSI_BLUE << idx << ANSI_NORMAL << ": ";
      show_debug_trace(std::cout, state->positions, i);
    }
  }

  else if (state->debugRepl && (command == ":env")) {
    for (const auto& [idx, i] : enumerate(state->debugTraces)) {
      if (idx == debug_trace_index) {
        print_env_bindings(*state, i.expr, i.env);
        break;
      }
    }
  }

  else if (state->debugRepl && (command == ":st")) {
    try {
      // change the DebugTrace index.
      debug_trace_index = stoi(arg);
    } catch (...) {
    }

    for (const auto& [idx, i] : enumerate(state->debugTraces)) {
      if (idx == debug_trace_index) {
        std::cout << "\n" << ANSI_BLUE << idx << ANSI_NORMAL << ": ";
        show_debug_trace(std::cout, state->positions, i);
        std::cout << std::endl;
        print_env_bindings(*state, i.expr, i.env);
        load_debug_trace_env(i);
        break;
      }
    }
  }

  else if (state->debugRepl && (command == ":s" || command == ":step")) {
    // set flag to stop at next DebugTrace; exit repl.
    state->debugStop = true;
    return process_line_result_t::Continue;
  }

  else if (state->debugRepl && (command == ":c" || command == ":continue")) {
    // set flag to run to next breakpoint or end of program; exit repl.
    state->debugStop = false;
    return process_line_result_t::Continue;
  }

  else if (command == ":a" || command == ":add") {
    value_t v;
    eval_string(arg, v);
    add_attrs_to_scope(v);
  }

  else if (command == ":l" || command == ":load") {
    state->resetFileCache();
    load_file(arg);
  }

  else if (command == ":lf" || command == ":load-flake") {
    load_flake(arg);
  }

  else if (command == ":ll" || command == ":last-loaded") {
    show_last_loaded();
  }

  else if (command == ":r" || command == ":reload") {
    state->resetFileCache();
    reload_files_and_flakes();
  }

  else if (command == ":e" || command == ":edit") {
    value_t v;
    eval_string(arg, v);

    const auto [path, line] = [&]() -> std::pair<source_path_t, uint32_t> {
      if (v.type() == nPath || v.type() == nString) {
        NixStringContext context;
        auto path = state->coerceToPath(no_pos, v, context, "while evaluating the filename to edit");
        return {path, 0};
      } else if (v.isLambda()) {
        auto pos = state->positions[v.lambda().fun->pos];
        if (auto path = std::get_if<source_path_t>(&pos.origin))
          return {*path, pos.line};
        else
          throw Error("'%s' cannot be shown in an editor", pos);
      } else {
        // assume it's a derivation
        return find_package_filename(*state, v, arg);
      }
    }();

    // Open in EDITOR
    auto args = editor_for(path, line);
    auto editor = args.front();
    args.pop_front();

    // runProgram redirects stdout to a StringSink,
    // using runProgram2 to allow editors to display their UI
    run_program2(
        run_options_t{.program = editor, .lookup_path = true, .args = args, .is_interactive = true});

    // Reload right after exiting the editor
    state->resetFileCache();
    reload_files_and_flakes();
  }

  else if (command == ":t") {
    value_t v;
    eval_string(arg, v);
    logger->cout(show_type(v));
  }

  else if (command == ":u") {
    value_t v, f, result;
    eval_string(arg, v);
    eval_string("drv: (import <nixpkgs> {}).runCommand \"shell\" { buildInputs = [ drv ]; } \"\"",
               f);
    state->callFunction(f, v, result, pos_idx_t());

    store_path_t drv_path = get_derivation_path(result);
    run_nix("nix-shell", {state->store->printStorePath(drv_path)});
  }

  else if (command == ":b" || command == ":bl" || command == ":i" || command == ":sh" ||
           command == ":log") {
    value_t v;
    eval_string(arg, v);
    store_path_t drv_path = get_derivation_path(v);
    // N.B. This need not be a local / native file path. For
    // example, we might be using an SSH store to a different OS.
    std::string drv_path_raw = state->store->printStorePath(drv_path);

    if (command == ":b" || command == ":bl") {
      state->store->build_paths({
          derived_path_t::Built{
              .drv_path = makeConstantStorePathRef(drv_path),
              .outputs = OutputsSpec::All{},
          },
      });
      auto drv = state->store->read_derivation(drv_path);
      logger->cout("\nThis derivation produced the following outputs:");
      for (auto& [output_name, output_path] : state->store->queryDerivationOutputMap(drv_path)) {
        auto localStore = state->store.dynamic_pointer_cast<local_fs_store>();
        if (localStore && command == ":bl") {
          std::string symlink = "repl-result-" + output_name;
          localStore->addPermRoot(output_path, abs_path(symlink));
          logger->cout("  ./%s -> %s", symlink, state->store->printStorePath(output_path));
        } else {
          logger->cout("  %s -> %s", output_name, state->store->printStorePath(output_path));
        }
      }
    } else if (command == ":i") {
      run_nix("nix-env", {"-i", drv_path_raw});
    } else if (command == ":log") {
      settings.readOnlyMode = true;
      finally_t ro_mode_reset([&]() { settings.readOnlyMode = false; });
      auto subs = get_default_substituters();

      subs.push_front(state->store);

      bool found_log = false;
      RunPager pager;
      for (auto& sub : subs) {
        auto* logSubP = dynamic_cast<LogStore*>(&*sub);
        if (!logSubP) {
          printInfo("Skipped '%s' which does not support retrieving build logs",
                    sub->config.getHumanReadableURI());
          continue;
        }
        auto& logSub = *logSubP;

        auto log = logSub.getBuildLog(drv_path);
        if (log) {
          printInfo("got build log for '%s' from '%s'", drv_path_raw,
                    logSub.config.getHumanReadableURI());
          logger->write_to_stdout(*log);
          found_log = true;
          break;
        }
      }
      if (!found_log)
        throw Error("build log of '%s' is not available", drv_path_raw);
    } else {
      run_nix("nix-shell", {drv_path_raw});
    }
  }

  else if (command == ":p" || command == ":print") {
    value_t v;
    eval_string(arg, v);
    auto suspension = logger->suspend();
    if (v.type() == nString) {
      std::cout << v.string_view();
    } else {
      print_value(std::cout, v);
    }
    std::cout << std::endl;
  }

  else if (command == ":q" || command == ":quit") {
    state->debugStop = false;
    return process_line_result_t::quit;
  }

  else if (command == ":doc") {
    value_t v;

    auto expr = parse_string(arg);
    std::string fallback_name;
    pos_idx_t fallback_pos;
    DocComment fallback_doc;
    if (auto select = dynamic_cast<ExprSelect*>(expr)) {
      value_t v_attrs;
      auto name = select->evalExceptFinalSelect(*state, *env, v_attrs);
      fallback_name = state->symbols[name];

      state->forceAttrs(v_attrs, no_pos,
                        "while evaluating an attribute set to look for documentation");
      auto attrs = v_attrs.attrs();
      assert(attrs);
      auto attr = attrs->get(name);
      if (!attr) {
        // When missing, trigger the normal exception
        // e.g. :doc builtins.foo
        // behaves like
        // nix-repl> builtins.foo<tab>
        // error: attribute 'foo' missing
        eval_string(arg, v);
        assert(false);
      }
      if (attr->pos) {
        fallback_pos = attr->pos;
        fallback_doc = state->getDocCommentForPos(fallback_pos);
      }
    }

    eval_string(arg, v);
    if (auto doc = state->getDoc(v)) {
      std::string markdown;

      if (!doc->args.empty() && doc->name) {
        auto args = doc->args;
        for (auto& arg : args)
          arg = "*" + arg + "*";

        markdown += "**Synopsis:** `builtins." + (std::string)(*doc->name) + "` " +
                    concat_strings_sep(" ", args) + "\n\n";
      }

      markdown += strip_indentation(doc->doc);

      logger->cout(trim(render_markdown_to_terminal(markdown)));
    } else if (fallback_pos) {
      std::ostringstream ss;
      ss << "Attribute `" << fallback_name << "`\n\n";
      ss << "  … defined at " << state->positions[fallback_pos] << "\n\n";
      if (fallback_doc) {
        ss << fallback_doc.getInnerText(state->positions);
      } else {
        ss << "No documentation found.\n\n";
      }

      auto markdown = ss.view();
      logger->cout(trim(render_markdown_to_terminal(markdown)));

    } else
      throw Error("value does not have documentation");
  }

  else if (command == ":te" || command == ":trace-enable") {
    if (arg == "false" || (arg == "" && logger_settings.show_trace)) {
      std::cout << "not showing error traces\n";
      logger_settings.show_trace = false;
    } else if (arg == "true" || (arg == "" && !logger_settings.show_trace)) {
      std::cout << "showing error traces\n";
      logger_settings.show_trace = true;
    } else {
      throw Error("unexpected argument '%s' to %s", arg, command);
    };
  }

  else if (command != "")
    throw Error("unknown command '%1%'", command);

  else {
    size_t p = line.find('=');
    std::string name;
    if (p != std::string::npos && p < line.size() && line[p + 1] != '=' &&
        is_var_name(name = remove_whitespace(line.substr(0, p)))) {
      expr_t* e = parse_string(line.substr(p + 1));
      value_t& v(*state->allocValue());
      v.mk_thunk(env, e);
      add_var_to_scope(state->symbols.create(name), v);
    } else {
      value_t v;
      eval_string(line, v);
      auto suspension = logger->suspend();
      print_value(std::cout, v, 1);
      std::cout << std::endl;
    }
  }

  return process_line_result_t::prompt_again;
}

void nix_repl_t::load_file(const std::filesystem::path& path) {
  loaded_files.remove(path);
  loaded_files.push_back(path);
  value_t v, v2;
  state->evalFile(lookup_file_arg(*state, path.string()), v);
  state->autoCallFunction(*auto_args, v, v2);
  add_attrs_to_scope(v2);
}

void nix_repl_t::load_flake(const std::string& flake_ref_s) {
  if (flake_ref_s.empty())
    throw Error("cannot use ':load-flake' without a path specified. (Use '.' for the current "
                "working directory.)");

  loaded_flakes.remove(flake_ref_s);
  loaded_flakes.push_back(flake_ref_s);

  std::filesystem::path cwd;
  try {
    cwd = std::filesystem::current_path();
  } catch (std::filesystem::filesystem_error& e) {
    throw sys_error_t("cannot determine current working directory");
  }

  auto flake_ref = parse_flake_ref(fetch_settings, flake_ref_s, cwd.string(), true);
  if (eval_settings.pureEval && !flake_ref.input.isLocked(fetch_settings))
    throw Error(
        "cannot use ':load-flake' on unlocked flake reference '%s' (use --impure to override)",
        flake_ref_s);

  value_t v;

  flake::call_flake(*state,
                   flake::lock_flake(flake_settings, *state, flake_ref,
                                    flake::LockFlags{
                                        .updateLockFile = false,
                                        .use_registries = !eval_settings.pureEval,
                                        .allowUnlocked = !eval_settings.pureEval,
                                    }),
                   v);
  add_attrs_to_scope(v);
}

void nix_repl_t::init_env() {
  env = &state->mem.allocEnv(env_size);
  env->up = &state->baseEnv;
  displ = 0;
  static_env->vars.clear();

  var_names.clear();
  for (auto& i : state->staticBaseEnv->vars)
    var_names.emplace(state->symbols[i.first]);
}

void nix_repl_t::show_last_loaded() {
  RunPager pager;

  for (auto& i : *last_loaded.attrs()) {
    std::string_view name = state->symbols[i.name];
    logger->cout(name);
  }
}

void nix_repl_t::reload_files_and_flakes() {
  init_env();

  load_files();
  load_flakes();
}

void nix_repl_t::load_files() {
  decltype(loaded_files) old = loaded_files;
  loaded_files.clear();

  for (auto& i : old) {
    notice("Loading '%1%'...", i);
    load_file(i);
  }

  for (auto& [i, what] : get_values()) {
    notice("Loading installable '%1%'...", what);
    add_attrs_to_scope(*i);
  }
}

void nix_repl_t::load_flakes() {
  strings_t old = loaded_flakes;
  loaded_flakes.clear();

  for (auto& i : old) {
    notice("Loading flake '%1%'...", i);
    load_flake(i);
  }
}

void nix_repl_t::add_attrs_to_scope(value_t& attrs) {
  state->forceAttrs(
      attrs, [&]() { return attrs.determinePos(no_pos); },
      "while evaluating an attribute set to be merged in the global scope");
  if (displ + attrs.attrs()->size() >= env_size)
    throw Error("environment full; cannot add more variables");

  for (auto& i : *attrs.attrs()) {
    static_env->vars.emplace_back(i.name, displ);
    env->values[displ++] = i.value;
    var_names.emplace(state->symbols[i.name]);
  }
  static_env->sort();
  static_env->deduplicate();
  notice("Added %1% variables.", attrs.attrs()->size());

  last_loaded = attrs;

  const int max_print = 20;
  int counter = 0;
  std::ostringstream loaded;
  for (auto& i : attrs.attrs()->lexicographicOrder(state->symbols)) {
    if (counter >= max_print)
      break;

    if (counter > 0)
      loaded << ", ";

    print_identifier(loaded, state->symbols[i->name]);
    counter += 1;
  }

  notice("%1%", loaded.str());

  if (attrs.attrs()->size() > max_print)
    notice("... and %1% more; view with :ll", attrs.attrs()->size() - max_print);
}

void nix_repl_t::add_var_to_scope(const symbol_t name, value_t& v) {
  if (displ >= env_size)
    throw Error("environment full; cannot add more variables");
  if (auto oldVar = static_env->find(name); oldVar != static_env->vars.end())
    static_env->vars.erase(oldVar);
  static_env->vars.emplace_back(name, displ);
  static_env->sort();
  env->values[displ++] = &v;
  var_names.emplace(state->symbols[name]);
}

expr_t* nix_repl_t::parse_string(std::string s) {
  try {
    return state->parseExprFromString(std::move(s), state->root_path("."), static_env);
  } catch (ParseError& e) {
    if (e.msg().find("unexpected end of file") != std::string::npos)
      // For parse errors on incomplete input, we continue waiting for the next line of
      // input without clearing the input so far.
      throw IncompleteReplExpr(e.msg());
    else
      throw;
  }
}

void nix_repl_t::eval_string(std::string s, value_t& v) {
  expr_t* e = parse_string(s);
  e->eval(*state, *env, v);
  state->forceValue(v, v.determinePos(no_pos));
}

void nix_repl_t::run_nix(const std::string& program, const strings_t& args,
                     const std::optional<std::string>& input) {
  if (run_nix_ptr)
    (*run_nix_ptr)(program, args, input);
  else
    throw Error("Cannot run '%s' because no method of calling the Nix CLI was provided. This is a "
                "configuration problem pertaining to how this program was built. See Nix 2.25 "
                "release notes",
                program);
}

std::unique_ptr<AbstractNixRepl>
AbstractNixRepl::create(const LookupPath& lookup_path, nix::ref<store_t> store, ref<eval_state_t> state,
                        std::function<AnnotatedValues()> get_values, RunNix* run_nix) {
  return std::make_unique<nix_repl_t>(lookup_path, std::move(store), state, get_values, run_nix);
}

ReplExitStatus AbstractNixRepl::runSimple(ref<eval_state_t> eval_state, const ValMap& extraEnv) {
  auto get_values = [&]() -> nix_repl_t::AnnotatedValues {
    nix_repl_t::AnnotatedValues values;
    return values;
  };
  LookupPath lookup_path = {};
  // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDelete)
  auto repl = std::make_unique<nix_repl_t>(lookup_path, open_store(), eval_state, get_values,
                                        /*run_nix=*/nullptr);

  repl->init_env();

  // add 'extra' vars.
  for (auto& [name, value] : extraEnv)
    repl->add_var_to_scope(repl->state->symbols.create(name), *value);

  return repl->main_loop();
}

} // namespace nix
