// straylight::nix::primitives::args - CLI argument parsing primitives
//
// Modern C++23 CLI argument parser replacing nix/util/args.h.
// Provides a CLI11-style fluent API for building command-line interfaces.
//
// Features:
//   - Fluent builder API for flags, options, and positional arguments
//   - Type-safe argument binding with automatic conversion
//   - Subcommand support with nested argument parsing
//   - Automatic help generation
//   - Required vs optional arguments with default values
//   - Validation callbacks for custom argument constraints
//   - Short (-f) and long (--flag) option syntax
//   - Combined short flags (-abc = -a -b -c)
//   - Value assignment (--opt=value, --opt value, -o value)
//
// Usage:
//   ArgumentParser parser("myapp", "My application description");
//   std::string name;
//   int count = 0;
//   bool verbose = false;
//
//   parser.add_option("--name,-n", "User name", name)
//         .required();
//   parser.add_option("--count,-c", "Item count", count)
//         .default_value(10);
//   parser.add_flag("--verbose,-v", "Enable verbose output", verbose);
//
//   auto result = parser.parse(argc, argv);
//   if (!result) {
//     std::cerr << result.error() << std::endl;
//     return 1;
//   }

#pragma once

#include <algorithm>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace straylight::nix::cli {

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────

class ArgumentParser;
class Subcommand;

// ─────────────────────────────────────────────────────────────────────────────
// ParseResult - Result type for argument parsing
// ─────────────────────────────────────────────────────────────────────────────

/// Result of parsing command-line arguments.
/// Contains either success or an error message.
class ParseResult {
public:
  /// Construct a success result
  ParseResult() : success_(true) {}

  /// Construct an error result
  explicit ParseResult(std::string error) : success_(false), error_(std::move(error)) {}

  /// Check if parsing succeeded
  [[nodiscard]] explicit operator bool() const noexcept { return success_; }

  /// Check if parsing succeeded
  [[nodiscard]] bool success() const noexcept { return success_; }

  /// Get the error message (empty if successful)
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

  /// Create a success result
  static ParseResult ok() { return {}; }

  /// Create an error result
  static ParseResult fail(std::string message) { return ParseResult(std::move(message)); }

private:
  bool success_;
  std::string error_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Type conversion utilities
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// Concept for types that can be converted from string
template <typename T>
concept string_convertible =
    std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view> ||
    std::is_arithmetic_v<T> || std::is_same_v<T, bool>;

/// Convert string to target type
template <typename T>
[[nodiscard]] std::optional<T> from_string(std::string_view value) {
  if constexpr (std::is_same_v<T, std::string>) {
    return std::string(value);
  } else if constexpr (std::is_same_v<T, std::string_view>) {
    return value;
  } else if constexpr (std::is_same_v<T, bool>) {
    if (value == "true" || value == "1" || value == "yes" || value == "on") {
      return true;
    }
    if (value == "false" || value == "0" || value == "no" || value == "off") {
      return false;
    }
    return std::nullopt;
  } else if constexpr (std::is_integral_v<T>) {
    T result{};
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (ec == std::errc{} && ptr == value.data() + value.size()) {
      return result;
    }
    return std::nullopt;
  } else if constexpr (std::is_floating_point_v<T>) {
    // std::from_chars for floating point may not be available on all platforms
    // Fall back to stringstream
    std::istringstream stream{std::string(value)};
    T result{};
    stream >> result;
    if (stream.fail() || !stream.eof()) {
      return std::nullopt;
    }
    return result;
  } else {
    return std::nullopt;
  }
}

/// Convert value to string for display
template <typename T>
[[nodiscard]] std::string to_string(const T& value) {
  if constexpr (std::is_same_v<T, std::string>) {
    return value;
  } else if constexpr (std::is_same_v<T, std::string_view>) {
    return std::string(value);
  } else if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_arithmetic_v<T>) {
    return std::to_string(value);
  } else {
    return "<unknown>";
  }
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Argument - Base class for all argument types
// ─────────────────────────────────────────────────────────────────────────────

/// Base class for argument definitions
class ArgumentBase {
public:
  virtual ~ArgumentBase() = default;

  /// Get the primary name of the argument
  [[nodiscard]] virtual std::string name() const = 0;

  /// Get the description
  [[nodiscard]] const std::string& description() const noexcept { return description_; }

  /// Check if this argument is required
  [[nodiscard]] bool is_required() const noexcept { return required_; }

  /// Check if this argument has been set
  [[nodiscard]] bool is_set() const noexcept { return set_; }

  /// Get the type name for help display
  [[nodiscard]] virtual std::string type_name() const = 0;

  /// Get the default value string for help display
  [[nodiscard]] virtual std::string default_string() const = 0;

  /// Process a value for this argument
  [[nodiscard]] virtual ParseResult process(std::string_view value) = 0;

