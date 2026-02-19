#pragma once
///@file

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>

#include <nlohmann/json_fwd.hpp>

#include "nix/util/experimental-features.h"
#include "nix/util/ref.h"
#include "nix/util/types.h"
#include "nix/util/util.h"

namespace nix {

enum struct hash_algorithm_t : char;
enum struct hash_format_t : int;

class multi_command_t;

class root_args_t;

class add_completions_t;

class Args {
public:
  /**
   * Return a short one-line description of the command.
   */
  virtual std::string description() { return ""; }

  virtual bool force_impure_by_default() { return false; }

  /**
   * Return documentation about this command, in Markdown format.
   */
  virtual std::string doc() { return ""; }

  /**
   * @brief Get the [base
   * directory](https://nix.dev/manual/nix/development/glossary.html#gloss-base-directory) for the
   * command.
   *
   * @return Generally the working directory, but in case of a shebang
   *         interpreter, returns the directory of the script.
   *
   * This only returns the correct value after parse_cmdline() has run.
   */
  virtual std::filesystem::path get_command_base_dir() const;

protected:
  /**
   * The largest `size_t` is used to indicate the "any" arity, for
   * handlers/flags/arguments that accept an arbitrary number of
   * arguments.
   */
  static const size_t arity_any = std::numeric_limits<size_t>::max();

  /**
   * Arguments (flags/options and positional) have a "handler" which is
   * caused when the argument is parsed. The handler has an arbitrary side
   * effect, including possible affect further command-line parsing.
   *
   * There are many constructors in order to support many shorthand
   * initializations, and this is used a lot.
   */
  struct Handler {
    std::function<void(std::vector<std::string>)> fun;
    size_t arity;

    Handler() = default;

    Handler(std::function<void(std::vector<std::string>)>&& fun)
        : fun(std::move(fun)), arity(arity_any) {}

    Handler(std::function<void()>&& handler)
        : fun([handler{std::move(handler)}](std::vector<std::string>) { handler(); }), arity(0) {}

    Handler(std::function<void(std::string)>&& handler)
        : fun([handler{std::move(handler)}](std::vector<std::string> ss) {
            handler(std::move(ss[0]));
          }),
          arity(1) {}

    Handler(std::function<void(std::string, std::string)>&& handler)
        : fun([handler{std::move(handler)}](std::vector<std::string> ss) {
            handler(std::move(ss[0]), std::move(ss[1]));
          }),
          arity(2) {}

    Handler(std::vector<std::string>* dest)
        : fun([dest](std::vector<std::string> ss) { *dest = ss; }), arity(arity_any) {}

    Handler(std::string* dest)
        : fun([dest](std::vector<std::string> ss) { *dest = ss[0]; }), arity(1) {}

    Handler(std::optional<std::string>* dest)
        : fun([dest](std::vector<std::string> ss) { *dest = ss[0]; }), arity(1) {}

    Handler(std::filesystem::path* dest)
        : fun([dest](std::vector<std::string> ss) { *dest = ss[0]; }), arity(1) {}

    Handler(std::optional<std::filesystem::path>* dest)
        : fun([dest](std::vector<std::string> ss) { *dest = ss[0]; }), arity(1) {}

    template <class T>
    Handler(T* dest, const T& val)
        : fun([dest, val](std::vector<std::string> ss) { *dest = val; }), arity(0) {}

    template <class I>
    Handler(I* dest)
        : fun([dest](std::vector<std::string> ss) { *dest = string2_int_with_unit_prefix<I>(ss[0]); }),
          arity(1) {}

    template <class I>
    Handler(std::optional<I>* dest)
        : fun([dest](std::vector<std::string> ss) { *dest = string2_int_with_unit_prefix<I>(ss[0]); }),
          arity(1) {}
  };

  /**
   * The basic function type of the completion callback.
   *
   * Used to define `completer_closure_t` and some common case completers
   * that individual flags/arguments can use.
   *
   * The `add_completions_t` that is passed is an interface to the state
   * stored as part of the root command
   */
  using completer_fun_t = void(add_completions_t&, size_t, std::string_view);

  /**
   * The closure type of the completion callback.
   *
   * This is what is actually stored as part of each flag_t / Expected
   * Arg.
   */
  using completer_closure_t = std::function<completer_fun_t>;

public:
  /**
   * Description of flags / options
   *
   * These are arguments like `-s` or `--long` that can (mostly)
   * appear in any order.
   */
  struct flag_t {
    using ptr = std::shared_ptr<flag_t>;

    std::string long_name;
    string_set_t aliases;
    char short_name = 0;
    std::string description;
    std::string category;
    strings_t labels;
    Handler handler;
    completer_closure_t completer;
    bool required = false;

    std::optional<experimental_feature_t> experimental_feature;

    // FIXME: this should be private, but that breaks designated initializers.
    size_t times_used = 0;
  };

protected:
  /**
   * Index of all registered "long" flag descriptions (flags like
   * `--long`).
   */
  std::map<std::string, flag_t::ptr> longFlags;

  /**
   * Index of all registered "short" flag descriptions (flags like
   * `-s`).
   */
  std::map<char, flag_t::ptr> shortFlags;

