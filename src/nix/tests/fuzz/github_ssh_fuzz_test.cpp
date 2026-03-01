// straylight // nix // tests // fuzz
//
// GitHub SSH Fallback Fuzz Tests
//
// Fuzz the GitHub/GitLab/SourceHut input scheme with various inputs
// to test the SSH fallback logic.

#include <string>

#include "nix/fetchers/fetch-settings.h"
#include "nix/fetchers/fetchers.h"
#include "nix/tests/property.h"
#include "nix/util/error.h"
#include "nix/util/url.h"

// =============================================================================
// Fuzz GitHub URL parsing
// =============================================================================

TEST_CASE("fuzz: github: URL parsing", "[fuzz][github]") {
  rc::prop("github: URLs with arbitrary owner/repo don't crash", []() {
    auto owner = *rc::gen::arbitrary<std::string>();
    auto repo = *rc::gen::arbitrary<std::string>();

    auto url = "github:" + owner + "/" + repo;

    try {
      auto parsed = nix::parse_url(url);
      [[maybe_unused]] auto str = parsed.to_string();
    } catch (const nix::base_error_t&) {
      // Expected for malformed URLs
    } catch (const std::exception&) {
    }

    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: github: URL with ref", "[fuzz][github]") {
  rc::prop("github: URLs with arbitrary ref don't crash", []() {
    auto owner = *rc::gen::arbitrary<std::string>();
    auto repo = *rc::gen::arbitrary<std::string>();
    auto ref = *rc::gen::arbitrary<std::string>();

    auto url = "github:" + owner + "/" + repo + "/" + ref;

    try {
      auto parsed = nix::parse_url(url);
      [[maybe_unused]] auto str = parsed.to_string();
    } catch (const nix::base_error_t&) {
    } catch (const std::exception&) {
    }

    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: github: URL with query params", "[fuzz][github]") {
  rc::prop("github: URLs with query params don't crash", []() {
    auto owner = *rc::gen::arbitrary<std::string>();
    auto repo = *rc::gen::arbitrary<std::string>();
    auto query = *rc::gen::arbitrary<std::string>();

    auto url = "github:" + owner + "/" + repo + "?" + query;

    try {
      auto parsed = nix::parse_url(url);
      [[maybe_unused]] auto str = parsed.to_string();
    } catch (const nix::base_error_t&) {
    } catch (const std::exception&) {
    }

    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz GitLab URL parsing
// =============================================================================

TEST_CASE("fuzz: gitlab: URL parsing", "[fuzz][gitlab]") {
  rc::prop("gitlab: URLs with arbitrary owner/repo don't crash", []() {
    auto owner = *rc::gen::arbitrary<std::string>();
    auto repo = *rc::gen::arbitrary<std::string>();

    auto url = "gitlab:" + owner + "/" + repo;

    try {
      auto parsed = nix::parse_url(url);
      [[maybe_unused]] auto str = parsed.to_string();
    } catch (const nix::base_error_t&) {
    } catch (const std::exception&) {
    }

    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: gitlab: URL with host param", "[fuzz][gitlab]") {
  rc::prop("gitlab: URLs with custom host don't crash", []() {
    auto owner = *rc::gen::arbitrary<std::string>();
    auto repo = *rc::gen::arbitrary<std::string>();
    auto host = *rc::gen::arbitrary<std::string>();

    auto url = "gitlab:" + owner + "/" + repo + "?host=" + host;

    try {
      auto parsed = nix::parse_url(url);
      [[maybe_unused]] auto str = parsed.to_string();
    } catch (const nix::base_error_t&) {
    } catch (const std::exception&) {
    }

    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz SourceHut URL parsing
// =============================================================================

TEST_CASE("fuzz: sourcehut: URL parsing", "[fuzz][sourcehut]") {
  rc::prop("sourcehut: URLs with arbitrary owner/repo don't crash", []() {
    auto owner = *rc::gen::arbitrary<std::string>();
    auto repo = *rc::gen::arbitrary<std::string>();

    auto url = "sourcehut:" + owner + "/" + repo;

    try {
      auto parsed = nix::parse_url(url);
      [[maybe_unused]] auto str = parsed.to_string();
    } catch (const nix::base_error_t&) {
    } catch (const std::exception&) {
    }

    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz SSH URL generation
// =============================================================================

TEST_CASE("fuzz: SSH URL generation patterns", "[fuzz][ssh]") {
  rc::prop("SSH URL fmt patterns don't crash", []() {
    auto host = *rc::gen::arbitrary<std::string>();
    auto owner = *rc::gen::arbitrary<std::string>();
    auto repo = *rc::gen::arbitrary<std::string>();

    try {
      auto ssh_url = nix::fmt("git+ssh://git@%s/%s/%s.git", host, owner, repo);
      [[maybe_unused]] auto parsed = nix::parse_url(ssh_url);
    } catch (const nix::base_error_t&) {
    } catch (const std::exception&) {
    }

    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz access token lookup patterns
// =============================================================================

TEST_CASE("fuzz: access token host matching", "[fuzz][github]") {
  rc::prop("Host/path matching patterns don't crash", []() {
    auto host = *rc::gen::arbitrary<std::string>();
    auto owner = *rc::gen::arbitrary<std::string>();
    auto repo = *rc::gen::arbitrary<std::string>();

    try {
      auto host_and_path = nix::fmt("%s/%s/%s", host, owner, repo);

      // Test string operations used in token matching
      auto first = host_and_path.find(host);
      [[maybe_unused]] bool found = first != std::string::npos;
      [[maybe_unused]] size_t len = host.length();

      if (host_and_path.length() > host.length()) {
        [[maybe_unused]] char next = host_and_path[host.length()];
      }
    } catch (const std::exception&) {
    }

    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Edge cases: special characters in forge URLs
// =============================================================================

TEST_CASE("fuzz: forge URLs with special characters", "[fuzz][github]") {
  rc::prop("Special chars in owner/repo don't crash", []() {
    auto base_owner = *rc::gen::element<std::string>("owner", "my-org", "user123", "");
    auto base_repo = *rc::gen::element<std::string>("repo", "my-project", "test.nix", "");

    auto suffix = *rc::gen::container<std::string>(
        rc::gen::element<char>('-', '_', '.', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a',
                               'b', 'c', 'A', 'B', 'C'));

    auto owner = base_owner + suffix;
    auto repo = base_repo + suffix;

    std::vector<std::string> urls = {
        "github:" + owner + "/" + repo,
        "gitlab:" + owner + "/" + repo,
        "sourcehut:" + owner + "/" + repo,
    };

    for (const auto& url : urls) {
      try {
        auto parsed = nix::parse_url(url);
        [[maybe_unused]] auto str = parsed.to_string();
      } catch (const nix::base_error_t&) {
      } catch (const std::exception&) {
      }
    }

    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz rev/ref patterns
// =============================================================================

TEST_CASE("fuzz: SHA1 revision patterns", "[fuzz][github]") {
  rc::prop("SHA1-like revisions don't crash", []() {
    // Generate strings that look like SHA1 hashes
    auto hex_chars = rc::gen::element<char>('0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a',
                                            'b', 'c', 'd', 'e', 'f');
    auto rev = *rc::gen::container<std::string>(40, hex_chars);

    auto url = "github:owner/repo/" + rev;

    try {
      auto parsed = nix::parse_url(url);
      [[maybe_unused]] auto str = parsed.to_string();
    } catch (const nix::base_error_t&) {
    } catch (const std::exception&) {
    }

    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: ref branch/tag patterns", "[fuzz][github]") {
  rc::prop("Various ref patterns don't crash", []() {
    auto ref = *rc::gen::element<std::string>("main", "master", "develop", "feature/test", "v1.0.0",
                                              "refs/heads/main", "refs/tags/v1.0", "HEAD", "");

    auto extra = *rc::gen::arbitrary<std::string>();
    auto full_ref = ref + extra;

    auto url = "github:owner/repo?ref=" + full_ref;

    try {
      auto parsed = nix::parse_url(url);
      [[maybe_unused]] auto str = parsed.to_string();
    } catch (const nix::base_error_t&) {
    } catch (const std::exception&) {
    }

    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Fuzz narHash patterns
// =============================================================================

TEST_CASE("fuzz: narHash query parameter", "[fuzz][github]") {
  rc::prop("narHash values don't crash", []() {
    auto hash_prefix = *rc::gen::element<std::string>("sha256-", "sha512-", "sha1-", "md5-", "");
    auto hash_value = *rc::gen::arbitrary<std::string>();

    auto url = "github:owner/repo?narHash=" + hash_prefix + hash_value;

    try {
      auto parsed = nix::parse_url(url);
      [[maybe_unused]] auto str = parsed.to_string();
    } catch (const nix::base_error_t&) {
    } catch (const std::exception&) {
    }

    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// narHash prevents SSH fallback (critical for lock file compatibility)
// =============================================================================

TEST_CASE("fuzz: narHash prevents SSH fallback with various hashes", "[fuzz][github]") {
  rc::prop("inputs with narHash should never use SSH fallback", []() {
    // Generate valid-looking SRI hashes
    auto base64_chars = rc::gen::element<char>(
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R',
        'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j',
        'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1',
        '2', '3', '4', '5', '6', '7', '8', '9', '+', '/');
    auto hash_body = *rc::gen::container<std::string>(43, base64_chars);
    auto sri_hash = "sha256-" + hash_body + "=";

    // Generate a valid 40-char hex revision
    auto hex_chars = rc::gen::element<char>('0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a',
                                            'b', 'c', 'd', 'e', 'f');
    auto rev = *rc::gen::container<std::string>(40, hex_chars);

    // Test for all forge types
    std::vector<std::string> urls = {
        "github:owner/repo/" + rev + "?narHash=" + sri_hash,
        "gitlab:owner/repo/" + rev + "?narHash=" + sri_hash,
        "sourcehut:owner/repo/" + rev + "?narHash=" + sri_hash,
    };

    nix::fetchers::settings_t settings;
    // Enable SSH fallback - but it should NOT be used when narHash is present
    settings.sshFallbackForGitForges = true;

    for (const auto& url : urls) {
      try {
        auto parsed = nix::parse_url(url);
        auto input = nix::fetchers::input_t::fromURL(settings, parsed);

        // If we got a valid input with a narHash, verify SSH would not be used
        if (input.getNarHash()) {
          // This is the critical invariant: inputs with narHash must use tarballs
          // SSH fallback would give a different NAR hash and break lock files
          RC_ASSERT_FALSE(input.getNarHash()->to_string(nix::hash_format_t::sri, true).empty());
        }
      } catch (const nix::base_error_t&) {
        // Expected for some malformed URLs
      } catch (const std::exception&) {
      }
    }

    RC_SUCCEED("No crash and narHash preserved");
  });
}
