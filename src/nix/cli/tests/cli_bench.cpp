// straylight // nix // cli // tests
//
// CLI performance benchmarks - ensuring sub-millisecond dispatch latency
//
// These benchmarks verify that CLI operations remain fast enough for
// interactive use. All dispatch operations should complete in < 1ms.
//
// Run with: buck2 test //src/nix/cli/tests:cli_bench
//
// Key performance targets:
//   - Legacy command lookup: < 100μs
//   - New command registration lookup: < 100μs
//   - Argument parsing (simple): < 500μs
//   - Help text generation: < 1ms
//   - Command completion: < 1ms

#include <chrono>
#include <string>
#include <vector>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/cmd/legacy.h"
#include "nix/util/args.h"
#include "nix/util/args/root.h"

// =============================================================================
// Legacy command lookup benchmarks
// =============================================================================

TEST_CASE("Legacy command lookup performance", "[benchmark][cli][legacy]") {
  auto& commands = nix::RegisterLegacyCommand::commands();

  // Verify we have commands to benchmark
  REQUIRE(commands.size() >= 4);

  SECTION("Lookup existing legacy command") {
    BENCHMARK("nix-env lookup") {
      auto it = commands.find("nix-env");
      return it != commands.end();
    };

    BENCHMARK("nix-daemon lookup") {
      auto it = commands.find("nix-daemon");
      return it != commands.end();
    };

    BENCHMARK("nix-hash lookup") {
      auto it = commands.find("nix-hash");
      return it != commands.end();
    };

    BENCHMARK("nix-prefetch-url lookup") {
      auto it = commands.find("nix-prefetch-url");
      return it != commands.end();
    };
  }

  SECTION("Lookup non-existent legacy command") {
    BENCHMARK("nonexistent command lookup") {
      auto it = commands.find("nix-nonexistent-command");
      return it == commands.end();
    };
  }

  SECTION("Full legacy dispatch simulation") {
    // Simulates the dispatch path in main.cpp:392
    // auto legacy = nix::RegisterLegacyCommand::commands()[program_name];
    BENCHMARK("legacy dispatch lookup (nix-env)") {
      auto legacy = nix::RegisterLegacyCommand::commands()["nix-env"];
      return legacy != nullptr;
    };

    BENCHMARK("legacy dispatch lookup (nix-daemon)") {
      auto legacy = nix::RegisterLegacyCommand::commands()["nix-daemon"];
      return legacy != nullptr;
    };
  }
}

// =============================================================================
// New command registration lookup benchmarks
// =============================================================================

TEST_CASE("New command registration lookup performance", "[benchmark][cli][commands]") {
  SECTION("Get commands for root level") {
    BENCHMARK("getCommandsFor({}) - root commands") {
      auto cmds = nix::RegisterCommand::getCommandsFor({});
      return cmds.size();
    };
  }

  SECTION("Get commands for subcommand prefixes") {
    BENCHMARK("getCommandsFor({\"store\"})") {
      auto cmds = nix::RegisterCommand::getCommandsFor({"store"});
      return cmds.size();
    };

    BENCHMARK("getCommandsFor({\"flake\"})") {
      auto cmds = nix::RegisterCommand::getCommandsFor({"flake"});
      return cmds.size();
    };

    BENCHMARK("getCommandsFor({\"profile\"})") {
      auto cmds = nix::RegisterCommand::getCommandsFor({"profile"});
      return cmds.size();
    };
  }

  SECTION("Command existence checks") {
    auto& commands = nix::RegisterCommand::commands();

    BENCHMARK("check build command exists") {
      auto it = commands.find({"build"});
      return it != commands.end();
    };

    BENCHMARK("check store info command exists") {
      auto it = commands.find({"store", "info"});
      return it != commands.end();
    };

    BENCHMARK("check flake show command exists") {
      auto it = commands.find({"flake", "show"});
      return it != commands.end();
    };
  }
}

// =============================================================================
// Argument parsing benchmarks
// =============================================================================

