// straylight // nix // cli // tests
//
// nix-env operations tests - executable specification for required nix-env operations
//
// This file tests specific nix-env operations that NixOS and legacy scripts depend on.
// Tests for unimplemented features are expected to FAIL - this is test-first development.
//
// Background:
//   - NixOS bootloader installer uses `nix-env --list-generations`
//   - Legacy NixOS scripts use `nix-env -q` to query installed packages
//   - User environment management historically used `nix-env -i` and `nix-env -e`
//   - Many existing scripts and documentation reference these operations

#include <catch2/catch_test_macros.hpp>

#include "nix/cmd/legacy.h"


// =============================================================================
// Helper: Check if nix-env command handler accepts an operation
// =============================================================================

// Note: We can't easily invoke the command handler directly without complex setup,
// so we test for command registration and document the expected behavior.
// Integration tests should verify actual operation behavior.

// =============================================================================
// IMPLEMENTED: --list-generations
// =============================================================================

TEST_CASE("nix-env --list-generations is registered", "[cli][nix-env][implemented]") {
  auto& commands = nix::RegisterLegacyCommand::commands();

  SECTION("nix-env command exists for --list-generations support") {
    INFO("NixOS bootloader installer uses: nix-env --list-generations -p "
         "/nix/var/nix/profiles/system");
    INFO("This command is required for system-boot, grub, and other bootloader tools");
    INFO("See: nixos/modules/system/boot/loader/generations-dir/generations-dir-builder.sh");

    auto it = commands.find("nix-env");
    REQUIRE(it != commands.end());
    REQUIRE(it->second != nullptr);
  }
}

// =============================================================================
// NOT IMPLEMENTED: -q / --query
// These tests document what needs to be implemented and SHOULD FAIL
// =============================================================================

TEST_CASE("nix-env -q / --query operation", "[cli][nix-env][query][!shouldfail]") {
  auto& commands = nix::RegisterLegacyCommand::commands();

  SECTION("query operation is needed for legacy NixOS scripts") {
    INFO("Legacy NixOS scripts use `nix-env -q` to list installed packages");
    INFO("Example: nix-env -q --installed --out-path");
    INFO("This is used by tools that need to inspect the current user environment");
    INFO("Some deployment scripts check for specific packages being installed");

    // The command itself is registered...
    auto it = commands.find("nix-env");
    REQUIRE(it != commands.end());

    // ...but query operation is NOT implemented.
    // This test documents the requirement. When -q is implemented,
    // remove the [!shouldfail] tag and update this test to verify the operation.

    // For now, we verify the operation is NOT available by checking that
    // attempting to use query-related functionality would fail.
    // Since we can't easily invoke the handler, we document the expected failure.

    // TODO: When implementing, support at minimum:
    //   -q, --query           Query installed packages
    //   --installed           Show only installed packages (default for -q)
    //   --available           Show available packages
    //   --out-path            Show output paths
    //   --description         Show package descriptions

    FAIL("nix-env -q/--query is not implemented - this test documents the requirement");
  }
}

// =============================================================================
// NOT IMPLEMENTED: -i / --install
// These tests document what needs to be implemented and SHOULD FAIL
// =============================================================================

TEST_CASE("nix-env -i / --install operation", "[cli][nix-env][install][!shouldfail]") {
  auto& commands = nix::RegisterLegacyCommand::commands();

  SECTION("install operation is needed for legacy package management") {
    INFO("Traditional Nix package management uses `nix-env -i` to install packages");
    INFO("Example: nix-env -iA nixpkgs.hello");
    INFO("Many tutorials and legacy scripts depend on this command");
    INFO("Note: Modern alternative is `nix profile install`");

    // The command itself is registered...
    auto it = commands.find("nix-env");
    REQUIRE(it != commands.end());

    // ...but install operation is NOT implemented.
    // This test documents the requirement.

    // TODO: When implementing, support at minimum:
    //   -i, --install         Install packages
    //   -A, --attr            Select by attribute path
    //   -f, --file            Use expression from file
    //   --from-profile        Install from another profile

    FAIL("nix-env -i/--install is not implemented - this test documents the requirement");
  }

  SECTION("install operation is used by nix-shell -p fallback") {
    INFO("Some nix-shell implementations fall back to nix-env -i for package installation");
    INFO("This affects users who use nix-shell -p for ad-hoc environments");

    FAIL("nix-env -i/--install is not implemented - this test documents the requirement");
  }
}

// =============================================================================
// NOT IMPLEMENTED: -e / --uninstall
// These tests document what needs to be implemented and SHOULD FAIL
// =============================================================================

TEST_CASE("nix-env -e / --uninstall operation", "[cli][nix-env][uninstall][!shouldfail]") {
  auto& commands = nix::RegisterLegacyCommand::commands();

  SECTION("uninstall operation is needed for legacy package management") {
    INFO("Traditional Nix package management uses `nix-env -e` to remove packages");
    INFO("Example: nix-env -e hello");
    INFO("Many tutorials and legacy scripts depend on this command");
    INFO("Note: Modern alternative is `nix profile remove`");

    // The command itself is registered...
    auto it = commands.find("nix-env");
    REQUIRE(it != commands.end());

    // ...but uninstall operation is NOT implemented.
    // This test documents the requirement.

    // TODO: When implementing, support:
    //   -e, --uninstall       Remove packages from environment
    //   Package can be specified by name or derivation path

    FAIL("nix-env -e/--uninstall is not implemented - this test documents the requirement");
  }
}

// =============================================================================
// Summary: Required nix-env operations for full compatibility
// =============================================================================

TEST_CASE("nix-env operations compatibility summary", "[cli][nix-env][summary]") {
  SECTION("Currently implemented operations") {
    INFO("--list-generations (-L): Lists profile generations");
    INFO("  Used by: NixOS bootloader installer, system management scripts");
    INFO("  Status: IMPLEMENTED");

    // This should pass
    auto& commands = nix::RegisterLegacyCommand::commands();
    REQUIRE(commands.contains("nix-env"));
  }

  SECTION("Operations needed for full legacy compatibility") {
    INFO("--query (-q): Query installed packages");
    INFO("  Used by: Package inspection scripts, CI/CD pipelines");
    INFO("  Status: NOT IMPLEMENTED");
    INFO("");
    INFO("--install (-i): Install packages");
    INFO("  Used by: Legacy tutorials, automation scripts, nix-shell fallback");
    INFO("  Status: NOT IMPLEMENTED");
    INFO("");
    INFO("--uninstall (-e): Remove packages");
    INFO("  Used by: Legacy package management, cleanup scripts");
    INFO("  Status: NOT IMPLEMENTED");
    INFO("");
    INFO("Modern alternative: Use 'nix profile' commands instead");

    // This section is informational - no assertions needed
    // The individual operation tests above will fail until implemented
  }
}
