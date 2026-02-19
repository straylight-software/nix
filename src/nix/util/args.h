#ifndef NIX_UTIL_ARGS_H
#define NIX_UTIL_ARGS_H
///@file

#include <compare>
#include <cstdint>
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
enum struct hash_format_t : std::uint8_t;

class multi_command_t;

class root_args_t;

class add_completions_t;

class Args {
public:
  virtual ~Args() = default;

  /**
   * Return a short one-line description of the command.
   */
  [[nodiscard]] virtual auto description() -> std::string { return ""; }

  [[nodiscard]] virtual auto force_impure_by_default() -> bool { return false; }

  /**
   * Return documentation about this command, in Markdown format.
   */
  [[nodiscard]] virtual auto doc() -> std::string { return ""; }

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
  [[nodiscard]] virtual auto get_command_base_dir() const -> std::filesystem::path;

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
  class handler_t {
  public:
    handler_t() : arity_(0) {}

    handler_t(std::function<void(std::vector<std::string>)>&& func)
        : fun_(std::move(func)), arity_(arity_any) {}

    handler_t(std::function<void()>&& handler)
        : fun_([handler{std::move(handler)}](const std::vector<std::string>& /*strs*/) {
            handler();
          }),
          arity_(0) {}

    handler_t(std::function<void(std::string)>&& handler)
        : fun_([handler{std::move(handler)}](std::vector<std::string> strs) {
            handler(std::move(strs[0]));
          }),
          arity_(1) {}

    handler_t(std::function<void(std::string, std::string)>&& handler)
        : fun_([handler{std::move(handler)}](std::vector<std::string> strs) {
            handler(std::move(strs[0]), std::move(strs[1]));
          }),
          arity_(2) {}

    handler_t(std::vector<std::string>* dest)
        : fun_([dest](std::vector<std::string> strs) { *dest = std::move(strs); }),
          arity_(arity_any) {}

    handler_t(std::string* dest)
        : fun_([dest](std::vector<std::string> strs) { *dest = strs[0]; }), arity_(1) {}

    handler_t(std::optional<std::string>* dest)
        : fun_([dest](std::vector<std::string> strs) { *dest = strs[0]; }), arity_(1) {}

    handler_t(std::filesystem::path* dest)
        : fun_([dest](std::vector<std::string> strs) { *dest = strs[0]; }), arity_(1) {}

    handler_t(std::optional<std::filesystem::path>* dest)
        : fun_([dest](std::vector<std::string> strs) { *dest = strs[0]; }), arity_(1) {}

    template <class T>
    handler_t(T* dest, const T& val)
        : fun_([dest, val](const std::vector<std::string>& /*strs*/) { *dest = val; }), arity_(0) {}

    template <class I>
    handler_t(I* dest)
        : fun_([dest](std::vector<std::string> strs) {
            *dest = string2_int_with_unit_prefix<I>(strs[0]);
          }),
          arity_(1) {}

    template <class I>
    handler_t(std::optional<I>* dest)
        : fun_([dest](std::vector<std::string> strs) {
            *dest = string2_int_with_unit_prefix<I>(strs[0]);
          }),
          arity_(1) {}

    [[nodiscard]] auto get_fun() const -> const std::function<void(std::vector<std::string>)>& {
      return fun_;
    }
    [[nodiscard]] auto get_arity() const -> size_t { return arity_; }

    void call(std::vector<std::string> args) const {
      if (fun_) {
        fun_(std::move(args));
      }
    }

  private:
    std::function<void(std::vector<std::string>)> fun_;
    size_t arity_;
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
    handler_t handler;
    completer_closure_t completer;
    bool required = false;

    std::optional<experimental_feature_t> experimental_feature;

    // FIXME: this should be private, but that breaks designated initializers.
    size_t times_used = 0;
  };

private:
  /**
   * Index of all registered "long" flag descriptions (flags like
   * `--long`).
   */
  std::map<std::string, flag_t::ptr> long_flags_;

  /**
   * Index of all registered "short" flag descriptions (flags like
   * `-s`).
   */
  std::map<char, flag_t::ptr> short_flags_;

protected:
  /**
   * Process a single flag and its arguments, pulling from an iterator
   * of raw CLI args as needed.
   */
  virtual auto process_flag(strings_t::iterator& pos, strings_t::iterator end) -> bool;

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
    handler_t handler;
    completer_closure_t completer;
  };

private:
  /**
   * Queue of expected positional argument forms.
   *
   * Positional argument descriptions are inserted on the back.
   *
   * As positional arguments are passed, these are popped from the
   * front, until there are hopefully none left as all args that were
   * expected in fact were passed.
   */
  std::list<expected_arg_t> expected_args_;
  /**
   * List of processed positional argument forms.
   *
   * All items removed from `expected_args_` are added here. After all
   * arguments were processed, this list should be exactly the same as
   * `expected_args_` was before.
   *
   * This list is used to extend the lifetime of the argument forms.
   * If this is not done, some closures that reference the command
   * itself will segfault.
   */
  std::list<expected_arg_t> processed_args_;

  /**
   * Hidden categories set.
   */
  string_set_t hidden_categories_;

protected:
  /**
   * Process some positional arguments
   *
   * @param finish: We have parsed everything else, and these are the only
   * arguments left. Used because we accumulate some "pending args" we might
   * have left over.
   */
  virtual auto process_args(const strings_t& args, bool finish) -> bool;

  virtual auto rewrite_args(strings_t& /*args*/, strings_t::iterator pos) -> strings_t::iterator {
    return pos;
  }

  virtual void check_args();

  /**
   * Called after all command line flags before the first non-flag
   * argument (if any) have been processed.
   */
  virtual void initial_flags_processed() {}