  /// Validate the argument after parsing
  [[nodiscard]] virtual ParseResult validate() const {
    if (required_ && !set_) {
      return ParseResult::fail("Required argument missing: " + name());
    }
    return ParseResult::ok();
  }

protected:
  std::string description_;
  bool required_ = false;
  bool set_ = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Option - Named options (--name=value, -n value)
// ─────────────────────────────────────────────────────────────────────────────

/// Named option that takes a value.
///
/// Supports both long (--name) and short (-n) forms.
/// Values can be provided as --name=value or --name value.
template <typename T>
class Option : public ArgumentBase {
public:
  using value_type = T;
  using validator_type = std::function<ParseResult(const T&)>;

  /// Construct an option bound to a variable
  Option(std::string long_name, char short_name, std::string description, T& target)
      : long_name_(std::move(long_name)), short_name_(short_name), target_(&target) {
    this->description_ = std::move(description);
  }

  /// Get the primary name
  [[nodiscard]] std::string name() const override {
    if (!long_name_.empty()) {
      return "--" + long_name_;
    }
    return std::string("-") + short_name_;
  }

  /// Get the type name
  [[nodiscard]] std::string type_name() const override {
    if constexpr (std::is_same_v<T, std::string>) {
      return "STRING";
    } else if constexpr (std::is_same_v<T, bool>) {
      return "BOOL";
    } else if constexpr (std::is_integral_v<T>) {
      return "INT";
    } else if constexpr (std::is_floating_point_v<T>) {
      return "FLOAT";
    } else {
      return "VALUE";
    }
  }

  /// Get the default value string
  [[nodiscard]] std::string default_string() const override {
    if (has_default_) {
      return detail::to_string(default_value_);
    }
    return "";
  }

  /// Process a value
  [[nodiscard]] ParseResult process(std::string_view value) override {
    auto converted = detail::from_string<T>(value);
    if (!converted) {
      return ParseResult::fail("Invalid value '" + std::string(value) + "' for " + name());
    }

    if (validator_) {
      auto result = validator_(*converted);
      if (!result) {
        return result;
      }
    }

    *target_ = std::move(*converted);
    this->set_ = true;
    return ParseResult::ok();
  }

  /// Mark as required
  Option& required() {
    this->required_ = true;
    return *this;
  }

  /// Set default value
  Option& default_value(T value) {
    default_value_ = std::move(value);
    has_default_ = true;
    *target_ = default_value_;
    return *this;
  }

  /// Add a validation callback
  Option& check(validator_type validator) {
    validator_ = std::move(validator);
    return *this;
  }

  /// Get the long name
  [[nodiscard]] const std::string& long_name() const noexcept { return long_name_; }

  /// Get the short name
  [[nodiscard]] char short_name() const noexcept { return short_name_; }

private:
  std::string long_name_;
  char short_name_ = '\0';
  T* target_;
  T default_value_{};
  bool has_default_ = false;
  validator_type validator_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Flag - Boolean flags (--flag, -f)
// ─────────────────────────────────────────────────────────────────────────────

/// Boolean flag that doesn't take a value.
///
/// Presence of the flag sets the target to true.
class Flag : public ArgumentBase {
public:
  /// Construct a flag bound to a boolean variable
  Flag(std::string long_name, char short_name, std::string description, bool& target)
      : long_name_(std::move(long_name)), short_name_(short_name), target_(&target) {
    this->description_ = std::move(description);
    *target_ = false;
  }

  /// Get the primary name
  [[nodiscard]] std::string name() const override {
    if (!long_name_.empty()) {
      return "--" + long_name_;
    }
    return std::string("-") + short_name_;
  }

  /// Get the type name
  [[nodiscard]] std::string type_name() const override { return ""; }

  /// Get the default value string
  [[nodiscard]] std::string default_string() const override { return "false"; }

  /// Process the flag (sets to true)
  [[nodiscard]] ParseResult process([[maybe_unused]] std::string_view value) override {
    *target_ = true;
    this->set_ = true;
    return ParseResult::ok();
  }

  /// Validate is always successful for flags
  [[nodiscard]] ParseResult validate() const override { return ParseResult::ok(); }

  /// Get the long name
  [[nodiscard]] const std::string& long_name() const noexcept { return long_name_; }

  /// Get the short name
  [[nodiscard]] char short_name() const noexcept { return short_name_; }

private:
  std::string long_name_;
  char short_name_ = '\0';
  bool* target_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Positional - Positional arguments
// ─────────────────────────────────────────────────────────────────────────────

/// Positional argument that takes a value by position.
template <typename T>
class Positional : public ArgumentBase {
public:
  using value_type = T;
  using validator_type = std::function<ParseResult(const T&)>;

  /// Construct a positional argument
  Positional(std::string name, std::string description, T& target)
      : positional_name_(std::move(name)), target_(&target) {
    this->description_ = std::move(description);
  }

  /// Get the name
  [[nodiscard]] std::string name() const override { return positional_name_; }