// A minimal nix::root_args_t subclass for benchmarking argument parsing
// We need nix::root_args_t to get access to parse_cmdline()
struct BenchmarkArgs : public nix::root_args_t {
  bool verbose = false;
  bool quiet = false;
  std::string store_uri;
  std::vector<std::string> installables;
  std::optional<std::string> out_link;
  int jobs = 0;

  BenchmarkArgs() {
    add_flag({
        .long_name = "verbose",
        .short_name = 'v',
        .description = "Increase verbosity level",
        .handler = {&verbose, true},
    });

    add_flag({
        .long_name = "quiet",
        .short_name = 'q',
        .description = "Decrease verbosity level",
        .handler = {&quiet, true},
    });

    add_flag({
        .long_name = "store",
        .description = "Store URI",
        .labels = {"uri"},
        .handler = {&store_uri},
    });

    add_flag({
        .long_name = "out-link",
        .short_name = 'o',
        .description = "Output symlink path",
        .labels = {"path"},
        .handler = {&out_link},
    });

    add_flag({
        .long_name = "jobs",
        .short_name = 'j',
        .description = "Number of parallel jobs",
        .labels = {"n"},
        .handler = {&jobs},
    });

    expect_args({
        .label = "installables",
        .handler = {&installables},
    });
  }
};

TEST_CASE("Argument parsing performance", "[benchmark][cli][args]") {
  SECTION("Parse simple flags") {
    BENCHMARK("parse -v flag") {
      BenchmarkArgs args;
      args.parse_cmdline({"-v"});
      return args.verbose;
    };

    BENCHMARK("parse --verbose flag") {
      BenchmarkArgs args;
      args.parse_cmdline({"--verbose"});
      return args.verbose;
    };
  }

  SECTION("Parse flags with values") {
    BENCHMARK("parse --store flag") {
      BenchmarkArgs args;
      args.parse_cmdline({"--store", "daemon"});
      return args.store_uri;
    };

    BENCHMARK("parse --jobs flag") {
      BenchmarkArgs args;
      args.parse_cmdline({"--jobs", "4"});
      return args.jobs;
    };
  }

  SECTION("Parse multiple flags") {
    BENCHMARK("parse -v -q --jobs 8 flags") {
      BenchmarkArgs args;
      args.parse_cmdline({"-v", "-q", "--jobs", "8"});
      return args.verbose && args.quiet && args.jobs == 8;
    };

    BENCHMARK("parse common build flags") {
      BenchmarkArgs args;
      args.parse_cmdline(
          {"--verbose", "--store", "daemon", "--jobs", "16", "--out-link", "result"});
      return args.verbose;
    };
  }

  SECTION("Parse with positional args") {
    BENCHMARK("parse installables") {
      BenchmarkArgs args;
      args.parse_cmdline({"nixpkgs#hello"});
      return args.installables.size();
    };

    BENCHMARK("parse multiple installables") {
      BenchmarkArgs args;
      args.parse_cmdline({"nixpkgs#hello", "nixpkgs#cowsay", "nixpkgs#figlet"});
      return args.installables.size();
    };

    BENCHMARK("parse flags and installables") {
      BenchmarkArgs args;
      args.parse_cmdline({"-v", "--jobs", "4", "nixpkgs#hello", "nixpkgs#cowsay"});
      return args.installables.size();
    };
  }
}

// =============================================================================
// Help text generation benchmarks
// =============================================================================

TEST_CASE("Help text generation performance", "[benchmark][cli][help]") {
  SECTION("Generate JSON representation of args") {
    BENCHMARK("args to_json()") {
      BenchmarkArgs args;
      auto json = args.to_json();
      return json.dump().size();
    };
  }

  SECTION("Generate description") {
    BENCHMARK("args description()") {
      BenchmarkArgs args;
      return args.description().size();
    };
  }
}

// =============================================================================
// Command completion benchmarks
// =============================================================================

// A simple completions collector for benchmarking
struct BenchmarkCompletions : public nix::add_completions_t {
  std::vector<std::pair<std::string, std::string>> completions;
  Type type = Type::normal;

  void set_type(Type t) override { type = t; }

  void add(std::string completion, std::string description = "") override {
    completions.emplace_back(std::move(completion), std::move(description));
  }

  void clear() {
    completions.clear();
    type = Type::normal;
  }
};

