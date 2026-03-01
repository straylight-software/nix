// straylight // nix // store // tests
//
// SSH agent discovery benchmarks - measuring overhead of socket probing
//
// These benchmarks measure the cost of SSH agent socket discovery that
// occurs at nix startup when running as root (e.g., sudo nix ...).
//
// Performance context:
//   - Discovery runs ONCE at startup, not per-operation
//   - Probes multiple filesystem locations for agent sockets
//   - Must be fast enough to not add noticeable startup latency
//   - Target: <1ms total discovery time
//
// Run with: buck2 test //src/nix/store/tests:ssh_bench

#include <filesystem>
#include <string>
#include <vector>

#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include "nix/util/environment-variables.h"
#include "nix/util/fmt.h"

using nix::fmt;
using nix::get_env;

// =============================================================================
// Benchmark helpers - simulate socket discovery paths
// =============================================================================

namespace {

/// Check if a path exists and is a socket (mirrors find_ssh_auth_sock logic)
auto check_socket_path(const std::string& path) -> bool {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

/// Get current user's UID for path construction
auto get_current_uid() -> uid_t {
  return getuid();
}

/// Get passwd entry for a username
auto get_passwd_entry(const char* username) -> struct passwd* {
  return getpwnam(username);
}

/// Generate candidate socket paths (mirrors find_ssh_auth_sock)
auto generate_candidate_paths(uid_t uid) -> std::vector<std::string> {
  return {
      fmt("/run/user/%d/ssh-agent.socket", uid),
      fmt("/run/user/%d/ssh-agent", uid),
      fmt("/run/user/%d/gnome-keyring/ssh", uid),
      fmt("/run/user/%d/keyring/ssh", uid),
  };
}

} // namespace

// =============================================================================
// Benchmarks - SSH agent discovery operations
// =============================================================================

TEST_CASE("SSH agent discovery benchmarks", "[bench][ssh]") {
  SECTION("getuid() call overhead") {
    // Baseline: how fast is getting the current UID?
    BENCHMARK("getuid") {
      return getuid();
    };
  }

  SECTION("getpwnam lookup overhead") {
    // Getting passwd entry for username resolution
    const char* username = "root";

    BENCHMARK("getpwnam(root)") {
      return getpwnam(username);
    };

    // Try current user if available
    if (auto sudo_user = get_env("USER")) {
      BENCHMARK("getpwnam(current_user)") {
        return getpwnam(sudo_user->c_str());
      };
    }
  }

  SECTION("stat() on non-existent paths") {
    // Worst case: probing paths that don't exist
    std::vector<std::string> fake_paths = {
        "/nonexistent/path/1",
        "/nonexistent/path/2",
        "/nonexistent/path/3",
        "/nonexistent/path/4",
    };

    BENCHMARK("stat 4 non-existent paths") {
      int count = 0;
      for (const auto& path : fake_paths) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
          count++;
        }
      }
      return count;
    };
  }

  SECTION("stat() on /run/user paths") {
    // Realistic case: probing actual /run/user locations
    uid_t uid = getuid();
    auto candidates = generate_candidate_paths(uid);

    BENCHMARK("stat candidate socket paths") {
      int found = 0;
      for (const auto& path : candidates) {
        if (check_socket_path(path)) {
          found++;
        }
      }
      return found;
    };
  }

  SECTION("directory iteration for ~/.ssh/agent") {
    // Probing user's home directory for agent sockets
    std::string home_agent_dir = "/tmp"; // Use /tmp as proxy for iteration test

    BENCHMARK("iterate /tmp directory") {
      int count = 0;
      try {
        for (const auto& entry : std::filesystem::directory_iterator(home_agent_dir)) {
          count++;
          if (count > 100)
            break; // Limit iteration
        }
      } catch (...) {
        // Ignore errors
      }
      return count;
    };
  }

  SECTION("full discovery simulation") {
    // Simulate complete find_ssh_auth_sock logic

    BENCHMARK("full agent discovery (no socket)") {
      // 1. Check if SSH_AUTH_SOCK already set
      if (get_env("__BENCH_SSH_AUTH_SOCK")) {
        return std::string{};
      }

      // 2. Get UID (simulated as non-root for benchmark)
      uid_t uid = getuid();

      // 3. Check candidate paths
      auto candidates = generate_candidate_paths(uid);
      for (const auto& path : candidates) {
        if (check_socket_path(path)) {
          return path;
        }
      }

      // 4. Would check ~/.ssh/agent and /tmp/ssh-* here
      // Skipped for benchmark to avoid filesystem variation

      return std::string{};
    };
  }

  SECTION("environment variable operations") {
    // Cost of get_env calls
    BENCHMARK("get_env(SSH_AUTH_SOCK)") {
      return get_env("SSH_AUTH_SOCK");
    };

    BENCHMARK("get_env(SUDO_USER)") {
      return get_env("SUDO_USER");
    };

    BENCHMARK("get_env(nonexistent)") {
      return get_env("__NONEXISTENT_VAR_12345__");
    };
  }
}

// =============================================================================
// Correctness tests - ensure discovery logic works
// =============================================================================

TEST_CASE("SSH agent discovery correctness", "[ssh]") {
  SECTION("candidate path generation") {
    auto paths = generate_candidate_paths(1000);

    REQUIRE(paths.size() == 4);
    REQUIRE(paths[0] == "/run/user/1000/ssh-agent.socket");
    REQUIRE(paths[1] == "/run/user/1000/ssh-agent");
    REQUIRE(paths[2] == "/run/user/1000/gnome-keyring/ssh");
    REQUIRE(paths[3] == "/run/user/1000/keyring/ssh");
  }

  SECTION("getpwnam returns valid entry for root") {
    auto* pw = getpwnam("root");
    REQUIRE(pw != nullptr);
    REQUIRE(pw->pw_uid == 0);
  }

  SECTION("stat correctly identifies non-sockets") {
    // /tmp exists but is a directory, not a socket
    REQUIRE_FALSE(check_socket_path("/tmp"));

    // /etc/passwd exists but is a file, not a socket
    REQUIRE_FALSE(check_socket_path("/etc/passwd"));

    // Non-existent path
    REQUIRE_FALSE(check_socket_path("/nonexistent/path"));
  }
}