  /// Get the type name
  [[nodiscard]] std::string type_name() const override {
    if constexpr (std::is_same_v<T, std::string>) {
      return "STRING";
    } else if constexpr (std::is_integral_v<T>) {
      return "INT";
    } else if constexpr (std::is_floating_point_v<T>) {
      return "FLOAT";
    } else {
      return "VALUE";
    }
  }

  /// Get the default value string
  [[nodiscard]] std::string default_string() const override {
    if (has_default_) {
      return detail::to_string(default_value_);
    }
    return "";
  }

  /// Process a value
  [[nodiscard]] ParseResult process(std::string_view value) override {
    auto converted = detail::from_string<T>(value);
    if (!converted) {
      return ParseResult::fail("Invalid value '" + std::string(value) + "' for " + name());
    }

    if (validator_) {
      auto result = validator_(*converted);
      if (!result) {
        return result;
      }
    }

    *target_ = std::move(*converted);
    this->set_ = true;
    return ParseResult::ok();
  }

  /// Mark as required
  Positional& required() {
    this->required_ = true;
    return *this;
  }

  /// Set default value
  Positional& default_value(T value) {
    default_value_ = std::move(value);
    has_default_ = true;
    *target_ = default_value_;
    return *this;
  }

  /// Add a validation callback
  Positional& check(validator_type validator) {
    validator_ = std::move(validator);
    return *this;
  }

private:
  std::string positional_name_;
  T* target_;
  T default_value_{};
  bool has_default_ = false;
  validator_type validator_;
};

// ─────────────────────────────────────────────────────────────────────────────
// MultiPositional - Multiple positional arguments (variadic)
// ─────────────────────────────────────────────────────────────────────────────

/// Positional argument that collects multiple values.
template <typename T>
class MultiPositional : public ArgumentBase {
public:
  using value_type = T;
  using container_type = std::vector<T>;

  /// Construct a multi-positional argument
  MultiPositional(std::string name, std::string description, container_type& target)
      : positional_name_(std::move(name)), target_(&target) {
    this->description_ = std::move(description);
  }

  /// Get the name
  [[nodiscard]] std::string name() const override { return positional_name_; }

  /// Get the type name
  [[nodiscard]] std::string type_name() const override {
    if constexpr (std::is_same_v<T, std::string>) {
      return "STRING...";
    } else if constexpr (std::is_integral_v<T>) {
      return "INT...";
    } else {
      return "VALUE...";
    }
  }

  /// Get the default value string
  [[nodiscard]] std::string default_string() const override { return ""; }

  /// Process a value
  [[nodiscard]] ParseResult process(std::string_view value) override {
    auto converted = detail::from_string<T>(value);
    if (!converted) {
      return ParseResult::fail("Invalid value '" + std::string(value) + "' for " + name());
    }
    target_->push_back(std::move(*converted));
    this->set_ = true;
    return ParseResult::ok();
  }

  /// Validate (check minimum count if required)
  [[nodiscard]] ParseResult validate() const override {
    if (this->required_ && target_->empty()) {
      return ParseResult::fail("Required argument missing: " + name());
    }
    if (target_->size() < min_count_) {
      return ParseResult::fail("Argument " + name() + " requires at least " +
                               std::to_string(min_count_) + " values");
    }
    return ParseResult::ok();
  }

  /// Mark as required
  MultiPositional& required() {
    this->required_ = true;
    return *this;
  }

  /// Set minimum number of values
  MultiPositional& min_values(std::size_t count) {
    min_count_ = count;
    return *this;
  }

private:
  std::string positional_name_;
  container_type* target_;
  std::size_t min_count_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Subcommand
// ─────────────────────────────────────────────────────────────────────────────

/// A subcommand with its own set of arguments.
class Subcommand {
public:
  /// Construct a subcommand
  Subcommand(std::string name, std::string description)
      : name_(std::move(name)), description_(std::move(description)) {}

  /// Get the subcommand name
  [[nodiscard]] const std::string& name() const noexcept { return name_; }

  /// Get the description
  [[nodiscard]] const std::string& description() const noexcept { return description_; }

  /// Add a flag
  Flag& add_flag(std::string_view names, std::string description, bool& target);

  /// Add an option
  template <typename T>
  Option<T>& add_option(std::string_view names, std::string description, T& target);

  /// Add a positional argument
  template <typename T>
  Positional<T>& add_positional(std::string name, std::string description, T& target);

  /// Add a multi-positional argument
  template <typename T>
  MultiPositional<T>& add_multi_positional(std::string name, std::string description,
                                           std::vector<T>& target);

  /// Set a callback to run when this subcommand is invoked
  Subcommand& callback(std::function<void()> callback) {
    callback_ = std::move(callback);
    return *this;
  }

  /// Check if this subcommand was selected
  [[nodiscard]] bool was_selected() const noexcept { return selected_; }

  /// Generate help text for this subcommand
  [[nodiscard]] std::string help() const;