TEST_CASE("Command completion performance", "[benchmark][cli][completion]") {
  SECTION("Complete legacy commands") {
    auto& commands = nix::RegisterLegacyCommand::commands();

    BENCHMARK("iterate all legacy commands") {
      size_t count = 0;
      for (const auto& [name, func] : commands) {
        if (name.starts_with("nix-")) {
          count++;
        }
      }
      return count;
    };
  }

  SECTION("Complete new commands") {
    BENCHMARK("iterate root commands") {
      auto cmds = nix::RegisterCommand::getCommandsFor({});
      size_t count = 0;
      for ([[maybe_unused]] const auto& cmd : cmds) {
        count++;
      }
      return count;
    };

    BENCHMARK("iterate store subcommands") {
      auto cmds = nix::RegisterCommand::getCommandsFor({"store"});
      size_t count = 0;
      for ([[maybe_unused]] const auto& cmd : cmds) {
        count++;
      }
      return count;
    };

    BENCHMARK("iterate flake subcommands") {
      auto cmds = nix::RegisterCommand::getCommandsFor({"flake"});
      size_t count = 0;
      for ([[maybe_unused]] const auto& cmd : cmds) {
        count++;
      }
      return count;
    };
  }

  SECTION("Filter commands by prefix") {
    auto cmds = nix::RegisterCommand::getCommandsFor({});

    BENCHMARK("filter commands starting with 'b'") {
      size_t count = 0;
      for (const auto& [name, _factory] : cmds) {
        if (name.starts_with("b")) {
          count++;
        }
      }
      return count;
    };

    BENCHMARK("filter commands starting with 'st'") {
      size_t count = 0;
      for (const auto& [name, _factory] : cmds) {
        if (name.starts_with("st")) {
          count++;
        }
      }
      return count;
    };
  }

  SECTION("Completions collection") {
    BenchmarkCompletions completions;
    auto cmds = nix::RegisterCommand::getCommandsFor({});

    BENCHMARK("collect all root command completions") {
      completions.clear();
      for (const auto& [name, _factory] : cmds) {
        completions.add(name, "");
      }
      return completions.completions.size();
    };
  }
}

// =============================================================================
// Combined dispatch path benchmarks
// =============================================================================

TEST_CASE("Full CLI dispatch simulation", "[benchmark][cli][dispatch]") {
  SECTION("Legacy command dispatch path") {
    // Simulates the full dispatch path from main.cpp:
    // 1. Extract program_name from argv[0]
    // 2. Strip extension
    // 3. Look up in RegisterLegacyCommand::commands()
    BENCHMARK("full legacy dispatch (nix-env)") {
      std::string program_path = "/usr/bin/nix-env";

      // Extract base name (simulating base_name_of)
      auto last_slash = program_path.find_last_of('/');
      std::string program_name =
          last_slash != std::string::npos ? program_path.substr(last_slash + 1) : program_path;

      // Strip extension (simulating extension stripping)
      auto extension_pos = program_name.find_last_of('.');
      if (extension_pos != std::string::npos) {
        program_name.erase(extension_pos);
      }

      // Look up command
      auto legacy = nix::RegisterLegacyCommand::commands()[program_name];
      return legacy != nullptr;
    };

    BENCHMARK("full legacy dispatch (nix-daemon)") {
      std::string program_path = "/run/current-system/sw/bin/nix-daemon";

      auto last_slash = program_path.find_last_of('/');
      std::string program_name =
          last_slash != std::string::npos ? program_path.substr(last_slash + 1) : program_path;

      auto extension_pos = program_name.find_last_of('.');
      if (extension_pos != std::string::npos) {
        program_name.erase(extension_pos);
      }

      auto legacy = nix::RegisterLegacyCommand::commands()[program_name];
      return legacy != nullptr;
    };
  }

  SECTION("New command dispatch path") {
    // Simulates looking up a new-style command
    BENCHMARK("new command dispatch (build)") {
      auto cmds = nix::RegisterCommand::getCommandsFor({});
      auto it = cmds.find("build");
      return it != cmds.end();
    };

    BENCHMARK("new command dispatch (store info)") {
      auto cmds = nix::RegisterCommand::getCommandsFor({"store"});
      auto it = cmds.find("info");
      return it != cmds.end();
    };

    BENCHMARK("new command dispatch (flake check)") {
      auto cmds = nix::RegisterCommand::getCommandsFor({"flake"});
      auto it = cmds.find("check");
      return it != cmds.end();
    };
  }
}

