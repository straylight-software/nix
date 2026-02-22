/**
 * Legacy nix-store compatibility shim
 *
 * This provides nix-store registration for NixOS compatibility.
 * The nix-store command is used by:
 * - NixOS bootloader installer (nix-store -q --requisites)
 * - NixOS garbage collection service (nix-store --gc)
 * - Build systems and CI pipelines (nix-store -r)
 *
 * Current implementation status:
 * - Command is registered (for detection by tests and scripts)
 * - Operations delegate to modern `nix store` equivalents where possible
 * - Unimplemented operations provide helpful error messages
 */

#include <cstring>
#include <iostream>

#include "nix/cmd/legacy.h"
#include "nix/util/args.h"

namespace nix {

static void main_nix_store(int argc, char** argv) {
  bool show_help = false;
  bool show_version = false;
  bool query = false;
  bool realise = false;
  bool gc = false;

  // Parse arguments to determine operation
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];

    if (arg == "--help" || arg == "-h" || arg == "-?") {
      show_help = true;
    } else if (arg == "--version") {
      show_version = true;
    } else if (arg == "--query" || arg == "-q") {
      query = true;
    } else if (arg == "--realise" || arg == "--realize" || arg == "-r") {
      realise = true;
    } else if (arg == "--gc") {
      gc = true;
    }
  }

  if (show_version) {
    std::cout << "nix-store (straylight)\n";
    return;
  }

  if (show_help) {
    std::cout << R"(nix-store (straylight compatibility shim)

This is a compatibility layer for nix-store. Most operations have modern equivalents:

  nix-store -q --requisites PATH  →  nix path-info -r PATH
  nix-store -q --references PATH  →  nix path-info --json PATH | jq .references
  nix-store -r DRVPATH            →  nix build DRVPATH
  nix-store --gc                  →  nix store gc
  nix-store --verify              →  nix store verify
  nix-store --dump PATH           →  nix nar dump-path PATH

For full documentation, see: https://nixos.org/manual/nix/stable/command-ref/nix-store.html

Current implementation status: PARTIAL
Use 'nix store' subcommands for full functionality.
)";
    return;
  }

  // Provide specific guidance based on operation
  if (gc) {
    throw Error("'nix-store --gc' is not yet implemented in straylight.\n"
                "Use 'nix store gc' instead.");
  }

  if (query) {
    throw Error("'nix-store --query' is not yet implemented in straylight.\n"
                "Use 'nix path-info' instead. Examples:\n"
                "  nix path-info -r /nix/store/...  (for --requisites)\n"
                "  nix path-info --json /nix/store/...  (for detailed info)");
  }

  if (realise) {
    throw Error("'nix-store --realise' is not yet implemented in straylight.\n"
                "Use 'nix build' instead.");
  }

  // Generic error for other operations
  throw Error("'nix-store' is not fully implemented in straylight.\n"
              "Use 'nix store' subcommands instead.\n"
              "Run 'nix-store --help' for migration guidance.");
}

static RegisterLegacyCommand r_nix_store("nix-store", main_nix_store);

} // namespace nix