  /// Parse arguments for this subcommand
  [[nodiscard]] ParseResult parse(std::vector<std::string_view>& args);

private:
  friend class ArgumentParser;

  /// Parse option names from "long_name,short_name" or "-s,--long" format
  static std::pair<std::string, char> parse_option_names(std::string_view names);

  std::string name_;
  std::string description_;
  std::vector<std::unique_ptr<ArgumentBase>> arguments_;
  std::vector<std::unique_ptr<ArgumentBase>> positionals_;
  std::unordered_map<std::string, ArgumentBase*> long_options_;
  std::unordered_map<char, ArgumentBase*> short_options_;
  std::function<void()> callback_;
  bool selected_ = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// ArgumentParser - Main parser class
// ─────────────────────────────────────────────────────────────────────────────

/// CLI argument parser with fluent API.
///
/// Example:
///   ArgumentParser parser("myapp", "My application");
///   std::string name;
///   bool verbose = false;
///
///   parser.add_option("--name,-n", "User name", name).required();
///   parser.add_flag("--verbose,-v", "Enable verbose output", verbose);
///
///   auto result = parser.parse(argc, argv);
///   if (!result) {
///     std::cerr << result.error() << std::endl;
///     std::cerr << parser.help() << std::endl;
///     return 1;
///   }
class ArgumentParser {
public:
  /// Construct a parser with program name and description
  ArgumentParser(std::string program_name, std::string description = "")
      : program_name_(std::move(program_name)), description_(std::move(description)) {}

  /// Get the program name
  [[nodiscard]] const std::string& program_name() const noexcept { return program_name_; }

  /// Get the description
  [[nodiscard]] const std::string& description() const noexcept { return description_; }

  /// Add a boolean flag
  ///
  /// @param names   Names in format "--long,-s" or "--long" or "-s"
  /// @param description  Human-readable description
  /// @param target  Boolean variable to bind to
  /// @return Reference to the created Flag for chaining
  Flag& add_flag(std::string_view names, std::string description, bool& target) {
    auto [long_name, short_name] = parse_option_names(names);
    auto flag =
        std::make_unique<Flag>(std::move(long_name), short_name, std::move(description), target);
    auto* ptr = flag.get();
    register_option(ptr, ptr->long_name(), ptr->short_name());
    arguments_.push_back(std::move(flag));
    return *ptr;
  }

  /// Add a named option
  ///
  /// @param names   Names in format "--long,-s" or "--long" or "-s"
  /// @param description  Human-readable description
  /// @param target  Variable to bind to
  /// @return Reference to the created Option for chaining
  template <typename T>
  Option<T>& add_option(std::string_view names, std::string description, T& target) {
    auto [long_name, short_name] = parse_option_names(names);
    auto option = std::make_unique<Option<T>>(std::move(long_name), short_name,
                                              std::move(description), target);
    auto* ptr = option.get();
    register_option(ptr, ptr->long_name(), ptr->short_name());
    arguments_.push_back(std::move(option));
    return *ptr;
  }

  /// Add a positional argument
  ///
  /// @param name   Argument name (for help display)
  /// @param description  Human-readable description
  /// @param target  Variable to bind to
  /// @return Reference to the created Positional for chaining
  template <typename T>
  Positional<T>& add_positional(std::string name, std::string description, T& target) {
    auto positional =
        std::make_unique<Positional<T>>(std::move(name), std::move(description), target);
    auto* ptr = positional.get();
    positionals_.push_back(std::move(positional));
    return *ptr;
  }

  /// Add a multi-positional argument (collects remaining positional args)
  ///
  /// @param name   Argument name (for help display)
  /// @param description  Human-readable description
  /// @param target  Vector to bind to
  /// @return Reference to the created MultiPositional for chaining
  template <typename T>
  MultiPositional<T>& add_multi_positional(std::string name, std::string description,
                                           std::vector<T>& target) {
    auto positional =
        std::make_unique<MultiPositional<T>>(std::move(name), std::move(description), target);
    auto* ptr = positional.get();
    positionals_.push_back(std::move(positional));
    return *ptr;
  }

  /// Add a subcommand
  ///
  /// @param name   Subcommand name
  /// @param description  Human-readable description
  /// @return Reference to the created Subcommand for chaining
  Subcommand& add_subcommand(std::string name, std::string description) {
    auto subcommand = std::make_unique<Subcommand>(std::move(name), std::move(description));
    auto* ptr = subcommand.get();
    subcommand_map_[ptr->name()] = ptr;
    subcommands_.push_back(std::move(subcommand));
    return *ptr;
  }

  /// Parse command-line arguments
  ///
  /// @param argc  Argument count from main()
  /// @param argv  Argument values from main()
  /// @return ParseResult indicating success or containing error message
  [[nodiscard]] ParseResult parse(int argc, char* argv[]) {
    std::vector<std::string_view> args;
    args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    // Skip program name (argv[0])
    for (int index = 1; index < argc; ++index) {
      args.emplace_back(argv[index]);
    }
    return parse(args);
  }

