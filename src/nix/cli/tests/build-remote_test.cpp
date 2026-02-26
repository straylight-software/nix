// straylight // nix // cli // tests
//
// build-remote command tests - executable specification for distributed builds
//
// These tests verify that straylight-nix implements the build-remote command
// required for NixOS distributed/remote builds.
//
// Background:
//   - NixOS uses distributed builds to offload compilation to remote machines
//   - The build hook (settings.buildHook) invokes `nix __build-remote`
//   - main.cpp translates `__build-remote` to lookup "build-remote" in RegisterLegacyCommand
//   - Without this command, distributed builds fail silently or error out
//
// Upstream reference:
//   - build-remote is registered via RegisterLegacyCommand in src/build-remote/build-remote.cc
//   - It handles SSH connections to remote builders and delegates builds
//
// See also:
//   - tests/functional/build-remote.sh for integration tests
//   - doc/manual/source/advanced-topics/distributed-builds.md

#include <catch2/catch_test_macros.hpp>

#include "nix/cmd/legacy.h"

// =============================================================================
// build-remote command registration tests
// =============================================================================

TEST_CASE("build-remote legacy command is registered", "[cli][legacy][distributed-builds]") {
  INFO("build-remote is required for NixOS distributed builds");
  INFO("The build hook setting defaults to 'nix __build-remote'");
  INFO("Without this command, remote/distributed builds will fail");

  auto& commands = nix::RegisterLegacyCommand::commands();

  SECTION("build-remote command exists") {
    auto it = commands.find("build-remote");
    REQUIRE(it != commands.end());
    REQUIRE(it->second != nullptr);
  }
}

// =============================================================================
// Distributed builds compatibility
// =============================================================================

TEST_CASE("build-remote is available for distributed builds",
          "[cli][legacy][nixos][distributed-builds]") {
  INFO("NixOS distributed builds require the build-remote command");
  INFO("This command is invoked by the build hook when builds are delegated to remote machines");
  INFO("See: https://nixos.org/manual/nix/stable/advanced-topics/distributed-builds.html");

  auto& commands = nix::RegisterLegacyCommand::commands();

  // build-remote must be registered for distributed builds to work
  REQUIRE(commands.contains("build-remote"));
}
