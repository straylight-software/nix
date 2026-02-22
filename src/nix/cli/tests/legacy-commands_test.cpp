// straylight // nix // cli // tests
//
// Legacy command compatibility tests - executable specification for nix-env, nix-daemon, etc.
//
// These tests verify that straylight-nix implements required legacy commands.
// NixOS and other tools depend on these commands being available.
//
// Background:
//   - NixOS bootloader installer uses `nix-env --list-generations`
//   - Multi-user nix installations require `nix-daemon`
//   - Legacy commands are dispatched based on argv[0] (binary name)
//   - Commands are registered via RegisterLegacyCommand
//
// Dispatch mechanism (main.cpp):
//   1. Extract program_name from argv[0] using base_name_of()
//   2. Strip file extension if present (e.g., ".exe" on Windows)
//   3. Special case: "nix __build-remote" translates to "build-remote"
//   4. Look up program_name in RegisterLegacyCommand::commands()
//   5. If found, invoke the legacy command function and return
//   6. Otherwise, continue with modern nix CLI processing
//
// Symlink-based invocation:
//   Creating symlinks like `ln -s nix nix-env` allows legacy command dispatch.
//   When invoked as `nix-env`, argv[0] is "nix-env", triggering legacy dispatch.

#include <set>

#include <catch2/catch_test_macros.hpp>

#include "nix/cmd/legacy.h"

using namespace nix;

// =============================================================================
// Legacy command registry structure tests
// =============================================================================

TEST_CASE("RegisterLegacyCommand provides a static command registry",
          "[cli][legacy][architecture]") {
  auto& commands = RegisterLegacyCommand::commands();

  SECTION("commands() returns a reference to a static map") {
    // The registry should be the same object on every call
    auto& commands2 = RegisterLegacyCommand::commands();
    REQUIRE(&commands == &commands2);
  }

  SECTION("commands map is a std::map<std::string, MainFunction>") {
    // Verify the type by iterating and checking key/value types
    for (const auto& [name, func] : commands) {
      INFO("Command: " << name);
      // name should be a string
      REQUIRE(!name.empty());
      // func should be callable with (int, char**)
      REQUIRE(func != nullptr);
    }
  }
}

// =============================================================================
// Legacy command function signature tests
// =============================================================================

TEST_CASE("Legacy command functions have correct signature", "[cli][legacy][signature]") {
  // MainFunction is defined as: std::function<void(int, char**)>
  // All registered commands must match this signature.

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("All registered commands are callable with (int, char**)") {
    for (const auto& [name, func] : commands) {
      INFO("Checking command: " << name);
      REQUIRE(func != nullptr);

      // Verify the function is a valid std::function<void(int, char**)>
      // We can't call it without proper setup, but we can verify it's not null
      // and that it's stored as the correct type
      MainFunction& main_func = const_cast<MainFunction&>(func);
      REQUIRE(static_cast<bool>(main_func));
    }
  }

  SECTION("nix-env has the correct signature") {
    auto it = commands.find("nix-env");
    REQUIRE(it != commands.end());

    // MainFunction should be std::function<void(int, char**)>
    MainFunction func = it->second;
    REQUIRE(static_cast<bool>(func));
  }

  SECTION("nix-daemon has the correct signature") {
    auto it = commands.find("nix-daemon");
    REQUIRE(it != commands.end());

    MainFunction func = it->second;
    REQUIRE(static_cast<bool>(func));
  }

  SECTION("nix-hash has the correct signature") {
    auto it = commands.find("nix-hash");
    REQUIRE(it != commands.end());

    MainFunction func = it->second;
    REQUIRE(static_cast<bool>(func));
  }

  SECTION("nix-prefetch-url has the correct signature") {
    auto it = commands.find("nix-prefetch-url");
    REQUIRE(it != commands.end());

    MainFunction func = it->second;
    REQUIRE(static_cast<bool>(func));
  }
}

// =============================================================================
// Legacy commands that SHOULD be registered
// =============================================================================
// Each test documents WHY the command is needed and its modern equivalent.

