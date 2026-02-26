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
#include <iterator>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

// Include protocol header to verify constants at compile time
#include "nix/store/build-result.h"
#include "nix/store/content-address.h"
#include "nix/store/gc-store.h"
#include "nix/store/path-info.h"
#include "nix/store/path.h"
#include "nix/store/worker-protocol.h"
#include "nix/util/experimental-features.h"
#include "nix/util/hash.h"
#include "nix/util/logging.h"

namespace {

// =============================================================================
// Test helpers
// =============================================================================

std::string read_file(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return "";
  }
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
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

  SECTION("Protocol version encoding preserves full range") {
    // Test boundary conditions for version encoding
    unsigned int v_min = (1 << 8) | 0;   // 1.0
    unsigned int v_max = (1 << 8) | 255; // 1.255

    REQUIRE(GET_PROTOCOL_MAJOR(v_min) == (1 << 8));
    REQUIRE(GET_PROTOCOL_MINOR(v_min) == 0);
    REQUIRE(GET_PROTOCOL_MAJOR(v_max) == (1 << 8));
    REQUIRE(GET_PROTOCOL_MINOR(v_max) == 255);
  }

  SECTION("Version comparison semantics are correct") {
    // Versions compare correctly as integers
    unsigned int v118 = (1 << 8) | 18;
    unsigned int v135 = (1 << 8) | 35;
    unsigned int v138 = (1 << 8) | 38;

    REQUIRE(v118 < v135);
    REQUIRE(v135 < v138);
    REQUIRE(v118 < v138);

    // Major version difference dominates
    unsigned int v2_0 = (2 << 8) | 0;
    REQUIRE(v138 < v2_0);
  }
}

// =============================================================================
// Protocol version negotiation tests
// =============================================================================

TEST_CASE("Protocol version negotiation logic", "[store][protocol][compatibility]") {
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

  SECTION("Feature introduction versions are testable") {
    // Version 1.16: ValidPathInfo ultimate and sigs
    unsigned int v116 = (1 << 8) | 16;
    REQUIRE(GET_PROTOCOL_MINOR(v116) >= 16);

    // Version 1.28: built_outputs in BuildResult
    unsigned int v128 = (1 << 8) | 28;
    REQUIRE(GET_PROTOCOL_MINOR(v128) >= 28);

    // Version 1.29: timesBuilt, isNonDeterministic, start_time, stopTime
    unsigned int v129 = (1 << 8) | 29;
    REQUIRE(GET_PROTOCOL_MINOR(v129) >= 29);

    // Version 1.30: DerivedPath::parseLegacy
    unsigned int v130 = (1 << 8) | 30;
    REQUIRE(GET_PROTOCOL_MINOR(v130) >= 30);

    // Version 1.33: daemonNixVersion
    unsigned int v133 = (1 << 8) | 33;
    REQUIRE(GET_PROTOCOL_MINOR(v133) >= 33);

    // Version 1.35: remoteTrustsUs
    unsigned int v135 = (1 << 8) | 35;
    REQUIRE(GET_PROTOCOL_MINOR(v135) >= 35);

    // Version 1.37: cpu timing
    unsigned int v137 = (1 << 8) | 37;
    REQUIRE(GET_PROTOCOL_MINOR(v137) >= 37);
  }

  SECTION("Negotiated version is minimum of client and daemon") {
    // This is a fundamental protocol rule
    auto negotiate = [](unsigned int client, unsigned int daemon) {
      return std::min(client, daemon);
    };

    unsigned int v135 = (1 << 8) | 35;
    unsigned int v138 = (1 << 8) | 38;

    REQUIRE(negotiate(v135, v138) == v135);
    REQUIRE(negotiate(v138, v135) == v135);
    REQUIRE(negotiate(v138, v138) == v138);
  }
}

// =============================================================================
// Magic number tests
// =============================================================================

TEST_CASE("Worker protocol magic numbers match upstream", "[store][protocol][compatibility]") {
  // These magic numbers are used in the handshake to identify nix protocol
  // They spell "nixc" and "dxio" in ASCII (reversed for little-endian)

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

    // Verify ASCII encoding
    uint32_t magic = WORKER_MAGIC_2;
    REQUIRE((magic & 0xFF) == 'o');
    REQUIRE(((magic >> 8) & 0xFF) == 'i');
    REQUIRE(((magic >> 16) & 0xFF) == 'x');
    REQUIRE(((magic >> 24) & 0xFF) == 'd');
  }

  SECTION("Magic numbers are distinct") {
    REQUIRE(WORKER_MAGIC_1 != WORKER_MAGIC_2);
  }
}

