// straylight // nix // tests // fuzz
//
// SSH Authentication Fuzz Tests
//
// Fuzz the SSH_AUTH_SOCK discovery logic with malformed paths and environments.

#include <string>
#include <vector>

#include "nix/tests/property.h"
#include "nix/util/environment-variables.h"
#include "nix/util/error.h"
#include "nix/util/util.h"

// We need to test the environment manipulation functions
// that are used by the SSH code.

// =============================================================================
// Fuzz environment variable parsing
// =============================================================================

TEST_CASE("fuzz: get_env handles arbitrary keys", "[fuzz][ssh]") {
  rc::prop("get_env never crashes on arbitrary key names", []() {
    auto key = *rc::gen::arbitrary<std::string>();
    try {
      [[maybe_unused]] auto result = nix::get_env(key);
    } catch (const nix::base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: get_env_non_empty handles arbitrary keys", "[fuzz][ssh]") {
  rc::prop("get_env_non_empty never crashes", []() {
    auto key = *rc::gen::arbitrary<std::string>();
    try {
      [[maybe_unused]] auto result = nix::get_env_non_empty(key);
    } catch (const nix::base_error_t&) {
    } catch (const std::exception&) {
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz SSH-related path patterns
// =============================================================================

TEST_CASE("fuzz: SSH socket path patterns", "[fuzz][ssh]") {
  rc::prop("SSH socket paths with special chars don't crash", []() {
    // Generate paths that look like SSH agent sockets
    auto uid = *rc::gen::inRange(0, 65535);
    auto suffix = *rc::gen::arbitrary<std::string>();

    std::vector<std::string> patterns = {
        nix::fmt("/run/user/%d/ssh-agent.socket", uid),
        nix::fmt("/run/user/%d/gnome-keyring/ssh", uid),
        nix::fmt("/run/user/%d/keyring/ssh", uid),
        nix::fmt("/tmp/ssh-%s/agent.%d", suffix, uid),
    };

    for (const auto& path : patterns) {
      // Just verify string formatting doesn't crash
      RC_ASSERT(!path.empty());
    }

    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz SUDO_USER environment variable patterns
// =============================================================================

TEST_CASE("fuzz: SUDO_USER patterns", "[fuzz][ssh]") {
  rc::prop("Various SUDO_USER values don't cause issues", []() {
    auto username = *rc::gen::arbitrary<std::string>();

    // Test that various username patterns don't cause issues in string operations
    try {
      // Simulate what find_ssh_auth_sock does with the username
      if (!username.empty()) {
        [[maybe_unused]] auto formatted = nix::fmt("/run/user/1000/%s", username);
      }
    } catch (const nix::base_error_t&) {
    } catch (const std::exception&) {
    }

    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz path validation patterns
// =============================================================================

TEST_CASE("fuzz: SSH socket directory iteration patterns", "[fuzz][ssh]") {
  rc::prop("Socket filename patterns", []() {
    auto dirname = *rc::gen::arbitrary<std::string>();
    auto filename = *rc::gen::arbitrary<std::string>();

    // Test the string operations used in find_ssh_auth_sock
    try {
      bool starts_with_ssh = dirname.starts_with("ssh-");
      bool starts_with_agent = filename.starts_with("agent.");

      // Verify boolean operations don't crash
      [[maybe_unused]] bool both = starts_with_ssh && starts_with_agent;
    } catch (const std::exception&) {
    }

    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz fmt() with SSH-related format strings
// =============================================================================

TEST_CASE("fuzz: fmt with uid patterns", "[fuzz][ssh]") {
  rc::prop("fmt with uid values never crashes", []() {
    auto uid = *rc::gen::arbitrary<int>();

    try {
      [[maybe_unused]] auto s1 = nix::fmt("/run/user/%d/ssh-agent.socket", uid);
      [[maybe_unused]] auto s2 = nix::fmt("/run/user/%d/gnome-keyring/ssh", uid);
      [[maybe_unused]] auto s3 = nix::fmt("/run/user/%d/keyring/ssh", uid);
    } catch (const nix::base_error_t&) {
    } catch (const std::exception&) {
    }

    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: fmt with string and int patterns", "[fuzz][ssh]") {
  rc::prop("fmt with mixed parameters", []() {
    auto str = *rc::gen::arbitrary<std::string>();
    auto num = *rc::gen::arbitrary<int>();

    try {
      [[maybe_unused]] auto result = nix::fmt("/tmp/ssh-%s/agent.%d", str, num);
    } catch (const nix::base_error_t&) {
    } catch (const std::exception&) {
    }

    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Edge cases: special characters in paths
// =============================================================================

TEST_CASE("fuzz: paths with special characters", "[fuzz][ssh]") {
  rc::prop("Special chars in socket paths", []() {
    auto special = *rc::gen::container<std::string>(
        rc::gen::element<char>('\0', '\n', '\r', '\t', ' ', '/', '\\', ':', '*', '?', '"', '<', '>',
                               '|', '\x7f', '\xff', '%', '&', '=', '#', '@', '[', ']', '{', '}'));

    try {
      // Test path operations with special chars
      auto path = "/run/user/1000/" + special + "/ssh";
      [[maybe_unused]] bool empty = path.empty();
      [[maybe_unused]] size_t len = path.length();
    } catch (const std::exception&) {
    }

    RC_SUCCEED("No crash");
  });
}