  /**
   * Process a single flag and its arguments, pulling from an iterator
   * of raw CLI args as needed.
   */
  virtual bool process_flag(strings_t::iterator& pos, strings_t::iterator end);

public:
  /**
   * Description of positional arguments
   *
   * These are arguments that do not start with a `-`, and for which
   * the order does matter.
   */
  struct expected_arg_t {
    std::string label;
    bool optional = false;
    Handler handler;
    completer_closure_t completer;
  };

protected:
  /**
   * Queue of expected positional argument forms.
   *
   * Positional argument descriptions are inserted on the back.
   *
   * As positional arguments are passed, these are popped from the
   * front, until there are hopefully none left as all args that were
   * expected in fact were passed.
   */
  std::list<expected_arg_t> expectedArgs;
  /**
   * List of processed positional argument forms.
   *
   * All items removed from `expectedArgs` are added here. After all
   * arguments were processed, this list should be exactly the same as
   * `expectedArgs` was before.
   *
   * This list is used to extend the lifetime of the argument forms.
   * If this is not done, some closures that reference the command
   * itself will segfault.
   */
  std::list<expected_arg_t> processedArgs;

  /**
   * Process some positional arguments
   *
   * @param finish: We have parsed everything else, and these are the only
   * arguments left. Used because we accumulate some "pending args" we might
   * have left over.
   */
  virtual bool process_args(const strings_t& args, bool finish);

  virtual strings_t::iterator rewrite_args(strings_t& args, strings_t::iterator pos) { return pos; }

  string_set_t hiddenCategories;

  virtual void check_args();

  /**
   * Called after all command line flags before the first non-flag
   * argument (if any) have been processed.
   */
  virtual void initial_flags_processed() {}

public:
  void add_flag(flag_t&& flag);

  void remove_flag(const std::string& long_name);

  void expect_args(expected_arg_t&& arg) { expectedArgs.emplace_back(std::move(arg)); }

  /**
   * Expect a string argument.
   */
  void expect_arg(const std::string& label, std::string* dest, bool optional = false) {
    expect_args({.label = label, .optional = optional, .handler = {dest}});
  }

  /**
   * Expect a path argument.
   */
  void expect_arg(const std::string& label, std::filesystem::path* dest, bool optional = false) {
    expect_args({.label = label, .optional = optional, .handler = {dest}});
  }

  /**
   * Expect 0 or more arguments.
   */
  void expect_args(const std::string& label, std::vector<std::string>* dest) {
    expect_args({.label = label, .handler = {dest}});
  }

  static completer_fun_t complete_path;

  static completer_fun_t complete_dir;

  virtual nlohmann::json to_json();

  friend class multi_command_t;

  /**
   * The parent command, used if this is a subcommand.
   *
   * Invariant: An Args with a null parent must also be a root_args_t
   *
   * \todo this would probably be better in the CommandClass.
   * get_root() could be an abstract method that peels off at most one
   * layer before recuring.
   */
  multi_command_t* parent = nullptr;

  /**
   * Traverse parent pointers until we find the \ref root_args_t "root
   * arguments" object.
   */
  root_args_t& get_root();
};

/**
 * A command is an argument parser that can be executed by calling its
 * run() method.
 */
struct command_t : virtual public Args {
  friend class multi_command_t;

  virtual ~command_t() = default;

  /**
   * Entry point to the command
   */
  virtual void run() = 0;

  using category_t = int;

  static constexpr category_t cat_default = 0;

  virtual std::optional<experimental_feature_t> experimental_feature();

  virtual category_t category() { return cat_default; }
};

using commands_t = std::map<std::string, std::function<ref<command_t>()>>;

/**
 * An argument parser that supports multiple subcommands,
 * i.e. `<command> <subcommand>`.
 */
class multi_command_t : virtual public Args {
public:
  commands_t commands;

  std::map<command_t::category_t, std::string> categories;

  /**
   * Selected command, if any.
   */
  std::optional<std::pair<std::string, ref<command_t>>> command;

  multi_command_t(std::string_view command_name, const commands_t& commands);

  bool process_flag(strings_t::iterator& pos, strings_t::iterator end) override;

  bool process_args(const strings_t& args, bool finish) override;

  nlohmann::json to_json() override;

  enum struct alias_status_t {
    /** Aliases that don't go away */
    accepted_shorthand,
    /** Aliases that will go away */
    deprecated,
  };

  /** An alias, except for the original syntax, which is in the map key. */
  struct alias_info_t {
    alias_status_t status;
    std::vector<std::string> replacement;
  };

  /**
   * A list of aliases (remapping a deprecated/shorthand subcommand
   * to something else).
   */
  std::map<std::string, alias_info_t> aliases;

  strings_t::iterator rewrite_args(strings_t& args, strings_t::iterator pos) override;

protected:
  std::string command_name = "";
  bool aliasUsed = false;

  void check_args() override;
};

strings_t argv_to_strings(int argc, char** argv);

struct completion_t {
  std::string completion;
  std::string description;

  auto operator<=>(const completion_t& other) const noexcept;
};

/**
 * The abstract interface for completions callbacks
 *
 * The idea is to restrict the callback so it can only add additional
 * completions to the collection, or set the completion type. By making
 * it go through this interface, the callback cannot make any other
 * changes, or even view the completions / completion type that have
 * been set so far.
 */
class add_completions_t {
public:
  /**
   * The type of completion we are collecting.
   */
  enum class Type {
    normal,
    filenames,
    Attrs,
  };

  /**
   * Set the type of the completions being collected
   *
   * \todo it should not be possible to change the type after it has been set.
   */
  virtual void set_type(Type type) = 0;

  /**
   * Add a single completion to the collection
   */
  virtual void add(std::string completion, std::string description = "") = 0;
};

strings_t parse_shebang_content(std::string_view s);

} // namespace nix