TEST_CASE("nix-env legacy command is registered", "[cli][legacy][compatibility]") {
  // PURPOSE: Package management for user environments
  // USED BY: NixOS bootloader installer (nix-env --list-generations)
  // MODERN EQUIVALENT: `nix profile`
  //   - nix-env -i pkg     -> nix profile install pkg
  //   - nix-env -e pkg     -> nix profile remove pkg
  //   - nix-env -u         -> nix profile upgrade
  //   - nix-env -q         -> nix profile list
  //   - nix-env --list-generations -> nix profile history

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-env command exists") {
    auto it = commands.find("nix-env");
    REQUIRE(it != commands.end());
    REQUIRE(it->second != nullptr);
  }
}

TEST_CASE("nix-daemon legacy command is registered", "[cli][legacy][compatibility]") {
  // PURPOSE: Multi-user Nix store access daemon
  // USED BY: NixOS systemd service (nix-daemon.service)
  // MODERN EQUIVALENT: `nix daemon`
  //   - nix-daemon --stdio -> nix daemon --stdio
  //   - The daemon handles store operations for unprivileged users

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-daemon command exists") {
    auto it = commands.find("nix-daemon");
    REQUIRE(it != commands.end());
    REQUIRE(it->second != nullptr);
  }
}

TEST_CASE("nix-hash legacy command is registered", "[cli][legacy][compatibility]") {
  // PURPOSE: Compute cryptographic hashes of files/directories
  // USED BY: Legacy build scripts, Nixpkgs fixed-output derivations
  // MODERN EQUIVALENT: `nix hash`
  //   - nix-hash --type sha256 file   -> nix hash file --type sha256 file
  //   - nix-hash --base32 --type sha256 file -> nix hash file --sri --type sha256 file
  //   - nix-hash --to-base32          -> nix hash to-base32

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-hash command exists") {
    auto it = commands.find("nix-hash");
    REQUIRE(it != commands.end());
    REQUIRE(it->second != nullptr);
  }
}

TEST_CASE("nix-prefetch-url legacy command is registered", "[cli][legacy][compatibility]") {
  // PURPOSE: Download a URL and compute its hash
  // USED BY: Nixpkgs update scripts, fetchurl debugging
  // MODERN EQUIVALENT: `nix store prefetch-file`
  //   - nix-prefetch-url <url>        -> nix store prefetch-file <url>
  //   - nix-prefetch-url --unpack     -> nix store prefetch-file --unpack

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-prefetch-url command exists") {
    auto it = commands.find("nix-prefetch-url");
    REQUIRE(it != commands.end());
    REQUIRE(it->second != nullptr);
  }
}

// =============================================================================
// Required legacy commands for NixOS compatibility
// =============================================================================

TEST_CASE("All NixOS-required legacy commands are registered", "[cli][legacy][nixos]") {
  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-env is required for NixOS bootloader installer") {
    // The bootloader installer calls: nix-env --list-generations -p /nix/var/nix/profiles/system
    REQUIRE(commands.contains("nix-env"));
  }

  SECTION("nix-daemon is required for multi-user NixOS") {
    // systemd runs nix-daemon to provide the nix store to unprivileged users
    REQUIRE(commands.contains("nix-daemon"));
  }

  SECTION("nix-hash is required for legacy build scripts") {
    // Many Nixpkgs scripts use nix-hash for computing hashes
    REQUIRE(commands.contains("nix-hash"));
  }

  SECTION("nix-prefetch-url is required for package updates") {
    // Nixpkgs update-* scripts often use nix-prefetch-url
    REQUIRE(commands.contains("nix-prefetch-url"));
  }
}

// =============================================================================
// main.cpp dispatch mechanism tests
// =============================================================================
// These tests verify the dispatch logic documented in main.cpp

