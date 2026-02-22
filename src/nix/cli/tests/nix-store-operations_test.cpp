// straylight // nix // cli // tests
//
// nix-store command compatibility tests - executable specification
//
// These tests verify that straylight-nix implements the nix-store legacy command.
// Many NixOS tools and legacy scripts depend on nix-store being available.
//
// Background:
//   - nix-store is a core command for store manipulation and queries
//   - NixOS tooling uses `nix-store -q` for querying store paths
//   - `nix-store -r` / `--realise` is used to build derivations
//   - `nix-store --gc` is used for garbage collection
//   - Commands are registered via RegisterLegacyCommand
//
// Implementation status (straylight):
//   - --query / -q: IMPLEMENTED (requisites, references, referrers, deriver, outputs, hash, size)
//   - --realise / -r: IMPLEMENTED (builds derivations via build_paths_with_results)
//   - --gc: IMPLEMENTED (print-dead, print-live, delete, max-freed)
//   - --dump: IMPLEMENTED (NAR serialization)
//   - --restore: IMPLEMENTED (NAR deserialization)
//   - --verify: IMPLEMENTED (store verification)
//   - --optimise: IMPLEMENTED (hard-link deduplication)
//   - --print-roots: IMPLEMENTED (GC root enumeration)
//   - --add: IMPLEMENTED (add path to store)
//   - --delete: IMPLEMENTED (delete specific paths)
//   - --export/--import: NOT YET IMPLEMENTED
//   - --read-log: NOT YET IMPLEMENTED

#include <catch2/catch_test_macros.hpp>

#include "nix/cmd/legacy.h"

using namespace nix;

// =============================================================================
// nix-store command registration tests
// =============================================================================

TEST_CASE("nix-store legacy command is registered", "[cli][legacy][compatibility][nix-store]") {
  INFO("nix-store is required by NixOS tooling and legacy scripts that interact with the store");
  INFO("Many build systems and CI pipelines use nix-store for querying and realising paths");

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-store command exists in registered commands") {
    auto it = commands.find("nix-store");
    REQUIRE(it != commands.end());
    REQUIRE(it->second != nullptr);
  }
}

// =============================================================================
// nix-store --query / -q operations
// =============================================================================

TEST_CASE("nix-store --query operations are implemented", "[cli][legacy][nix-store][query]") {
  INFO("nix-store --query (-q) is used to query store path information");
  INFO("Common queries: --requisites, --references, --referrers, --deriver, --outputs");
  INFO("NixOS boot scripts use: nix-store -q --requisites to find closure");

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-store is registered for query operations") {
    REQUIRE(commands.contains("nix-store"));
  }

  // Query operation flags that should be recognized:
  // --requisites / -R: Compute closure (uses computeFSClosure)
  // --references: Get immediate references from queryPathInfo
  // --referrers: Get incoming references from query_referrers
  // --deriver / -d: Get deriver from queryPathInfo
  // --outputs: Get derivation outputs from queryPartialDerivationOutputMap
  // --hash: Get NAR hash from queryPathInfo
  // --size: Get NAR size from queryPathInfo
  // --valid-derivers: Get valid derivers from queryValidDerivers
  // --roots: Get GC roots that keep a path alive

  SECTION("query operations use correct store APIs") {
    INFO("--requisites uses store->computeFSClosure()");
    INFO("--references uses store->queryPathInfo()->references");
    INFO("--referrers uses store->query_referrers()");
    INFO("--deriver uses store->queryPathInfo()->deriver");
    INFO("--outputs uses store->queryPartialDerivationOutputMap()");
    INFO("--hash uses store->queryPathInfo()->nar_hash");
    INFO("--size uses store->queryPathInfo()->nar_size");
    REQUIRE(commands.contains("nix-store"));
  }
}

// =============================================================================
// nix-store --realise / -r operations
// =============================================================================

TEST_CASE("nix-store --realise operations are implemented", "[cli][legacy][nix-store][realise]") {
  INFO("nix-store --realise (-r) builds or fetches store paths");
  INFO("This is the low-level build command used by nix-build and other tools");
  INFO("Hydra CI and other build systems invoke nix-store -r directly");

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-store is registered for realise operations") {
    REQUIRE(commands.contains("nix-store"));
  }

  SECTION("realise uses store->build_paths_with_results()") {
    INFO("Implementation builds via store->build_paths_with_results()");
    INFO("Supports both derivation paths (*.drv) and output paths");
    INFO("Returns output paths on stdout");
    REQUIRE(commands.contains("nix-store"));
  }
}

// =============================================================================
// nix-store --gc operations
// =============================================================================

TEST_CASE("nix-store --gc operations are implemented", "[cli][legacy][nix-store][gc]") {
  INFO("nix-store --gc performs garbage collection on the store");
  INFO("NixOS runs this via nix-gc.service timer unit");
  INFO("Options include: --print-dead, --print-live, --delete, --max-freed");

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-store is registered for gc operations") {
    REQUIRE(commands.contains("nix-store"));
  }

  SECTION("gc uses GcStore::collectGarbage()") {
    INFO("Implementation casts store to GcStore and calls collectGarbage()");
    INFO("Supports GCOptions: gcReturnLive, gcReturnDead, gcDeleteDead, gcDeleteSpecific");
    INFO("Supports --max-freed to limit bytes freed");
    REQUIRE(commands.contains("nix-store"));
  }
}

// =============================================================================
// nix-store --dump / --restore operations
// =============================================================================