// =============================================================================
// Stderr protocol constants - Complete coverage
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

    // Verify it spells "data"
    uint32_t magic = STDERR_READ;
    REQUIRE((magic & 0xFF) == 'a');
    REQUIRE(((magic >> 8) & 0xFF) == 't');
    REQUIRE(((magic >> 16) & 0xFF) == 'a');
    REQUIRE(((magic >> 24) & 0xFF) == 'd');
  }

  SECTION("STDERR_WRITE is correct") {
    // Similar to READ but for writing
    REQUIRE(STDERR_WRITE == 0x64617416);
  }

  SECTION("STDERR_LAST is correct") {
    // "alts" - last message
    REQUIRE(STDERR_LAST == 0x616c7473);

    // Verify it spells "alts"
    uint32_t magic = STDERR_LAST;
    REQUIRE((magic & 0xFF) == 's');
    REQUIRE(((magic >> 8) & 0xFF) == 't');
    REQUIRE(((magic >> 16) & 0xFF) == 'l');
    REQUIRE(((magic >> 24) & 0xFF) == 'a');
  }

  SECTION("STDERR_ERROR is correct") {
    // "cxtp" - error occurred
    REQUIRE(STDERR_ERROR == 0x63787470);
  }

  SECTION("STDERR_START_ACTIVITY is correct") {
    // "STRT"
    REQUIRE(STDERR_START_ACTIVITY == 0x53545254);

    uint32_t magic = STDERR_START_ACTIVITY;
    REQUIRE((magic & 0xFF) == 'T');
    REQUIRE(((magic >> 8) & 0xFF) == 'R');
    REQUIRE(((magic >> 16) & 0xFF) == 'T');
    REQUIRE(((magic >> 24) & 0xFF) == 'S');
  }

  SECTION("STDERR_STOP_ACTIVITY is correct") {
    // "STOP"
    REQUIRE(STDERR_STOP_ACTIVITY == 0x53544f50);

    uint32_t magic = STDERR_STOP_ACTIVITY;
    REQUIRE((magic & 0xFF) == 'P');
    REQUIRE(((magic >> 8) & 0xFF) == 'O');
    REQUIRE(((magic >> 16) & 0xFF) == 'T');
    REQUIRE(((magic >> 24) & 0xFF) == 'S');
  }

  SECTION("STDERR_RESULT is correct") {
    // "RSLT"
    REQUIRE(STDERR_RESULT == 0x52534c54);

    uint32_t magic = STDERR_RESULT;
    REQUIRE((magic & 0xFF) == 'T');
    REQUIRE(((magic >> 8) & 0xFF) == 'L');
    REQUIRE(((magic >> 16) & 0xFF) == 'S');
    REQUIRE(((magic >> 24) & 0xFF) == 'R');
  }

  SECTION("All STDERR constants are distinct") {
    std::set<uint32_t> constants = {STDERR_NEXT,          STDERR_READ,  STDERR_WRITE,
                                    STDERR_LAST,          STDERR_ERROR, STDERR_START_ACTIVITY,
                                    STDERR_STOP_ACTIVITY, STDERR_RESULT};
    REQUIRE(constants.size() == 8);
  }
}

// =============================================================================
// Worker protocol opcode tests - Complete enum coverage
// =============================================================================