// =============================================================================
// Performance verification tests
// =============================================================================

TEST_CASE("CLI dispatch is sub-millisecond", "[cli][performance]") {
  // These are not benchmarks but actual performance verification tests.
  // They measure real execution time and fail if dispatch takes too long.

  using clock = std::chrono::high_resolution_clock;
  using microseconds = std::chrono::microseconds;

  SECTION("Legacy command lookup is fast") {
    auto start = clock::now();

    // Perform 1000 lookups
    for (int i = 0; i < 1000; i++) {
      auto legacy = nix::RegisterLegacyCommand::commands()["nix-env"];
      (void)legacy;
    }

    auto end = clock::now();
    auto duration = std::chrono::duration_cast<microseconds>(end - start);

    // 1000 lookups should complete in < 10ms (10μs per lookup average)
    INFO("1000 legacy lookups took " << duration.count() << "μs");
    REQUIRE(duration.count() < 10000);
  }

  SECTION("New command lookup is fast") {
    auto start = clock::now();

    // Perform 1000 lookups
    for (int i = 0; i < 1000; i++) {
      auto cmds = nix::RegisterCommand::getCommandsFor({});
      auto it = cmds.find("build");
      (void)it;
    }

    auto end = clock::now();
    auto duration = std::chrono::duration_cast<microseconds>(end - start);

    // 1000 lookups should complete in < 100ms (100μs per lookup average)
    // This is higher because getCommandsFor() creates a new map
    INFO("1000 new command lookups took " << duration.count() << "μs");
    REQUIRE(duration.count() < 100000);
  }

  SECTION("Single dispatch is sub-millisecond") {
    // Test that a single dispatch operation is < 1ms

    auto start = clock::now();

    // Full legacy dispatch
    std::string program_path = "/usr/bin/nix-env";
    auto last_slash = program_path.find_last_of('/');
    std::string program_name =
        last_slash != std::string::npos ? program_path.substr(last_slash + 1) : program_path;
    auto legacy = nix::RegisterLegacyCommand::commands()[program_name];
    (void)legacy;

    auto end = clock::now();
    auto duration = std::chrono::duration_cast<microseconds>(end - start);

    INFO("Single legacy dispatch took " << duration.count() << "μs");
    REQUIRE(duration.count() < 1000); // < 1ms
  }
}

// =============================================================================
// Scalability tests
// =============================================================================

TEST_CASE("Command lookup scales with command count", "[benchmark][cli][scalability]") {
  auto& legacy_commands = nix::RegisterLegacyCommand::commands();
  auto& new_commands = nix::RegisterCommand::commands();

  INFO("Legacy commands registered: " << legacy_commands.size());
  INFO("New commands registered: " << new_commands.size());

  SECTION("Legacy command count") {
    // Document the number of legacy commands
    REQUIRE(legacy_commands.size() >= 4);
    REQUIRE(legacy_commands.size() < 100); // Sanity check
  }

  SECTION("New command count") {
    // Document the number of new commands
    REQUIRE(new_commands.size() > 0);
    REQUIRE(new_commands.size() < 500); // Sanity check
  }

  SECTION("Benchmark all legacy lookups") {
    BENCHMARK("lookup all legacy commands") {
      size_t found = 0;
      for (const auto& [name, func] : legacy_commands) {
        auto it = legacy_commands.find(name);
        if (it != legacy_commands.end()) {
          found++;
        }
      }
      return found;
    };
  }

  SECTION("Benchmark all new command lookups") {
    BENCHMARK("lookup all new commands") {
      size_t found = 0;
      for (const auto& [path, factory] : new_commands) {
        auto it = new_commands.find(path);
        if (it != new_commands.end()) {
          found++;
        }
      }
      return found;
    };
  }
}
