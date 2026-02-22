/**
 * Legacy nix-env compatibility shim
 *
 * This provides minimal nix-env compatibility for tools that need it,
 * like the NixOS bootloader installer which uses `nix-env --list-generations`.
 *
 * Most nix-env functionality is NOT supported - use `nix profile` instead.
 */

#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "nix/cmd/legacy.h"
#include "nix/store/profiles.h"
#include "nix/util/args.h"
#include "nix/util/file-system.h"

namespace nix {

static void list_generations(const std::filesystem::path& profile) {
  auto [gens, current_gen] = findGenerations(profile);

  for (const auto& gen : gens) {
    // Format: "  <number>   <date>   (current)"
    std::cout << std::setw(4) << gen.number << "   ";

    // Format timestamp like: 2025-09-30 03:02:27
    char time_buf[64];
    struct tm* tm_info = localtime(&gen.creationTime);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
    std::cout << time_buf << "   ";

    if (current_gen && *current_gen == gen.number) {
      std::cout << "(current)";
    }

    std::cout << "\n";
  }
}

static void main_nix_env(int argc, char** argv) {
  std::string profile;
  bool list_gens = false;
  bool show_help = false;

  // Simple argument parsing for the subset we support
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];

    if (arg == "--list-generations" || arg == "-L") {
      list_gens = true;
    } else if (arg == "--profile" || arg == "-p") {
      if (i + 1 < argc) {
        profile = argv[++i];
      } else {
        throw Error("--profile requires an argument");
      }
    } else if (arg == "--help" || arg == "-h" || arg == "-?") {
      show_help = true;
    } else if (arg[0] == '-') {
      throw Error("'nix-env' is not fully supported in this build; only --list-generations is "
                  "implemented.\n"
                  "For package management, use 'nix profile' instead.\n"
                  "Unrecognized option: %s",
                  arg);
    }
  }

  if (show_help) {
    std::cout << R"(nix-env (straylight compatibility shim)

This is a minimal nix-env compatibility layer. Only --list-generations is supported.
For full package management, use 'nix profile' instead.

Supported options:
  -p, --profile <path>    Path to the profile
  -L, --list-generations  List all generations of the profile
  -h, --help              Show this help message

Example:
  nix-env --list-generations -p /nix/var/nix/profiles/system
)";
    return;
  }

  if (!list_gens) {
    throw Error("'nix-env' is not fully supported in this build; only --list-generations is "
                "implemented.\n"
                "For package management, use 'nix profile' instead.");
  }

  if (profile.empty()) {
    profile = get_default_profile().string();
  }

  list_generations(profile);
}

static RegisterLegacyCommand r_nix_env("nix-env", main_nix_env);

} // namespace nix
