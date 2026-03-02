/**
 * build-remote command for distributed builds
 *
 * This command is invoked by the Nix build hook for distributed builds.
 * The default build hook setting is 'nix __build-remote', which dispatches
 * to this command via RegisterLegacyCommand("build-remote", ...).
 *
 * Protocol:
 * 1. Read config settings from stdin (key=value pairs, terminated by null)
 * 2. Read build requests: "try <slots> <system> <drv_path> <features>"
 * 3. Reply: "# decline", "# postpone", or "# accept" + machine name
 * 4. If accepted: read input paths and output names, copy files, build, copy back
 *
 * See:
 * - https://nixos.org/manual/nix/stable/advanced-topics/distributed-builds.html
 * - src/build-remote/build-remote.cc in upstream nix
 */

#include <algorithm>
#include <cstring>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "nix/cmd/legacy.h"
#include "nix/store/build-result.h"
#include "nix/store/builder-health.h"
#include "nix/store/common-protocol-impl.h"
#include "nix/store/derivations.h"
#include "nix/store/globals.h"
#include "nix/store/machines.h"
#include "nix/store/store-api.h"
#include "nix/store/store-open.h"
#include "nix/util/args.h"
#include "nix/util/serialise.h"
#include "nix/util/strings.h"

namespace nix {

namespace {

// Logger output goes to stderr (fd 2)
// Build output goes to fd 4 (builder_out pipe)
constexpr int BUILDER_OUT_FD = 4;

struct BuildRequest {
  size_t available_local_slots = 0;
  std::string system;
  std::optional<store_path_t> drv_path;
  string_set_t required_features;
};

std::optional<BuildRequest> parse_build_request(const std::string& line, store_t& store) {
  // Format: "try <slots> <system> <drv_path> [<features>...]"
  auto tokens = tokenize_string<std::vector<std::string>>(line);
  if (tokens.empty() || tokens[0] != "try") {
    return std::nullopt;
  }
  if (tokens.size() < 4) {
    return std::nullopt;
  }

  BuildRequest req;
  req.available_local_slots = std::stoull(tokens[1]);
  req.system = tokens[2];
  req.drv_path = store.parseStorePath(tokens[3]);

  // Features are space-separated after the drv path
  for (size_t i = 4; i < tokens.size(); i++) {
    req.required_features.insert(tokens[i]);
  }

  return req;
}

struct SelectedMachine {
  const Machine* machine = nullptr;
  ref<store_t> remote_store;
};

std::optional<SelectedMachine> select_machine(const Machines& machines,
                                              const BuildRequest& request) {
  // Find a machine that:
  // 1. Supports the required system
  // 2. Supports all required features
  // 3. Has mandatory features that are a subset of required features
  // 4. Is enabled
  // 5. Is not in backoff period due to recent failures

  auto& healthTracker = BuilderHealthTracker::instance();
  unsigned int initialBackoff = settings.builderFailureBackoffInitial;
  unsigned int maxBackoff = settings.builderFailureBackoffMax;

  // Sort by speed factor (descending) to prefer faster machines
  std::vector<const Machine*> candidates;
  std::vector<const Machine*> skipped_machines;

  for (const auto& m : machines) {
    if (m.enabled && m.systemSupported(request.system) &&
        m.allSupported(request.required_features) && m.mandatoryMet(request.required_features)) {
      auto uri = m.storeUri.render();

      // Check if this builder should be skipped due to recent failures
      if (healthTracker.shouldSkip(uri, initialBackoff, maxBackoff)) {
        skipped_machines.push_back(&m);
      } else {
        candidates.push_back(&m);
      }
    }
  }

  // If all suitable machines are in backoff, include them anyway
  // (better to try a potentially-down machine than fail completely)
  if (candidates.empty() && !skipped_machines.empty()) {
    debug("all suitable builders are in backoff, trying them anyway");
    candidates = std::move(skipped_machines);
  }

  if (candidates.empty()) {
    return std::nullopt;
  }

  // Sort by speed factor (higher is better)
  std::ranges::sort(candidates, [](const Machine* a, const Machine* b) {
    return a->speedFactor > b->speedFactor;
  });

  // Try to connect to machines in order of preference
  for (const auto* machine : candidates) {
    auto uri = machine->storeUri.render();
    try {
      auto store = machine->open_store();
      // Connection succeeded - record success to reset backoff
      healthTracker.recordSuccess(uri);
      return SelectedMachine{.machine = machine, .remote_store = store};
    } catch (Error& e) {
      // Record failure to trigger backoff
      healthTracker.recordFailure(uri, initialBackoff, maxBackoff);
      printMsg(lvl_warn, "cannot connect to '%s': %s", uri, e.what());
    }
  }

  return std::nullopt;
}

std::string get_machine_name(const Machine& machine) {
  // Extract a machine name from the store URI for display
  auto uri = machine.storeUri.render();
  // For ssh://user@host, return "user@host"
  auto pos = uri.find("://");
  if (pos != std::string::npos) {
    return uri.substr(pos + 3);
  }
  return uri;
}

void do_build(ref<store_t> local_store, ref<store_t> remote_store, const Machine& machine,
              const BuildRequest& request) {
  auto machine_name = get_machine_name(machine);
  const auto& drv_path = *request.drv_path;

  // Read input paths from stdin using CommonProto
  fd_source_t from(STDIN_FILENO);
  CommonProto::ReadConn read_conn{.from = from};
  auto input_paths = CommonProto::Serialise<store_path_set_t>::read(*local_store, read_conn);

  // Read required outputs
  auto outputs = CommonProto::Serialise<string_set_t>::read(*local_store, read_conn);

  // Copy input closure to remote
  printMsg(lvl_info, "copying inputs to '%s'...", machine_name);
  copy_closure(*local_store, *remote_store, input_paths, NoRepair, NoCheckSigs, NoSubstitute);

  // Read derivation and build on remote
  printMsg(lvl_info, "building '%s' on '%s'...", local_store->printStorePath(drv_path),
           machine_name);

  auto drv = local_store->read_derivation(drv_path);
  auto result = remote_store->buildDerivation(drv_path, drv, bmNormal);

  if (const auto* failure = result.tryGetFailure()) {
    throw Error("build of '%s' on '%s' failed: %s", local_store->printStorePath(drv_path),
                machine_name, failure->errorMsg);
  }

  // Copy outputs back
  if (const auto* success = result.tryGetSuccess()) {
    store_path_set_t output_paths;
    for (const auto& [name, realisation] : success->built_outputs) {
      output_paths.insert(realisation.out_path);
    }

    if (!output_paths.empty()) {
      printMsg(lvl_info, "copying outputs from '%s'...", machine_name);
      copy_closure(*remote_store, *local_store, output_paths, NoRepair, CheckSigs, NoSubstitute);
    }

    printMsg(lvl_info, "build of '%s' on '%s' succeeded", local_store->printStorePath(drv_path),
             machine_name);
  }
}

void main_build_remote(int argc, char** argv) {
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

See: https://nixos.org/manual/nix/stable/advanced-topics/distributed-builds.html
)";
      return;
    }

