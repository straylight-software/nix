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

#include <catch2/catch_test_macros.hpp>

#include "nix/cmd/legacy.h"

using namespace nix;

// =============================================================================
// Legacy command registration tests
// =============================================================================

TEST_CASE("nix-env legacy command is registered", "[cli][legacy][compatibility]") {
  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-env command exists") {
    auto it = commands.find("nix-env");
    REQUIRE(it != commands.end());
    REQUIRE(it->second != nullptr);
  }
}

TEST_CASE("nix-daemon legacy command is registered", "[cli][legacy][compatibility]") {
  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-daemon command exists") {
    auto it = commands.find("nix-daemon");
    REQUIRE(it != commands.end());
    REQUIRE(it->second != nullptr);
  }
}

TEST_CASE("nix-hash legacy command is registered", "[cli][legacy][compatibility]") {
  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-hash command exists") {
    auto it = commands.find("nix-hash");
    REQUIRE(it != commands.end());
    REQUIRE(it->second != nullptr);
  }
}

TEST_CASE("nix-prefetch-url legacy command is registered", "[cli][legacy][compatibility]") {
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

  // These commands are required for NixOS to function correctly
  SECTION("nix-env is required for NixOS bootloader installer") {
    // The bootloader installer calls: nix-env --list-generations -p /nix/var/nix/profiles/system
    REQUIRE(commands.contains("nix-env"));
  }

  SECTION("nix-daemon is required for multi-user NixOS") {
    // systemd runs nix-daemon to provide the nix store to unprivileged users
    REQUIRE(commands.contains("nix-daemon"));
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