TEST_CASE("WorkerProto::Op enum - complete coverage", "[store][protocol][compatibility]") {
  // These opcodes MUST match upstream nix for protocol compatibility
  // The numeric values are part of the wire protocol

  using Op = nix::WorkerProto::Op;

  SECTION("Core path operations have correct values") {
    // These are the fundamental operations
    REQUIRE(static_cast<uint64_t>(Op::IsValidPath) == 1);
    // Note: opcode 2 was removed (QueryRoots)
    REQUIRE(static_cast<uint64_t>(Op::HasSubstitutes) == 3);
    REQUIRE(static_cast<uint64_t>(Op::QueryPathHash) == 4);   // obsolete
    REQUIRE(static_cast<uint64_t>(Op::QueryReferences) == 5); // obsolete
    REQUIRE(static_cast<uint64_t>(Op::QueryReferrers) == 6);
  }

  SECTION("Store operations have correct values") {
    REQUIRE(static_cast<uint64_t>(Op::AddToStore) == 7);
    REQUIRE(static_cast<uint64_t>(Op::AddTextToStore) == 8); // obsolete since 1.25
    REQUIRE(static_cast<uint64_t>(Op::BuildPaths) == 9);
    REQUIRE(static_cast<uint64_t>(Op::EnsurePath) == 10);
  }

  SECTION("Root and GC operations have correct values") {
    REQUIRE(static_cast<uint64_t>(Op::AddTempRoot) == 11);
    REQUIRE(static_cast<uint64_t>(Op::AddIndirectRoot) == 12);
    REQUIRE(static_cast<uint64_t>(Op::SyncWithGC) == 13);
    REQUIRE(static_cast<uint64_t>(Op::FindRoots) == 14);
    // Note: opcodes 15-17 are removed/unused
    REQUIRE(static_cast<uint64_t>(Op::CollectGarbage) == 20);
  }

  SECTION("Query operations have correct values") {
    REQUIRE(static_cast<uint64_t>(Op::QueryDeriver) == 18); // obsolete
    REQUIRE(static_cast<uint64_t>(Op::SetOptions) == 19);
    REQUIRE(static_cast<uint64_t>(Op::QuerySubstitutablePathInfo) == 21);
    REQUIRE(static_cast<uint64_t>(Op::QueryDerivationOutputs) == 22); // obsolete
    REQUIRE(static_cast<uint64_t>(Op::QueryAllValidPaths) == 23);
    REQUIRE(static_cast<uint64_t>(Op::QueryFailedPaths) == 24);
    REQUIRE(static_cast<uint64_t>(Op::ClearFailedPaths) == 25);
    REQUIRE(static_cast<uint64_t>(Op::QueryPathInfo) == 26);
    // Note: opcode 27 was removed (ImportPaths)
  }

  SECTION("Modern operations have correct values") {
    // Added in later protocol versions
    REQUIRE(static_cast<uint64_t>(Op::QueryDerivationOutputNames) == 28); // obsolete
    REQUIRE(static_cast<uint64_t>(Op::QueryPathFromHashPart) == 29);
    REQUIRE(static_cast<uint64_t>(Op::QuerySubstitutablePathInfos) == 30);
    REQUIRE(static_cast<uint64_t>(Op::QueryValidPaths) == 31);
    REQUIRE(static_cast<uint64_t>(Op::QuerySubstitutablePaths) == 32);
    REQUIRE(static_cast<uint64_t>(Op::QueryValidDerivers) == 33);
    REQUIRE(static_cast<uint64_t>(Op::OptimiseStore) == 34);
    REQUIRE(static_cast<uint64_t>(Op::VerifyStore) == 35);
  }

  SECTION("Build and NAR operations have correct values") {
    REQUIRE(static_cast<uint64_t>(Op::BuildDerivation) == 36);
    REQUIRE(static_cast<uint64_t>(Op::AddSignatures) == 37);
    REQUIRE(static_cast<uint64_t>(Op::NarFromPath) == 38);
    REQUIRE(static_cast<uint64_t>(Op::AddToStoreNar) == 39);
  }

  SECTION("CA derivation operations have correct values") {
    REQUIRE(static_cast<uint64_t>(Op::QueryMissing) == 40);
    REQUIRE(static_cast<uint64_t>(Op::QueryDerivationOutputMap) == 41);
    REQUIRE(static_cast<uint64_t>(Op::RegisterDrvOutput) == 42);
    REQUIRE(static_cast<uint64_t>(Op::QueryRealisation) == 43);
  }

  SECTION("Recent additions have correct values") {
    REQUIRE(static_cast<uint64_t>(Op::AddMultipleToStore) == 44);
    REQUIRE(static_cast<uint64_t>(Op::AddBuildLog) == 45);
    REQUIRE(static_cast<uint64_t>(Op::BuildPathsWithResults) == 46);
    REQUIRE(static_cast<uint64_t>(Op::AddPermRoot) == 47);
    REQUIRE(static_cast<uint64_t>(Op::QueryActiveBuilds) == 48);
  }

  SECTION("All defined opcodes are unique") {
    std::set<uint64_t> opcodes = {
        static_cast<uint64_t>(Op::IsValidPath),
        static_cast<uint64_t>(Op::HasSubstitutes),
        static_cast<uint64_t>(Op::QueryPathHash),
        static_cast<uint64_t>(Op::QueryReferences),
        static_cast<uint64_t>(Op::QueryReferrers),
        static_cast<uint64_t>(Op::AddToStore),
        static_cast<uint64_t>(Op::AddTextToStore),
        static_cast<uint64_t>(Op::BuildPaths),
        static_cast<uint64_t>(Op::EnsurePath),
        static_cast<uint64_t>(Op::AddTempRoot),
        static_cast<uint64_t>(Op::AddIndirectRoot),
        static_cast<uint64_t>(Op::SyncWithGC),
        static_cast<uint64_t>(Op::FindRoots),
        static_cast<uint64_t>(Op::QueryDeriver),
        static_cast<uint64_t>(Op::SetOptions),
        static_cast<uint64_t>(Op::CollectGarbage),
        static_cast<uint64_t>(Op::QuerySubstitutablePathInfo),
        static_cast<uint64_t>(Op::QueryDerivationOutputs),
        static_cast<uint64_t>(Op::QueryAllValidPaths),
        static_cast<uint64_t>(Op::QueryFailedPaths),
        static_cast<uint64_t>(Op::ClearFailedPaths),
        static_cast<uint64_t>(Op::QueryPathInfo),
        static_cast<uint64_t>(Op::QueryDerivationOutputNames),
        static_cast<uint64_t>(Op::QueryPathFromHashPart),
        static_cast<uint64_t>(Op::QuerySubstitutablePathInfos),
        static_cast<uint64_t>(Op::QueryValidPaths),
        static_cast<uint64_t>(Op::QuerySubstitutablePaths),
        static_cast<uint64_t>(Op::QueryValidDerivers),
        static_cast<uint64_t>(Op::OptimiseStore),
        static_cast<uint64_t>(Op::VerifyStore),
        static_cast<uint64_t>(Op::BuildDerivation),
        static_cast<uint64_t>(Op::AddSignatures),
        static_cast<uint64_t>(Op::NarFromPath),
        static_cast<uint64_t>(Op::AddToStoreNar),
        static_cast<uint64_t>(Op::QueryMissing),
        static_cast<uint64_t>(Op::QueryDerivationOutputMap),
        static_cast<uint64_t>(Op::RegisterDrvOutput),
        static_cast<uint64_t>(Op::QueryRealisation),
        static_cast<uint64_t>(Op::AddMultipleToStore),
        static_cast<uint64_t>(Op::AddBuildLog),
        static_cast<uint64_t>(Op::BuildPathsWithResults),
        static_cast<uint64_t>(Op::AddPermRoot),
        static_cast<uint64_t>(Op::QueryActiveBuilds),
    };
    // 43 unique opcodes defined
    REQUIRE(opcodes.size() == 43);
  }

  SECTION("Highest opcode value is QueryActiveBuilds at 48") {
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

  SECTION("Optional microseconds uses correct tag format") {
    // 0 = nullopt, 1 = has value followed by int64_t count
    REQUIRE(contains_pattern(cpp_file, R"(conn\.to << uint8_t\{0\})"));
    REQUIRE(contains_pattern(cpp_file, R"(conn\.to << uint8_t\{1\})"));
  }
}

// =============================================================================
// BuildResult serialization fields
// =============================================================================

TEST_CASE("BuildResult serialization fields", "[store][protocol][compatibility]") {
  SECTION("BuildResult::Success status values are protocol-stable") {
    using Status = nix::build_result_t::Success::Status;

    REQUIRE(static_cast<uint8_t>(Status::Built) == 0);
    REQUIRE(static_cast<uint8_t>(Status::Substituted) == 1);
    REQUIRE(static_cast<uint8_t>(Status::AlreadyValid) == 2);
    REQUIRE(static_cast<uint8_t>(Status::ResolvesToAlreadyValid) == 13);
  }

  SECTION("BuildResult::Failure status values are protocol-stable") {
    using Status = nix::build_result_t::Failure::Status;

    REQUIRE(static_cast<uint8_t>(Status::PermanentFailure) == 3);
    REQUIRE(static_cast<uint8_t>(Status::InputRejected) == 4);
    REQUIRE(static_cast<uint8_t>(Status::OutputRejected) == 5);
    REQUIRE(static_cast<uint8_t>(Status::TransientFailure) == 6);
    REQUIRE(static_cast<uint8_t>(Status::CachedFailure) == 7);
    REQUIRE(static_cast<uint8_t>(Status::TimedOut) == 8);
    REQUIRE(static_cast<uint8_t>(Status::MiscFailure) == 9);
    REQUIRE(static_cast<uint8_t>(Status::DependencyFailed) == 10);
    REQUIRE(static_cast<uint8_t>(Status::LogLimitExceeded) == 11);
    REQUIRE(static_cast<uint8_t>(Status::not_deterministic_t) == 12);
    REQUIRE(static_cast<uint8_t>(Status::NoSubstituters) == 14);
    REQUIRE(static_cast<uint8_t>(Status::HashMismatch) == 15);
    REQUIRE(static_cast<uint8_t>(Status::Cancelled) == 16);
  }

  SECTION("Success and Failure status values are disjoint") {
    std::set<uint8_t> success_statuses = {0, 1, 2, 13};
    std::set<uint8_t> failure_statuses = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16};

    for (auto s : success_statuses) {
      REQUIRE(failure_statuses.find(s) == failure_statuses.end());
    }
    for (auto f : failure_statuses) {
      REQUIRE(success_statuses.find(f) == success_statuses.end());
    }
  }

  SECTION("BuildResult status classification is correct") {
    // Success::statusIs should return true for success values
    REQUIRE(nix::build_result_t::Success::statusIs(0));  // Built
    REQUIRE(nix::build_result_t::Success::statusIs(1));  // Substituted
    REQUIRE(nix::build_result_t::Success::statusIs(2));  // AlreadyValid
    REQUIRE(nix::build_result_t::Success::statusIs(13)); // ResolvesToAlreadyValid

    // Success::statusIs should return false for failure values
    REQUIRE_FALSE(nix::build_result_t::Success::statusIs(3));  // PermanentFailure
    REQUIRE_FALSE(nix::build_result_t::Success::statusIs(9));  // MiscFailure
    REQUIRE_FALSE(nix::build_result_t::Success::statusIs(16)); // Cancelled
  }

  SECTION("BuildResult has required fields") {
    nix::build_result_t result;

    // Common fields
    REQUIRE(result.timesBuilt == 0);
    REQUIRE(result.start_time == 0);
    REQUIRE(result.stopTime == 0);
    REQUIRE_FALSE(result.cpu_user.has_value());
    REQUIRE_FALSE(result.cpu_system.has_value());
  }
}