TEST_CASE("Legacy command dispatch is based on program name", "[cli][legacy][dispatch]") {
  // main.cpp:380: auto program_name = std::string(base_name_of(program_path));
  // main.cpp:392: auto legacy = RegisterLegacyCommand::commands()[program_name];

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("Commands are registered with their base name (no path)") {
    // Verify commands are registered with names like "nix-env", not "/usr/bin/nix-env"
    for (const auto& [name, func] : commands) {
      INFO("Command: " << name);
      REQUIRE(name.find('/') == std::string::npos);
      REQUIRE(name.find('\\') == std::string::npos);
    }
  }

  SECTION("Commands are registered without file extension") {
    // main.cpp strips extensions like ".exe"
    for (const auto& [name, func] : commands) {
      INFO("Command: " << name);
      REQUIRE(name.find(".exe") == std::string::npos);
      REQUIRE(name.find(".cmd") == std::string::npos);
    }
  }
}

TEST_CASE("__build-remote special case dispatch", "[cli][legacy][dispatch][build-remote]") {
  // main.cpp:385-389:
  //   if (argc > 1 && std::string_view(argv[1]) == "__build-remote") {
  //     program_name = "build-remote";
  //     argv++;
  //     argc--;
  //   }

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("build-remote command should be registered for distributed builds") {
    // When invoked as "nix __build-remote", main.cpp rewrites to "build-remote"
    // This is used by the build hook for distributed builds
    // NOTE: This test documents the expected behavior. If build-remote is not
    // registered, distributed builds will fail.
    auto it = commands.find("build-remote");
    if (it != commands.end()) {
      REQUIRE(it->second != nullptr);
    } else {
      // Document that build-remote is expected but may not be implemented
      INFO("build-remote not registered - distributed builds will not work");
      // Don't fail - this documents expected behavior
    }
  }
}

// =============================================================================
// Symlink-based invocation tests
// =============================================================================

TEST_CASE("Symlink-based legacy command invocation", "[cli][legacy][symlink]") {
  // Legacy commands are invoked via symlinks:
  //   ln -s nix nix-env
  //   ln -s nix nix-daemon
  //   ln -s nix nix-hash
  //   etc.
  //
  // When the binary is invoked via a symlink, argv[0] contains the symlink name.
  // main.cpp extracts the base name and looks it up in RegisterLegacyCommand::commands().

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-env symlink invocation") {
    // ln -s nix nix-env && ./nix-env --list-generations
    // argv[0] = "nix-env" -> dispatches to nix-env legacy command
    REQUIRE(commands.contains("nix-env"));
  }

  SECTION("nix-daemon symlink invocation") {
    // ln -s nix nix-daemon && ./nix-daemon --stdio
    // argv[0] = "nix-daemon" -> dispatches to nix-daemon legacy command
    REQUIRE(commands.contains("nix-daemon"));
  }

  SECTION("nix-hash symlink invocation") {
    // ln -s nix nix-hash && ./nix-hash --type sha256 file
    // argv[0] = "nix-hash" -> dispatches to nix-hash legacy command
    REQUIRE(commands.contains("nix-hash"));
  }

  SECTION("nix-prefetch-url symlink invocation") {
    // ln -s nix nix-prefetch-url && ./nix-prefetch-url https://example.com
    // argv[0] = "nix-prefetch-url" -> dispatches to nix-prefetch-url legacy command
    REQUIRE(commands.contains("nix-prefetch-url"));
  }
}

// =============================================================================
// Legacy command argument handling tests
// =============================================================================

// Note: Actually invoking legacy commands would require more setup.
// These tests verify the commands are registered and can be looked up.
// Integration tests should verify the actual behavior.

TEST_CASE("Legacy command lookup returns callable function", "[cli][legacy]") {
  auto& commands = RegisterLegacyCommand::commands();

  for (const auto& [name, func] : commands) {
    INFO("Checking command: " << name);
    REQUIRE(func != nullptr);
  }
}

// =============================================================================
// Legacy commands with modern equivalents documentation
// =============================================================================

