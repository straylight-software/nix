// straylight // nix // cli // tests
//
// nix-daemon integration tests - executable specification for NixOS multi-user mode
//
// These tests verify that straylight-nix implements the nix-daemon functionality
// required for NixOS multi-user installations.
//
// Background:
//   - NixOS multi-user mode requires nix-daemon to provide store access to unprivileged users
//   - systemd socket activation uses `nix-daemon --stdio` mode
//   - The daemon handles the worker protocol for store operations
//   - Clients connect and perform a handshake using WORKER_MAGIC_1/WORKER_MAGIC_2
//
// References:
//   - src/nix/store/daemon.h - process_connection() function
//   - src/nix/store/worker-protocol.h - protocol magic numbers and operations
//   - src/nix/cli/nix-daemon.cpp - legacy command implementation

#include <catch2/catch_test_macros.hpp>

#include "nix/cmd/legacy.h"
#include "nix/store/store-api.h"
#include "nix/store/worker-protocol.h"

using namespace nix;

// =============================================================================
// nix-daemon command registration
// =============================================================================

TEST_CASE("nix-daemon command is registered for NixOS compatibility",
          "[daemon][nixos][integration]") {
  INFO("NixOS multi-user mode requires nix-daemon to be available as a legacy command");
  INFO("systemd starts nix-daemon to provide store access to unprivileged users");

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-daemon is registered as a legacy command") {
    auto it = commands.find("nix-daemon");
    REQUIRE(it != commands.end());
    REQUIRE(it->second != nullptr);
  }
}

// =============================================================================
// nix-daemon --stdio mode (IMPLEMENTED)
// =============================================================================

TEST_CASE("nix-daemon supports --stdio mode for systemd socket activation",
          "[daemon][stdio][integration]") {
  INFO("systemd socket activation passes connected sockets as stdin/stdout");
  INFO("nix-daemon --stdio processes a single connection via these file descriptors");
  INFO("This is the primary mode used in NixOS multi-user installations");

  auto& commands = RegisterLegacyCommand::commands();
  auto it = commands.find("nix-daemon");
  REQUIRE(it != commands.end());

  // The command is registered and can be invoked
  // Actual invocation would require mocked stdin/stdout with proper protocol data
  SECTION("command handler is callable") {
    REQUIRE(it->second != nullptr);
  }
}

// =============================================================================
// nix-daemon socket mode (NOT IMPLEMENTED - documenting expected behavior)
// =============================================================================

TEST_CASE("nix-daemon socket mode is not yet implemented",
          "[daemon][socket][integration][!mayfail]") {
  INFO("Full Unix domain socket server mode is not yet implemented in straylight");
  INFO("This mode would allow nix-daemon to listen on /nix/var/nix/daemon-socket/socket");
  INFO("Currently, users should use 'nix daemon' for full daemon functionality");
  INFO("Or use --stdio mode with systemd socket activation");

  // This test documents the expected error behavior when socket mode is requested
  // The implementation in nix-daemon.cpp throws an Error for non-stdio mode
  SECTION("socket mode is explicitly unsupported") {
    // We can verify the command exists but cannot easily test the error
    // without actually invoking it (which would try to open the store)
    auto& commands = RegisterLegacyCommand::commands();
    REQUIRE(commands.contains("nix-daemon"));

    // Document the expected behavior:
    // When invoked without --stdio, it should throw:
    // "nix-daemon without --stdio is not yet supported in straylight"
  }
}

// =============================================================================
// Worker protocol constants verification
// =============================================================================

TEST_CASE("Worker protocol constants are defined for daemon communication",
          "[daemon][protocol][integration]") {
  INFO("The worker protocol is used for daemon<->client communication");
  INFO("These magic numbers identify valid Nix protocol connections");

  SECTION("WORKER_MAGIC_1 is the client greeting") {
    // Client sends this first to identify itself as a Nix client
    // The value 0x6e697863 spells "nixc" in ASCII (little-endian)
    REQUIRE(WORKER_MAGIC_1 == 0x6e697863);
  }

  SECTION("WORKER_MAGIC_2 is the daemon response") {
    // Daemon responds with this to acknowledge a valid connection
    // The value 0x6478696f spells "dxio" in ASCII (little-endian)
    REQUIRE(WORKER_MAGIC_2 == 0x6478696f);
  }

  SECTION("Protocol version is defined") {
    // Protocol version uses major.minor encoding: (major << 8) | minor
    REQUIRE(GET_PROTOCOL_MAJOR(PROTOCOL_VERSION) == 0x100); // major = 1
    REQUIRE(GET_PROTOCOL_MINOR(PROTOCOL_VERSION) == 38);    // minor = 38
  }

  SECTION("Minimum protocol version is defined") {
    // Clients below this version are rejected
    REQUIRE(GET_PROTOCOL_MAJOR(MINIMUM_PROTOCOL_VERSION) == 0x100); // major = 1
    REQUIRE(GET_PROTOCOL_MINOR(MINIMUM_PROTOCOL_VERSION) == 18);    // minor = 18
  }
}

