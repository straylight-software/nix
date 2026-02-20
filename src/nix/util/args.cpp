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

void args_t::add_flag(flag_t&& flag_arg) {
  auto flag = std::make_shared<flag_t>(std::move(flag_arg));
  if (flag->handler.get_arity() != arity_any) {
    assert(flag->handler.get_arity() == flag->labels.size());
  }
  assert(flag->long_name != "");
  long_flags_[flag->long_name] = flag;
  for (const auto& flag_alias : flag->aliases) {
    long_flags_[flag_alias] = flag;
  }
  if (flag->short_name != 0) {
    short_flags_[flag->short_name] = flag;
  }
}

void args_t::remove_flag(const std::string& long_name) {
  auto flag = long_flags_.find(long_name);
  assert(flag != long_flags_.end());
  if (flag->second->short_name != 0) {
    short_flags_.erase(flag->second->short_name);
  }
  long_flags_.erase(flag);
}

void completions_t::set_type(add_completions_t::Type completion_type) {
  type = completion_type;
}

void completions_t::add(std::string completion, std::string desc) {
  desc = trim(desc);
  // ellipsize overflowing content on the back of the description
  auto end_index = desc.find_first_of(".\n");
  if (end_index != std::string::npos) {
    auto needs_ellipsis = end_index != desc.size() - 1;
    desc.resize(end_index);
    if (needs_ellipsis) {
      desc.append(" [...]");
    }
  }
  completions.insert(completion_t(std::move(completion), std::move(desc)));
}

auto completion_t::operator<=>(const completion_t& other) const noexcept -> std::strong_ordering {
  if (auto cmp = get_completion() <=> other.get_completion(); cmp != 0) {
    return cmp;
  }
  return get_description() <=> other.get_description();
}

const std::string completion_marker = "___COMPLETE___";