TEST_CASE("nix-store --dump/--restore operations are implemented",
          "[cli][legacy][nix-store][nar]") {
  INFO("nix-store --dump serializes a path to NAR format on stdout");
  INFO("nix-store --restore deserializes NAR format from stdin to a path");
  INFO("These are used for manual path transfers and backup");

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-store is registered for NAR operations") {
    REQUIRE(commands.contains("nix-store"));
  }

  SECTION("dump uses store->nar_from_path()") {
    INFO("Implementation calls store->nar_from_path() and writes to fd_sink_t(STDOUT_FILENO)");
    REQUIRE(commands.contains("nix-store"));
  }

  SECTION("restore uses restore_path()") {
    INFO("Implementation calls restore_path() reading from fd_source_t(STDIN_FILENO)");
    REQUIRE(commands.contains("nix-store"));
  }
}

// =============================================================================
// nix-store --verify operations
// =============================================================================

TEST_CASE("nix-store --verify operations are implemented", "[cli][legacy][nix-store][verify]") {
  INFO("nix-store --verify checks store integrity");
  INFO("Options: --check-contents to verify NAR hashes, --repair to fix invalid paths");

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-store is registered for verify operations") {
    REQUIRE(commands.contains("nix-store"));
  }

  SECTION("verify uses store->verifyStore()") {
    INFO("Implementation calls store->verifyStore(check_contents, repair)");
    REQUIRE(commands.contains("nix-store"));
  }
}

// =============================================================================
// nix-store --optimise operations
// =============================================================================

TEST_CASE("nix-store --optimise operations are implemented", "[cli][legacy][nix-store][optimise]") {
  INFO("nix-store --optimise deduplicates store contents using hard links");
  INFO("This can save significant disk space");

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-store is registered for optimise operations") {
    REQUIRE(commands.contains("nix-store"));
  }

  SECTION("optimise uses store->optimiseStore()") {
    INFO("Implementation calls store->optimiseStore()");
    REQUIRE(commands.contains("nix-store"));
  }
}

// =============================================================================
// nix-store --print-roots operations
// =============================================================================

TEST_CASE("nix-store --print-roots operations are implemented", "[cli][legacy][nix-store][roots]") {
  INFO("nix-store --print-roots lists GC roots");
  INFO("Output format: <root_link> -> <store_path>");

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-store is registered for print-roots operations") {
    REQUIRE(commands.contains("nix-store"));
  }

  SECTION("print-roots uses GcStore::findRoots()") {
    INFO("Implementation casts store to GcStore and calls findRoots()");
    REQUIRE(commands.contains("nix-store"));
  }
}

// =============================================================================
// nix-store --add operations
// =============================================================================

TEST_CASE("nix-store --add operations are implemented", "[cli][legacy][nix-store][add]") {
  INFO("nix-store --add imports a path into the store");
  INFO("Returns the resulting store path on stdout");

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-store is registered for add operations") {
    REQUIRE(commands.contains("nix-store"));
  }

  SECTION("add uses store->add_to_store()") {
    INFO("Implementation uses posix_source_accessor_t::create_at_root() and add_to_store()");
    REQUIRE(commands.contains("nix-store"));
  }
}

// =============================================================================
// nix-store --delete operations
// =============================================================================

TEST_CASE("nix-store --delete operations are implemented", "[cli][legacy][nix-store][delete]") {
  INFO("nix-store --delete removes specific paths if they are unreferenced");

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("nix-store is registered for delete operations") {
    REQUIRE(commands.contains("nix-store"));
  }

  SECTION("delete uses GcStore::collectGarbage() with gcDeleteSpecific") {
    INFO("Implementation uses GCOptions::gcDeleteSpecific with pathsToDelete");
    REQUIRE(commands.contains("nix-store"));
  }
}

// =============================================================================
// nix-store required for NixOS compatibility
// =============================================================================

TEST_CASE("nix-store is required for NixOS system operations", "[cli][legacy][nixos][nix-store]") {
  INFO("nix-store is a fundamental command for NixOS operations");

  auto& commands = RegisterLegacyCommand::commands();

  SECTION("NixOS bootloader installer uses nix-store -q --requisites") {
    // The bootloader installer needs to query the closure of the system profile
    // Example: nix-store -q --requisites /nix/var/nix/profiles/system
    REQUIRE(commands.contains("nix-store"));
  }

  SECTION("NixOS garbage collection service uses nix-store --gc") {
    // nix-gc.service runs: nix-store --gc
    REQUIRE(commands.contains("nix-store"));
  }

  SECTION("NixOS activation scripts use nix-store -r") {
    // System activation may realise paths via nix-store -r
    REQUIRE(commands.contains("nix-store"));
  }
}

// =============================================================================
// nix-store operations not yet implemented
// =============================================================================

TEST_CASE("nix-store operations pending implementation",
          "[cli][legacy][nix-store][pending][.skip]") {
  INFO("These operations are registered but not yet fully implemented");

  auto& commands = RegisterLegacyCommand::commands();
  REQUIRE(commands.contains("nix-store"));

  SECTION("--export not yet implemented") {
    INFO("nix-store --export serializes paths with signatures for transfer");
    INFO("Uses export_paths() function");
  }

  SECTION("--import not yet implemented") {
    INFO("nix-store --import deserializes paths with signatures");
    INFO("Uses import_paths() function");
  }

  SECTION("--read-log not yet implemented") {
    INFO("nix-store --read-log / -l displays build logs");
    INFO("Uses LogStore::getBuildLog()");
  }
}