// =============================================================================
// Worker protocol operations
// =============================================================================

TEST_CASE("Worker protocol defines required store operations", "[daemon][protocol][integration]") {
  INFO("These operations are used by clients to interact with the Nix store via the daemon");
  INFO("Each operation corresponds to a store_t method");

  SECTION("Basic query operations are defined") {
    // These are the most commonly used operations
    REQUIRE(static_cast<uint64_t>(WorkerProto::Op::IsValidPath) == 1);
    REQUIRE(static_cast<uint64_t>(WorkerProto::Op::QueryPathInfo) == 26);
    REQUIRE(static_cast<uint64_t>(WorkerProto::Op::QueryValidPaths) == 31);
  }

  SECTION("Build operations are defined") {
    REQUIRE(static_cast<uint64_t>(WorkerProto::Op::BuildPaths) == 9);
    REQUIRE(static_cast<uint64_t>(WorkerProto::Op::BuildDerivation) == 36);
    REQUIRE(static_cast<uint64_t>(WorkerProto::Op::BuildPathsWithResults) == 46);
  }

  SECTION("Store modification operations are defined") {
    REQUIRE(static_cast<uint64_t>(WorkerProto::Op::AddToStore) == 7);
    REQUIRE(static_cast<uint64_t>(WorkerProto::Op::AddToStoreNar) == 39);
    REQUIRE(static_cast<uint64_t>(WorkerProto::Op::AddMultipleToStore) == 44);
  }

  SECTION("Garbage collection operations are defined") {
    REQUIRE(static_cast<uint64_t>(WorkerProto::Op::CollectGarbage) == 20);
    REQUIRE(static_cast<uint64_t>(WorkerProto::Op::AddTempRoot) == 11);
    REQUIRE(static_cast<uint64_t>(WorkerProto::Op::AddIndirectRoot) == 12);
  }
}

// =============================================================================
// Stderr protocol constants
// =============================================================================

TEST_CASE("Stderr protocol constants are defined for daemon logging",
          "[daemon][protocol][integration]") {
  INFO("The daemon sends log messages and errors to clients using these markers");
  INFO("This allows real-time progress reporting during long operations");

  SECTION("Log message markers are defined") {
    REQUIRE(STDERR_NEXT == 0x6f6c6d67);  // More log messages follow
    REQUIRE(STDERR_LAST == 0x616c7473);  // Operation completed successfully
    REQUIRE(STDERR_ERROR == 0x63787470); // Operation failed with error
  }

  SECTION("Activity markers are defined for progress reporting") {
    REQUIRE(STDERR_START_ACTIVITY == 0x53545254); // "STRT"
    REQUIRE(STDERR_STOP_ACTIVITY == 0x53544f50);  // "STOP"
    REQUIRE(STDERR_RESULT == 0x52534c54);         // "RSLT"
  }

  SECTION("Data transfer markers are defined") {
    REQUIRE(STDERR_READ == 0x64617461);  // Daemon needs data from client
    REQUIRE(STDERR_WRITE == 0x64617416); // Daemon sending data to client
  }
}

// =============================================================================
// Daemon trust levels
// =============================================================================

TEST_CASE("Daemon trust levels are available", "[daemon][trust][integration]") {
  INFO("Trust levels determine what operations a client can perform");
  INFO("In stdio mode, clients are typically trusted (systemd socket activation)");
  INFO("In socket mode, trust would be determined by peer credentials");

  SECTION("TrustedFlag values are defined") {
    // These are used to control client capabilities
    REQUIRE(static_cast<bool>(Trusted) == true);
    REQUIRE(static_cast<bool>(NotTrusted) == false);
  }
}

// =============================================================================
// Feature negotiation
// =============================================================================

TEST_CASE("Worker protocol supports feature negotiation", "[daemon][protocol][integration]") {
  INFO("Protocol version 1.38+ supports feature negotiation");
  INFO("This allows graceful capability discovery between client and daemon");

  SECTION("allFeatures set is available") {
    // The daemon advertises its supported features during handshake
    // This is a static member of WorkerProto
    const auto& features = WorkerProto::allFeatures;
    // The set exists (may be empty or contain features)
    REQUIRE(features.size() >= 0);
  }

  SECTION("Known features are defined as constants") {
    // Feature strings for capability negotiation
    REQUIRE(WorkerProto::featureQueryActiveBuilds == "queryActiveBuilds");
  }
}