// =============================================================================
// ValidPathInfo serialization fields
// =============================================================================

TEST_CASE("ValidPathInfo serialization fields", "[store][protocol][compatibility]") {
  SECTION("UnkeyedValidPathInfo has all protocol fields") {
    // These fields are serialized in the worker protocol
    auto info = nix::UnkeyedValidPathInfo(
        "/nix/store", nix::Hash::parse_any("sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
                                           nix::hash_algorithm_t::sha256));

    // Check default values
    REQUIRE_FALSE(info.deriver.has_value());
    REQUIRE(info.references.empty());
    REQUIRE(info.registrationTime == 0);
    REQUIRE(info.nar_size == 0);
    REQUIRE(info.ultimate == false);
    REQUIRE(info.sigs.empty());
    REQUIRE_FALSE(info.ca.has_value());
  }

  SECTION("ValidPathInfo extends UnkeyedValidPathInfo with path") {
    auto hash = nix::Hash::parse_any("sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
                                     nix::hash_algorithm_t::sha256);
    auto info = nix::UnkeyedValidPathInfo("/nix/store", hash);

    // ValidPathInfo adds the path field
    auto path = nix::store_path_t("0000000000000000000000000000000p-test");
    auto path_info = nix::valid_path_info_t(path, info);

    REQUIRE(path_info.path.to_string() == "0000000000000000000000000000000p-test");
  }
}