auto args_t::get_root() -> root_args_t& {
  args_t* ptr = this;
  while (ptr->get_parent() != nullptr) {
    ptr = ptr->get_parent();
  }

  auto* res = dynamic_cast<root_args_t*>(ptr);
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

auto args_t::get_command_base_dir() const -> std::filesystem::path {
  assert(parent_);
  return parent_->get_command_base_dir();
}

std::filesystem::path root_args_t::get_command_base_dir() const {
  return commandBaseDir;
}

auto args_t::process_flag(strings_t::iterator& pos, strings_t::iterator end) -> bool {
  assert(pos != end);

  auto& root_args = get_root();

  auto process = [&](const std::string& name, flag_t& flag) -> bool {
    ++pos;

    if (auto& feature = flag.experimental_feature) {
      root_args.flagExperimentalFeatures.insert(*feature);
    }

    std::vector<std::string> args;
    bool any_completed = false;
    for (size_t idx = 0; idx < flag.handler.get_arity(); ++idx) {
      if (pos == end) {
        if (flag.handler.get_arity() == arity_any || any_completed) {
          break;
        }
        throw UsageError("flag '%s' requires %d argument(s), but only %d were given", name,
                         flag.handler.get_arity(), idx);
      }
      if (auto prefix = root_args.needs_completion(*pos)) {
        any_completed = true;
        if (flag.completer) {
          root_args.deferredCompletions.push_back({
              .completer = flag.completer,
              .n = idx,
              .prefix = *prefix,
          });
        }
      }
      args.push_back(*pos++);
    }
    if (!any_completed) {
      flag.handler.call(std::move(args));
    }
    flag.times_used++;
    return true;
  };

  if (std::string(*pos, 0, 2) == "--") {
    if (auto prefix = root_args.needs_completion(*pos)) {
      for (auto& [name, flag] : long_flags_) {
        if (!hidden_categories_.count(flag->category) &&
            has_prefix(name, std::string(*prefix, 2))) {
          if (auto& feature = flag->experimental_feature) {
            root_args.flagExperimentalFeatures.insert(*feature);
          }
          root_args.completions->add("--" + name, flag->description);
        }
      }
      return false;
    }
    auto iter = long_flags_.find(std::string(*pos, 2));
    if (iter == long_flags_.end()) {
      return false;
    }
    return process("--" + iter->first, *iter->second);
  }

  if (std::string(*pos, 0, 1) == "-" && pos->size() == 2) {
    auto chr = (*pos)[1];
    auto iter = short_flags_.find(chr);
    if (iter == short_flags_.end()) {
      return false;
    }
    return process(std::string("-") + chr, *iter->second);
  }

  if (auto prefix = root_args.needs_completion(*pos)) {
    if (prefix == "-") {
      root_args.completions->add("--");
      for (auto& [flag_name, flag] : short_flags_) {
        if (experimental_feature_settings.is_enabled(flag->experimental_feature)) {
          root_args.completions->add(std::string("-") + flag_name, flag->description);
        }
      }
    }
  }

  return false;
}

auto args_t::process_args(const strings_t& args, bool finish) -> bool {
  if (expected_args_.empty()) {
    if (!args.empty()) {
      throw UsageError("unexpected argument '%1%'", args.front());
    }
    return true;
  }

  auto& root_args = get_root();

  auto& exp = expected_args_.front();

  bool res = false;

  if ((exp.handler.get_arity() == arity_any && finish) ||
      (exp.handler.get_arity() != arity_any && args.size() == exp.handler.get_arity())) {
    std::vector<std::string> strs;
    bool any_completed = false;
    for (const auto& [idx, str] : enumerate(args)) {
      if (auto prefix = root_args.needs_completion(str)) {
        any_completed = true;
        strs.push_back(*prefix);
        if (exp.completer) {
          root_args.deferredCompletions.push_back({
              .completer = exp.completer,
              .n = idx,
              .prefix = *prefix,
          });
        }
      } else {
        strs.push_back(str);
      }
    }
    if (!any_completed) {
      exp.handler.call(strs);
    }

    /* Move the list element to the processed_args_. This is almost the same as
       `processed_args_.push_back(expected_args_.front()); expected_args_.pop_front()`,
       except that it will only adjust the next and prev pointers of the list
       elements, meaning the actual contents don't move in memory. This is
       critical to prevent invalidating internal pointers! */
    processed_args_.splice(processed_args_.end(), expected_args_, expected_args_.begin(),
                           ++expected_args_.begin());

    res = true;
  }

  if (finish && !expected_args_.empty() && !expected_args_.front().optional) {
    throw UsageError("more arguments are required");
  }

  return res;
}

void args_t::check_args() {
  for (auto& [name, flag] : long_flags_) {
    if (flag->required && flag->times_used == 0) {
      throw UsageError("required argument '%s' is missing", "--" + name);
    }
  }
}

auto args_t::to_json() -> nlohmann::json {
  auto flags = nlohmann::json::object();

  for (auto& [name, flag] : long_flags_) {
    auto json_obj = nlohmann::json::object();
    json_obj["hiddenCategory"] = hidden_categories_.count(flag->category) > 0;
    if (flag->aliases.count(name)) {
      continue;
    }
    if (flag->short_name) {
      json_obj["shortName"] = std::string(1, flag->short_name);
    }
    if (flag->description != "") {
      json_obj["description"] = trim(flag->description);
    }
    json_obj["category"] = flag->category;
    if (flag->handler.get_arity() != arity_any) {
      json_obj["arity"] = flag->handler.get_arity();
    }
    if (!flag->labels.empty()) {
      json_obj["labels"] = flag->labels;
    }
    json_obj["experimental-feature"] = flag->experimental_feature;
    flags[name] = std::move(json_obj);
  }

  auto args = nlohmann::json::array();

  for (auto& arg : expected_args_) {
    auto json_obj = nlohmann::json::object();
    json_obj["label"] = arg.label;
    json_obj["optional"] = arg.optional;
    if (arg.handler.get_arity() != arity_any) {
      json_obj["arity"] = arg.handler.get_arity();
    }
    args.push_back(std::move(json_obj));
  }

  auto res = nlohmann::json::object();
  res["description"] = trim(description());
  res["flags"] = std::move(flags);
  res["args"] = std::move(args);
  auto doc_str = doc();
  if (doc_str != "") {
    res.emplace("doc", strip_indentation(doc_str));
  }
  return res;
}

static void complete_path_(add_completions_t& completions_ref, std::string_view prefix,
                           bool only_dirs) {
  completions_ref.set_type(completions_t::Type::filenames);
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
    for (size_t idx = 0; idx < globbuf.gl_pathc; ++idx) {
      if (only_dirs) {
        auto statbuf = stat(globbuf.gl_pathv[idx]);
        if (!S_ISDIR(statbuf.st_mode)) {
          continue;
        }
      }
      completions_ref.add(globbuf.gl_pathv[idx]);
    }
  }
  globfree(&globbuf);
#endif
}

