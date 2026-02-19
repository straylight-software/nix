#include "nix/util/args.h"

#include <fstream>
#include <regex>
#include <string>

#include "nix/util/args/root.h"
#include "nix/util/environment-variables.h"
#include "nix/util/hash.h"
#include "nix/util/json-utils.h"
#include "nix/util/signals.h"
#include "nix/util/users.h"
#ifndef _WIN32
#  include <glob.h>
#endif

namespace nix {

void Args::add_flag(flag_t&& flag_) {
  auto flag = std::make_shared<flag_t>(std::move(flag_));
  if (flag->handler.arity != arity_any) {
    assert(flag->handler.arity == flag->labels.size());
}
  assert(flag->long_name != "");
  longFlags[flag->long_name] = flag;
  for (auto& alias : flag->aliases) {
    longFlags[alias] = flag;
}
  if (flag->short_name) {
    shortFlags[flag->short_name] = flag;
}
}

void Args::remove_flag(const std::string& long_name) {
  auto flag = longFlags.find(long_name);
  assert(flag != longFlags.end());
  if (flag->second->short_name) {
    shortFlags.erase(flag->second->short_name);
}
  longFlags.erase(flag);
}

void completions_t::set_type(add_completions_t::Type t) {
  type = t;
}

void completions_t::add(std::string completion, std::string description) {
  description = trim(description);
  // ellipsize overflowing content on the back of the description
  auto end_index = description.find_first_of(".\n");
  if (end_index != std::string::npos) {
    auto needs_ellipsis = end_index != description.size() - 1;
    description.resize(end_index);
    if (needs_ellipsis) {
      description.append(" [...]");
}
  }
  completions.insert(completion_t{.completion = completion, .description = description});
}

auto completion_t::operator<=>(const completion_t& other) const noexcept = default;

std::string completion_marker = "___COMPLETE___";

root_args_t& Args::get_root() {
  Args* p = this;
  while (p->parent) {
    p = p->parent;
}

  auto* res = dynamic_cast<root_args_t*>(p);
  assert(res);
  return *res;
}

std::optional<std::string> root_args_t::needs_completion(std::string_view s) {
  if (!completions) {
    return {};
}
  auto i = s.find(completion_marker);
  if (i != std::string::npos) {
    return std::string(s.begin(), i);
}
  return {};
}

namespace {

/**
 * Basically this is `typedef std::optional<Parser> Parser(std::string_view s, strings_t & r);`
 *
 * Except we can't recursively reference the Parser typedef, so we have to write a class.
 */
struct Parser {
  std::string_view remaining;

  /**
   * @brief Parse the next character(s)
   */
  virtual void operator()(std::shared_ptr<Parser>& state, strings_t& r) = 0;

  Parser(std::string_view s) : remaining(s) {};

  virtual ~Parser() {};
};

struct parse_quoted_t : public Parser {
  /**
   * @brief Accumulated string
   *
   * Parsed argument up to this point.
   */
  std::string acc;

  parse_quoted_t(std::string_view s) : Parser(s) {};

  virtual void operator()(std::shared_ptr<Parser>& state, strings_t& r) override;
};

struct parse_unquoted_t : public Parser {
  /**
   * @brief Accumulated string
   *
   * Parsed argument up to this point. Empty string is not representable in
   * unquoted syntax, so we use it for the initial state.
   */
  std::string acc;

  parse_unquoted_t(std::string_view s) : Parser(s) {};

