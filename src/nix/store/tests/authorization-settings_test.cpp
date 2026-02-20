// straylight // nix // store // tests
//
// Unit tests for authorization settings (trusted-users, allowed-users)

#include <catch2/catch_test_macros.hpp>

#include "nix/store/authorization-settings.h"

using namespace nix;

// =============================================================================
// authorization_settings_t registration tests
// =============================================================================

TEST_CASE("authorization_settings exists and is registered", "[store][authorization]") {
  // The settings should exist and have default values
  REQUIRE(authorization_settings.trusted_users.get().size() >= 1);
  REQUIRE(authorization_settings.allowed_users.get().size() >= 1);
}

TEST_CASE("trusted_users default includes root", "[store][authorization]") {
  const auto& trusted = authorization_settings.trusted_users.get();
  bool has_root = false;
  for (const auto& user : trusted) {
    if (user == "root") {
      has_root = true;
      break;
    }
  }
  REQUIRE(has_root);
}

TEST_CASE("allowed_users default includes wildcard", "[store][authorization]") {
  const auto& allowed = authorization_settings.allowed_users.get();
  bool has_wildcard = false;
  for (const auto& user : allowed) {
    if (user == "*") {
      has_wildcard = true;
      break;
    }
  }
  REQUIRE(has_wildcard);
}

// =============================================================================
// match_user tests (Unix only)
// =============================================================================

#ifndef _WIN32

TEST_CASE("match_user with wildcard matches any user", "[store][authorization]") {
  strings_t users = {"*"};
  REQUIRE(match_user("alice", "users", users));
  REQUIRE(match_user("bob", "wheel", users));
  REQUIRE(match_user(std::nullopt, std::nullopt, users));
}

TEST_CASE("match_user with exact username match", "[store][authorization]") {
  strings_t users = {"alice", "bob"};
  REQUIRE(match_user("alice", "users", users));
  REQUIRE(match_user("bob", "users", users));
  REQUIRE_FALSE(match_user("charlie", "users", users));
}

TEST_CASE("match_user with no match returns false", "[store][authorization]") {
  strings_t users = {"alice"};
  REQUIRE_FALSE(match_user("bob", "users", users));
}

TEST_CASE("match_user with empty list returns false", "[store][authorization]") {
  strings_t users = {};
  REQUIRE_FALSE(match_user("alice", "users", users));
}

TEST_CASE("match_user with nullopt user and no wildcard returns false", "[store][authorization]") {
  strings_t users = {"alice", "bob"};
  REQUIRE_FALSE(match_user(std::nullopt, "users", users));
}

TEST_CASE("match_user with group prefix matches primary group", "[store][authorization]") {
  strings_t users = {"@wheel"};
  REQUIRE(match_user("alice", "wheel", users));
  REQUIRE_FALSE(match_user("alice", "users", users));
}

TEST_CASE("match_user with group prefix and nullopt group returns false",
          "[store][authorization]") {
  strings_t users = {"@wheel"};
  // Even if the user might be in wheel via supplementary groups,
  // if we can't look them up, we should handle gracefully
  // This tests that nullopt group doesn't cause a crash
  REQUIRE_FALSE(match_user("alice", std::nullopt, users));
}

TEST_CASE("match_user with mixed users and groups", "[store][authorization]") {
  strings_t users = {"root", "@wheel", "nix-builder"};

  // Direct user match
  REQUIRE(match_user("root", "users", users));
  REQUIRE(match_user("nix-builder", "users", users));

  // Group match
  REQUIRE(match_user("someuser", "wheel", users));

  // No match
  REQUIRE_FALSE(match_user("random", "users", users));
}

#endif // _WIN32