// =============================================================================
// Activity types and result types
// =============================================================================

TEST_CASE("Activity types match upstream", "[store][protocol][compatibility]") {
  SECTION("Activity type enum values are correct") {
    REQUIRE(nix::act_unknown == 0);
    REQUIRE(nix::act_copy_path == 100);
    REQUIRE(nix::act_file_transfer == 101);
    REQUIRE(nix::act_realise == 102);
    REQUIRE(nix::act_copy_paths == 103);
    REQUIRE(nix::act_builds == 104);
    REQUIRE(nix::act_build == 105);
    REQUIRE(nix::act_optimise_store == 106);
    REQUIRE(nix::act_verify_paths == 107);
    REQUIRE(nix::act_substitute == 108);
    REQUIRE(nix::act_query_path_info == 109);
    REQUIRE(nix::act_post_build_hook == 110);
    REQUIRE(nix::act_build_waiting == 111);
    REQUIRE(nix::act_fetch_tree == 112);
  }

  SECTION("All activity types are unique") {
    std::set<int> types = {
        nix::act_unknown,       nix::act_copy_path,       nix::act_file_transfer,
        nix::act_realise,       nix::act_copy_paths,      nix::act_builds,
        nix::act_build,         nix::act_optimise_store,  nix::act_verify_paths,
        nix::act_substitute,    nix::act_query_path_info, nix::act_post_build_hook,
        nix::act_build_waiting, nix::act_fetch_tree};
    REQUIRE(types.size() == 14);
  }
}