  /// Parse command-line arguments from vector
  [[nodiscard]] ParseResult parse(std::vector<std::string_view>& args) {
    std::size_t positional_index = 0;

    while (!args.empty()) {
      std::string_view current_arg = args.front();

      // Check for help flag
      if (current_arg == "--help" || current_arg == "-h") {
        help_requested_ = true;
        return ParseResult::ok();
      }

      // Check for subcommand
      if (!current_arg.starts_with("-") && !subcommands_.empty()) {
        auto subcommand_iterator = subcommand_map_.find(std::string(current_arg));
        if (subcommand_iterator != subcommand_map_.end()) {
          args.erase(args.begin());
          selected_subcommand_ = subcommand_iterator->second;
          selected_subcommand_->selected_ = true;
          return selected_subcommand_->parse(args);
        }
      }

      // Long option (--name or --name=value)
      if (current_arg.starts_with("--")) {
        args.erase(args.begin());
        auto result = process_long_option(current_arg.substr(2), args);
        if (!result) {
          return result;
        }
        continue;
      }

      // Short option(s) (-f or -abc or -f value)
      if (current_arg.starts_with("-") && current_arg.size() > 1) {
        args.erase(args.begin());
        auto result = process_short_options(current_arg.substr(1), args);
        if (!result) {
          return result;
        }
        continue;
      }

      // Positional argument
      if (positional_index < positionals_.size()) {
        args.erase(args.begin());
        auto result = positionals_[positional_index]->process(current_arg);
        if (!result) {
          return result;
        }
        // Only advance index for non-multi positionals
        if (positional_index + 1 < positionals_.size()) {
          ++positional_index;
        }
        continue;
      }

      // Unknown argument
      return ParseResult::fail("Unknown argument: " + std::string(current_arg));
    }

    // Validate all arguments
    for (const auto& argument : arguments_) {
      auto result = argument->validate();
      if (!result) {
        return result;
      }
    }
    for (const auto& positional : positionals_) {
      auto result = positional->validate();
      if (!result) {
        return result;
      }
    }

    // Check for required subcommand
    if (require_subcommand_ && !subcommands_.empty() && !selected_subcommand_) {
      return ParseResult::fail("A subcommand is required");
    }

    return ParseResult::ok();
  }

  /// Check if help was requested
  [[nodiscard]] bool help_requested() const noexcept { return help_requested_; }

  /// Get the selected subcommand (nullptr if none)
  [[nodiscard]] Subcommand* selected_subcommand() const noexcept { return selected_subcommand_; }

  /// Require a subcommand to be specified
  ArgumentParser& require_subcommand(bool require = true) {
    require_subcommand_ = require;
    return *this;
  }

  /// Generate help text
  [[nodiscard]] std::string help() const {
    std::ostringstream output_stream;

    // Usage line
    output_stream << "Usage: " << program_name_;

    if (!arguments_.empty()) {
      output_stream << " [OPTIONS]";
    }

    if (!subcommands_.empty()) {
      output_stream << " COMMAND";
    }

    for (const auto& positional : positionals_) {
      if (positional->is_required()) {
        output_stream << " <" << positional->name() << ">";
      } else {
        output_stream << " [" << positional->name() << "]";
      }
    }

    output_stream << "\n";

    // Description
    if (!description_.empty()) {
      output_stream << "\n" << description_ << "\n";
    }

    // Options
    if (!arguments_.empty()) {
      output_stream << "\nOptions:\n";
      for (const auto& argument : arguments_) {
        output_stream << "  " << format_option_help(*argument) << "\n";
      }
    }

    // Positionals
    if (!positionals_.empty()) {
      output_stream << "\nArguments:\n";
      for (const auto& positional : positionals_) {
        output_stream << "  " << positional->name();
        if (!positional->type_name().empty()) {
          output_stream << " <" << positional->type_name() << ">";
        }
        output_stream << "\n";
        output_stream << "      " << positional->description();
        if (positional->is_required()) {
          output_stream << " (required)";
        }
        auto default_string = positional->default_string();
        if (!default_string.empty()) {
          output_stream << " [default: " << default_string << "]";
        }
        output_stream << "\n";
      }
    }

    // Subcommands
    if (!subcommands_.empty()) {
      output_stream << "\nCommands:\n";
      for (const auto& subcommand : subcommands_) {
        output_stream << "  " << subcommand->name();
        output_stream << "\n";
        output_stream << "      " << subcommand->description() << "\n";
      }
    }

    output_stream << "\n  -h, --help    Show this help message\n";

    return output_stream.str();
  }

private:
  /// Parse option names from various formats
  static std::pair<std::string, char> parse_option_names(std::string_view names) {
    std::string long_name;
    char short_name = '\0';

    // Split on comma
    auto comma_position = names.find(',');
    if (comma_position != std::string_view::npos) {
      auto first_part = names.substr(0, comma_position);
      auto second_part = names.substr(comma_position + 1);

      // Trim whitespace
      while (!first_part.empty() && first_part.front() == ' ') {
        first_part.remove_prefix(1);
      }
      while (!second_part.empty() && second_part.front() == ' ') {
        second_part.remove_prefix(1);
      }

      for (auto part : {first_part, second_part}) {
        if (part.starts_with("--")) {
          long_name = std::string(part.substr(2));
        } else if (part.starts_with("-") && part.size() == 2) {
          short_name = part[1];
        }
      }
    } else {
      if (names.starts_with("--")) {
        long_name = std::string(names.substr(2));
      } else if (names.starts_with("-") && names.size() == 2) {
        short_name = names[1];
      }
    }

    return {long_name, short_name};
  }