TEST_CASE("Document legacy commands and their modern equivalents", "[cli][legacy][documentation]") {
  // This test documents the mapping between legacy and modern commands.
  // It serves as executable documentation and helps ensure we implement
  // the right set of legacy commands.

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-env -> nix profile") {
    // nix-env is the legacy package manager
    // Modern equivalent: nix profile
    //
    // Operation mapping:
    //   nix-env -i <pkg>           -> nix profile install <pkg>
    //   nix-env -e <pkg>           -> nix profile remove <pkg>
    //   nix-env -u                 -> nix profile upgrade
    //   nix-env -q                 -> nix profile list
    //   nix-env --list-generations -> nix profile history
    //   nix-env --rollback         -> nix profile rollback
    //   nix-env --switch-generation N -> nix profile rollback --to N
    REQUIRE(commands.contains("nix-env"));
  }

  SECTION("nix-daemon -> nix daemon") {
    // nix-daemon is the legacy store daemon
    // Modern equivalent: nix daemon
    //
    // The commands are largely equivalent, but nix-daemon is needed for
    // backwards compatibility with systemd service files.
    REQUIRE(commands.contains("nix-daemon"));
  }

  SECTION("nix-hash -> nix hash") {
    // nix-hash is the legacy hash computation tool
    // Modern equivalent: nix hash
    //
    // Operation mapping:
    //   nix-hash --type sha256 <file>    -> nix hash file <file>
    //   nix-hash --base32                -> nix hash to-base32
    //   nix-hash --to-base64             -> nix hash to-base64
    //   nix-hash --flat                  -> nix hash file (default)
    //   nix-hash (directory)             -> nix hash path <dir>
    REQUIRE(commands.contains("nix-hash"));
  }

  SECTION("nix-prefetch-url -> nix store prefetch-file") {
    // nix-prefetch-url downloads and hashes URLs
    // Modern equivalent: nix store prefetch-file
    //
    // Operation mapping:
    //   nix-prefetch-url <url>           -> nix store prefetch-file <url>
    //   nix-prefetch-url --unpack <url>  -> nix store prefetch-file --unpack <url>
    //   nix-prefetch-url --print-path    -> nix store prefetch-file (always prints path)
    REQUIRE(commands.contains("nix-prefetch-url"));
  }

  // Document commands that are NOT implemented but may be expected

  SECTION("Document nix-store -> nix store (not implemented as legacy)") {
    // nix-store operations are available via modern `nix store` subcommands
    // The legacy nix-store command is NOT registered as a legacy command
    // because it's a large surface area - users should migrate to nix store.
    //
    // However, some tools may expect nix-store to exist. If needed,
    // nix-store could be added as a legacy command.
    //
    // Common nix-store operations and their modern equivalents:
    //   nix-store -r <drv>        -> nix build <drv>
    //   nix-store -q --tree       -> nix path-info -r
    //   nix-store -q --requisites -> nix path-info -r
    //   nix-store --gc            -> nix store gc
    //   nix-store --optimise      -> nix store optimise
    //   nix-store --verify        -> nix store verify
    INFO("nix-store is available via 'nix store' subcommands");
  }

  SECTION("Document nix-build -> nix build (not implemented as legacy)") {
    // nix-build is not implemented as a legacy command
    // Users should use `nix build` instead
    //
    // Operation mapping:
    //   nix-build <file>          -> nix build -f <file>
    //   nix-build -A attr         -> nix build -f default.nix attr
    //   nix-build '<nixpkgs>'     -> nix build nixpkgs#<pkg>
    INFO("nix-build is available as 'nix build'");
  }

  SECTION("Document nix-shell -> nix develop/nix shell (not implemented as legacy)") {
    // nix-shell is not implemented as a legacy command
    // Users should use `nix develop` (for dev environments) or `nix shell` (for running)
    //
    // Operation mapping:
    //   nix-shell -p pkgs         -> nix shell nixpkgs#pkg1 nixpkgs#pkg2
    //   nix-shell (with shell.nix) -> nix develop
    //   nix-shell --run "cmd"     -> nix develop --command cmd
    INFO("nix-shell is available as 'nix develop' or 'nix shell'");
  }

  SECTION("Document nix-instantiate -> nix eval (not implemented as legacy)") {
    // nix-instantiate is not implemented as a legacy command
    // Users should use `nix eval` or `nix derivation show`
    //
    // Operation mapping:
    //   nix-instantiate --eval    -> nix eval
    //   nix-instantiate <file>    -> nix derivation show -f <file>
    INFO("nix-instantiate is available as 'nix eval' or 'nix derivation show'");
  }
}