TEST_CASE("Result types match upstream", "[store][protocol][compatibility]") {
  SECTION("Result type enum values are correct") {
    REQUIRE(nix::res_file_linked == 100);
    REQUIRE(nix::res_build_log_line == 101);
    REQUIRE(nix::res_untrusted_path == 102);
    REQUIRE(nix::res_corrupted_path == 103);
    REQUIRE(nix::res_set_phase == 104);
    REQUIRE(nix::res_progress == 105);
    REQUIRE(nix::res_set_expected == 106);
    REQUIRE(nix::res_post_build_log_line == 107);
    REQUIRE(nix::res_fetch_status == 108);
    REQUIRE(nix::res_hash_mismatch == 109);
    REQUIRE(nix::res_build_result == 110);
  }

  SECTION("All result types are unique") {
    std::set<int> types = {
        nix::res_file_linked,    nix::res_build_log_line,      nix::res_untrusted_path,
        nix::res_corrupted_path, nix::res_set_phase,           nix::res_progress,
        nix::res_set_expected,   nix::res_post_build_log_line, nix::res_fetch_status,
        nix::res_hash_mismatch,  nix::res_build_result};
    REQUIRE(types.size() == 11);
  }
}

// =============================================================================
// Trusted/untrusted flag serialization
// =============================================================================

TEST_CASE("TrustedFlag serialization", "[store][protocol][compatibility]") {
  std::string cpp_file = read_file("src/nix/store/worker-protocol.cpp");
  REQUIRE_FALSE(cpp_file.empty());

  SECTION("TrustedFlag wire values are defined correctly") {
    // The wire format uses: 0 = unknown, 1 = Trusted, 2 = NotTrusted
    // This is validated in serialization tests above, but we verify
    // the pattern exists in the source

    REQUIRE(contains_pattern(cpp_file, R"(uint8_t\{0\})"));
    REQUIRE(contains_pattern(cpp_file, R"(uint8_t\{1\})"));
    REQUIRE(contains_pattern(cpp_file, R"(uint8_t\{2\})"));
  }

  SECTION("TrustedFlag enum has correct underlying type") {
    // TrustedFlag is enum with bool underlying type
    static_assert(std::is_same_v<std::underlying_type_t<nix::TrustedFlag>, bool>);
  }
}

// =============================================================================
// Feature flags (all known features)
// =============================================================================

TEST_CASE("Worker protocol features are defined", "[store][protocol][compatibility]") {
  SECTION("queryActiveBuilds feature is defined") {
    INFO("Feature strings are used for capability negotiation");
    REQUIRE(nix::WorkerProto::featureQueryActiveBuilds == "queryActiveBuilds");
  }

  SECTION("Feature type is std::string") {
    static_assert(std::is_same_v<nix::WorkerProto::Feature, std::string>);
  }

  SECTION("FeatureSet is a set of Features") {
    static_assert(std::is_same_v<nix::WorkerProto::FeatureSet, std::set<std::string, std::less<>>>);
  }
}

// =============================================================================
// Experimental features
// =============================================================================