public:
  void add_flag(flag_t&& flag);

  void remove_flag(const std::string& long_name);

  void hide_category(const std::string& category) { hidden_categories_.insert(category); }

  void expect_args(expected_arg_t&& arg) { expected_args_.emplace_back(std::move(arg)); }

  void clear_expected_args() { expected_args_.clear(); }

  /**
   * Expect a string argument.
   */
  void expect_arg(const std::string& label, std::string* dest, bool optional = false) {
    expect_args({.label = label, .optional = optional, .handler = {dest}, .completer = {}});
  }

  /**
   * Expect a path argument.
   */
  void expect_arg(const std::string& label, std::filesystem::path* dest, bool optional = false) {
    expect_args({.label = label, .optional = optional, .handler = {dest}, .completer = {}});
  }

  /**
   * Expect 0 or more arguments.
   */
  void expect_args(const std::string& label, std::vector<std::string>* dest) {
    expect_args({.label = label, .handler = {dest}, .completer = {}});
  }

  static completer_fun_t complete_path;

  static completer_fun_t complete_dir;

  virtual auto to_json() -> nlohmann::json;

  friend class multi_command_t;

  /**
   * Traverse parent pointers until we find the \ref root_args_t "root
   * arguments" object.
   */
  [[nodiscard]] auto get_root() -> root_args_t&;

  /**
   * Get parent command pointer.
   */
  [[nodiscard]] auto get_parent() const -> multi_command_t* { return parent_; }

  /**
   * Set parent command pointer.
   */
  void set_parent(multi_command_t* parent) { parent_ = parent; }

private:
  /**
   * The parent command, used if this is a subcommand.
   *
   * Invariant: An Args with a null parent must also be a root_args_t
   *
   * \todo this would probably be better in the CommandClass.
   * get_root() could be an abstract method that peels off at most one
   * layer before recuring.
   */
  multi_command_t* parent_ = nullptr;
};

/**
 * A command is an argument parser that can be executed by calling its
 * run() method.
 */
struct command_t : virtual public Args {
  friend class multi_command_t;

  ~command_t() override = default;

  /**
   * Entry point to the command
   */
  virtual void run() = 0;

  using category_t = int;

  static constexpr category_t cat_default = 0;

  [[nodiscard]] virtual auto experimental_feature() -> std::optional<experimental_feature_t>;

  [[nodiscard]] virtual auto category() -> category_t { return cat_default; }
};

using commands_t = std::map<std::string, std::function<ref<command_t>()>>;

/**
 * An argument parser that supports multiple subcommands,
 * i.e. `<command> <subcommand>`.
 */
class multi_command_t : virtual public Args {
public:
  ~multi_command_t() override = default;

  multi_command_t(std::string_view cmd_name, const commands_t& cmds);

  auto process_flag(strings_t::iterator& pos, strings_t::iterator end) -> bool override;

  auto process_args(const strings_t& args, bool finish) -> bool override;

  auto to_json() -> nlohmann::json override;

  enum struct alias_status_t : std::uint8_t {
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

  auto rewrite_args(strings_t& args, strings_t::iterator pos) -> strings_t::iterator override;

  // Accessors for private members
  [[nodiscard]] auto get_commands() const -> const commands_t& { return commands_; }
  [[nodiscard]] auto get_commands() -> commands_t& { return commands_; }
  [[nodiscard]] auto get_categories() const -> const std::map<command_t::category_t, std::string>& {
    return categories_;
  }
  [[nodiscard]] auto get_categories() -> std::map<command_t::category_t, std::string>& {
    return categories_;
  }
  [[nodiscard]] auto get_command() const
      -> const std::optional<std::pair<std::string, ref<command_t>>>& {
    return command_;
  }
  [[nodiscard]] auto get_command() -> std::optional<std::pair<std::string, ref<command_t>>>& {
    return command_;
  }
  [[nodiscard]] auto get_command_name() const -> const std::string& { return command_name_; }
  [[nodiscard]] auto get_aliases() const -> const std::map<std::string, alias_info_t>& {
    return aliases_;
  }
  [[nodiscard]] auto get_aliases() -> std::map<std::string, alias_info_t>& { return aliases_; }

protected:
  void check_args() override;

private:
  commands_t commands_;

  std::map<command_t::category_t, std::string> categories_;

  /**
   * Selected command, if any.
   */
  std::optional<std::pair<std::string, ref<command_t>>> command_;

  /**
   * A list of aliases (remapping a deprecated/shorthand subcommand
   * to something else).
   */
  std::map<std::string, alias_info_t> aliases_;

  std::string command_name_;
  bool alias_used_ = false;
};

auto argv_to_strings(int argc, char** argv) -> strings_t;

/**
 * A completion entry with its description.
 */
class completion_t {
public:
  completion_t(std::string comp, std::string desc)
      : completion_(std::move(comp)), description_(std::move(desc)) {}

  [[nodiscard]] auto get_completion() const -> const std::string& { return completion_; }
  [[nodiscard]] auto get_description() const -> const std::string& { return description_; }

  [[nodiscard]] auto operator<=>(const completion_t& other) const noexcept -> std::strong_ordering;

private:
  std::string completion_;
  std::string description_;
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
  virtual ~add_completions_t() = default;

  /**
   * The type of completion we are collecting.
   */
  enum class Type : std::uint8_t {
    normal,
    filenames,
    attrs,
  };

  /**
   * Set the type of the completions being collected
   *
   * \todo it should not be possible to change the type after it has been set.
   */
  virtual void set_type(Type completion_type) = 0;

  /**
   * Add a single completion to the collection
   */
  virtual void add(std::string completion, std::string description = "") = 0;
};

strings_t parse_shebang_content(std::string_view s);

} // namespace nix

#endif // NIX_UTIL_ARGS_H