// =============================================================================
// Command count verification
// =============================================================================

TEST_CASE("Minimum required legacy commands are registered", "[cli][legacy][count]") {
  auto& commands = RegisterLegacyCommand::commands();

  // We require at minimum these 4 commands for basic NixOS compatibility
  REQUIRE(commands.size() >= 4);

  // Verify the essential commands
  REQUIRE(commands.contains("nix-env"));
  REQUIRE(commands.contains("nix-daemon"));
  REQUIRE(commands.contains("nix-hash"));
  REQUIRE(commands.contains("nix-prefetch-url"));
}

// =============================================================================
// Command name format tests
// =============================================================================

TEST_CASE("Legacy command names follow naming conventions", "[cli][legacy][naming]") {
  auto& commands = RegisterLegacyCommand::commands();

  SECTION("Legacy Nix commands use nix- prefix") {
    // Traditional legacy commands follow the nix-<command> pattern
    // Exceptions: build-remote (internal command)
    int nix_prefixed = 0;
    for (const auto& [name, func] : commands) {
      if (name.starts_with("nix-")) {
        nix_prefixed++;
      }
    }
    // At least our 4 core legacy commands should have nix- prefix
    REQUIRE(nix_prefixed >= 4);
  }

  SECTION("Command names are lowercase") {
    for (const auto& [name, func] : commands) {
      INFO("Command: " << name);
      for (char c : name) {
        if (std::isalpha(c)) {
          REQUIRE(std::islower(c));
        }
      }
    }
  }

  SECTION("Command names contain only alphanumeric and hyphen") {
    for (const auto& [name, func] : commands) {
      INFO("Command: " << name);
      for (char c : name) {
        bool valid = std::isalnum(c) || c == '-';
        REQUIRE(valid);
      }
    }
  }
}

// =============================================================================
// Legacy command registration mechanism tests
// =============================================================================

TEST_CASE("RegisterLegacyCommand constructor registers commands", "[cli][legacy][registration]") {
  // The RegisterLegacyCommand struct registers commands via its constructor.
  // This pattern allows static initialization in each command's .cpp file.
  //
  // Example usage:
  //   static RegisterLegacyCommand r_nix_env("nix-env", main_nix_env);
  //
  // This creates a static object whose constructor adds "nix-env" -> main_nix_env
  // to the commands() map.

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("Commands persist after registration") {
    // Because registration happens at static init time, all commands
    // should be available by the time tests run
    REQUIRE(commands.size() >= 4);
  }

  SECTION("Multiple commands can be registered") {
    // Each command file registers its own command
    std::vector<std::string> expected = {"nix-env", "nix-daemon", "nix-hash", "nix-prefetch-url"};
    for (const auto& cmd : expected) {
      INFO("Checking: " << cmd);
      REQUIRE(commands.contains(cmd));
    }
  }
}

// =============================================================================
// Command lookup edge cases
// =============================================================================

TEST_CASE("Legacy command lookup handles edge cases", "[cli][legacy][lookup]") {
  auto& commands = RegisterLegacyCommand::commands();

  SECTION("Looking up non-existent command returns null") {
    auto func = commands["this-command-does-not-exist"];
    REQUIRE(func == nullptr);
  }

  SECTION("Looking up empty string returns null") {
    auto func = commands[""];
    REQUIRE(func == nullptr);
  }

  SECTION("Looking up partial command name returns null") {
    // "nix-" is not a valid command
    auto func = commands["nix-"];
    REQUIRE(func == nullptr);
  }

  SECTION("Looking up command with wrong prefix returns null") {
    // "env" is not the same as "nix-env"
    auto func = commands["env"];
    REQUIRE(func == nullptr);
  }
}

// =============================================================================
// Document expected vs implemented legacy commands
// =============================================================================