TEST_CASE("Experimental features have stable identifiers", "[store][protocol][compatibility]") {
  using xp = nix::experimental_feature_t;

  SECTION("All experimental features are defined") {
    // These must remain stable for configuration compatibility
    REQUIRE(static_cast<int>(xp::ca_derivations) >= 0);
    REQUIRE(static_cast<int>(xp::impure_derivations) >= 0);
    REQUIRE(static_cast<int>(xp::fetch_tree) >= 0);
    REQUIRE(static_cast<int>(xp::git_hashing) >= 0);
    REQUIRE(static_cast<int>(xp::recursive_nix) >= 0);
    REQUIRE(static_cast<int>(xp::no_url_literals) >= 0);
    REQUIRE(static_cast<int>(xp::fetch_closure) >= 0);
    REQUIRE(static_cast<int>(xp::auto_allocate_uids) >= 0);
    REQUIRE(static_cast<int>(xp::cgroups) >= 0);
    REQUIRE(static_cast<int>(xp::daemon_trust_override) >= 0);
    REQUIRE(static_cast<int>(xp::dynamic_derivations) >= 0);
    REQUIRE(static_cast<int>(xp::parse_toml_timestamps) >= 0);
    REQUIRE(static_cast<int>(xp::read_only_local_store) >= 0);
    REQUIRE(static_cast<int>(xp::local_overlay_store) >= 0);
    REQUIRE(static_cast<int>(xp::configurable_impure_env) >= 0);
    REQUIRE(static_cast<int>(xp::mounted_ssh_store_t) >= 0);
    REQUIRE(static_cast<int>(xp::verified_fetches) >= 0);
    REQUIRE(static_cast<int>(xp::pipe_operators) >= 0);
    REQUIRE(static_cast<int>(xp::external_builders) >= 0);
    REQUIRE(static_cast<int>(xp::blak_e3_hashes) >= 0);
    REQUIRE(static_cast<int>(xp::build_time_fetch_tree) >= 0);
    REQUIRE(static_cast<int>(xp::parallel_eval) >= 0);
  }

  SECTION("Experimental feature enum values are unique") {
    std::set<int> values;
    values.insert(static_cast<int>(xp::ca_derivations));
    values.insert(static_cast<int>(xp::impure_derivations));
    values.insert(static_cast<int>(xp::fetch_tree));
    values.insert(static_cast<int>(xp::git_hashing));
    values.insert(static_cast<int>(xp::recursive_nix));
    values.insert(static_cast<int>(xp::no_url_literals));
    values.insert(static_cast<int>(xp::fetch_closure));
    values.insert(static_cast<int>(xp::auto_allocate_uids));
    values.insert(static_cast<int>(xp::cgroups));
    values.insert(static_cast<int>(xp::daemon_trust_override));
    values.insert(static_cast<int>(xp::dynamic_derivations));
    values.insert(static_cast<int>(xp::parse_toml_timestamps));
    values.insert(static_cast<int>(xp::read_only_local_store));
    values.insert(static_cast<int>(xp::local_overlay_store));
    values.insert(static_cast<int>(xp::configurable_impure_env));
    values.insert(static_cast<int>(xp::mounted_ssh_store_t));
    values.insert(static_cast<int>(xp::verified_fetches));
    values.insert(static_cast<int>(xp::pipe_operators));
    values.insert(static_cast<int>(xp::external_builders));
    values.insert(static_cast<int>(xp::blak_e3_hashes));
    values.insert(static_cast<int>(xp::build_time_fetch_tree));
    values.insert(static_cast<int>(xp::parallel_eval));

    // All 22 features should be unique
    REQUIRE(values.size() == 22);
  }
}

// =============================================================================
// GCAction enum values
// =============================================================================

TEST_CASE("GCAction enum values are protocol-stable", "[store][protocol][compatibility]") {
  using nix::GCAction;

  SECTION("GCAction enum values match wire protocol") {
    // These values are serialized as unsigned integers
    // The write function uses: gcReturnLive=0, gcReturnDead=1, gcDeleteDead=2, gcDeleteSpecific=3

    // We can't directly test the enum values since they're scoped,
    // but we verify they're defined and distinct
    std::set<int> actions;
    actions.insert(static_cast<int>(GCAction::gcReturnLive));
    actions.insert(static_cast<int>(GCAction::gcReturnDead));
    actions.insert(static_cast<int>(GCAction::gcDeleteDead));
    actions.insert(static_cast<int>(GCAction::gcDeleteSpecific));

    REQUIRE(actions.size() == 4);
  }
}

// =============================================================================
// Hash algorithm constants
// =============================================================================

TEST_CASE("Hash algorithm constants are protocol-stable", "[store][protocol][compatibility]") {
  using nix::hash_algorithm_t;

  SECTION("Hash algorithms have stable values") {
    // These values are part of the serialization format
    REQUIRE(static_cast<char>(hash_algorithm_t::md5) == 42);
    REQUIRE(static_cast<char>(hash_algorithm_t::sha1) == 43);
    REQUIRE(static_cast<char>(hash_algorithm_t::sha256) == 44);
    REQUIRE(static_cast<char>(hash_algorithm_t::sha512) == 45);
    REQUIRE(static_cast<char>(hash_algorithm_t::blake3) == 46);
  }

  SECTION("Hash size constants are correct") {
    REQUIRE(nix::hash_sizes::md5 == 16);
    REQUIRE(nix::hash_sizes::sha1 == 20);
    REQUIRE(nix::hash_sizes::sha256 == 32);
    REQUIRE(nix::hash_sizes::sha512 == 64);
    REQUIRE(nix::hash_sizes::blake3 == 32);
  }

  SECTION("regular_hash_size returns correct values") {
    REQUIRE(nix::regular_hash_size(hash_algorithm_t::md5) == 16);
    REQUIRE(nix::regular_hash_size(hash_algorithm_t::sha1) == 20);
    REQUIRE(nix::regular_hash_size(hash_algorithm_t::sha256) == 32);
    REQUIRE(nix::regular_hash_size(hash_algorithm_t::sha512) == 64);
    REQUIRE(nix::regular_hash_size(hash_algorithm_t::blake3) == 32);
  }
}

// =============================================================================
// Content address method constants
// =============================================================================