  virtual void operator()(std::shared_ptr<Parser>& state, strings_t& r) override {
    if (remaining.empty()) {
      if (!acc.empty()) {
        r.push_back(acc);
}
      state = nullptr; // done
      return;
    }
    switch (remaining[0]) {
      case ' ':
      case '\t':
      case '\n':
      case '\r':
        if (!acc.empty()) {
          r.push_back(acc);
}
        state = std::make_shared<parse_unquoted_t>(parse_unquoted_t(remaining.substr(1)));
        return;
      case '`':
        if (remaining.size() > 1 && remaining[1] == '`') {
          state = std::make_shared<parse_quoted_t>(parse_quoted_t(remaining.substr(2)));
          return;
        } else {
          throw Error("single backtick is not a supported syntax in the nix shebang.");
}

      // reserved characters
      // meaning to be determined, or may be reserved indefinitely so that
      // #!nix syntax looks unambiguous
      case '$':
      case '*':
      case '~':
      case '<':
      case '>':
      case '|':
      case ';':
      case '(':
      case ')':
      case '[':
      case ']':
      case '{':
      case '}':
      case '\'':
      case '"':
      case '\\':
        throw Error("unsupported unquoted character in nix shebang: " +
                    std::string(1, remaining[0]) + ". Use double backticks to escape?");

      case '#':
        if (acc.empty()) {
          throw Error(
              "unquoted nix shebang argument cannot start with #. Use double backticks to escape?");
        } else {
          acc += remaining[0];
          remaining = remaining.substr(1);
          return;
        }

      default:
        acc += remaining[0];
        remaining = remaining.substr(1);
        return;
    }
    assert(false);
  }
};

void parse_quoted_t::operator()(std::shared_ptr<Parser>& state, strings_t& r) {
  if (remaining.empty()) {
    throw Error("unterminated quoted string in nix shebang");
  }
  switch (remaining[0]) {
    case ' ':
      if ((remaining.size() == 3 && remaining[1] == '`' && remaining[2] == '`') ||
          (remaining.size() > 3 && remaining[1] == '`' && remaining[2] == '`' &&
           remaining[3] != '`')) {
        // exactly two backticks mark the end of a quoted string, but a preceding space is ignored
        // if present.
        state = std::make_shared<parse_unquoted_t>(parse_unquoted_t(remaining.substr(3)));
        r.push_back(acc);
        return;
      } else {
        // just a normal space
        acc += remaining[0];
        remaining = remaining.substr(1);
        return;
      }
    case '`':
      // exactly two backticks mark the end of a quoted string
      if ((remaining.size() == 2 && remaining[1] == '`') ||
          (remaining.size() > 2 && remaining[1] == '`' && remaining[2] != '`')) {
        state = std::make_shared<parse_unquoted_t>(parse_unquoted_t(remaining.substr(2)));
        r.push_back(acc);
        return;
      }

      // a sequence of at least 3 backticks is one escape-backtick which is ignored, followed by any
      // number of backticks, which are verbatim
      else if (remaining.size() >= 3 && remaining[1] == '`' && remaining[2] == '`') {
        // ignore "escape" backtick
        remaining = remaining.substr(1);
        // add the rest
        while (remaining.size() > 0 && remaining[0] == '`') {
          acc += '`';
          remaining = remaining.substr(1);
        }
        return;
      } else {
        acc += remaining[0];
        remaining = remaining.substr(1);
        return;
      }
    default:
      acc += remaining[0];
      remaining = remaining.substr(1);
      return;
  }
  assert(false);
}

} // namespace

strings_t parse_shebang_content(std::string_view s) {
  strings_t result;
  std::shared_ptr<Parser> parser_state(std::make_shared<parse_unquoted_t>(parse_unquoted_t(s)));

  // trampoline == iterated strategy pattern
  while (parser_state) {
    auto current_state = parser_state;
    (*current_state)(parser_state, result);
  }

  return result;
}

void root_args_t::parse_cmdline(const strings_t& _cmdline, bool allow_shebang) {
  strings_t pending_args;
  bool dash_dash = false;

  strings_t cmdline(_cmdline);

  if (auto s = get_env("NIX_GET_COMPLETIONS")) {
    size_t n = std::stoi(*s);
    assert(n > 0 && n <= cmdline.size());
    *std::next(cmdline.begin(), n - 1) += completion_marker;
    completions = std::make_shared<completions_t>();
    verbosity = lvl_error;
  }

  // Heuristic to see if we're invoked as a shebang script, namely,
  // if we have at least one argument, it's the name of an
  // executable file, and it starts with "#!".
  strings_t saved_args;
  if (allow_shebang) {
    auto script = *cmdline.begin();
    try {
      std::ifstream stream(script);
      char shebang[3] = {0, 0, 0};
      stream.get(shebang, 3);
      if (strncmp(shebang, "#!", 2) == 0) {
        for (auto pos = std::next(cmdline.begin()); pos != cmdline.end(); pos++) {
          saved_args.push_back(*pos);
}
        cmdline.clear();

        std::string line;
        std::getline(stream, line);
        static const std::string comment_chars("#/\\%@*-(");
        std::string shebang_content;
        while (std::getline(stream, line) && !line.empty() &&
               comment_chars.find(line[0]) != std::string::npos) {
          line = chomp(line);

          std::smatch match;
          // We match one space after `nix` so that we preserve indentation.
          // No space is necessary for an empty line. An empty line has basically no effect.
          if (std::regex_match(line, match, std::regex("^#!\\s*nix(:? |$)(.*)$"))) {
            shebang_content += std::string_view{match[2].first, match[2].second} + "\n";
}
        }
        for (const auto& word : parse_shebang_content(shebang_content)) {
          cmdline.push_back(word);
        }
        cmdline.push_back(script);
        commandBaseDir = dir_of(script);
        for (auto pos = saved_args.begin(); pos != saved_args.end(); pos++) {
          cmdline.push_back(*pos);
}
      }
    } catch (SystemError&) {
    }
  }

  for (auto pos = cmdline.begin(); pos != cmdline.end();) {
    auto arg = *pos;

    /* Expand compound dash options (i.e., `-qlf' -> `-q -l -f',
       `-j3` -> `-j 3`). */
    if (!dash_dash && arg.length() > 2 && arg[0] == '-' && arg[1] != '-' && isalpha(arg[1])) {
      *pos = (std::string) "-" + arg[1];
      auto next = pos;
      ++next;
      for (unsigned int j = 2; j < arg.length(); j++) {
        if (isalpha(arg[j])) {
          cmdline.insert(next, (std::string) "-" + arg[j]);
        } else {
          cmdline.insert(next, std::string(arg, j));
          break;
        }
}
      arg = *pos;
    }

    if (!dash_dash && arg == "--") {
      dash_dash = true;
      ++pos;
    } else if (!dash_dash && std::string(arg, 0, 1) == "-") {
      if (!process_flag(pos, cmdline.end())) {
        throw UsageError("unrecognised flag '%1%'", arg);
}
    } else {
      pos = rewrite_args(cmdline, pos);
      pending_args.push_back(*pos++);
      if (process_args(pending_args, false)) {
        pending_args.clear();
}
    }
  }

  process_args(pending_args, true);

  if (!completions) {
    check_args();
}

  initial_flags_processed();

  /* Now that we are done parsing, make sure that any experimental
   * feature required by the flags is enabled */
  for (auto& f : flagExperimentalFeatures) {
    experimental_feature_settings.require(f);
}

  /* Now that all the other args are processed, run the deferred completions.
   */
  for (const auto& d : deferredCompletions) {
    d.completer(*completions, d.n, d.prefix);
}
}

std::filesystem::path Args::get_command_base_dir() const {
  assert(parent);
  return parent->get_command_base_dir();
}

std::filesystem::path root_args_t::get_command_base_dir() const {
  return commandBaseDir;
}

bool Args::process_flag(strings_t::iterator& pos, strings_t::iterator end) {
  assert(pos != end);

  auto& root_args = get_root();

  auto process = [&](const std::string& name, flag_t& flag) -> bool {
    ++pos;

    if (auto& f = flag.experimental_feature) {
      root_args.flagExperimentalFeatures.insert(*f);
}

    std::vector<std::string> args;
    bool any_completed = false;
    for (size_t n = 0; n < flag.handler.arity; ++n) {
      if (pos == end) {
        if (flag.handler.arity == arity_any || any_completed) {
          break;
}
        throw UsageError("flag '%s' requires %d argument(s), but only %d were given", name,
                         flag.handler.arity, n);
      }
      if (auto prefix = root_args.needs_completion(*pos)) {
        any_completed = true;
        if (flag.completer) {
          root_args.deferredCompletions.push_back({
              .completer = flag.completer,
              .n = n,
              .prefix = *prefix,
          });
        }
      }
      args.push_back(*pos++);
    }
    if (!any_completed) {
      flag.handler.fun(std::move(args));
}
    flag.times_used++;
    return true;
  };

  if (std::string(*pos, 0, 2) == "--") {
    if (auto prefix = root_args.needs_completion(*pos)) {
      for (auto& [name, flag] : longFlags) {
        if (!hiddenCategories.count(flag->category) && has_prefix(name, std::string(*prefix, 2))) {
          if (auto& f = flag->experimental_feature) {
            root_args.flagExperimentalFeatures.insert(*f);
}
          root_args.completions->add("--" + name, flag->description);
        }
      }
      return false;
    }
    auto i = longFlags.find(std::string(*pos, 2));
    if (i == longFlags.end()) {
      return false;
}
    return process("--" + i->first, *i->second);
  }

  if (std::string(*pos, 0, 1) == "-" && pos->size() == 2) {
    auto c = (*pos)[1];
    auto i = shortFlags.find(c);
    if (i == shortFlags.end()) {
      return false;
}
    return process(std::string("-") + c, *i->second);
  }

  if (auto prefix = root_args.needs_completion(*pos)) {
    if (prefix == "-") {
      root_args.completions->add("--");
      for (auto& [flagName, flag] : shortFlags) {
        if (experimental_feature_settings.is_enabled(flag->experimental_feature)) {
          root_args.completions->add(std::string("-") + flagName, flag->description);
}
}
    }
  }

  return false;
}

bool Args::process_args(const strings_t& args, bool finish) {
  if (expectedArgs.empty()) {
    if (!args.empty()) {
      throw UsageError("unexpected argument '%1%'", args.front());
}
    return true;
  }

  auto& root_args = get_root();

  auto& exp = expectedArgs.front();

  bool res = false;

  if ((exp.handler.arity == arity_any && finish) ||
      (exp.handler.arity != arity_any && args.size() == exp.handler.arity)) {
    std::vector<std::string> ss;
    bool any_completed = false;
    for (const auto& [n, s] : enumerate(args)) {
      if (auto prefix = root_args.needs_completion(s)) {
        any_completed = true;
        ss.push_back(*prefix);
        if (exp.completer) {
          root_args.deferredCompletions.push_back({
              .completer = exp.completer,
              .n = n,
              .prefix = *prefix,
          });
        }
      } else {
        ss.push_back(s);
}
    }
    if (!any_completed) {
      exp.handler.fun(ss);
}

    /* Move the list element to the processedArgs. This is almost the same as
       `processedArgs.push_back(expectedArgs.front()); expectedArgs.pop_front()`,
       except that it will only adjust the next and prev pointers of the list
       elements, meaning the actual contents don't move in memory. This is
       critical to prevent invalidating internal pointers! */
    processedArgs.splice(processedArgs.end(), expectedArgs, expectedArgs.begin(),
                         ++expectedArgs.begin());

    res = true;
  }

  if (finish && !expectedArgs.empty() && !expectedArgs.front().optional) {
    throw UsageError("more arguments are required");
}

  return res;
}

void Args::check_args() {
  for (auto& [name, flag] : longFlags) {
    if (flag->required && flag->times_used == 0) {
      throw UsageError("required argument '%s' is missing", "--" + name);
}
  }
}

nlohmann::json Args::to_json() {
  auto flags = nlohmann::json::object();

  for (auto& [name, flag] : longFlags) {
    auto j = nlohmann::json::object();
    j["hiddenCategory"] = hiddenCategories.count(flag->category) > 0;
    if (flag->aliases.count(name)) {
      continue;
}
    if (flag->short_name) {
      j["shortName"] = std::string(1, flag->short_name);
}
    if (flag->description != "") {
      j["description"] = trim(flag->description);
}
    j["category"] = flag->category;
    if (flag->handler.arity != arity_any) {
      j["arity"] = flag->handler.arity;
}
    if (!flag->labels.empty()) {
      j["labels"] = flag->labels;
}
    j["experimental-feature"] = flag->experimental_feature;
    flags[name] = std::move(j);
  }

  auto args = nlohmann::json::array();

  for (auto& arg : expectedArgs) {
    auto j = nlohmann::json::object();
    j["label"] = arg.label;
    j["optional"] = arg.optional;
    if (arg.handler.arity != arity_any) {
      j["arity"] = arg.handler.arity;
}
    args.push_back(std::move(j));
  }

  auto res = nlohmann::json::object();
  res["description"] = trim(description());
  res["flags"] = std::move(flags);
  res["args"] = std::move(args);
  auto s = doc();
  if (s != "") {
    res.emplace("doc", strip_indentation(s));
}
  return res;
}

static void complete_path_(add_completions_t& completions, std::string_view prefix, bool only_dirs) {
  completions.set_type(completions_t::Type::filenames);
#ifndef _WIN32 // TODO implement globbing completions on Windows
  glob_t globbuf;
  int flags = GLOB_NOESCAPE;
#  ifdef GLOB_ONLYDIR
  if (only_dirs) {
    flags |= GLOB_ONLYDIR;
}
#  endif
  // using expandTilde here instead of GLOB_TILDE(_CHECK) so that ~<Tab> expands to /home/user/
  if (glob((expand_tilde(prefix) + "*").c_str(), flags, nullptr, &globbuf) == 0) {
    for (size_t i = 0; i < globbuf.gl_pathc; ++i) {
      if (only_dirs) {
        auto st = stat(globbuf.gl_pathv[i]);
        if (!S_ISDIR(st.st_mode)) {
          continue;
}
      }
      completions.add(globbuf.gl_pathv[i]);
    }
  }
  globfree(&globbuf);
#endif
}

void Args::complete_path(add_completions_t& completions, size_t, std::string_view prefix) {
  complete_path_(completions, prefix, false);
}

void Args::complete_dir(add_completions_t& completions, size_t, std::string_view prefix) {
  complete_path_(completions, prefix, true);
}

strings_t argv_to_strings(int argc, char** argv) {
  strings_t args;
  argc--;
  argv++;
  while (argc--) {
    args.push_back(*argv++);
}
  return args;
}

std::optional<experimental_feature_t> command_t::experimental_feature() {
  return {};
}

multi_command_t::multi_command_t(std::string_view command_name, const commands_t& commands_)
    : commands(commands_), command_name(command_name) {
  expect_args({.label = "subcommand",
              .optional = true,
              .handler = {[=, this](std::string s) {
                assert(!command);
                auto i = commands.find(s);
                if (i == commands.end()) {
                  string_set_t command_names;
                  for (auto& [name, _] : commands) {
                    command_names.insert(name);
}
                  auto suggestions = suggestions_t::best_matches(command_names, s);
                  throw UsageError(suggestions, "'%s' is not a recognised command", s);
                }
                command = {s, i->second()};
                command->second->parent = this;
              }},
              .completer = {[&](add_completions_t& completions, size_t, std::string_view prefix) {
                for (auto& [name, command] : commands) {
                  if (has_prefix(name, prefix)) {
                    completions.add(name);
}
}
              }}});

  categories[command_t::cat_default] = "Available commands";
}

bool multi_command_t::process_flag(strings_t::iterator& pos, strings_t::iterator end) {
  if (Args::process_flag(pos, end)) {
    return true;
}
  if (command && command->second->process_flag(pos, end)) {
    return true;
}
  return false;
}

bool multi_command_t::process_args(const strings_t& args, bool finish) {
  if (command) {
    return command->second->process_args(args, finish);
  } else {
    return Args::process_args(args, finish);
}
}

void multi_command_t::check_args() {
  Args::check_args();
  if (command) {
    command->second->check_args();
}
}

nlohmann::json multi_command_t::to_json() {
  auto cmds = nlohmann::json::object();

  for (auto& [name, commandFun] : commands) {
    auto command = commandFun();
    auto j = command->to_json();
    auto cat = nlohmann::json::object();
    cat["id"] = command->category();
    cat["description"] = trim(categories[command->category()]);
    cat["experimental-feature"] = command->experimental_feature();
    j["category"] = std::move(cat);
    cmds[name] = std::move(j);
  }

  auto res = Args::to_json();
  res["commands"] = std::move(cmds);
  return res;
}

strings_t::iterator multi_command_t::rewrite_args(strings_t& args, strings_t::iterator pos) {
  if (command) {
    return command->second->rewrite_args(args, pos);
}

  if (aliasUsed || pos == args.end()) {
    return pos;
}
  auto arg = *pos;
  auto i = aliases.find(arg);
  if (i == aliases.end()) {
    return pos;
}
  auto& info = i->second;
  if (info.status == alias_status_t::deprecated) {
    warn("'%s' is a deprecated alias for '%s'", arg, concat_strings_sep(" ", info.replacement));
  }
  pos = args.erase(pos);
  for (auto j = info.replacement.rbegin(); j != info.replacement.rend(); ++j) {
    pos = args.insert(pos, *j);
}
  aliasUsed = true;
  return pos;
}

} // namespace nix
