// straylight // nix // store // tests
//
// Protocol compatibility tests - executable specification for Nix worker protocol
//
// These tests verify that straylight-nix uses the same worker protocol as upstream nix.
// Protocol compatibility is CRITICAL for interoperability with upstream nix daemons.
//
// Background:
//   - The worker protocol is used for IPC between nix client and daemon (unix://, ssh-ng://)
//   - Protocol version and opcodes MUST match upstream for cross-version compatibility
//   - Serialization byte order must be consistent (little-endian on most platforms)
//   - Breaking protocol compatibility would prevent straylight-nix from communicating
//     with upstream nix daemons, breaking multi-user setups and remote builds
//
// Reference: https://github.com/NixOS/nix/blob/master/src/libstore/worker-protocol.hh

#include <cstdint>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

// Include protocol header to verify constants at compile time
#include "nix/store/worker-protocol.h"

namespace {

// =============================================================================
// Test helpers
// =============================================================================

std::string read_file(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return "";
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

bool contains_pattern(const std::string& content, const std::string& pattern) {
  std::regex re(pattern);
  return std::regex_search(content, re);
}

} // namespace

// =============================================================================
// Protocol version compatibility tests
// =============================================================================

TEST_CASE("Worker protocol version matches upstream", "[store][protocol][compatibility]") {
  // Protocol version format: (major << 8) | minor
  // Current upstream nix uses version 1.38 (as of nix 2.24+)

  SECTION("PROTOCOL_VERSION is correctly encoded") {
    INFO("Protocol version must be encoded as (major << 8 | minor)");
    INFO("Current version: " << PROTOCOL_VERSION);

    // Extract major and minor
    unsigned int major = GET_PROTOCOL_MAJOR(PROTOCOL_VERSION) >> 8;
    unsigned int minor = GET_PROTOCOL_MINOR(PROTOCOL_VERSION);

    // Major version should be 1 (has been 1 since Nix inception)
    REQUIRE(major == 1);

    // Minor version should be at least 35 for modern nix features
    // Version 1.35 introduced remoteTrustsUs
    // Version 1.37 introduced cpu timing
    // Version 1.38 introduced QueryActiveBuilds
    REQUIRE(minor >= 35);

    // Verify encoding roundtrip
    REQUIRE(PROTOCOL_VERSION == ((1 << 8) | minor));
  }

  SECTION("MINIMUM_PROTOCOL_VERSION allows older clients") {
    INFO("Minimum version allows older clients to connect");

    unsigned int min_major = GET_PROTOCOL_MAJOR(MINIMUM_PROTOCOL_VERSION) >> 8;
    unsigned int min_minor = GET_PROTOCOL_MINOR(MINIMUM_PROTOCOL_VERSION);

    // Should be major version 1
    REQUIRE(min_major == 1);

    // Minimum should be lower than current
    REQUIRE(MINIMUM_PROTOCOL_VERSION < PROTOCOL_VERSION);

    // Version 1.18 is the historical minimum for modern nix
    REQUIRE(min_minor >= 18);
  }
}

// =============================================================================
// Magic number tests
// =============================================================================

TEST_CASE("Worker protocol magic numbers match upstream", "[store][protocol][compatibility]") {
  // These magic numbers are used in the handshake to identify nix protocol
  // They spell "nixc" and "oixd" in ASCII (reversed for little-endian)

  SECTION("WORKER_MAGIC_1 is correct") {
    INFO("WORKER_MAGIC_1 identifies nix client connection");
    // 0x6e697863 = "nixc" in little-endian ASCII
    REQUIRE(WORKER_MAGIC_1 == 0x6e697863);

    // Verify ASCII encoding
    uint32_t magic = WORKER_MAGIC_1;
    REQUIRE((magic & 0xFF) == 'c');
    REQUIRE(((magic >> 8) & 0xFF) == 'x');
    REQUIRE(((magic >> 16) & 0xFF) == 'i');
    REQUIRE(((magic >> 24) & 0xFF) == 'n');
  }

  SECTION("WORKER_MAGIC_2 is correct") {
    INFO("WORKER_MAGIC_2 is daemon response");
    // 0x6478696f = "dxio" in little-endian ASCII
    REQUIRE(WORKER_MAGIC_2 == 0x6478696f);
  }
}

// =============================================================================
// Stderr protocol constants
// =============================================================================

TEST_CASE("Stderr protocol constants match upstream", "[store][protocol][compatibility]") {
  // These constants are used to multiplex stderr messages during RPC

  SECTION("STDERR_NEXT is correct") {
    // "olmg" - signals more output follows
    REQUIRE(STDERR_NEXT == 0x6f6c6d67);
  }

  SECTION("STDERR_READ is correct") {
    // "data" - daemon needs data from client
    REQUIRE(STDERR_READ == 0x64617461);
  }

  SECTION("STDERR_WRITE is correct") {
    // Similar to READ but for writing
    REQUIRE(STDERR_WRITE == 0x64617416);
  }

  SECTION("STDERR_LAST is correct") {
    // "alts" - last message
    REQUIRE(STDERR_LAST == 0x616c7473);
  }

  SECTION("STDERR_ERROR is correct") {
    // "cxtp" - error occurred
    REQUIRE(STDERR_ERROR == 0x63787470);
  }

  SECTION("Activity constants are correct") {
    // These are used for progress reporting
    REQUIRE(STDERR_START_ACTIVITY == 0x53545254); // "STRT"
    REQUIRE(STDERR_STOP_ACTIVITY == 0x53544f50);  // "STOP"
    REQUIRE(STDERR_RESULT == 0x52534c54);         // "RSLT"
  }
}

// =============================================================================
// Worker protocol opcode tests
// =============================================================================

TEST_CASE("WorkerProto::Op enum has required opcodes", "[store][protocol][compatibility]") {
  // These opcodes MUST match upstream nix for protocol compatibility
  // The numeric values are part of the wire protocol

  SECTION("Core path operations have correct values") {
    using Op = nix::WorkerProto::Op;

    // These are the fundamental operations
    REQUIRE(static_cast<uint64_t>(Op::IsValidPath) == 1);
    REQUIRE(static_cast<uint64_t>(Op::HasSubstitutes) == 3);
    REQUIRE(static_cast<uint64_t>(Op::QueryPathHash) == 4);
    REQUIRE(static_cast<uint64_t>(Op::QueryReferences) == 5);
    REQUIRE(static_cast<uint64_t>(Op::QueryReferrers) == 6);
  }

  SECTION("Store operations have correct values") {
    using Op = nix::WorkerProto::Op;

    REQUIRE(static_cast<uint64_t>(Op::AddToStore) == 7);
    REQUIRE(static_cast<uint64_t>(Op::AddTextToStore) == 8);
    REQUIRE(static_cast<uint64_t>(Op::BuildPaths) == 9);
    REQUIRE(static_cast<uint64_t>(Op::EnsurePath) == 10);
  }

  SECTION("Root and GC operations have correct values") {
    using Op = nix::WorkerProto::Op;

    REQUIRE(static_cast<uint64_t>(Op::AddTempRoot) == 11);
    REQUIRE(static_cast<uint64_t>(Op::AddIndirectRoot) == 12);
    REQUIRE(static_cast<uint64_t>(Op::SyncWithGC) == 13);
    REQUIRE(static_cast<uint64_t>(Op::FindRoots) == 14);
    REQUIRE(static_cast<uint64_t>(Op::CollectGarbage) == 20);
  }

  SECTION("Query operations have correct values") {
    using Op = nix::WorkerProto::Op;

    REQUIRE(static_cast<uint64_t>(Op::QueryDeriver) == 18);
    REQUIRE(static_cast<uint64_t>(Op::SetOptions) == 19);
    REQUIRE(static_cast<uint64_t>(Op::QuerySubstitutablePathInfo) == 21);
    REQUIRE(static_cast<uint64_t>(Op::QueryDerivationOutputs) == 22);
    REQUIRE(static_cast<uint64_t>(Op::QueryAllValidPaths) == 23);
    REQUIRE(static_cast<uint64_t>(Op::QueryFailedPaths) == 24);
    REQUIRE(static_cast<uint64_t>(Op::ClearFailedPaths) == 25);
    REQUIRE(static_cast<uint64_t>(Op::QueryPathInfo) == 26);
  }

  SECTION("Modern operations have correct values") {
    using Op = nix::WorkerProto::Op;

    // Added in later protocol versions
    REQUIRE(static_cast<uint64_t>(Op::QueryDerivationOutputNames) == 28);
    REQUIRE(static_cast<uint64_t>(Op::QueryPathFromHashPart) == 29);
    REQUIRE(static_cast<uint64_t>(Op::QuerySubstitutablePathInfos) == 30);
    REQUIRE(static_cast<uint64_t>(Op::QueryValidPaths) == 31);
    REQUIRE(static_cast<uint64_t>(Op::QuerySubstitutablePaths) == 32);
    REQUIRE(static_cast<uint64_t>(Op::QueryValidDerivers) == 33);
    REQUIRE(static_cast<uint64_t>(Op::OptimiseStore) == 34);
    REQUIRE(static_cast<uint64_t>(Op::VerifyStore) == 35);
  }

  SECTION("Build and NAR operations have correct values") {
    using Op = nix::WorkerProto::Op;

    REQUIRE(static_cast<uint64_t>(Op::BuildDerivation) == 36);
    REQUIRE(static_cast<uint64_t>(Op::AddSignatures) == 37);
    REQUIRE(static_cast<uint64_t>(Op::NarFromPath) == 38);
    REQUIRE(static_cast<uint64_t>(Op::AddToStoreNar) == 39);
  }

  SECTION("CA derivation operations have correct values") {
    using Op = nix::WorkerProto::Op;

    REQUIRE(static_cast<uint64_t>(Op::QueryMissing) == 40);
    REQUIRE(static_cast<uint64_t>(Op::QueryDerivationOutputMap) == 41);
    REQUIRE(static_cast<uint64_t>(Op::RegisterDrvOutput) == 42);
    REQUIRE(static_cast<uint64_t>(Op::QueryRealisation) == 43);
  }

  SECTION("Recent additions have correct values") {
    using Op = nix::WorkerProto::Op;

    REQUIRE(static_cast<uint64_t>(Op::AddMultipleToStore) == 44);
    REQUIRE(static_cast<uint64_t>(Op::AddBuildLog) == 45);
    REQUIRE(static_cast<uint64_t>(Op::BuildPathsWithResults) == 46);
    REQUIRE(static_cast<uint64_t>(Op::AddPermRoot) == 47);
    REQUIRE(static_cast<uint64_t>(Op::QueryActiveBuilds) == 48);
  }
}

// =============================================================================
// Serialization format tests
// =============================================================================

TEST_CASE("Worker protocol serialization uses correct format", "[store][protocol][compatibility]") {
  std::string cpp_file = read_file("src/nix/store/worker-protocol.cpp");
  REQUIRE_FALSE(cpp_file.empty());

  SECTION("Opcodes are serialized as uint64_t") {
    // The operator<< must cast to uint64_t for wire compatibility
    std::string header = read_file("src/nix/store/worker-protocol.h");
    REQUIRE_FALSE(header.empty());

    INFO("Op enum must serialize as uint64_t");
    REQUIRE(contains_pattern(header, R"(static_cast<uint64_t>\(op\))"));
  }

  SECTION("BuildMode uses correct wire values") {
    // bmNormal = 0, bmRepair = 1, bmCheck = 2
    INFO("BuildMode serialization must use correct numeric values");

    // Check that write function uses correct values
    REQUIRE(contains_pattern(cpp_file, R"(case bmNormal:\s*\n\s*conn\.to << uint8_t\{0\})"));
    REQUIRE(contains_pattern(cpp_file, R"(case bmRepair:\s*\n\s*conn\.to << uint8_t\{1\})"));
    REQUIRE(contains_pattern(cpp_file, R"(case bmCheck:\s*\n\s*conn\.to << uint8_t\{2\})"));
  }

  SECTION("GCAction uses correct wire values") {
    // gcReturnLive = 0, gcReturnDead = 1, gcDeleteDead = 2, gcDeleteSpecific = 3
    INFO("GCAction serialization must use correct numeric values");

    REQUIRE(contains_pattern(cpp_file, R"(case gcReturnLive:\s*\n\s*conn\.to << unsigned\{0\})"));
    REQUIRE(contains_pattern(cpp_file, R"(case gcReturnDead:\s*\n\s*conn\.to << unsigned\{1\})"));
    REQUIRE(contains_pattern(cpp_file, R"(case gcDeleteDead:\s*\n\s*conn\.to << unsigned\{2\})"));
    REQUIRE(
        contains_pattern(cpp_file, R"(case gcDeleteSpecific:\s*\n\s*conn\.to << unsigned\{3\})"));
  }

  SECTION("TrustedFlag uses correct wire values") {
    // nullopt = 0, Trusted = 1, NotTrusted = 2
    INFO("TrustedFlag serialization must match upstream");

    // Check read function for correct interpretation
    REQUIRE(contains_pattern(cpp_file, R"(case 0:\s*\n\s*return std::nullopt)"));
    REQUIRE(contains_pattern(cpp_file, R"(case 1:\s*\n\s*return \{Trusted\})"));
    REQUIRE(contains_pattern(cpp_file, R"(case 2:\s*\n\s*return \{NotTrusted\})"));
  }
}

// =============================================================================
// Protocol version negotiation tests
// =============================================================================

TEST_CASE("Protocol version macros work correctly", "[store][protocol][compatibility]") {
  SECTION("GET_PROTOCOL_MAJOR extracts high byte") {
    // Version 1.38 = (1 << 8) | 38 = 294
    unsigned int version = (1 << 8) | 38;
    REQUIRE(GET_PROTOCOL_MAJOR(version) == (1 << 8));
    REQUIRE((GET_PROTOCOL_MAJOR(version) >> 8) == 1);
  }

  SECTION("GET_PROTOCOL_MINOR extracts low byte") {
    unsigned int version = (1 << 8) | 38;
    REQUIRE(GET_PROTOCOL_MINOR(version) == 38);
  }

  SECTION("Version comparison works correctly") {
    // Version 1.35 < 1.38
    unsigned int v135 = (1 << 8) | 35;
    unsigned int v138 = (1 << 8) | 38;
    REQUIRE(v135 < v138);
    REQUIRE(GET_PROTOCOL_MINOR(v135) < GET_PROTOCOL_MINOR(v138));
  }
}

// =============================================================================
// Feature negotiation tests
// =============================================================================

TEST_CASE("Worker protocol features are defined", "[store][protocol][compatibility]") {
  SECTION("queryActiveBuilds feature is defined") {
    INFO("Feature strings are used for capability negotiation");
    REQUIRE(nix::WorkerProto::featureQueryActiveBuilds == "queryActiveBuilds");
  }
}