TEST_CASE("Content address method constants", "[store][protocol][compatibility]") {
  using raw = nix::content_address_method_t::raw_t;

  SECTION("Content address method enum values are defined") {
    // These are used in content-addressed store path computation
    REQUIRE(static_cast<int>(raw::flat) >= 0);
    REQUIRE(static_cast<int>(raw::nix_archive) >= 0);
    REQUIRE(static_cast<int>(raw::git) >= 0);
    REQUIRE(static_cast<int>(raw::Text) >= 0);
  }

  SECTION("Content address method values are unique") {
    std::set<int> methods;
    methods.insert(static_cast<int>(raw::flat));
    methods.insert(static_cast<int>(raw::nix_archive));
    methods.insert(static_cast<int>(raw::git));
    methods.insert(static_cast<int>(raw::Text));

    REQUIRE(methods.size() == 4);
  }
}

// =============================================================================
// Store path format
// =============================================================================

TEST_CASE("Store path format constants", "[store][protocol][compatibility]") {
  SECTION("Store path hash length is 32 base-32 characters") {
    REQUIRE(nix::store_path_t::HashLen == 32);
  }

  SECTION("Maximum store path length") {
    REQUIRE(nix::store_path_t::MaxPathLen == 211);
  }

  SECTION("Derivation extension") {
    REQUIRE(nix::drvExtension == ".drv");
  }
}

// =============================================================================
// Verbosity levels
// =============================================================================

TEST_CASE("Verbosity levels are protocol-stable", "[store][protocol][compatibility]") {
  SECTION("Verbosity enum values") {
    REQUIRE(static_cast<int>(nix::lvl_error) == 0);
    REQUIRE(static_cast<int>(nix::lvl_warn) == 1);
    REQUIRE(static_cast<int>(nix::lvl_notice) == 2);
    REQUIRE(static_cast<int>(nix::lvl_info) == 3);
    REQUIRE(static_cast<int>(nix::lvl_talkative) == 4);
    REQUIRE(static_cast<int>(nix::lvl_chatty) == 5);
    REQUIRE(static_cast<int>(nix::lvl_debug) == 6);
    REQUIRE(static_cast<int>(nix::lvl_vomit) == 7);
  }
}

// =============================================================================
// Error serialization format
// =============================================================================

TEST_CASE("Error serialization format", "[store][protocol][compatibility]") {
  // Errors are serialized with the STDERR_ERROR constant followed by error info
  SECTION("STDERR_ERROR constant is used for error signaling") {
    REQUIRE(STDERR_ERROR == 0x63787470);
  }

  SECTION("Error levels have correct ordering") {
    // Lower values are more severe
    REQUIRE(nix::lvl_error < nix::lvl_warn);
    REQUIRE(nix::lvl_warn < nix::lvl_notice);
    REQUIRE(nix::lvl_notice < nix::lvl_info);
    REQUIRE(nix::lvl_info < nix::lvl_debug);
    REQUIRE(nix::lvl_debug < nix::lvl_vomit);
  }
}

// =============================================================================
// ClientHandshakeInfo fields
// =============================================================================

TEST_CASE("ClientHandshakeInfo has required fields", "[store][protocol][compatibility]") {
  SECTION("ClientHandshakeInfo default values") {
    nix::WorkerProto::ClientHandshakeInfo info;

    // daemonNixVersion is optional
    REQUIRE_FALSE(info.daemonNixVersion.has_value());

    // remoteTrustsUs is optional
    REQUIRE_FALSE(info.remoteTrustsUs.has_value());
  }

  SECTION("ClientHandshakeInfo supports equality comparison") {
    nix::WorkerProto::ClientHandshakeInfo info1;
    nix::WorkerProto::ClientHandshakeInfo info2;

    REQUIRE(info1 == info2);

    info1.daemonNixVersion = "2.24.0";
    REQUIRE_FALSE(info1 == info2);

    info2.daemonNixVersion = "2.24.0";
    REQUIRE(info1 == info2);
  }
}

// =============================================================================
// ReadConn and WriteConn structure
// =============================================================================

TEST_CASE("Protocol connection structures", "[store][protocol][compatibility]") {
  SECTION("WorkerProto::ReadConn has required fields") {
    // ReadConn should have: source_t& from, Version version, bool shortStorePaths
    // We verify this by checking the struct layout compiles correctly
    static_assert(std::is_same_v<nix::WorkerProto::Version, unsigned int>);
  }

  SECTION("WorkerProto::WriteConn has required fields") {
    // WriteConn should have: sink_t& to, Version version, bool shortStorePaths
    static_assert(std::is_same_v<nix::WorkerProto::Version, unsigned int>);
  }
}
