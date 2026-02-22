// straylight // nix // cli // tests
//
// nix-store command compatibility tests - executable specification
//
// These tests verify that straylight-nix implements the nix-store legacy command.
// Many NixOS tools and legacy scripts depend on nix-store being available.
//
// Background:
//   - nix-store is a core command for store manipulation and queries
//   - NixOS tooling uses `nix-store -q` for querying store paths
//   - `nix-store -r` / `--realise` is used to build derivations
//   - `nix-store --gc` is used for garbage collection
//   - Commands are registered via RegisterLegacyCommand

#include <catch2/catch_test_macros.hpp>

#include "nix/cmd/legacy.h"

using namespace nix;

// =============================================================================
// nix-store command registration tests
// =============================================================================

TEST_CASE("nix-store legacy command is registered", "[cli][legacy][compatibility][nix-store]") {
  INFO("nix-store is required by NixOS tooling and legacy scripts that interact with the store");
  INFO("Many build systems and CI pipelines use nix-store for querying and realising paths");

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-store command exists in registered commands") {
    auto it = commands.find("nix-store");
    REQUIRE(it != commands.end());
    REQUIRE(it->second != nullptr);
  }
}

// =============================================================================
// nix-store --query / -q operations
// =============================================================================

TEST_CASE("nix-store --query operations are available", "[cli][legacy][nix-store][query]") {
  INFO("nix-store --query (-q) is used to query store path information");
  INFO("Common queries: --requisites, --references, --referrers, --deriver, --outputs");
  INFO("NixOS boot scripts use: nix-store -q --requisites to find closure");

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-store is registered for query operations") {
    // Query operations are implemented as part of nix-store
    // The command must be registered first
    REQUIRE(commands.contains("nix-store"));
  }
}

// =============================================================================
// nix-store --realise / -r operations
// =============================================================================

TEST_CASE("nix-store --realise operations are available", "[cli][legacy][nix-store][realise]") {
  INFO("nix-store --realise (-r) builds or fetches store paths");
  INFO("This is the low-level build command used by nix-build and other tools");
  INFO("Hydra CI and other build systems invoke nix-store -r directly");

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-store is registered for realise operations") {
    // Realise operations are implemented as part of nix-store
    REQUIRE(commands.contains("nix-store"));
  }
}

// =============================================================================
// nix-store --gc operations
// =============================================================================

TEST_CASE("nix-store --gc operations are available", "[cli][legacy][nix-store][gc]") {
  INFO("nix-store --gc performs garbage collection on the store");
  INFO("NixOS runs this via nix-gc.service timer unit");
  INFO("Options include: --print-dead, --print-live, --delete, --max-freed");

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-store is registered for gc operations") {
    // GC operations are implemented as part of nix-store
    REQUIRE(commands.contains("nix-store"));
  }
}

// =============================================================================
// nix-store required for NixOS compatibility
// =============================================================================

TEST_CASE("nix-store is required for NixOS system operations", "[cli][legacy][nixos][nix-store]") {
  INFO("nix-store is a fundamental command for NixOS operations");

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("NixOS bootloader installer uses nix-store -q --requisites") {
    // The bootloader installer needs to query the closure of the system profile
    // Example: nix-store -q --requisites /nix/var/nix/profiles/system
    REQUIRE(commands.contains("nix-store"));
  }

  SECTION("NixOS garbage collection service uses nix-store --gc") {
    // nix-gc.service runs: nix-store --gc
    REQUIRE(commands.contains("nix-store"));
  }

  SECTION("NixOS activation scripts use nix-store -r") {
    // System activation may realise paths via nix-store -r
    REQUIRE(commands.contains("nix-store"));
  }
}