    if (arg == "--version") {
      std::cout << "build-remote (straylight)\n";
      return;
    }
  }

  // Open local store
  auto local_store = open_store();

  // Read settings from stdin (terminated by empty line or null byte)
  // These are key=value pairs for additional settings
  std::string settings_line;
  while (std::getline(std::cin, settings_line) && !settings_line.empty()) {
    // Settings are passed but we don't need to modify global settings
    // The daemon has already configured the environment
  }

  // Get available machines
  Machines machines = get_machines();

  if (machines.empty()) {
    // No machines configured, decline all builds
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.starts_with("try")) {
        std::cout << "# decline-permanently\n";
        std::cout.flush();
      }
    }
    return;
  }

  // Process build requests
  std::string line;
  while (std::getline(std::cin, line)) {
    auto request_opt = parse_build_request(line, *local_store);
    if (!request_opt) {
      continue;
    }
    auto& request = *request_opt;

    // Try to find a suitable machine
    auto selected = select_machine(machines, request);
    if (!selected) {
      std::cout << "# decline\n";
      std::cout.flush();
      continue;
    }

    const auto& machine = *selected->machine;
    auto& remote_store = selected->remote_store;
    auto machine_name = get_machine_name(machine);

    std::cout << "# accept\n";
    std::cout << machine_name << "\n";
    std::cout.flush();

    try {
      do_build(local_store, remote_store, machine, request);
    } catch (Error& e) {
      // Send error to builder output fd if available
      try {
        fd_sink_t builder_out(BUILDER_OUT_FD);
        builder_out << fmt("error: %s\n", e.what());
      } catch (...) {
        // Ignore write errors to builder fd
      }
      throw;
    }
  }
}

} // anonymous namespace

static RegisterLegacyCommand r_build_remote("build-remote", main_build_remote);

} // namespace nix