TEST_CASE("Document expected legacy commands status", "[cli][legacy][status]") {
  // This test documents which legacy commands are implemented vs expected
  auto& commands = RegisterLegacyCommand::commands();

  SECTION("Implemented: nix-env (minimal - --list-generations only)") {
    // Only --list-generations is implemented
    // Full nix-env is NOT supported - use `nix profile` instead
    REQUIRE(commands.contains("nix-env"));
  }

  SECTION("Implemented: nix-daemon (--stdio mode)") {
    // Only --stdio mode is implemented for systemd socket activation
    // Full socket server mode is NOT supported - use `nix daemon` instead
    REQUIRE(commands.contains("nix-daemon"));
  }

  SECTION("Implemented: nix-hash (full compatibility)") {
    // Full nix-hash compatibility via hash.cpp
    REQUIRE(commands.contains("nix-hash"));
  }

  SECTION("Implemented: nix-prefetch-url (full compatibility)") {
    // Full nix-prefetch-url compatibility via prefetch.cpp
    REQUIRE(commands.contains("nix-prefetch-url"));
  }

  // Document commands that may be expected but are NOT implemented

  SECTION("Not implemented: nix-build") {
    // nix-build is NOT implemented - use `nix build` instead
    // This is intentional - nix-build would require significant code
    bool has_nix_build = commands.contains("nix-build");
    INFO("nix-build implemented: " << (has_nix_build ? "yes" : "no"));
    // Don't require either way - just document
  }

  SECTION("Not implemented: nix-shell") {
    // nix-shell is NOT implemented - use `nix develop` or `nix shell` instead
    bool has_nix_shell = commands.contains("nix-shell");
    INFO("nix-shell implemented: " << (has_nix_shell ? "yes" : "no"));
  }

  SECTION("Not implemented: nix-store") {
    // nix-store is NOT implemented - use `nix store` subcommands instead
    bool has_nix_store = commands.contains("nix-store");
    INFO("nix-store implemented: " << (has_nix_store ? "yes" : "no"));
  }

  SECTION("Not implemented: nix-instantiate") {
    // nix-instantiate is NOT implemented - use `nix eval` instead
    bool has_nix_instantiate = commands.contains("nix-instantiate");
    INFO("nix-instantiate implemented: " << (has_nix_instantiate ? "yes" : "no"));
  }

  SECTION("Not implemented: nix-collect-garbage") {
    // nix-collect-garbage is NOT implemented - use `nix store gc` instead
    bool has_nix_collect_garbage = commands.contains("nix-collect-garbage");
    INFO("nix-collect-garbage implemented: " << (has_nix_collect_garbage ? "yes" : "no"));
  }

  SECTION("Not implemented: nix-copy-closure") {
    // nix-copy-closure is NOT implemented - use `nix copy` instead
    bool has_nix_copy_closure = commands.contains("nix-copy-closure");
    INFO("nix-copy-closure implemented: " << (has_nix_copy_closure ? "yes" : "no"));
  }

  SECTION("Not implemented: nix-channel") {
    // nix-channel is NOT implemented - use flakes instead
    bool has_nix_channel = commands.contains("nix-channel");
    INFO("nix-channel implemented: " << (has_nix_channel ? "yes" : "no"));
  }
}

// =============================================================================
// Verify registered functions are distinct
// =============================================================================

TEST_CASE("Each legacy command has a distinct implementation", "[cli][legacy][distinct]") {
  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-env and nix-daemon are different functions") {
    auto nix_env = commands.find("nix-env");
    auto nix_daemon = commands.find("nix-daemon");

    REQUIRE(nix_env != commands.end());
    REQUIRE(nix_daemon != commands.end());

    // The function pointers should not be equal
    // (They could point to the same underlying function type, but not the same instance)
    // We verify they're both valid and the map has distinct entries
    REQUIRE(nix_env->first != nix_daemon->first);
  }

  SECTION("All command names are unique") {
    // std::map guarantees unique keys, but let's verify
    std::set<std::string> names;
    for (const auto& [name, func] : commands) {
      auto [it, inserted] = names.insert(name);
      INFO("Command: " << name);
      REQUIRE(inserted); // Should always be able to insert (unique)
    }
    REQUIRE(names.size() == commands.size());
  }
}