  /// Register an option in the lookup maps
  void register_option(ArgumentBase* argument, const std::string& long_name, char short_name) {
    if (!long_name.empty()) {
      long_options_[long_name] = argument;
    }
    if (short_name != '\0') {
      short_options_[short_name] = argument;
    }
  }

  /// Process a long option (without the -- prefix)
  [[nodiscard]] ParseResult process_long_option(std::string_view option_text,
                                                std::vector<std::string_view>& remaining_args) {
    std::string_view option_name = option_text;
    std::string_view option_value;
    bool has_inline_value = false;

    // Check for --name=value format
    auto equals_position = option_text.find('=');
    if (equals_position != std::string_view::npos) {
      option_name = option_text.substr(0, equals_position);
      option_value = option_text.substr(equals_position + 1);
      has_inline_value = true;
    }

    auto option_iterator = long_options_.find(std::string(option_name));
    if (option_iterator == long_options_.end()) {
      return ParseResult::fail("Unknown option: --" + std::string(option_name));
    }

    auto* argument = option_iterator->second;

    // Flags don't take values
    if (dynamic_cast<Flag*>(argument)) {
      if (has_inline_value) {
        return ParseResult::fail("Flag --" + std::string(option_name) + " does not accept a value");
      }
      return argument->process("");
    }

    // Options require values
    if (!has_inline_value) {
      if (remaining_args.empty()) {
        return ParseResult::fail("Option --" + std::string(option_name) + " requires a value");
      }
      option_value = remaining_args.front();
      remaining_args.erase(remaining_args.begin());
    }

    return argument->process(option_value);
  }

  /// Process short options (without the - prefix)
  [[nodiscard]] ParseResult process_short_options(std::string_view options_text,
                                                  std::vector<std::string_view>& remaining_args) {
    for (std::size_t index = 0; index < options_text.size(); ++index) {
      char option_char = options_text[index];
      auto option_iterator = short_options_.find(option_char);
      if (option_iterator == short_options_.end()) {
        return ParseResult::fail("Unknown option: -" + std::string(1, option_char));
      }

      auto* argument = option_iterator->second;

      // Flags just get set
      if (dynamic_cast<Flag*>(argument)) {
        auto result = argument->process("");
        if (!result) {
          return result;
        }
        continue;
      }

      // Options need a value - either the rest of this string or the next arg
      std::string_view option_value;
      if (index + 1 < options_text.size()) {
        // Rest of string is the value (e.g., -nvalue)
        option_value = options_text.substr(index + 1);
        return argument->process(option_value);
      } else if (!remaining_args.empty()) {
        // Next argument is the value
        option_value = remaining_args.front();
        remaining_args.erase(remaining_args.begin());
        return argument->process(option_value);
      } else {
        return ParseResult::fail("Option -" + std::string(1, option_char) + " requires a value");
      }
    }

    return ParseResult::ok();
  }

  /// Format an option for help display
  [[nodiscard]] std::string format_option_help(const ArgumentBase& argument) const {
    std::ostringstream output_stream;

    // Try to get short and long names
    const Flag* flag_argument = dynamic_cast<const Flag*>(&argument);
    std::string long_name;
    char short_name = '\0';

    if (flag_argument) {
      long_name = flag_argument->long_name();
      short_name = flag_argument->short_name();
    } else {
      // Check Option types - we need to handle this polymorphically
      for (const auto& [stored_long_name, stored_argument] : long_options_) {
        if (stored_argument == &argument) {
          long_name = stored_long_name;
          break;
        }
      }
      for (const auto& [stored_short_name, stored_argument] : short_options_) {
        if (stored_argument == &argument) {
          short_name = stored_short_name;
          break;
        }
      }
    }

    if (short_name != '\0') {
      output_stream << "-" << short_name;
      if (!long_name.empty()) {
        output_stream << ", ";
      }
    } else {
      output_stream << "    ";
    }

    if (!long_name.empty()) {
      output_stream << "--" << long_name;
    }

    if (!argument.type_name().empty()) {
      output_stream << " <" << argument.type_name() << ">";
    }

    output_stream << "\n      " << argument.description();

    if (argument.is_required()) {
      output_stream << " (required)";
    }

    auto default_string = argument.default_string();
    if (!default_string.empty() && default_string != "false") {
      output_stream << " [default: " << default_string << "]";
    }

    return output_stream.str();
  }