void args_t::complete_path(add_completions_t& completions_ref, size_t /*idx*/,
                         std::string_view prefix) {
  complete_path_(completions_ref, prefix, false);
}

void args_t::complete_dir(add_completions_t& completions_ref, size_t /*idx*/,
                        std::string_view prefix) {
  complete_path_(completions_ref, prefix, true);
}

auto argv_to_strings(int argc, char** argv) -> strings_t {
  strings_t args;
  argc--;
  argv++;
  while (argc-- != 0) {
    args.push_back(*argv++);
  }
  return args;
}

auto command_t::experimental_feature() -> std::optional<experimental_feature_t> {
  return {};
}

multi_command_t::multi_command_t(std::string_view cmd_name, const commands_t& cmds)
    : commands_(cmds), command_name_(cmd_name) {
  expect_args(
      {.label = "subcommand",
       .optional = true,
       .handler = {[=, this](std::string str) {
         assert(!command_);
         auto iter = commands_.find(str);
         if (iter == commands_.end()) {
           string_set_t cmd_names;
           for (auto& [name, _] : commands_) {
             cmd_names.insert(name);
           }
           auto suggestions = suggestions_t::best_matches(cmd_names, str);
           throw UsageError(suggestions, "'%s' is not a recognised command", str);
         }
         command_ = {str, iter->second()};
         command_->second->set_parent(this);
       }},
       .completer = {[&](add_completions_t& completions, size_t /*idx*/, std::string_view prefix) {
         for (auto& [name, cmd] : commands_) {
           if (has_prefix(name, prefix)) {
             completions.add(name);
           }
         }
       }}});

  categories_[command_t::cat_default] = "Available commands";
}

auto multi_command_t::process_flag(strings_t::iterator& pos, strings_t::iterator end) -> bool {
  if (args_t::process_flag(pos, end)) {
    return true;
  }
  if (command_ && command_->second->process_flag(pos, end)) {
    return true;
  }
  return false;
}

auto multi_command_t::process_args(const strings_t& args, bool finish) -> bool {
  if (command_) {
    return command_->second->process_args(args, finish);
  } else {
    return args_t::process_args(args, finish);
  }
}

void multi_command_t::check_args() {
  args_t::check_args();
  if (command_) {
    command_->second->check_args();
  }
}

auto multi_command_t::to_json() -> nlohmann::json {
  auto cmds = nlohmann::json::object();

  for (auto& [name, cmd_fun] : commands_) {
    auto cmd = cmd_fun();
    auto json_obj = cmd->to_json();
    auto cat = nlohmann::json::object();
    cat["id"] = cmd->category();
    cat["description"] = trim(categories_[cmd->category()]);
    cat["experimental-feature"] = cmd->experimental_feature();
    json_obj["category"] = std::move(cat);
    cmds[name] = std::move(json_obj);
  }

  auto res = args_t::to_json();
  res["commands"] = std::move(cmds);
  return res;
}

auto multi_command_t::rewrite_args(strings_t& args, strings_t::iterator pos)
    -> strings_t::iterator {
  if (command_) {
    return command_->second->rewrite_args(args, pos);
  }

  if (alias_used_ || pos == args.end()) {
    return pos;
  }
  auto arg = *pos;
  auto iter = aliases_.find(arg);
  if (iter == aliases_.end()) {
    return pos;
  }
  auto& info = iter->second;
  if (info.status == alias_status_t::deprecated) {
    warn("'%s' is a deprecated alias for '%s'", arg, concat_strings_sep(" ", info.replacement));
  }
  pos = args.erase(pos);
  for (auto jiter = info.replacement.rbegin(); jiter != info.replacement.rend(); ++jiter) {
    pos = args.insert(pos, *jiter);
  }
  alias_used_ = true;
  return pos;
}

} // namespace nix
