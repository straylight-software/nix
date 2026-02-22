/**
 * Legacy build-remote command for distributed builds
 *
 * This command is invoked by the Nix build hook for distributed builds.
 * The default build hook setting is 'nix __build-remote', which dispatches
 * to this command via RegisterLegacyCommand("build-remote", ...).
 *
 * Current implementation status:
 * - Command is registered (required for build hook detection)
 * - Actual distributed build logic is NOT yet implemented
 * - Provides helpful error message directing users to documentation
 *
 * To implement fully, port from upstream:
 * - src/build-remote/build-remote.cc
 *
 * See also:
 * - https://nixos.org/manual/nix/stable/advanced-topics/distributed-builds.html
 */

#include <iostream>

#include "nix/cmd/legacy.h"
#include "nix/util/args.h"

namespace nix {

static void main_build_remote(int argc, char** argv) {
  // Check for help/version
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];

    if (arg == "--help" || arg == "-h") {
      std::cout << R"(build-remote (straylight)

This command handles distributed/remote builds for Nix.

It is invoked by the Nix daemon when:
  1. A build is requested
  2. Local resources are insufficient or remote builders are configured
  3. The build hook (default: 'nix __build-remote') delegates the build

Configuration:
  Set 'builders' in nix.conf to configure remote build machines.
  Example: builders = ssh://builder@machine x86_64-linux

Current implementation status: NOT IMPLEMENTED
Distributed builds are not yet supported in straylight.

See: https://nixos.org/manual/nix/stable/advanced-topics/distributed-builds.html
)";
      return;
    }

    if (arg == "--version") {
      std::cout << "build-remote (straylight)\n";
      return;
    }
  }

  // The build hook protocol expects specific behavior:
  // 1. Read derivation info from stdin
  // 2. Connect to remote builder via SSH
  // 3. Execute build on remote
  // 4. Copy results back
  //
  // For now, we fail with a clear error message.
  throw Error("Distributed builds are not yet implemented in straylight.\n"
              "The 'build-remote' command is registered but non-functional.\n"
              "\n"
              "To build locally instead, either:\n"
              "  1. Remove 'builders' from nix.conf\n"
              "  2. Set 'builders = ' (empty) in nix.conf\n"
              "  3. Use --builders '' on the command line\n"
              "\n"
              "See: https://nixos.org/manual/nix/stable/advanced-topics/distributed-builds.html");
}

static RegisterLegacyCommand r_build_remote("build-remote", main_build_remote);

} // namespace nix
