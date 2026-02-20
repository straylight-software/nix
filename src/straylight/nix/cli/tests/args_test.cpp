// straylight::nix::cli::args tests
//
// Tests for CLI argument parsing primitives: ArgumentParser, Flag, Option,
// Positional, Subcommand, and validators.

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "straylight/nix/cli/args.h"

namespace args = straylight::nix::cli;

// ─────────────────────────────────────────────────────────────────────────────
// ParseResult tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ParseResult success is truthy", "[args][parse_result]") {
  auto result = args::ParseResult::ok();
  REQUIRE(result);
  REQUIRE(result.success());
  REQUIRE(result.error().empty());
}

TEST_CASE("ParseResult failure is falsy", "[args][parse_result]") {
  auto result = args::ParseResult::fail("test error");
  REQUIRE_FALSE(result);
  REQUIRE_FALSE(result.success());
  REQUIRE(result.error() == "test error");
}

// ─────────────────────────────────────────────────────────────────────────────
// Basic flag tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Flag with long name only", "[args][flag]") {
  args::ArgumentParser parser("test", "Test application");
  bool verbose = false;

  parser.add_flag("--verbose", "Enable verbose output", verbose);

  std::vector<std::string_view> arguments = {"--verbose"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(verbose);
}

TEST_CASE("Flag with short name only", "[args][flag]") {
  args::ArgumentParser parser("test", "Test application");
  bool verbose = false;

  parser.add_flag("-v", "Enable verbose output", verbose);

  std::vector<std::string_view> arguments = {"-v"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(verbose);
}

TEST_CASE("Flag with both long and short names", "[args][flag]") {
  args::ArgumentParser parser("test", "Test application");
  bool verbose = false;

  parser.add_flag("--verbose,-v", "Enable verbose output", verbose);

  SECTION("Long name works") {
    std::vector<std::string_view> arguments = {"--verbose"};
    auto result = parser.parse(arguments);
    REQUIRE(result);
    REQUIRE(verbose);
  }

  SECTION("Short name works") {
    std::vector<std::string_view> arguments = {"-v"};
    auto result = parser.parse(arguments);
    REQUIRE(result);
    REQUIRE(verbose);
  }
}

TEST_CASE("Flag defaults to false", "[args][flag]") {
  args::ArgumentParser parser("test", "Test application");
  bool verbose = true; // Start with true

  parser.add_flag("--verbose", "Enable verbose output", verbose);

  REQUIRE_FALSE(verbose); // Flag constructor sets to false

  std::vector<std::string_view> arguments = {};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE_FALSE(verbose);
}

TEST_CASE("Multiple flags can be combined with short syntax", "[args][flag]") {
  args::ArgumentParser parser("test", "Test application");
  bool flag_a = false;
  bool flag_b = false;
  bool flag_c = false;

  parser.add_flag("-a", "Flag A", flag_a);
  parser.add_flag("-b", "Flag B", flag_b);
  parser.add_flag("-c", "Flag C", flag_c);

  std::vector<std::string_view> arguments = {"-abc"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(flag_a);
  REQUIRE(flag_b);
  REQUIRE(flag_c);
}

TEST_CASE("Flag does not accept value with equals", "[args][flag]") {
  args::ArgumentParser parser("test", "Test application");
  bool verbose = false;

  parser.add_flag("--verbose", "Enable verbose output", verbose);

  std::vector<std::string_view> arguments = {"--verbose=true"};
  auto result = parser.parse(arguments);

  REQUIRE_FALSE(result);
  REQUIRE(result.error().find("does not accept a value") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Option tests - string type
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("String option with long name and space separator", "[args][option]") {
  args::ArgumentParser parser("test", "Test application");
  std::string name;

  parser.add_option("--name", "User name", name);

  std::vector<std::string_view> arguments = {"--name", "alice"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(name == "alice");
}

TEST_CASE("String option with long name and equals separator", "[args][option]") {
  args::ArgumentParser parser("test", "Test application");
  std::string name;

  parser.add_option("--name", "User name", name);

  std::vector<std::string_view> arguments = {"--name=bob"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(name == "bob");
}

TEST_CASE("String option with short name", "[args][option]") {
  args::ArgumentParser parser("test", "Test application");
  std::string name;

  parser.add_option("-n", "User name", name);

  std::vector<std::string_view> arguments = {"-n", "charlie"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(name == "charlie");
}

TEST_CASE("String option with short name and attached value", "[args][option]") {
  args::ArgumentParser parser("test", "Test application");
  std::string name;

  parser.add_option("-n", "User name", name);

  std::vector<std::string_view> arguments = {"-ndave"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(name == "dave");
}

TEST_CASE("String option with both long and short names", "[args][option]") {
  args::ArgumentParser parser("test", "Test application");
  std::string name;

  parser.add_option("--name,-n", "User name", name);

  SECTION("Long name with equals works") {
    std::vector<std::string_view> arguments = {"--name=eve"};
    auto result = parser.parse(arguments);
    REQUIRE(result);
    REQUIRE(name == "eve");
  }

  SECTION("Short name works") {
    std::vector<std::string_view> arguments = {"-n", "frank"};
    auto result = parser.parse(arguments);
    REQUIRE(result);
    REQUIRE(name == "frank");
  }
}

TEST_CASE("Option without value fails", "[args][option]") {
  args::ArgumentParser parser("test", "Test application");
  std::string name;

  parser.add_option("--name", "User name", name);

  std::vector<std::string_view> arguments = {"--name"};
  auto result = parser.parse(arguments);

  REQUIRE_FALSE(result);
  REQUIRE(result.error().find("requires a value") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Option tests - integer type
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Integer option parses correctly", "[args][option]") {
  args::ArgumentParser parser("test", "Test application");
  int count = 0;

  parser.add_option("--count,-c", "Item count", count);

  std::vector<std::string_view> arguments = {"--count=42"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(count == 42);
}

TEST_CASE("Integer option with negative value", "[args][option]") {
  args::ArgumentParser parser("test", "Test application");
  int value = 0;

  parser.add_option("--value", "A value", value);

  std::vector<std::string_view> arguments = {"--value=-10"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(value == -10);
}

TEST_CASE("Integer option with invalid value fails", "[args][option]") {
  args::ArgumentParser parser("test", "Test application");
  int count = 0;

  parser.add_option("--count", "Item count", count);

  std::vector<std::string_view> arguments = {"--count=abc"};
  auto result = parser.parse(arguments);

  REQUIRE_FALSE(result);
  REQUIRE(result.error().find("Invalid value") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Option tests - floating point type
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Float option parses correctly", "[args][option]") {
  args::ArgumentParser parser("test", "Test application");
  double ratio = 0.0;

  parser.add_option("--ratio", "Ratio value", ratio);

  std::vector<std::string_view> arguments = {"--ratio=3.14"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(ratio == Catch::Approx(3.14));
}

TEST_CASE("Float option with negative value", "[args][option]") {
  args::ArgumentParser parser("test", "Test application");
  double value = 0.0;

  parser.add_option("--value", "A value", value);

  std::vector<std::string_view> arguments = {"--value=-2.5"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(value == Catch::Approx(-2.5));
}

// ─────────────────────────────────────────────────────────────────────────────
// Required and default value tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Required option fails when missing", "[args][option]") {
  args::ArgumentParser parser("test", "Test application");
  std::string name;

  parser.add_option("--name", "User name", name).required();

  std::vector<std::string_view> arguments = {};
  auto result = parser.parse(arguments);

  REQUIRE_FALSE(result);
  REQUIRE(result.error().find("Required") != std::string::npos);
}

TEST_CASE("Required option succeeds when provided", "[args][option]") {
  args::ArgumentParser parser("test", "Test application");
  std::string name;

  parser.add_option("--name", "User name", name).required();

  std::vector<std::string_view> arguments = {"--name=test"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(name == "test");
}

TEST_CASE("Default value is used when option not provided", "[args][option]") {
  args::ArgumentParser parser("test", "Test application");
  int count = 0;

  parser.add_option("--count", "Item count", count).default_value(10);

  REQUIRE(count == 10); // Default applied immediately

  std::vector<std::string_view> arguments = {};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(count == 10);
}

TEST_CASE("Provided value overrides default", "[args][option]") {
  args::ArgumentParser parser("test", "Test application");
  int count = 0;

  parser.add_option("--count", "Item count", count).default_value(10);

  std::vector<std::string_view> arguments = {"--count=5"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(count == 5);
}

// ─────────────────────────────────────────────────────────────────────────────
// Positional argument tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Single positional argument", "[args][positional]") {
  args::ArgumentParser parser("test", "Test application");
  std::string filename;

  parser.add_positional("filename", "Input file", filename);

  std::vector<std::string_view> arguments = {"input.txt"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(filename == "input.txt");
}

TEST_CASE("Multiple positional arguments", "[args][positional]") {
  args::ArgumentParser parser("test", "Test application");
  std::string source;
  std::string destination;

  parser.add_positional("source", "Source file", source);
  parser.add_positional("destination", "Destination file", destination);

  std::vector<std::string_view> arguments = {"input.txt", "output.txt"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(source == "input.txt");
  REQUIRE(destination == "output.txt");
}

TEST_CASE("Required positional argument fails when missing", "[args][positional]") {
  args::ArgumentParser parser("test", "Test application");
  std::string filename;

  parser.add_positional("filename", "Input file", filename).required();

  std::vector<std::string_view> arguments = {};
  auto result = parser.parse(arguments);

  REQUIRE_FALSE(result);
  REQUIRE(result.error().find("Required") != std::string::npos);
}

TEST_CASE("Positional argument with default value", "[args][positional]") {
  args::ArgumentParser parser("test", "Test application");
  std::string filename;

  parser.add_positional("filename", "Input file", filename)
      .default_value(std::string("default.txt"));

  std::vector<std::string_view> arguments = {};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(filename == "default.txt");
}

TEST_CASE("Integer positional argument", "[args][positional]") {
  args::ArgumentParser parser("test", "Test application");
  int port = 0;

  parser.add_positional("port", "Port number", port);

  std::vector<std::string_view> arguments = {"8080"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(port == 8080);
}

// ─────────────────────────────────────────────────────────────────────────────
// Multi-positional argument tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Multi-positional collects multiple values", "[args][positional]") {
  args::ArgumentParser parser("test", "Test application");
  std::vector<std::string> files;

  parser.add_multi_positional("files", "Input files", files);

  std::vector<std::string_view> arguments = {"a.txt", "b.txt", "c.txt"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(files.size() == 3);
  REQUIRE(files[0] == "a.txt");
  REQUIRE(files[1] == "b.txt");
  REQUIRE(files[2] == "c.txt");
}

TEST_CASE("Multi-positional with minimum values", "[args][positional]") {
  args::ArgumentParser parser("test", "Test application");
  std::vector<std::string> files;

  parser.add_multi_positional("files", "Input files", files).min_values(2);

  SECTION("Fails when fewer than minimum") {
    std::vector<std::string_view> arguments = {"a.txt"};
    auto result = parser.parse(arguments);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().find("at least") != std::string::npos);
  }

  SECTION("Succeeds with minimum values") {
    std::vector<std::string_view> arguments = {"a.txt", "b.txt"};
    auto result = parser.parse(arguments);
    REQUIRE(result);
    REQUIRE(files.size() == 2);
  }
}

TEST_CASE("Multi-positional integer values", "[args][positional]") {
  args::ArgumentParser parser("test", "Test application");
  std::vector<int> numbers;

  parser.add_multi_positional("numbers", "Numbers", numbers);

  std::vector<std::string_view> arguments = {"1", "2", "3", "4", "5"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(numbers.size() == 5);
  REQUIRE(numbers[0] == 1);
  REQUIRE(numbers[4] == 5);
}

// ─────────────────────────────────────────────────────────────────────────────
// Mixed options and positionals
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Options and positionals mixed", "[args][mixed]") {
  args::ArgumentParser parser("test", "Test application");
  bool verbose = false;
  std::string output;
  std::string input;

  parser.add_flag("--verbose,-v", "Verbose output", verbose);
  parser.add_option("--output,-o", "Output file", output);
  parser.add_positional("input", "Input file", input);

  std::vector<std::string_view> arguments = {"-v", "--output=out.txt", "input.txt"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(verbose);
  REQUIRE(output == "out.txt");
  REQUIRE(input == "input.txt");
}

TEST_CASE("Options can come after positionals", "[args][mixed]") {
  args::ArgumentParser parser("test", "Test application");
  bool verbose = false;
  std::string input;

  parser.add_flag("--verbose", "Verbose output", verbose);
  parser.add_positional("input", "Input file", input);

  // This should work because input.txt doesn't start with -
  std::vector<std::string_view> arguments = {"input.txt", "--verbose"};
  auto result = parser.parse(arguments);

  // Actually, with the current implementation, once we hit a positional,
  // we don't go back to options. Let's check what happens.
  // The parser should still work as it processes one arg at a time.
  REQUIRE(result);
  REQUIRE(input == "input.txt");
  REQUIRE(verbose);
}

// ─────────────────────────────────────────────────────────────────────────────
// Help flag tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Help flag with --help", "[args][help]") {
  args::ArgumentParser parser("test", "Test application");
  std::string name;

  parser.add_option("--name", "User name", name).required();

  std::vector<std::string_view> arguments = {"--help"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(parser.help_requested());
}

TEST_CASE("Help flag with -h", "[args][help]") {
  args::ArgumentParser parser("test", "Test application");

  std::vector<std::string_view> arguments = {"-h"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(parser.help_requested());
}

TEST_CASE("Help text contains program name", "[args][help]") {
  args::ArgumentParser parser("myprogram", "My awesome program");

  auto help_text = parser.help();

  REQUIRE(help_text.find("myprogram") != std::string::npos);
}

TEST_CASE("Help text contains description", "[args][help]") {
  args::ArgumentParser parser("test", "This is a test program");

  auto help_text = parser.help();

  REQUIRE(help_text.find("This is a test program") != std::string::npos);
}

TEST_CASE("Help text contains option descriptions", "[args][help]") {
  args::ArgumentParser parser("test", "Test");
  bool verbose = false;
  std::string name;

  parser.add_flag("--verbose", "Enable verbose output", verbose);
  parser.add_option("--name", "User name", name);

  auto help_text = parser.help();

  REQUIRE(help_text.find("verbose") != std::string::npos);
  REQUIRE(help_text.find("Enable verbose output") != std::string::npos);
  REQUIRE(help_text.find("name") != std::string::npos);
  REQUIRE(help_text.find("User name") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Subcommand tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Subcommand is selected", "[args][subcommand]") {
  args::ArgumentParser parser("test", "Test application");

  auto& install = parser.add_subcommand("install", "Install a package");
  auto& remove = parser.add_subcommand("remove", "Remove a package");

  std::vector<std::string_view> arguments = {"install"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(parser.selected_subcommand() == &install);
  REQUIRE(install.was_selected());
  REQUIRE_FALSE(remove.was_selected());
}

TEST_CASE("Subcommand with options", "[args][subcommand]") {
  args::ArgumentParser parser("test", "Test application");

  auto& install = parser.add_subcommand("install", "Install a package");
  std::string package;
  bool global = false;

  install.add_positional("package", "Package name", package);
  install.add_flag("--global,-g", "Install globally", global);

  std::vector<std::string_view> arguments = {"install", "lodash", "--global"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(install.was_selected());
  REQUIRE(package == "lodash");
  REQUIRE(global);
}

TEST_CASE("Subcommand with required option", "[args][subcommand]") {
  args::ArgumentParser parser("test", "Test application");

  auto& install = parser.add_subcommand("install", "Install a package");
  std::string package;

  install.add_positional("package", "Package name", package).required();

  SECTION("Fails when required option missing") {
    std::vector<std::string_view> arguments = {"install"};
    auto result = parser.parse(arguments);
    REQUIRE_FALSE(result);
  }

  SECTION("Succeeds when required option provided") {
    std::vector<std::string_view> arguments = {"install", "react"};
    auto result = parser.parse(arguments);
    REQUIRE(result);
    REQUIRE(package == "react");
  }
}

TEST_CASE("Required subcommand fails when not provided", "[args][subcommand]") {
  args::ArgumentParser parser("test", "Test application");

  parser.add_subcommand("install", "Install a package");
  parser.require_subcommand();

  std::vector<std::string_view> arguments = {};
  auto result = parser.parse(arguments);

  REQUIRE_FALSE(result);
  REQUIRE(result.error().find("subcommand is required") != std::string::npos);
}

TEST_CASE("Subcommand callback is invoked", "[args][subcommand]") {
  args::ArgumentParser parser("test", "Test application");
  bool callback_invoked = false;

  auto& install = parser.add_subcommand("install", "Install a package");
  install.callback([&callback_invoked]() { callback_invoked = true; });

  std::vector<std::string_view> arguments = {"install"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(callback_invoked);
}

// ─────────────────────────────────────────────────────────────────────────────
// Validation tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Custom validator - range", "[args][validation]") {
  args::ArgumentParser parser("test", "Test application");
  int port = 0;

  parser.add_option("--port", "Port number", port).check(args::validators::range(1, 65535));

  SECTION("Valid value in range") {
    std::vector<std::string_view> arguments = {"--port=8080"};
    auto result = parser.parse(arguments);
    REQUIRE(result);
    REQUIRE(port == 8080);
  }

  SECTION("Invalid value below range") {
    std::vector<std::string_view> arguments = {"--port=0"};
    auto result = parser.parse(arguments);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().find("outside range") != std::string::npos);
  }

  SECTION("Invalid value above range") {
    std::vector<std::string_view> arguments = {"--port=70000"};
    auto result = parser.parse(arguments);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().find("outside range") != std::string::npos);
  }
}

TEST_CASE("Custom validator - positive", "[args][validation]") {
  args::ArgumentParser parser("test", "Test application");
  int count = 0;

  parser.add_option("--count", "Item count", count).check(args::validators::positive<int>());

  SECTION("Valid positive value") {
    std::vector<std::string_view> arguments = {"--count=5"};
    auto result = parser.parse(arguments);
    REQUIRE(result);
    REQUIRE(count == 5);
  }

  SECTION("Invalid zero value") {
    std::vector<std::string_view> arguments = {"--count=0"};
    auto result = parser.parse(arguments);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().find("positive") != std::string::npos);
  }

  SECTION("Invalid negative value") {
    std::vector<std::string_view> arguments = {"--count=-1"};
    auto result = parser.parse(arguments);
    REQUIRE_FALSE(result);
  }
}

TEST_CASE("Custom validator - non_empty", "[args][validation]") {
  args::ArgumentParser parser("test", "Test application");
  std::string name;

  parser.add_option("--name", "User name", name).check(args::validators::non_empty());

  SECTION("Valid non-empty value") {
    std::vector<std::string_view> arguments = {"--name=alice"};
    auto result = parser.parse(arguments);
    REQUIRE(result);
    REQUIRE(name == "alice");
  }

  SECTION("Invalid empty value") {
    std::vector<std::string_view> arguments = {"--name="};
    auto result = parser.parse(arguments);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().find("not be empty") != std::string::npos);
  }
}

TEST_CASE("Custom validator - one_of", "[args][validation]") {
  args::ArgumentParser parser("test", "Test application");
  std::string level;

  parser.add_option("--level", "Log level", level)
      .check(args::validators::one_of<std::string>({"debug", "info", "warn", "error"}));

  SECTION("Valid value in set") {
    std::vector<std::string_view> arguments = {"--level=info"};
    auto result = parser.parse(arguments);
    REQUIRE(result);
    REQUIRE(level == "info");
  }

  SECTION("Invalid value not in set") {
    std::vector<std::string_view> arguments = {"--level=verbose"};
    auto result = parser.parse(arguments);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().find("must be one of") != std::string::npos);
  }
}

TEST_CASE("Custom validation callback", "[args][validation]") {
  args::ArgumentParser parser("test", "Test application");
  std::string email;

  parser.add_option("--email", "Email address", email).check([](const std::string& value) {
    if (value.find('@') == std::string::npos) {
      return args::ParseResult::fail("Invalid email format");
    }
    return args::ParseResult::ok();
  });

  SECTION("Valid email") {
    std::vector<std::string_view> arguments = {"--email=test@example.com"};
    auto result = parser.parse(arguments);
    REQUIRE(result);
    REQUIRE(email == "test@example.com");
  }

  SECTION("Invalid email") {
    std::vector<std::string_view> arguments = {"--email=invalid"};
    auto result = parser.parse(arguments);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().find("Invalid email") != std::string::npos);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Error handling tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Unknown long option fails", "[args][error]") {
  args::ArgumentParser parser("test", "Test application");

  std::vector<std::string_view> arguments = {"--unknown"};
  auto result = parser.parse(arguments);

  REQUIRE_FALSE(result);
  REQUIRE(result.error().find("Unknown option") != std::string::npos);
}

TEST_CASE("Unknown short option fails", "[args][error]") {
  args::ArgumentParser parser("test", "Test application");

  std::vector<std::string_view> arguments = {"-x"};
  auto result = parser.parse(arguments);

  REQUIRE_FALSE(result);
  REQUIRE(result.error().find("Unknown option") != std::string::npos);
}

TEST_CASE("Unknown positional fails", "[args][error]") {
  args::ArgumentParser parser("test", "Test application");

  std::vector<std::string_view> arguments = {"unknown_arg"};
  auto result = parser.parse(arguments);

  REQUIRE_FALSE(result);
  REQUIRE(result.error().find("Unknown argument") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Empty argument list succeeds for parser with no required args", "[args][edge]") {
  args::ArgumentParser parser("test", "Test application");
  bool verbose = false;
  std::string name;

  parser.add_flag("--verbose", "Verbose", verbose);
  parser.add_option("--name", "Name", name);

  std::vector<std::string_view> arguments = {};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE_FALSE(verbose);
  REQUIRE(name.empty());
}

TEST_CASE("Option with empty string value", "[args][edge]") {
  args::ArgumentParser parser("test", "Test application");
  std::string value;

  parser.add_option("--value", "A value", value);

  std::vector<std::string_view> arguments = {"--value="};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(value.empty());
}

TEST_CASE("Option value starting with dash", "[args][edge]") {
  args::ArgumentParser parser("test", "Test application");
  std::string value;

  parser.add_option("--value", "A value", value);

  // The value "-10" should be parsed as a value, not an option
  std::vector<std::string_view> arguments = {"--value=-10"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(value == "-10");
}

TEST_CASE("Multiple options of same type", "[args][edge]") {
  args::ArgumentParser parser("test", "Test application");
  std::string first;
  std::string second;

  parser.add_option("--first", "First value", first);
  parser.add_option("--second", "Second value", second);

  std::vector<std::string_view> arguments = {"--first=a", "--second=b"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(first == "a");
  REQUIRE(second == "b");
}

TEST_CASE("Same option provided multiple times uses last value", "[args][edge]") {
  args::ArgumentParser parser("test", "Test application");
  std::string value;

  parser.add_option("--value", "A value", value);

  std::vector<std::string_view> arguments = {"--value=first", "--value=second"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(value == "second");
}

// ─────────────────────────────────────────────────────────────────────────────
// Type conversion edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Boolean option parses various truthy values", "[args][conversion]") {
  args::ArgumentParser parser("test", "Test application");
  bool value = false;

  parser.add_option("--value", "Boolean value", value);

  for (const auto& truthy : {"true", "1", "yes", "on"}) {
    value = false;
    std::vector<std::string_view> arguments = {"--value", truthy};
    auto result = parser.parse(arguments);
    REQUIRE(result);
    REQUIRE(value);
  }
}

TEST_CASE("Boolean option parses various falsy values", "[args][conversion]") {
  args::ArgumentParser parser("test", "Test application");
  bool value = true;

  parser.add_option("--value", "Boolean value", value);

  for (const auto& falsy : {"false", "0", "no", "off"}) {
    value = true;
    std::vector<std::string_view> arguments = {"--value", falsy};
    auto result = parser.parse(arguments);
    REQUIRE(result);
    REQUIRE_FALSE(value);
  }
}

TEST_CASE("Invalid boolean value fails", "[args][conversion]") {
  args::ArgumentParser parser("test", "Test application");
  bool value = false;

  parser.add_option("--value", "Boolean value", value);

  std::vector<std::string_view> arguments = {"--value=maybe"};
  auto result = parser.parse(arguments);

  REQUIRE_FALSE(result);
  REQUIRE(result.error().find("Invalid value") != std::string::npos);
}

TEST_CASE("Large integer values", "[args][conversion]") {
  args::ArgumentParser parser("test", "Test application");
  std::int64_t value = 0;

  parser.add_option("--value", "Large value", value);

  std::vector<std::string_view> arguments = {"--value=9223372036854775807"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(value == INT64_MAX);
}

// ─────────────────────────────────────────────────────────────────────────────
// Argument ordering tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Short flags can be combined before options", "[args][ordering]") {
  args::ArgumentParser parser("test", "Test application");
  bool flag_a = false;
  bool flag_b = false;
  std::string name;

  parser.add_flag("-a", "Flag A", flag_a);
  parser.add_flag("-b", "Flag B", flag_b);
  parser.add_option("-n", "Name", name);

  std::vector<std::string_view> arguments = {"-ab", "-n", "test"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(flag_a);
  REQUIRE(flag_b);
  REQUIRE(name == "test");
}

TEST_CASE("Short option in combined flags", "[args][ordering]") {
  args::ArgumentParser parser("test", "Test application");
  bool flag_a = false;
  std::string name;

  parser.add_flag("-a", "Flag A", flag_a);
  parser.add_option("-n", "Name", name);

  // -an should set flag a, then use "test" as value for n
  std::vector<std::string_view> arguments = {"-an", "test"};
  auto result = parser.parse(arguments);

  REQUIRE(result);
  REQUIRE(flag_a);
  REQUIRE(name == "test");
}
