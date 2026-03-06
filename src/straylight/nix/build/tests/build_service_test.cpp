// straylight::nix::build::build_service tests
//
// Unit tests for the build service abstraction and factory functions.
// These tests verify that the build service factory correctly creates
// services and that the interface is properly defined.

#include <catch2/catch_test_macros.hpp>

#include "straylight/nix/build/build_service.h"

using namespace straylight::nix::build;

// =============================================================================
// Factory Function Tests
// =============================================================================

TEST_CASE("make_default_build_service returns non-null", "[build][build_service]") {
  INFO("The default build service should always be available");
  INFO("It returns firecracker for embedded builds");

  auto service = make_default_build_service();
  REQUIRE(service != nullptr);
}

TEST_CASE("make_firecracker_build_service returns non-null", "[build][build_service]") {
  INFO("Firecracker build service should be creatable");
  INFO("It may not be 'available' without KVM, but should be constructable");

  auto service = make_firecracker_build_service();
  REQUIRE(service != nullptr);
}

TEST_CASE("make_daemon_build_service returns non-null", "[build][build_service]") {
  INFO("Daemon build service should be creatable");
  INFO("It may not be 'available' without a daemon socket, but should be constructable");

  auto service = make_daemon_build_service();
  REQUIRE(service != nullptr);
}

TEST_CASE("make_reapi_build_service returns nullptr (not yet implemented)",
          "[build][build_service]") {
  INFO("REAPI build service is not yet implemented");
  INFO("This test documents expected behavior for future implementation");

  auto service = make_reapi_build_service("localhost:8980", "default");

  // Currently returns nullptr - will change when implemented
  CHECK(service == nullptr);
}

// =============================================================================
// Build Service Interface Tests
// =============================================================================

TEST_CASE("build_service interface has required methods", "[build][build_service]") {
  INFO("The build_service interface defines the contract for all backends");

  auto service = make_default_build_service();
  REQUIRE(service != nullptr);

  SECTION("is_available returns bool") {
    // Just verify it doesn't crash and returns a value
    bool available = service->is_available();
    INFO("is_available() returned: " << (available ? "true" : "false"));
    // Don't check the value - depends on environment (KVM, daemon, etc.)
  }
}

// =============================================================================
// Firecracker Build Service Availability Tests
// =============================================================================

TEST_CASE("firecracker_build_service availability checks", "[build][firecracker]") {
  auto service = make_firecracker_build_service();
  REQUIRE(service != nullptr);

  SECTION("service reports availability correctly") {
    bool available = service->is_available();

    // The service requires:
    // 1. /dev/kvm access
    // 2. Embedded guest data OR external kernel/initrd files
    //
    // In CI without KVM, this will return false - that's expected
    INFO("Firecracker service available: " << (available ? "true" : "false"));

    // If not available, builds should fail gracefully rather than crash
    // (tested elsewhere)
  }
}

// =============================================================================
// Daemon Build Service Availability Tests
// =============================================================================

TEST_CASE("daemon_build_service availability checks", "[build][daemon]") {
  auto service = make_daemon_build_service();
  REQUIRE(service != nullptr);

  SECTION("service reports availability based on socket") {
    bool available = service->is_available();

    // The service requires a running nix-daemon with socket at
    // /nix/var/nix/daemon-socket/socket (or custom path)
    INFO("Daemon service available: " << (available ? "true" : "false"));

    // Availability depends on whether daemon is running
    // We don't assert on the value
  }
}