  std::string program_name_;
  std::string description_;
  std::vector<std::unique_ptr<ArgumentBase>> arguments_;
  std::vector<std::unique_ptr<ArgumentBase>> positionals_;
  std::unordered_map<std::string, ArgumentBase*> long_options_;
  std::unordered_map<char, ArgumentBase*> short_options_;
  std::vector<std::unique_ptr<Subcommand>> subcommands_;
  std::unordered_map<std::string, Subcommand*> subcommand_map_;
  Subcommand* selected_subcommand_ = nullptr;
  bool help_requested_ = false;
  bool require_subcommand_ = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Subcommand implementation
// ─────────────────────────────────────────────────────────────────────────────

inline std::pair<std::string, char> Subcommand::parse_option_names(std::string_view names) {
  std::string long_name;
  char short_name = '\0';

  auto comma_position = names.find(',');
  if (comma_position != std::string_view::npos) {
    auto first_part = names.substr(0, comma_position);
    auto second_part = names.substr(comma_position + 1);

    while (!first_part.empty() && first_part.front() == ' ') {
      first_part.remove_prefix(1);
    }
    while (!second_part.empty() && second_part.front() == ' ') {
      second_part.remove_prefix(1);
    }

    for (auto part : {first_part, second_part}) {
      if (part.starts_with("--")) {
        long_name = std::string(part.substr(2));
      } else if (part.starts_with("-") && part.size() == 2) {
        short_name = part[1];
      }
    }
  } else {
    if (names.starts_with("--")) {
      long_name = std::string(names.substr(2));
    } else if (names.starts_with("-") && names.size() == 2) {
      short_name = names[1];
    }
  }

  return {long_name, short_name};
}

inline Flag& Subcommand::add_flag(std::string_view names, std::string description, bool& target) {
  auto [long_name, short_name] = parse_option_names(names);
  auto flag =
      std::make_unique<Flag>(std::move(long_name), short_name, std::move(description), target);
  auto* ptr = flag.get();
  if (!ptr->long_name().empty()) {
    long_options_[ptr->long_name()] = ptr;
  }
  if (ptr->short_name() != '\0') {
    short_options_[ptr->short_name()] = ptr;
  }
  arguments_.push_back(std::move(flag));
  return *ptr;
}

template <typename T>
Option<T>& Subcommand::add_option(std::string_view names, std::string description, T& target) {
  auto [long_name, short_name] = parse_option_names(names);
  auto option =
      std::make_unique<Option<T>>(std::move(long_name), short_name, std::move(description), target);
  auto* ptr = option.get();
  if (!ptr->long_name().empty()) {
    long_options_[ptr->long_name()] = ptr;
  }
  if (ptr->short_name() != '\0') {
    short_options_[ptr->short_name()] = ptr;
  }
  arguments_.push_back(std::move(option));
  return *ptr;
}

template <typename T>
Positional<T>& Subcommand::add_positional(std::string name, std::string description, T& target) {
  auto positional =
      std::make_unique<Positional<T>>(std::move(name), std::move(description), target);
  auto* ptr = positional.get();
  positionals_.push_back(std::move(positional));
  return *ptr;
}

template <typename T>
MultiPositional<T>& Subcommand::add_multi_positional(std::string name, std::string description,
                                                     std::vector<T>& target) {
  auto positional =
      std::make_unique<MultiPositional<T>>(std::move(name), std::move(description), target);
  auto* ptr = positional.get();
  positionals_.push_back(std::move(positional));
  return *ptr;
}

inline std::string Subcommand::help() const {
  std::ostringstream output_stream;

  output_stream << "Subcommand: " << name_ << "\n";
  if (!description_.empty()) {
    output_stream << "\n" << description_ << "\n";
  }

  if (!arguments_.empty()) {
    output_stream << "\nOptions:\n";
    for (const auto& argument : arguments_) {
      output_stream << "  " << argument->name() << "\n";
      output_stream << "      " << argument->description() << "\n";
    }
  }

  if (!positionals_.empty()) {
    output_stream << "\nArguments:\n";
    for (const auto& positional : positionals_) {
      output_stream << "  " << positional->name() << "\n";
      output_stream << "      " << positional->description() << "\n";
    }
  }

  return output_stream.str();
}

inline ParseResult Subcommand::parse(std::vector<std::string_view>& args) {
  std::size_t positional_index = 0;

  while (!args.empty()) {
    std::string_view current_arg = args.front();

    // Check for help flag
    if (current_arg == "--help" || current_arg == "-h") {
      return ParseResult::ok();
    }

    // Long option
    if (current_arg.starts_with("--")) {
      args.erase(args.begin());
      std::string_view option_text = current_arg.substr(2);
      std::string_view option_name = option_text;
      std::string_view option_value;
      bool has_inline_value = false;

      auto equals_position = option_text.find('=');
      if (equals_position != std::string_view::npos) {
        option_name = option_text.substr(0, equals_position);
        option_value = option_text.substr(equals_position + 1);
        has_inline_value = true;
      }

      auto option_iterator = long_options_.find(std::string(option_name));
      if (option_iterator == long_options_.end()) {
        return ParseResult::fail("Unknown option: --" + std::string(option_name));
      }

      auto* argument = option_iterator->second;

      if (dynamic_cast<Flag*>(argument)) {
        if (has_inline_value) {
          return ParseResult::fail("Flag --" + std::string(option_name) +
                                   " does not accept a value");
        }
        auto result = argument->process("");
        if (!result) {
          return result;
        }
      } else {
        if (!has_inline_value) {
          if (args.empty()) {
            return ParseResult::fail("Option --" + std::string(option_name) + " requires a value");
          }
          option_value = args.front();
          args.erase(args.begin());
        }
        auto result = argument->process(option_value);
        if (!result) {
          return result;
        }
      }
      continue;
    }

    // Short option(s)
    if (current_arg.starts_with("-") && current_arg.size() > 1) {
      args.erase(args.begin());
      std::string_view options_text = current_arg.substr(1);

      for (std::size_t index = 0; index < options_text.size(); ++index) {
        char option_char = options_text[index];
        auto option_iterator = short_options_.find(option_char);
        if (option_iterator == short_options_.end()) {
          return ParseResult::fail("Unknown option: -" + std::string(1, option_char));
        }

        auto* argument = option_iterator->second;

        if (dynamic_cast<Flag*>(argument)) {
          auto result = argument->process("");
          if (!result) {
            return result;
          }
          continue;
        }

        std::string_view option_value;
        if (index + 1 < options_text.size()) {
          option_value = options_text.substr(index + 1);
          auto result = argument->process(option_value);
          if (!result) {
            return result;
          }
          break;
        } else if (!args.empty()) {
          option_value = args.front();
          args.erase(args.begin());
          auto result = argument->process(option_value);
          if (!result) {
            return result;
          }
          break;
        } else {
          return ParseResult::fail("Option -" + std::string(1, option_char) + " requires a value");
        }
      }
      continue;
    }

    // Positional argument
    if (positional_index < positionals_.size()) {
      args.erase(args.begin());
      auto result = positionals_[positional_index]->process(current_arg);
      if (!result) {
        return result;
      }
      if (positional_index + 1 < positionals_.size()) {
        ++positional_index;
      }
      continue;
    }

    return ParseResult::fail("Unknown argument: " + std::string(current_arg));
  }

  // Validate all arguments
  for (const auto& argument : arguments_) {
    auto result = argument->validate();
    if (!result) {
      return result;
    }
  }
  for (const auto& positional : positionals_) {
    auto result = positional->validate();
    if (!result) {
      return result;
    }
  }

  // Execute callback if set
  if (callback_) {
    callback_();
  }

  return ParseResult::ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// Common validators
// ─────────────────────────────────────────────────────────────────────────────

namespace validators {

/// Validate that a value is within a range [min, max]
template <typename T>
auto range(T min_value, T max_value) {
  return [min_value, max_value](const T& value) -> ParseResult {
    if (value < min_value || value > max_value) {
      return ParseResult::fail("Value " + detail::to_string(value) + " is outside range [" +
                               detail::to_string(min_value) + ", " + detail::to_string(max_value) +
                               "]");
    }
    return ParseResult::ok();
  };
}

/// Validate that a value is positive (> 0)
template <typename T>
auto positive() {
  return [](const T& value) -> ParseResult {
    if (value <= T{0}) {
      return ParseResult::fail("Value must be positive");
    }
    return ParseResult::ok();
  };
}

/// Validate that a value is non-negative (>= 0)
template <typename T>
auto non_negative() {
  return [](const T& value) -> ParseResult {
    if (value < T{0}) {
      return ParseResult::fail("Value must be non-negative");
    }
    return ParseResult::ok();
  };
}

/// Validate that a string is non-empty
inline auto non_empty() {
  return [](const std::string& value) -> ParseResult {
    if (value.empty()) {
      return ParseResult::fail("Value must not be empty");
    }
    return ParseResult::ok();
  };
}

/// Validate that a value is one of a set of allowed values
template <typename T>
auto one_of(std::initializer_list<T> allowed_values) {
  return [allowed = std::vector<T>(allowed_values)](const T& value) -> ParseResult {
    if (std::find(allowed.begin(), allowed.end(), value) == allowed.end()) {
      std::ostringstream message;
      message << "Value must be one of: ";
      bool first = true;
      for (const auto& allowed_value : allowed) {
        if (!first) {
          message << ", ";
        }
        message << detail::to_string(allowed_value);
        first = false;
      }
      return ParseResult::fail(message.str());
    }
    return ParseResult::ok();
  };
}

} // namespace validators

} // namespace straylight::nix::cli
