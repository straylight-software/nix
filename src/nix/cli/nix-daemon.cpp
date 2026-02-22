/**
 * Legacy nix-daemon compatibility shim
 *
 * This provides nix-daemon compatibility for NixOS multi-user mode.
 * The daemon handles store operations on behalf of unprivileged users.
 *
 * Supported modes:
 * - --stdio: Process a single connection via stdin/stdout (for systemd socket activation)
 *
 * For the full Unix domain socket server mode, this implementation defers to
 * the main `nix daemon` command.
 */

#include <cstring>
#include <iostream>
#include <optional>

#include "nix/cmd/legacy.h"
#include "nix/store/daemon.h"
#include "nix/store/globals.h"
#include "nix/store/store-api.h"
#include "nix/store/store-open.h"
#include "nix/util/args.h"

namespace nix {

/**
 * Process a client connecting via stdin/stdout.
 *
 * This is used for systemd socket activation where the socket is already
 * connected and passed as stdin/stdout.
 */
static void process_stdio_connection(ref<store_t> store, TrustedFlag trusted) {
  daemon::process_connection(store, fd_source_t(STDIN_FILENO), fd_sink_t(STDOUT_FILENO), trusted,
                             daemon::NotRecursive);
}

static void main_nix_daemon(int argc, char** argv) {
  bool stdio = false;
  bool show_help = false;
  bool show_version = false;
  std::optional<TrustedFlag> trusted_opt = std::nullopt;

  // Simple argument parsing
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];

    if (arg == "--daemon") {
      // Ignored for backwards compatibility
    } else if (arg == "--stdio") {
      stdio = true;
    } else if (arg == "--help" || arg == "-h") {
      show_help = true;
    } else if (arg == "--version") {
      show_version = true;
    } else if (arg == "--force-trusted") {
      trusted_opt = Trusted;
    } else if (arg == "--force-untrusted") {
      trusted_opt = NotTrusted;
    } else if (arg[0] == '-') {
      throw Error("unrecognized option: %s\nUse --help for usage information.", arg);
    }
  }

  if (show_version) {
    std::cout << "nix-daemon (straylight)\n";
    return;
  }

  if (show_help) {
    std::cout << R"(nix-daemon (straylight)

The Nix daemon allows unprivileged users to access the Nix store.

Usage: nix-daemon [options]

Options:
  --stdio             Read/write on stdin/stdout instead of socket
  --force-trusted     Force trusting connecting clients
  --force-untrusted   Force not trusting connecting clients
  --help              Show this help message
  --version           Show version information

The --stdio mode is used for systemd socket activation.

For production use, systemd will typically handle socket activation:
  nix-daemon --stdio < /dev/null

)";
    return;
  }

  // Open the store
  auto store = open_store();

  if (stdio) {
    // For stdio mode, trust is typically granted because we can't verify
    // the peer. Use Trusted as default unless explicitly overridden.
    process_stdio_connection(store, trusted_opt.value_or(Trusted));
  } else {
    // Non-stdio mode requires the full Unix socket server infrastructure.
    // For now, we only support --stdio mode.
    throw Error("nix-daemon without --stdio is not yet supported in straylight.\n"
                "Use 'nix daemon' for the full daemon functionality, or use --stdio for "
                "systemd socket activation.");
  }
}

static RegisterLegacyCommand r_nix_daemon("nix-daemon", main_nix_daemon);

} // namespace nix
